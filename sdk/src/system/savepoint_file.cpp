/**
 * @file        system/savepoint_file.cpp
 * @brief       Fresh-process save points: the in-process capture written to a file, loaded at launch
 *
 * File (little endian): "REXSP2\0\0", u32 version, u8 uuid[16] (main executable LC_UUID), u32 frame,
 *   u32 heap count, per heap {u32 base, u32 physical, u32 unreserved pages, u32 n, u64 page entries[n]},
 *   u32 region count, per region {u32 space (0 virtual, 1 physical), u32 base, u32 size, u32 protect,
 *   bytes}, u32 GPU state size, GPU state bytes (GraphicsFileHook), u32 'END!'.
 * Physical bytes already carry the pages only the GPU copy held (resolves are not read back to guest
 * memory). Function tables (host pointers) are written but never loaded.
 */

#include <rex/system/savepoint.h>

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <functional>
#include <string>
#include <vector>

#include <fcntl.h>
#include <mach-o/dyld.h>
#include <mach-o/loader.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

#include <rex/cvar.h>
#include <rex/logging.h>
#include <rex/runtime.h>
#include <rex/system/kernel_state.h>
#include <rex/system/xmemory.h>

REXCVAR_DEFINE_STRING(load_savepoint, "", "Test",
                      "Start from this save point file (written by 'savepoint file PATH') instead of booting");

