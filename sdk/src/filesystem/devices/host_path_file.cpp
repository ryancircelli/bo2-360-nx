/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2013 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 *
 * @modified    Tom Clay, 2026 - Adapted for ReXGlue runtime
 */

#include <rex/filesystem/devices/host_path_entry.h>
#include <rex/filesystem/devices/host_path_file.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstring>
#include <thread>
#include <map>
#include <memory>
#include <mutex>
#include <new>
#include <string>
#include <unordered_map>
#include <vector>

#include <rex/cvar.h>
#include <rex/logging.h>

#include <fmt/format.h>

/*
 * Size of the read-ahead window, in KB. 0 turns it off.
 *
 * 256 KB rather than 64 KB. With 64 KB the entry threshold was 16 KB (window/4), and the game does
 * not do a single read of 16 KB or less: its average is 375 KB and the smallest one it repeats in a
 * loop is 64 KB (measured with 64 KB: 5 hits out of 1,232 reads). With 256 KB the threshold lands
 * exactly on those 64 KB. The RAM ceiling goes down, not up: 16 windows x 256 KB = 4 MB, against
 * 128 x 64 KB = 8 MB.
 */
REXCVAR_DEFINE_INT32(switch_io_window_kb, 256, "Filesystem",
                     "Read-ahead per file, in KB (0 = off). Read-only data only.");

// How many windows can exist at once. A hard cap so that opening many files does not eat the RAM.
// 128 was an arbitrary number; in a whole measured session there were never more than 10 live windows.
REXCVAR_DEFINE_INT32(switch_io_windows_max, 16, "Filesystem",
                     "Most files with read-ahead at once.");

/*
 * Split large direct reads into pieces of this many MB. 0 = a single call.
 *
 * The worst case measured is a pread() of 19,932,032 bytes that takes 261.5 ms (there are 7 reads
 * over 8 MB, 169.2 ms on average). During those 261 ms the thread passes no point where the
 * scheduler can cleanly switch it out, and the per-file lock of XFile (file_lock_) stays held the
 * whole time.
 *
 * The cost of splitting can be bounded with the same data: fitting time = fixed + bytes/rate over
 * the 4.00 MB reads (59.8 ms on average, 21 samples) and the 19.01 MB reads (275.6 ms, 2 samples)
 * gives rate = 69.5 MB/s and fixed = 2.3 ms per call. So splitting 19 MB into 4 MB pieces is 4
 * extra calls = +9 ms over 275 (+3 %); 1 MB pieces would be 18 extra = +41 ms (+15 %). Hence 4
 * and not 1.
 *
 * In exchange, the worst pread drops from 261 ms to about 60, and short reads from the file system
 * are no longer visible: the loop keeps asking from where it stopped and only stops when a piece
 * returns 0 bytes (real end of file). The unsplit path returned the short read as it was.
 *
 * Measured: this does not raise FPS. Reads run on a thread other than the render thread, and outside
 * loading screens there is no correlation between slow reads and stutters. It is on because it
 * bounds the worst case and fixes short reads, not because a gain is expected.
 */
REXCVAR_DEFINE_INT32(switch_io_chunk_mb, 4, "Filesystem",
                     "Split large direct reads into pieces of this many MB (0 = all at once).");

/*
 * RAM read cache by blocks (switch_io_cache_mb). Off: on the console it does the opposite of what it promises.
 *
 * It is meant for a game that streams per zone pack: on leaving a zone it releases the whole pack and asks for it
 * again on entering, so most of what it reads from the SD has been read before, and its memory manager cannot be
 * told no. The only remedy is to make the second read not cost a trip to the SD. The cache keeps 256 KB blocks in an LRU and reads missing blocks grouped, in a single pread per contiguous
 * range, so a first read costs the same as without it; only read-only devices and files opened without write
 * access go through it.
 *
 * On the console it reads more than it saves. When a game asks for more unique data than fits, the LRU evicts nonstop, and each 256 KB block is
 * brought in to serve a small request and thrown away before it is reused. It also blocks: g_cache_mutex is a
 * single mutex for all files, held during the disk read, so every other reading thread waits behind it, and that
 * wait does not show in `[io] 15 s`, which only times inside read(). The range cache below replaces it.
 */
REXCVAR_DEFINE_INT32(switch_io_cache_mb, 0, "Filesystem",
                     "RAM cache of what was already read from disk, in MB (0 = off). Read-only data "
                     "only (off: on the console it reads more than it saves).");

/*
 * Read cache by exact range (switch_io_ranges_mb), the replacement for the block cache above.
 *
 * Why. A streamed pack that the game releases and asks for again, at the same offset and with the same size, is a
 * slow reread of a large file, and such a read can stall a frame for tens of milliseconds while the GPU and the
 * recording are normal.
 *
 * Why by range and not by block. When few of the offsets asked for are aligned to 256 KB, a block cache reads
 * several times more from the SD than is requested. Storing the exact range (file, offset, bytes) has no
 * amplification by construction: not a byte is read that the game did not ask for.
 *
 * Sizing. What repeats is usually small: requests of a few hundred KB to a couple of MB, each repeated a few times.
 * Only reads of switch_io_ranges_min_kb (256 KB) or more are kept; a 4 MB floor would see almost none of them. A
 * level load's sequential sweep (consecutive 4 MB blocks, each read once) is not stored at all (see
 * IsSweepAndAnnotate): it would only fill the LRU. The cap has to hold the whole repeating set: below it the cache is
 * not slightly worse but worthless, because entries are evicted before they are reused and nothing hits.
 *
 * The memory comes from the host heap (1,024 MB, runtime_switch.cpp), of which the guest backing and the thread
 * stacks take ~506 MB; the GPU budget is separate. 64 MB leaves margin; 128 would not in the worst case.
 *
 * The lock is not held across the disk read: it is taken to look up the table and released before going to disk.
 * The data lives in a shared_ptr, so copying it needs no lock either, and an eviction cannot free the buffer
 * another thread is reading.
 */