namespace rex::system {

// savepoint.cpp
void VisitInProcessSnapshot(
    const std::function<void(memory::BaseHeap*, const memory::BaseHeap::RewindState&)>& heap,
    const std::function<void(uint32_t space, uint32_t base, uint32_t size, uint32_t protect,
                             const uint8_t* data)>& region);

namespace {
using Clock = std::chrono::steady_clock;
constexpr char kMagic[8] = {'R', 'E', 'X', 'S', 'P', '2', 0, 0};
constexpr uint32_t kVersion = 1;
constexpr uint32_t kEnd = 0x21444E45;  // "END!"

std::atomic<IGraphicsSystem*> g_gfx{nullptr};
std::atomic<GraphicsFileHook> g_gfx_hook{nullptr};

double Ms(Clock::time_point a, Clock::time_point b) {
  return std::chrono::duration<double, std::milli>(b - a).count();
}

bool MainUuid(uint8_t out[16]) {
  auto* h = reinterpret_cast<const mach_header_64*>(_dyld_get_image_header(0));
  if (!h) return false;
  auto* lc = reinterpret_cast<const load_command*>(h + 1);
  for (uint32_t i = 0; i < h->ncmds; ++i) {
    if (lc->cmd == LC_UUID) {
      std::memcpy(out, reinterpret_cast<const uuid_command*>(lc)->uuid, 16);
      return true;
    }
    lc = reinterpret_cast<const load_command*>(reinterpret_cast<const uint8_t*>(lc) + lc->cmdsize);
  }
  return false;
}

struct Out {
  int fd;
  bool ok = true;
  uint64_t bytes = 0;
  void Put(const void* p, size_t n) {
    auto* c = static_cast<const uint8_t*>(p);
    while (ok && n) {
      ssize_t w = ::write(fd, c, n);
      if (w <= 0) {
        ok = false;
        break;
      }
      c += w, n -= size_t(w), bytes += uint64_t(w);
    }
  }
  void U32(uint32_t v) { Put(&v, 4); }
};

struct In {
  const uint8_t* p;
  const uint8_t* end;
  bool ok = true;
  const uint8_t* Take(size_t n) {
    if (!ok || size_t(end - p) < n) {
      ok = false;
      return nullptr;
    }
    auto* r = p;
    p += n;
    return r;
  }
  uint32_t U32() {
    uint32_t v = 0;
    if (auto* r = Take(4)) std::memcpy(&v, r, 4);
    return v;
  }
};

memory::BaseHeap* HeapFor(memory::Memory* mem, uint32_t base, bool physical) {
  return physical ? mem->GetPhysicalHeap() : mem->LookupHeap(base);
}

// Puts each committed run's host access back in line with the (restored) page table.
void SyncHeapAccess(memory::BaseHeap* heap) {
  uint64_t a = heap->heap_base(), end = uint64_t(heap->heap_base()) + heap->heap_size();
  while (a < end) {
    memory::HeapAllocationInfo info{};
    if (!heap->QueryRegionInfo(uint32_t(a), &info)) break;
    uint32_t size = info.region_size ? info.region_size : heap->page_size();
    if (info.state & memory::kMemoryAllocationCommit) heap->Protect(uint32_t(a), size, info.protect);
    a += size;
  }
}
}  // namespace

void SetGraphicsFileHook(IGraphicsSystem* graphics, GraphicsFileHook hook) {
  g_gfx_hook.store(hook, std::memory_order_release);
  g_gfx.store(graphics, std::memory_order_release);
}

std::string WriteSavepointFile(const std::string& path, int frame) {
  auto t0 = Clock::now();
  auto* gfx = g_gfx.load(std::memory_order_acquire);
  auto hook = g_gfx_hook.load(std::memory_order_acquire);
  if (!gfx || !hook) return "error no GPU file hook (Metal only)";
  uint8_t uuid[16];
  if (!MainUuid(uuid)) return "error no LC_UUID";
  SavepointStats st = CaptureInProcessSavepoint();
  if (!st.ok) return "error capture failed: " + st.error;
  auto t1 = Clock::now();
  // Same command-processor safe point as the capture: nothing has been drawn since.
  std::vector<uint8_t> gpu_state, gpu_pages;
  if (!hook(gfx, 0, gpu_state, gpu_pages)) return "error GPU save failed (drain timed out?)";
  struct Page {
    uint32_t address, size;
    const uint8_t* data;
  };
  std::vector<Page> pages;
  for (size_t at = 0; at + 8 <= gpu_pages.size();) {
    Page pg;
    std::memcpy(&pg.address, &gpu_pages[at], 4);
    std::memcpy(&pg.size, &gpu_pages[at + 4], 4);
    pg.data = &gpu_pages[at + 8];
    at += 8 + pg.size;
    pages.push_back(pg);
  }

  std::string tmp = path + ".tmp";
  int fd = ::open(tmp.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
  if (fd < 0) return "error open " + tmp + ": " + std::strerror(errno);
  Out out{fd};
  out.Put(kMagic, 8);
  out.U32(kVersion);
  out.Put(uuid, 16);
  out.U32(uint32_t(frame));
  auto* mem = Runtime::instance()->memory();
  std::vector<std::pair<memory::BaseHeap*, const memory::BaseHeap::RewindState*>> heaps;
  struct Region {
    uint32_t space, base, size, protect;
    const uint8_t* data;
  };
  std::vector<Region> regions;
  VisitInProcessSnapshot(
      [&](memory::BaseHeap* h, const memory::BaseHeap::RewindState& s) { heaps.push_back({h, &s}); },
      [&](uint32_t space, uint32_t base, uint32_t size, uint32_t protect, const uint8_t* data) {
        regions.push_back({space, base, size, protect, data});
      });
  out.U32(uint32_t(heaps.size()));
  for (auto& [h, s] : heaps) {
    out.U32(h->heap_base());
    out.U32(h == mem->GetPhysicalHeap() ? 1 : 0);
    out.U32(s->unreserved_page_count);
    out.U32(uint32_t(s->pages.size()));
    out.Put(s->pages.data(), s->pages.size() * 8);
  }
  out.U32(uint32_t(regions.size()));
  uint64_t overlaid = 0;
  std::vector<uint8_t> patched;
  for (auto& r : regions) {
    out.U32(r.space), out.U32(r.base), out.U32(r.size), out.U32(r.protect);
    const uint8_t* data = r.data;
    if (r.space == 1) {
      for (auto& pg : pages) {
        uint64_t lo = std::max<uint64_t>(pg.address, r.base);
        uint64_t hi = std::min<uint64_t>(uint64_t(pg.address) + pg.size, uint64_t(r.base) + r.size);
        if (lo >= hi) continue;
        if (data != patched.data()) patched.assign(r.data, r.data + r.size), data = patched.data();
        std::memcpy(patched.data() + (lo - r.base), pg.data + (lo - pg.address), hi - lo);
        overlaid += hi - lo;
      }
    }
    out.Put(data, r.size);
  }
  out.U32(uint32_t(gpu_state.size()));
  out.Put(gpu_state.data(), gpu_state.size());
  out.U32(kEnd);
  bool ok = out.ok && ::close(fd) == 0 && ::rename(tmp.c_str(), path.c_str()) == 0;
  if (!ok) {
    ::unlink(tmp.c_str());
    return "error write failed: " + path;
  }
  auto t2 = Clock::now();
  char buf[512];
  std::snprintf(buf, sizeof buf,
                "ok file frame=%d mb=%llu gpu_pages_kb=%llu capture_ms=%.1f write_ms=%.1f threads=%u", frame,
                (unsigned long long)(out.bytes >> 20), (unsigned long long)(overlaid >> 10), Ms(t0, t1),
                Ms(t1, t2), st.threads_parked);
  REXSYS_INFO("[savepoint] wrote {}: {}", path, buf);
  return buf;
}

bool LoadSavepointFile(const std::string& path) {
  auto t0 = Clock::now();
  int fd = ::open(path.c_str(), O_RDONLY);
  struct stat sb{};
  if (fd < 0 || ::fstat(fd, &sb) != 0) {
    REXSYS_ERROR("[savepoint] load: cannot open {}", path);
    if (fd >= 0) ::close(fd);
    return false;
  }
  void* map = ::mmap(nullptr, size_t(sb.st_size), PROT_READ, MAP_PRIVATE, fd, 0);
  ::close(fd);
  if (map == MAP_FAILED) {
    REXSYS_ERROR("[savepoint] load: mmap failed for {}", path);
    return false;
  }
  struct Unmap {
    void* p;
    size_t n;
    ~Unmap() { ::munmap(p, n); }
  } unmap{map, size_t(sb.st_size)};
  In in{static_cast<const uint8_t*>(map), static_cast<const uint8_t*>(map) + sb.st_size};
  auto* magic = in.Take(8);
  if (!magic || std::memcmp(magic, kMagic, 8) != 0 || in.U32() != kVersion) {
    REXSYS_ERROR("[savepoint] load: {} is not a version {} save point file", path, kVersion);
    return false;
  }
  uint8_t uuid[16];
  auto* file_uuid = in.Take(16);
  if (!MainUuid(uuid) || !file_uuid || std::memcmp(uuid, file_uuid, 16) != 0) {
    REXSYS_ERROR("[savepoint] load: {} was saved by a different executable build; refusing", path);
    return false;
  }
  uint32_t frame = in.U32();

  auto* mem = Runtime::instance()->memory();
  // This process's function tables hold its own host pointers; keep them over the restored bytes.
  std::vector<std::pair<uint32_t, std::vector<uint8_t>>> tables;
  for (auto [base, size] : mem->FunctionTableRanges()) {
    auto* p = mem->TranslateVirtual<uint8_t*>(base);
    tables.push_back({base, {p, p + size}});
  }

  uint32_t heap_count = in.U32();
  std::vector<memory::BaseHeap*> view_heaps;
  for (uint32_t i = 0; in.ok && i < heap_count; ++i) {
    uint32_t base = in.U32(), physical = in.U32();
    memory::BaseHeap::RewindState state;
    state.unreserved_page_count = in.U32();
    uint32_t n = in.U32();
    auto* pages = in.Take(size_t(n) * 8);
    auto* heap = HeapFor(mem, base, physical != 0);
    if (!pages || !heap || heap->heap_base() != base ||
        heap->CaptureRewindState().pages.size() != n) {
      REXSYS_ERROR("[savepoint] load: heap {:08X} layout differs from this process", base);
      return false;
    }
    state.pages.resize(n);
    std::memcpy(state.pages.data(), pages, size_t(n) * 8);
    heap->Rewind(state);
    if (!physical && base >= 0xA0000000u) view_heaps.push_back(heap);
  }

  uint32_t region_count = in.U32();
  uint64_t bytes = 0;
  const uint32_t rw = memory::kMemoryProtectRead | memory::kMemoryProtectWrite;
  for (uint32_t i = 0; in.ok && i < region_count; ++i) {
    uint32_t space = in.U32(), base = in.U32(), size = in.U32(), protect = in.U32();
    auto* data = in.Take(size);
    auto* heap = space == 0 ? mem->LookupHeap(base) : mem->GetPhysicalHeap();
    if (!data || !heap || !heap->Protect(base, size, rw)) {
      REXSYS_ERROR("[savepoint] load: region {}:{:08X} +{:X} cannot be restored", space, base, size);
      return false;
    }
    uint8_t* host = (space == 0 ? mem->virtual_membase() : mem->physical_membase()) + base;
    std::memcpy(host, data, size);
    heap->Protect(base, size, protect);
    bytes += size;
  }
  for (auto* heap : view_heaps) SyncHeapAccess(heap);
  for (auto& [base, saved] : tables) {
    std::memcpy(mem->TranslateVirtual<uint8_t*>(base), saved.data(), saved.size());
  }

  std::vector<uint8_t> gpu_state(in.U32()), unused;
  if (auto* s = in.Take(gpu_state.size())) std::memcpy(gpu_state.data(), s, gpu_state.size());
  if (!in.ok || in.U32() != kEnd) {
    REXSYS_ERROR("[savepoint] load: {} is truncated", path);
    return false;
  }
  auto t1 = Clock::now();
  auto* gfx = g_gfx.load(std::memory_order_acquire);
  auto hook = g_gfx_hook.load(std::memory_order_acquire);
  if (!gfx || !hook || !hook(gfx, 1, gpu_state, unused)) {
    REXSYS_ERROR("[savepoint] load: GPU state restore failed");
    return false;
  }
  REXSYS_INFO("[savepoint] restored {}: frame {}, {} MB in {} regions, memory {:.1f} ms, total {:.1f} ms", path,
              frame, bytes >> 20, region_count, Ms(t0, t1), Ms(t0, Clock::now()));
  return true;
}

int LoadSavepointAtLaunch() {
  const std::string path = REXCVAR_GET(load_savepoint);
  if (path.empty()) return 0;
  return LoadSavepointFile(path) ? 1 : -1;
}

}  // namespace rex::system