/*
 * The read rate of BO2's streamed image packs (.ipak).
 *
 * The game's image streaming keeps its pool full and, in a Town match, evicts and reads back the same images
 * without end: on the PC 1,700-27,000 MB per 15 s of base.ipak, every byte of it read before (the range cache above
 * serves it from RAM), with 30,000-420,000 searches of the pool allocator per 10 s (each under the game's critical
 * section 6) and the job threads putting back the jobs that wait for that section. On the 360 its disk bounded that
 * loop (an installed game reads at most ~30-40 MB/s); here a read returns at memory speed and the loop runs as fast
 * as the CPU allows. Delivering the packs at 32 MB/s (the reading thread sleeps before each read, so the rate holds
 * however the reads are split) gives the game the throughput it was made for: on the PC 3-7 MB/s are read on
 * average (the loop settles instead of spinning), 170-2,200 allocator searches per 10 s, each job worker
 * ~105 -> ~84 ms/s of CPU, the Backend ~118 -> ~91, Main ~142 -> ~121, frame times and level load time unchanged.
 * 0 = no limit.
 */
REXCVAR_DEFINE_INT32(switch_io_ipak_mb_s, 32, "Filesystem",
                     "Reads of the game's streamed image packs (.ipak files) are delivered at most this many MB/s, "
                     "about what the 360's disk allowed (0 = no limit). Without it the game's image streaming "
                     "rereads the same images as fast as the CPU allows");
REXCVAR_DEFINE_BOOL(switch_io_report_files, false, "Filesystem",
                    "Diagnostics: the [io] summary also lists the files read most in its interval (reads, MB)");

REXCVAR_DEFINE_INT32(switch_io_ranges_mb, 64, "Filesystem",
                     "Read cache by exact range, total cap in MB (0 = off). 64 = the footprint "
                     "of the repeated reads with no evictions; more buys nothing.");

// Floor, in KB. A 4 MB floor would leave out the small requests that repeat most (a few hundred KB to 2 MB).
REXCVAR_DEFINE_INT32(switch_io_ranges_min_kb, 256, "Filesystem",
                     "Only reads of this many KB or more are cached. 256 = the point where the simulation "
                     "stops gaining (with 128 evictions go up and hits go down).");

// Compatibility: the old cvar in MB. If a toml sets it to a value > 0, it overrides the KB one.
REXCVAR_DEFINE_INT32(switch_io_ranges_min_mb, 0, "Filesystem",
                     "OBSOLETE: use switch_io_ranges_min_kb. If > 0 it overrides the KB one.");

// Per-entry ceiling: a single read cannot take more than this out of the total cap.
REXCVAR_DEFINE_INT32(switch_io_ranges_max_mb, 12, "Filesystem",
                     "No range cache entry is larger than this many MB.");

namespace rex::filesystem {

namespace {
thread_local const char* t_last_read_kind = "none";
}  // namespace

const char* LastReadKind() { return t_last_read_kind; }
void ResetLastReadKind() { t_last_read_kind = "not-host-file"; }

namespace {
std::atomic<uint64_t> g_hits{0};
std::atomic<uint64_t> g_fills{0};
std::atomic<uint64_t> g_direct{0};
std::atomic<uint64_t> g_bytes_ram{0};
std::atomic<int64_t> g_windows_live{0};

// Above this it does not pay off: the request is already large and the window would only add an extra copy.
constexpr size_t kRequestMaxFraction = 4;  // requested <= window/4

/* The read cache (see switch_io_cache_mb). */
constexpr size_t kBlockCache = 256 * 1024;

struct BlockCache {
  std::vector<uint8_t> data;  // may be shorter than kBlockCache at end of file
  uint64_t use = 0;            // for the LRU
};

std::mutex g_cache_mutex;
std::unordered_map<uint64_t, BlockCache> g_cache;   // key: file id << 32 | block
std::map<uint64_t, uint64_t> g_cache_lru;            // use -> key, oldest first
std::unordered_map<std::string, uint32_t> g_cache_ids;  // path -> id, so the key is exact
uint64_t g_cache_bytes = 0;
uint64_t g_cache_use = 0;
uint64_t g_cache_hits = 0;
uint64_t g_cache_misses = 0;
uint64_t g_cache_bytes_served = 0;
uint64_t g_cache_bytes_disk = 0;
uint64_t g_cache_evicted = 0;
uint64_t g_cache_report = 0;
bool g_cache_no_memory = false;  // turned itself off for lack of RAM: not retried

// Stable id per path, assigned the first time it is opened. With it, the block key cannot
// collide between files (a 64-bit hash could, and a collision would serve another file's data).
uint32_t PathId(const std::string& path_str) {
  std::lock_guard lock(g_cache_mutex);
  const auto [it, fresh] = g_cache_ids.try_emplace(path_str, uint32_t(g_cache_ids.size() + 1));
  return it->second;
}

// Marks a block as just used. Called with g_cache_mutex held.
void TouchBlock(uint64_t key, BlockCache& block) {
  if (block.use) {
    g_cache_lru.erase(block.use);
  }
  block.use = ++g_cache_use;
  g_cache_lru[block.use] = key;
}

// Brings the cache under the limit by dropping the oldest blocks. Called with g_cache_mutex held.
void PruneCache(size_t limit) {
  while (g_cache_bytes > limit && !g_cache_lru.empty()) {
    const auto old = g_cache_lru.begin();
    const auto it = g_cache.find(old->second);
    if (it != g_cache.end()) {
      g_cache_bytes -= it->second.data.size();
      g_cache.erase(it);
      ++g_cache_evicted;
    }
    g_cache_lru.erase(old);
  }
}

/*
 * Serves the read from RAM, bringing from disk only the missing blocks (grouped, one pread per
 * contiguous range). Returns false if it cannot: then the usual path answers and nothing that
 * matters has been touched.
 */
bool ReadWithCache(FileHandle* fh, uint32_t id, std::span<uint8_t> buffer, size_t byte_offset,
                  size_t* out_bytes_read) {
  const size_t limit = static_cast<size_t>(REXCVAR_GET(switch_io_cache_mb)) * 1024u * 1024u;
  const size_t requested = buffer.size();
  if (!limit || !requested || !id || g_cache_no_memory) {
    return false;
  }
  const uint64_t first = byte_offset / kBlockCache;
  const uint64_t last = (byte_offset + requested - 1) / kBlockCache;
  // A request larger than half the cache would fill it on its own and evict everything useful.
  if ((last - first + 1) * kBlockCache > limit / 2) {
    return false;
  }

  std::lock_guard lock(g_cache_mutex);
  size_t copied = 0;
  for (uint64_t b = first; b <= last; ++b) {
    const uint64_t key = (static_cast<uint64_t>(id) << 32) | b;
    auto it = g_cache.find(key);
    if (it == g_cache.end()) {
      // Contiguous run of missing blocks: a single read for all of them.
      uint64_t end = b;
      while (end < last && !g_cache.count((static_cast<uint64_t>(id) << 32) | (end + 1))) {
        ++end;
      }
      const size_t span = static_cast<size_t>(end - b + 1) * kBlockCache;
      size_t read_count = 0;
      /*
       * If the console does not have that RAM, the cache turns itself off and does not try again:
       * the game carries on down the usual path. It can never bring the process down for lack of memory.
       */
      try {
        std::vector<uint8_t> tmp(span);
        /*
         * Same as ReadDirect: in pieces, and a short read is not end of file (the file system
         * truncates large ones). The loop only stops when a piece returns 0 bytes. Without this,
         * half-filled blocks would be cached and the game would get incomplete data.
         */
        const int32_t chunk_mb = REXCVAR_GET(switch_io_chunk_mb);
        const size_t chunk = chunk_mb > 0 ? static_cast<size_t>(chunk_mb) * 1024u * 1024u : span;
        while (read_count < span) {
          size_t n = 0;
          const size_t asks = std::min(chunk, span - read_count);
          if (!fh->Read(b * kBlockCache + read_count, tmp.data() + read_count, asks, &n)) {
            if (read_count == 0) {
              return false;  // real error: let the usual path answer
            }
            break;
          }
          if (n == 0) {
            break;  // end of file
          }
          read_count += n;
        }
        ++g_cache_misses;
        g_cache_bytes_disk += read_count;
        for (uint64_t k = b; k <= end; ++k) {
          const size_t since = static_cast<size_t>(k - b) * kBlockCache;
          if (since >= read_count) {
            break;  // end of file
          }
          const size_t n = std::min(kBlockCache, read_count - since);
          const uint64_t ck = (static_cast<uint64_t>(id) << 32) | k;
          BlockCache& fresh = g_cache[ck];
          fresh.data.assign(tmp.begin() + since, tmp.begin() + since + n);
          g_cache_bytes += n;
          TouchBlock(ck, fresh);
        }
      } catch (const std::bad_alloc&) {
        const uint64_t had_mb = g_cache_bytes >> 20;
        g_cache_no_memory = true;
        g_cache.clear();
        g_cache_lru.clear();
        g_cache_bytes = 0;
        REXLOG_WARN("[io] RAM cache: out of memory, turning off (it was at {} MB). The game keeps reading "
                    "from disk as before",
                    had_mb);
        return false;
      }
      PruneCache(limit);
      it = g_cache.find(key);  // the insertion may have rebuilt the table
      if (it == g_cache.end()) {
        break;  // not even the first block had bytes: end of file
      }
    } else {
      ++g_cache_hits;
      TouchBlock(key, it->second);
    }
    const size_t inside = byte_offset + copied - static_cast<size_t>(b * kBlockCache);
    if (inside >= it->second.data.size()) {
      break;  // the block ends before the requested range: end of file
    }
    const size_t n = std::min(it->second.data.size() - inside, requested - copied);
    std::memcpy(buffer.data() + copied, it->second.data.data() + inside, n);
    copied += n;
    if (it->second.data.size() < kBlockCache) {
      break;  // short block: no more file
    }
  }
  g_cache_bytes_served += copied;
  *out_bytes_read = copied;

  const uint64_t loops = g_cache_hits + g_cache_misses;
  if (loops >= g_cache_report + 200) {
    g_cache_report = loops;
    REXLOG_INFO("[io] RAM cache: {} blocks ({} MB of {}), {} served from RAM and {} from disk "
                "({:.1f} % hits); {:.1f} MB delivered, {:.1f} read from the SD, {} blocks "
                "evicted",
                g_cache.size(), g_cache_bytes >> 20, REXCVAR_GET(switch_io_cache_mb), g_cache_hits,
                g_cache_misses, loops ? 100.0 * double(g_cache_hits) / double(loops) : 0.0,
                double(g_cache_bytes_served) / 1048576.0, double(g_cache_bytes_disk) / 1048576.0,
                g_cache_evicted);
  }
  return true;
}

/*
 * ================================================================================================
 * The exact-range cache. See switch_io_ranges_mb above.
 *
 * The table is a vector and is scanned in full. This is deliberate: it was sized for a 32 MB cap
 * and a 4 MB floor per entry, where at most eight entries fit and the linear search is eight
 * integer comparisons (the current 64 MB cap and 256 KB floor allow up to 256). A hash map would
 * only add a hash function, a rehash that invalidates references and a possible collision that
 * would serve another file's data.
 * ================================================================================================
 */
struct RangeEntry {
  uint32_t id = 0;               // file (stable id per path, the same as PathId)
  uint64_t offset_bytes = 0;   // where the read starts
  uint32_t requested = 0;           // how much the game asked for: part of the key, not a detail
  // The bytes the disk returned. It can be shorter than `requested` if the file ends earlier; it is
  // stored as is and returned as is, which is what the disk would have answered.
  std::shared_ptr<const std::vector<uint8_t>> data;
  uint64_t use = 0;              // for the LRU
};

std::mutex g_ranges_mutex;
std::vector<RangeEntry> g_ranges;
uint64_t g_ranges_use = 0;
uint64_t g_ranges_bytes = 0;
bool g_ranges_no_memory = false;  // turned itself off for lack of RAM: not retried

// Counters. Atomic so the periodic summary can read them without taking the lock.
std::atomic<uint64_t> g_ranges_hits{0};
std::atomic<uint64_t> g_ranges_misses{0};
std::atomic<uint64_t> g_ranges_bytes_ram{0};
std::atomic<uint64_t> g_ranges_bytes_disk{0};
std::atomic<uint64_t> g_range_evictions{0};
std::atomic<uint64_t> g_range_entries{0};
std::atomic<uint64_t> g_range_bytes_live{0};
// Why something is not cached. Without this the cache could not be tuned: the counter said how
// often it hit, but not whether the misses came from the floor, the ceiling or the load sweep.
std::atomic<uint64_t> g_ranges_low_floor{0};     // reads skipped for being too small
std::atomic<uint64_t> g_ranges_over_ceiling{0};    // skipped for being too large
std::atomic<uint64_t> g_range_sequential{0};   // rejected as part of the load sweep
std::atomic<uint64_t> g_range_sequential_mb{0};

// Last byte read for each file, to detect the sequential sweep. Guarded by g_ranges_mutex.
std::unordered_map<uint32_t, uint64_t> g_ranges_last_end;

int64_t FloorBytes() {
  const int64_t old_mb = REXCVAR_GET(switch_io_ranges_min_mb);
  if (old_mb > 0) {
    return old_mb * 1024 * 1024;  // an old toml sets it: honor it
  }
  return int64_t(REXCVAR_GET(switch_io_ranges_min_kb)) * 1024;
}

// Does this read go into the cache? Only the size decides; the constructor already took care of
// the file being read-only when it gave (or did not give) it an id.
bool RangeEligible(size_t requested) {
  const int64_t cap = int64_t(REXCVAR_GET(switch_io_ranges_mb)) * 1024 * 1024;
  if (cap <= 0) {
    return false;
  }
  const int64_t ceiling = int64_t(REXCVAR_GET(switch_io_ranges_max_mb)) * 1024 * 1024;
  if (int64_t(requested) < FloorBytes()) {
    g_ranges_low_floor.fetch_add(1, std::memory_order_relaxed);
    return false;
  }
  if (int64_t(requested) > ceiling || int64_t(requested) > cap) {
    g_ranges_over_ceiling.fetch_add(1, std::memory_order_relaxed);
    return false;
  }
  return true;
}

/*
 * The level load sweep: large reads whose offset is exactly the end of the previous read of the
 * same file. Dozens of consecutive 4 MB blocks, each read only once, would fill the LRU with garbage
 * before play starts.
 *
 * Bounded so as not to drop anything that is reread: it only counts as a sweep if the read is also
 * 4 MB or more, above the sizes that are typically reread. The end of every eligible read is
 * recorded (including the first read of the sweep, which is stored: it is one block of many and
 * gets evicted on its own).
 */
constexpr uint64_t kSweepMin = uint64_t(4) << 20;

bool IsSweepAndAnnotate(uint32_t id, uint64_t offset_bytes, uint32_t requested) {
  // Call with g_ranges_mutex held.
  auto it = g_ranges_last_end.find(id);
  const bool contiguous = it != g_ranges_last_end.end() && it->second == offset_bytes;
  g_ranges_last_end[id] = offset_bytes + requested;
  return contiguous && requested >= kSweepMin;
}

/*
 * Looks up the exact range. Returns the buffer, or nullptr if it was not there.
 *
 * It returns a shared_ptr on purpose: the caller releases the lock before copying the megabytes,
 * and while it copies another thread may evict this entry. With the shared_ptr that only removes
 * the entry from the table; the bytes stay alive until the last reader releases them.
 */
std::shared_ptr<const std::vector<uint8_t>> FindRange(uint32_t id, uint64_t offset_bytes,
                                                        uint32_t requested) {
  std::lock_guard lock(g_ranges_mutex);
  for (auto& e : g_ranges) {
    if (e.id == id && e.offset_bytes == offset_bytes && e.requested == requested) {
      e.use = ++g_ranges_use;
      return e.data;
    }
  }
  return nullptr;
}

/*
 * Stores what the disk just returned. Called after the read, with the lock released for the whole
 * trip to the SD.
 *
 * The copy is done outside the lock: it is up to 12 MB of memcpy plus a heap allocation, and
 * neither has any reason to block another thread that only wants to look at the table.
 */
void SaveRange(uint32_t id, uint64_t offset_bytes, uint32_t requested, const uint8_t* data,
                  size_t bytes) {
  if (!bytes) {
    return;
  }
  {
    std::lock_guard lock(g_ranges_mutex);
    if (g_ranges_no_memory) {
      return;
    }
    // The load sweep does not go in. Decided here, before copying the megabytes.
    if (IsSweepAndAnnotate(id, offset_bytes, requested)) {
      g_range_sequential.fetch_add(1, std::memory_order_relaxed);
      g_range_sequential_mb.fetch_add(bytes, std::memory_order_relaxed);
      return;
    }
  }

  std::shared_ptr<std::vector<uint8_t>> copy;
  try {
    copy = std::make_shared<std::vector<uint8_t>>(data, data + bytes);
  } catch (const std::bad_alloc&) {
    // If the console does not have that RAM, the cache turns off and is not retried. The game keeps
    // reading from disk as usual: it can never bring the process down for lack of memory.
    uint64_t had_mb = 0;
    {
      std::lock_guard lock(g_ranges_mutex);
      had_mb = g_ranges_bytes >> 20;
      g_ranges_no_memory = true;
      g_ranges.clear();
      g_ranges_bytes = 0;
    }
    g_range_entries.store(0, std::memory_order_relaxed);
    g_range_bytes_live.store(0, std::memory_order_relaxed);
    REXLOG_WARN("[io] range-cache: out of memory, turning off (it was at {} MB). The game keeps reading "
                "from disk as before",
                had_mb);
    return;
  }

  const size_t cap = size_t(REXCVAR_GET(switch_io_ranges_mb)) * 1024u * 1024u;
  std::lock_guard lock(g_ranges_mutex);
  for (const auto& e : g_ranges) {
    if (e.id == id && e.offset_bytes == offset_bytes && e.requested == requested) {
      return;  // another thread asked for the same range and got there first: its copy is just as good
    }
  }
  RangeEntry& fresh_one = g_ranges.emplace_back();
  fresh_one.id = id;
  fresh_one.offset_bytes = offset_bytes;
  fresh_one.requested = requested;
  fresh_one.data = std::move(copy);
  fresh_one.use = ++g_ranges_use;
  g_ranges_bytes += bytes;

  // LRU: evict the oldest until it fits. The entry just added has the highest use count, so it is
  // never the one evicted.
  while (g_ranges_bytes > cap && g_ranges.size() > 1) {
    size_t old = 0;
    for (size_t i = 1; i < g_ranges.size(); ++i) {
      if (g_ranges[i].use < g_ranges[old].use) {
        old = i;
      }
    }
    g_ranges_bytes -= g_ranges[old].data->size();
    g_ranges.erase(g_ranges.begin() + static_cast<ptrdiff_t>(old));
    g_range_evictions.fetch_add(1, std::memory_order_relaxed);
  }
  g_range_entries.store(g_ranges.size(), std::memory_order_relaxed);
  g_range_bytes_live.store(g_ranges_bytes, std::memory_order_relaxed);
}
}  // namespace

RangeStats ReadRangeStats() {
  RangeStats e;
  e.hits = g_ranges_hits.load(std::memory_order_relaxed);
  e.misses = g_ranges_misses.load(std::memory_order_relaxed);
  e.bytes_ram = g_ranges_bytes_ram.load(std::memory_order_relaxed);
  e.bytes_disk = g_ranges_bytes_disk.load(std::memory_order_relaxed);
  e.entries = g_range_entries.load(std::memory_order_relaxed);
  e.live_bytes = g_range_bytes_live.load(std::memory_order_relaxed);
  e.evictions = g_range_evictions.load(std::memory_order_relaxed);
  e.low_floor = g_ranges_low_floor.load(std::memory_order_relaxed);
  e.over_ceiling = g_ranges_over_ceiling.load(std::memory_order_relaxed);
  e.sequential = g_range_sequential.load(std::memory_order_relaxed);
  e.sequential_bytes = g_range_sequential_mb.load(std::memory_order_relaxed);
  e.floor_kb = uint64_t(FloorBytes() / 1024);
  const int32_t cap = REXCVAR_GET(switch_io_ranges_mb);
  e.cap_mb = cap > 0 ? uint64_t(cap) : 0;
  {
    std::lock_guard lock(g_ranges_mutex);
    e.no_memory = g_ranges_no_memory;
  }
  return e;
}

namespace {
struct FileReads {
  uint64_t reads = 0;
  uint64_t bytes = 0;
};
std::mutex g_files_mutex;
std::unordered_map<std::string, FileReads> g_files;

void AnnotateFileRead(const Entry* entry, size_t bytes) {
  if (!entry) {
    return;
  }
  std::lock_guard lock(g_files_mutex);
  FileReads& f = g_files[entry->name()];
  ++f.reads;
  f.bytes += bytes;
}

// switch_io_ipak_mb_s: each read of an .ipak file waits until the bytes before it have had their time at the set
// rate, then takes its own share.
std::mutex g_throttle_mutex;
std::chrono::steady_clock::time_point g_throttle_next{};

bool IsImagePack(const std::string& name) {
  static constexpr char kSuffix[] = ".ipak";
  constexpr size_t n = sizeof(kSuffix) - 1;
  if (name.size() < n) {
    return false;
  }
  for (size_t i = 0; i < n; ++i) {
    char c = name[name.size() - n + i];
    if (c >= 'A' && c <= 'Z') {
      c = char(c - 'A' + 'a');
    }
    if (c != kSuffix[i]) {
      return false;
    }
  }
  return true;
}

void ThrottleRead(const Entry* entry, size_t bytes) {
  const int32_t mb_s = REXCVAR_GET(switch_io_ipak_mb_s);
  if (mb_s <= 0 || !entry || bytes == 0 || !IsImagePack(entry->name())) {
    return;
  }
  std::chrono::steady_clock::time_point at;
  {
    std::lock_guard lock(g_throttle_mutex);
    const auto now = std::chrono::steady_clock::now();
    if (g_throttle_next < now) {
      g_throttle_next = now;
    }
    at = g_throttle_next;
    g_throttle_next += std::chrono::nanoseconds(uint64_t(double(bytes) * 1e9 / (double(mb_s) * 1048576.0)));
  }
  std::this_thread::sleep_until(at);
}
}  // namespace

std::string TakeFilesReport(size_t top) {
  if (!REXCVAR_GET(switch_io_report_files)) {
    return {};
  }
  std::vector<std::pair<std::string, FileReads>> files;
  {
    std::lock_guard lock(g_files_mutex);
    files.assign(g_files.begin(), g_files.end());
    g_files.clear();
  }
  std::sort(files.begin(), files.end(), [](const auto& a, const auto& b) { return a.second.bytes > b.second.bytes; });
  std::string out;
  for (size_t i = 0; i < files.size() && i < top; ++i) {
    out += fmt::format("{}{}: {} reads, {:.1f} MB", i ? "; " : "", files[i].first, files[i].second.reads,
                       double(files[i].second.bytes) / (1024.0 * 1024.0));
  }
  return out;
}

WindowStats ReadWindowStats() {
  WindowStats e;
  e.hits = g_hits.load(std::memory_order_relaxed);
  e.fills = g_fills.load(std::memory_order_relaxed);
  e.direct = g_direct.load(std::memory_order_relaxed);
  e.bytes_ram = g_bytes_ram.load(std::memory_order_relaxed);
  int64_t live = g_windows_live.load(std::memory_order_relaxed);
  e.windows_live = live > 0 ? static_cast<uint64_t>(live) : 0;
  return e;
}

HostPathFile::HostPathFile(uint32_t file_access, HostPathEntry* entry,
                           std::unique_ptr<rex::filesystem::FileHandle> file_handle)
    : File(file_access, entry), file_handle_(std::move(file_handle)) {
  // The window is decided once, at open time. Conditions: there is a handle (it is not a directory),
  // the device is read-only, the file was not opened for writing and the cvar allows it.
  const int32_t kb = REXCVAR_GET(switch_io_window_kb);
  const bool wants_write =
      (file_access & (FileAccess::kGenericWrite | FileAccess::kFileWriteData |
                      FileAccess::kFileAppendData | FileAccess::kGenericAll)) != 0;
  /*
   * The RAM cache, with the same conditions as the window except that it has its own cvar. The id
   * is requested once, at open time, so the path map is not touched on every read.
   *
   * The same id serves both caches, the block one (off) and the range one. That the device is
   * read-only and the file was not opened for writing is checked here and only here: that is why
   * neither cache can ever touch a save or a profile, where the bytes change underneath us and
   * serving a stale copy would mean a corrupt save.
   */
  if (file_handle_ && entry && entry->is_read_only() && !wants_write &&
      (REXCVAR_GET(switch_io_cache_mb) > 0 || REXCVAR_GET(switch_io_ranges_mb) > 0)) {
    cache_id_ = PathId(entry->path());
  }
  if (file_handle_ && kb > 0 && entry && entry->is_read_only() && !wants_write) {
    const int64_t max = REXCVAR_GET(switch_io_windows_max);
    if (g_windows_live.fetch_add(1, std::memory_order_relaxed) < max) {
      window_sz_ = static_cast<size_t>(kb) * 1024u;
      window_active_ = true;
      window_counted_ = true;
    } else {
      g_windows_live.fetch_sub(1, std::memory_order_relaxed);
    }
  }
}

HostPathFile::~HostPathFile() {
  if (window_counted_) {
    g_windows_live.fetch_sub(1, std::memory_order_relaxed);
  }
}

void HostPathFile::Destroy() {
  delete this;
}

// fsync on POSIX, FlushFileBuffers on Windows. Called by NtFlushBuffersFile, which the game uses right
// after saving; without it, the save could be left half written on the SD if the game was closed
// right afterwards.
X_STATUS HostPathFile::Flush() {
  if (!file_handle_) {
    return X_STATUS_SUCCESS;
  }
  file_handle_->Flush();
  return X_STATUS_SUCCESS;
}

bool HostPathFile::FillWindow(size_t byte_offset, size_t requested, std::span<uint8_t> buffer,
                                   size_t* out_bytes_read, X_STATUS* out_status) {
  if (window_.size() != window_sz_) {
    window_.resize(window_sz_);
  }

  size_t read_count = 0;
  ++window_fills_;
  g_fills.fetch_add(1, std::memory_order_relaxed);
  if (!file_handle_->Read(byte_offset, window_.data(), window_sz_, &read_count)) {
    // Real error. The window is turned off and the normal path answers.
    window_bytes_ = 0;
    window_active_ = false;
    window_.clear();
    window_.shrink_to_fit();
    return false;
  }

  // What was read is valid whether or not it covers the request: they are real bytes of the file and
  // the file is read-only.
  window_start_ = byte_offset;
  window_bytes_ = read_count;

  if (read_count < requested) {
    /*
     * The window does not cover the request. It can be the end of the file (and then the direct read
     * will return the same) or a short read from the file system, which cannot be told apart here. In
     * both cases the answer that cannot be wrong is the usual one: reply with the direct read. If this
     * happens several times in a row, the file system is truncating large reads, and the window would
     * only add an extra trip per read: it is turned off for this file.
     */
    if (++window_short_ >= 4) {
      window_active_ = false;
      window_bytes_ = 0;
      window_.clear();
      window_.shrink_to_fit();
    }
    return false;
  }
  window_short_ = 0;

  std::memcpy(buffer.data(), window_.data(), requested);
  *out_bytes_read = requested;
  *out_status = X_STATUS_SUCCESS;
  g_bytes_ram.fetch_add(requested, std::memory_order_relaxed);
  return true;
}

/*
 * The direct read, split into pieces. See switch_io_chunk_mb above.
 *
 * It must return exactly the same as a single pread in every case the game can see: success with
 * whatever count results, and X_STATUS_END_OF_FILE only if the first call really fails.
 */
X_STATUS HostPathFile::ReadDirect(std::span<uint8_t> buffer, size_t byte_offset,
                                   size_t* out_bytes_read) {
  const size_t requested = buffer.size();
  const int32_t chunk_mb = REXCVAR_GET(switch_io_chunk_mb);
  const size_t chunk = chunk_mb > 0 ? static_cast<size_t>(chunk_mb) * 1024u * 1024u : 0;

  if (!chunk || requested <= chunk) {
    if (file_handle_->Read(byte_offset, buffer.data(), requested, out_bytes_read)) {
      return X_STATUS_SUCCESS;
    }
    *out_bytes_read = 0;
    return X_STATUS_END_OF_FILE;
  }

  size_t total = 0;
  while (total < requested) {
    const size_t n = std::min(chunk, requested - total);
    size_t read_count = 0;
    if (!file_handle_->Read(byte_offset + total, buffer.data() + total, n, &read_count)) {
      // Real error. If there were already good bytes they are delivered; if not, it is the usual failure.
      if (total == 0) {
        *out_bytes_read = 0;
        return X_STATUS_END_OF_FILE;
      }
      break;
    }
    if (read_count == 0) {
      // End of file. A short but non-empty piece does not stop the loop: it asks again from where it
      // stopped, which is better than a single pread (that returned the short read and nothing more).
      break;
    }
    total += read_count;
  }

  *out_bytes_read = total;
  return X_STATUS_SUCCESS;
}

X_STATUS HostPathFile::ReadSync(std::span<uint8_t> buffer, size_t byte_offset,
                                size_t* out_bytes_read) {
  // No handle means a directory (see HostPathEntry::Open). A directory is enumerated, not read.
  if (!file_handle_) {
    return X_STATUS_FILE_IS_A_DIRECTORY;
  }
  if (!(file_access_ & (FileAccess::kGenericRead | FileAccess::kFileReadData))) {
    return X_STATUS_ACCESS_DENIED;
  }

  const size_t requested = buffer.size();
  t_last_read_kind = "other";
  if (REXCVAR_GET(switch_io_report_files)) {
    AnnotateFileRead(entry(), requested);
  }
  ThrottleRead(entry(), requested);
  if (window_active_ && requested && requested <= window_sz_ / kRequestMaxFraction) {
    // The request falls entirely within what is already in RAM.
    if (window_bytes_ && byte_offset >= window_start_ &&
        byte_offset + requested <= window_start_ + window_bytes_) {
      std::memcpy(buffer.data(), window_.data() + (byte_offset - window_start_), requested);
      *out_bytes_read = requested;
      t_last_read_kind = "window";
      ++window_hits_;
      g_hits.fetch_add(1, std::memory_order_relaxed);
      g_bytes_ram.fetch_add(requested, std::memory_order_relaxed);
      return X_STATUS_SUCCESS;
    }

    X_STATUS state = X_STATUS_SUCCESS;
    const bool served = FillWindow(byte_offset, requested, buffer, out_bytes_read, &state);
    if (served) {
      t_last_read_kind = "window-fill";
    }

    /*
     * Audit. Each fill is a 256 KB read from the SD that only pays off if hits follow. If after
     * 8 fills this file has not given at least one hit per fill, the game is not reading it
     * sequentially and the window is pure cost: it is turned off for this file. This prevents a
     * repeat of the measured case where 4.1 MB of SD reads served 0.3 MB without anyone noticing.
     */
    if (window_active_ && window_fills_ >= 8 && window_hits_ < window_fills_) {
      window_active_ = false;
      window_bytes_ = 0;
      window_.clear();
      window_.shrink_to_fit();
    }

    if (served) {
      return state;
    }
    // If it could not, it falls through to the usual path below.
  }

  /*
   * The exact-range cache, before going to disk. See switch_io_ranges_mb.
   *
   * It is the only thing between the game and the SD for large reads: the zone pack that the game
   * releases on leaving and asks for again on entering, byte for byte, at the same place and with
   * the same size. Those rereads are what stalls a frame in the middle of play.
   */
  const bool range_eligible = cache_id_ && RangeEligible(requested);
  if (range_eligible) {
    // The lock only lasts as long as the table lookup; the memcpy happens outside it, and the
    // shared_ptr keeps the data alive even if another thread evicts this entry while we copy.
    if (auto data = FindRange(cache_id_, byte_offset, uint32_t(requested))) {
      const size_t n = std::min(data->size(), requested);
      std::memcpy(buffer.data(), data->data(), n);
      *out_bytes_read = n;
      g_ranges_hits.fetch_add(1, std::memory_order_relaxed);
      t_last_read_kind = "range-hit";
      g_ranges_bytes_ram.fetch_add(n, std::memory_order_relaxed);
      return X_STATUS_SUCCESS;
    }
  }

  // The block cache, before going to disk (switch_io_cache_mb, off by default; see its comment).
  if (cache_id_ && requested) {
    size_t read_count = 0;
    if (ReadWithCache(file_handle_.get(), cache_id_, buffer, byte_offset, &read_count)) {
      *out_bytes_read = read_count;
      t_last_read_kind = "block-cache";
      return X_STATUS_SUCCESS;
    }
  }

  g_direct.fetch_add(1, std::memory_order_relaxed);
  const X_STATUS state = ReadDirect(buffer, byte_offset, out_bytes_read);
  t_last_read_kind = "disk";

  /*
   * And here what just arrived is stored. None of this happens with a shared lock held: the read
   * above has already finished. That exact mistake (g_cache_mutex held inside the pread) is what
   * made the block cache block the other threads without showing in any counter.
   */
  if (range_eligible && state == X_STATUS_SUCCESS && *out_bytes_read) {
    g_ranges_misses.fetch_add(1, std::memory_order_relaxed);
    g_ranges_bytes_disk.fetch_add(*out_bytes_read, std::memory_order_relaxed);
    SaveRange(cache_id_, byte_offset, uint32_t(requested), buffer.data(), *out_bytes_read);
    t_last_read_kind = "disk+range-saved";
  }
  return state;
}

X_STATUS HostPathFile::WriteSync(std::span<const uint8_t> buffer, size_t byte_offset,
                                 size_t* out_bytes_written) {
  if (!file_handle_) {
    return X_STATUS_FILE_IS_A_DIRECTORY;
  }
  if (!(file_access_ &
        (FileAccess::kGenericWrite | FileAccess::kFileWriteData | FileAccess::kFileAppendData))) {
    return X_STATUS_ACCESS_DENIED;
  }

  // Safety belt: a file being written cannot have a window. It should never get here with one (it is
  // only enabled on read-only devices and without write access), but if it did, the window is dropped.
  window_active_ = false;
  window_bytes_ = 0;

  if (file_handle_->Write(byte_offset, buffer.data(), buffer.size(), out_bytes_written)) {
    return X_STATUS_SUCCESS;
  } else {
    return X_STATUS_END_OF_FILE;
  }
}

X_STATUS HostPathFile::SetLength(size_t length) {
  if (!file_handle_) {
    return X_STATUS_FILE_IS_A_DIRECTORY;
  }
  if (!(file_access_ & (FileAccess::kGenericWrite | FileAccess::kFileWriteData))) {
    return X_STATUS_ACCESS_DENIED;
  }

  window_active_ = false;
  window_bytes_ = 0;

  if (file_handle_->SetLength(length)) {
    return X_STATUS_SUCCESS;
  } else {
    return X_STATUS_END_OF_FILE;
  }
}

}  // namespace rex::filesystem
