// Write trace (REX_TRACE_WRITE_PAGES): see rex/system/write_trace.h.

#include <rex/system/write_trace.h>

#include <dlfcn.h>
#include <pthread.h>

#include <array>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

#include <fmt/format.h>
#include <xxhash.h>

#include <rex/logging.h>
#include <rex/memory/utils.h>
#include <rex/system/thread_state.h>
#include <rex/system/xmemory.h>

namespace rex::write_trace {

std::atomic<bool> g_enabled{false};

namespace {

constexpr size_t kMaxRanges = 128;
constexpr uint32_t kViews[3] = {0xA0000000u, 0xC0000000u, 0xE0000000u};

// One guest view of one range in host memory: [host_start, host_end) holds physical [phys_start, ...).
struct HostView {
  uintptr_t host_start;
  uintptr_t host_end;
  uint32_t phys_start;
};

// A watched 4 KB page.
struct Page {
  uint32_t phys;
  uint64_t hash;
  std::atomic<uint32_t> touched;  // notes and faults since the last scan
  std::string touched_by;         // under g_mutex: seqs of those notes/faults
};

// Ranges are only added (from the environment at start, then by REX_TRACE_WRITE_FOLLOW matches), never removed: a
// slot is filled under g_mutex and published by the release store of g_range_count, so readers need no lock.
struct Range {
  uint32_t start;  // physical, 4 KB aligned
  uint32_t end;    // exclusive
  Page* pages;
  size_t page_count;
  HostView views[3];
  size_t view_count;
};
Range g_ranges[kMaxRanges];
std::atomic<size_t> g_range_count{0};

uintptr_t g_host_page = 16384;
bool g_fault_mode = true;

memory::Memory* g_memory = nullptr;
std::atomic<Describer> g_describer{nullptr};
std::atomic<Ticker> g_ticker{nullptr};
FILE* g_aux = nullptr;
std::mutex g_aux_mutex;

// REX_TRACE_WRITE_FOLLOW: 512-byte blocks of known contents (hash -> blob, offset). A noted write that lands one of
// them in guest RAM adds a watched range around it.
struct FollowBlob {
  std::string name;
  uint32_t length;
};
std::vector<FollowBlob> g_follow_blobs;
std::unordered_map<uint64_t, std::pair<uint32_t, uint32_t>> g_follow_index;
bool g_follow = false;
uint32_t g_follow_span = 0x10000;  // watched on each side of the matched block

std::mutex g_mutex;  // the log file, the dump counter, touched_by
FILE* g_log = nullptr;
std::string g_dir;
uint32_t g_dumps_left = 20000;
std::atomic<uint64_t> g_seq{0};
const auto g_t0 = std::chrono::steady_clock::now();

// The fault ring: filled from the exception handler (no locks, no allocation), drained by the watcher.
struct Fault {
  std::atomic<uint32_t> ready;
  uint64_t host_address;
  uint64_t host_pc;
  uint64_t host_lr;
  uint32_t phys;
  uint32_t guest_lr;
  uint64_t t_ns;
  uint64_t thread;
};
constexpr uint32_t kFaultRing = 1u << 14;
Fault g_faults[kFaultRing];
std::atomic<uint64_t> g_fault_write{0};
uint64_t g_fault_read = 0;
std::atomic<uint64_t> g_fault_dropped{0};

double NowMs() {
  return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - g_t0).count();
}

std::string ThreadName() {
  char name[64] = {};
  pthread_getname_np(pthread_self(), name, sizeof(name));
  uint64_t tid = 0;
  pthread_threadid_np(nullptr, &tid);
  return fmt::format("{}:{}", tid, name[0] ? name : "?");
}

std::string HostSymbol(uint64_t pc) {
  Dl_info info{};
  if (pc && dladdr(reinterpret_cast<void*>(pc), &info) && info.dli_fname) {
    const char* image = std::strrchr(info.dli_fname, '/');
    image = image ? image + 1 : info.dli_fname;
    std::string out = fmt::format("{}+0x{:X}", image, pc - uint64_t(uintptr_t(info.dli_fbase)));
    if (info.dli_sname) {
      out += fmt::format("({}+0x{:X})", info.dli_sname, pc - uint64_t(uintptr_t(info.dli_saddr)));
    }
    return out;
  }
  return fmt::format("0x{:X}", pc);
}

void Line(const std::string& text) {
  // Caller holds g_mutex.
  if (g_log) {
    std::fputs(text.c_str(), g_log);
    std::fputc('\n', g_log);
    std::fflush(g_log);
  } else {
    REXSYS_WARN("[write-trace] {}", text);
  }
}

std::string Describe(uint32_t physical, uint32_t length) {
  std::string out;
  if (Describer d = g_describer.load(std::memory_order_acquire)) {
    d(physical, length, out);
  }
  return out;
}

// Dumps [physical, physical + length) as it is now; returns the file name or "".
std::string Dump(const char* prefix, uint64_t seq, uint32_t physical, uint32_t length) {
  // Caller holds g_mutex.
  if (g_dir.empty() || g_dumps_left == 0 || !g_memory) {
    return {};
  }
  --g_dumps_left;
  std::string name = fmt::format("{}{:07}_{:08X}_{:X}.bin", prefix, seq, physical, length);
  if (FILE* f = std::fopen((g_dir + "/" + name).c_str(), "wb")) {
    std::fwrite(g_memory->TranslatePhysical(physical), 1, length, f);
    std::fclose(f);
    return name;
  }
  return {};
}

void MarkTouched(uint32_t physical, uint32_t length, uint64_t seq) {
  // Caller holds g_mutex.
  const uint64_t end = uint64_t(physical) + length;
  const size_t n = g_range_count.load(std::memory_order_acquire);
  for (size_t ri = 0; ri < n; ++ri) {
    Range& r = g_ranges[ri];
    if (r.start >= end || r.end <= physical) {
      continue;
    }
    for (size_t pi = 0; pi < r.page_count; ++pi) {
      Page& p = r.pages[pi];
      if (p.phys < end && uint64_t(p.phys) + 4096 > physical) {
        p.touched.fetch_add(1, std::memory_order_relaxed);
        if (p.touched_by.size() < 400) {
          p.touched_by += fmt::format("{}{}", p.touched_by.empty() ? "" : ",", seq);
        }
      }
    }
  }
}

// The watched part of [physical, physical + length): first overlapping range's clipped piece.
bool Clip(uint32_t physical, uint32_t length, uint32_t* out_start, uint32_t* out_length) {
  const uint64_t end = uint64_t(physical) + length;
  const size_t n = g_range_count.load(std::memory_order_acquire);
  for (size_t i = 0; i < n; ++i) {
    if (g_ranges[i].start < end && g_ranges[i].end > physical) {
      const uint32_t s = std::max(g_ranges[i].start, physical);
      const uint32_t e = uint32_t(std::min<uint64_t>(g_ranges[i].end, end));
      *out_start = s;
      *out_length = e - s;
      return true;
    }
  }
  return false;
}

void Arm() {
  if (!g_fault_mode) {
    return;
  }
  const size_t n = g_range_count.load(std::memory_order_acquire);
  for (size_t ri = 0; ri < n; ++ri) {
    const Range& r = g_ranges[ri];
    for (size_t i = 0; i < r.view_count; ++i) {
      for (uintptr_t a = r.views[i].host_start & ~(g_host_page - 1); a < r.views[i].host_end; a += g_host_page) {
        size_t len = g_host_page;
        memory::PageAccess access = memory::PageAccess::kNoAccess;
        if (memory::QueryProtect(reinterpret_cast<void*>(a), len, access) &&
            access == memory::PageAccess::kReadWrite) {
          memory::Protect(reinterpret_cast<void*>(a), g_host_page, memory::PageAccess::kReadOnly, nullptr);
        }
      }
    }
  }
}

void DrainFaults() {
  // Caller holds g_mutex.
  const uint64_t w = g_fault_write.load(std::memory_order_acquire);
  while (g_fault_read < w) {
    Fault& f = g_faults[g_fault_read % kFaultRing];
    if (!f.ready.load(std::memory_order_acquire)) {
      break;  // being written
    }
    const uint64_t seq = ++g_seq;
    Line(fmt::format(
        "t={:.3f} seq={} src=write-fault phys={:08X} thr={} host_pc={} host_lr={} guest_lr={:08X}{}",
        std::chrono::duration<double, std::milli>(std::chrono::nanoseconds(f.t_ns)).count(), seq, f.phys,
        f.thread, HostSymbol(f.host_pc), HostSymbol(f.host_lr), f.guest_lr,
        [&] {
          std::string d = Describe(f.phys, 4);
          return d.empty() ? std::string() : " | " + d;
        }()));
    MarkTouched(f.phys, 4, seq);
    f.ready.store(0, std::memory_order_release);
    ++g_fault_read;
  }
}

void Scan() {
  std::lock_guard<std::mutex> lock(g_mutex);
  DrainFaults();
  const size_t n = g_range_count.load(std::memory_order_acquire);
  for (size_t ri = 0; ri < n; ++ri)
  for (size_t pi = 0; pi < g_ranges[ri].page_count; ++pi) {
    Page& p = g_ranges[ri].pages[pi];
    const uint64_t h = XXH3_64bits(g_memory->TranslatePhysical(p.phys), 4096);
    const uint32_t touched = p.touched.exchange(0, std::memory_order_relaxed);
    if (h != p.hash) {
      const uint64_t seq = ++g_seq;
      const std::string dump = Dump("p", seq, p.phys, 4096);
      Line(fmt::format("t={:.3f} seq={} change page={:08X} hash {:016X} -> {:016X} by [{}]{}{}{}", NowMs(), seq,
                       p.phys, p.hash, h, touched ? p.touched_by : "UNATTRIBUTED",
                       dump.empty() ? "" : " dump=", dump,
                       [&] {
                         std::string d = Describe(p.phys, 4096);
                         return d.empty() ? std::string() : " | " + d;
                       }()));
      p.hash = h;
    }
    p.touched_by.clear();
  }
}

void Watcher() {
  pthread_setname_np("rex-write-trace");
  auto next_status = std::chrono::steady_clock::now() + std::chrono::seconds(10);
  while (true) {
    Arm();
    std::this_thread::sleep_for(std::chrono::microseconds(500));
    Scan();
    if (Ticker t = g_ticker.load(std::memory_order_acquire)) {
      t();
    }
    if (std::chrono::steady_clock::now() >= next_status) {
      next_status += std::chrono::seconds(10);
      std::lock_guard<std::mutex> lock(g_mutex);
      Line(fmt::format("t={:.3f} status faults={} dropped={} dumps_left={}", NowMs(),
                       g_fault_write.load(), g_fault_dropped.load(), g_dumps_left));
    }
  }
}

// Adds a watched range (caller holds g_mutex). False when full or already covered.
bool AddRange(uint32_t start, uint32_t end) {
  start &= ~0xFFFu;
  end = (end + 0xFFFu) & ~0xFFFu;
  const size_t n = g_range_count.load(std::memory_order_relaxed);
  if (n >= kMaxRanges || end <= start) {
    return false;
  }
  for (size_t i = 0; i < n; ++i) {
    if (g_ranges[i].start <= start && g_ranges[i].end >= end) {
      return false;
    }
  }
  Range& r = g_ranges[n];
  r.start = start;
  r.end = end;
  r.page_count = (end - start) / 4096;
  r.pages = new Page[r.page_count];
  for (size_t i = 0; i < r.page_count; ++i) {
    r.pages[i].phys = start + uint32_t(i) * 4096;
    r.pages[i].hash = XXH3_64bits(g_memory->TranslatePhysical(r.pages[i].phys), 4096);
    r.pages[i].touched.store(0);
  }
  r.view_count = 0;
  for (uint32_t view : kViews) {
    // The E view starts at physical 0x1000 (ToPhysical).
    if (view == 0xE0000000u && start < 0x1000u) {
      continue;
    }
    const uint32_t guest = view + start - (view == 0xE0000000u ? 0x1000u : 0u);
    const uintptr_t host = reinterpret_cast<uintptr_t>(g_memory->TranslateVirtual(guest));
    r.views[r.view_count++] = {host, host + (end - start), start};
  }
  g_range_count.store(n + 1, std::memory_order_release);
  return true;
}

bool ParseRanges(const char* text) {
  std::string s(text);
  size_t pos = 0;
  while (pos < s.size()) {
    size_t comma = s.find(',', pos);
    std::string item = s.substr(pos, comma == std::string::npos ? std::string::npos : comma - pos);
    pos = comma == std::string::npos ? s.size() : comma + 1;
    if (item.empty()) {
      continue;
    }
    const size_t dash = item.find_first_of("-+");
    if (dash == std::string::npos) {
      return false;
    }
    uint64_t a = std::strtoull(item.substr(0, dash).c_str(), nullptr, 16);
    uint64_t b = std::strtoull(item.substr(dash + 1).c_str(), nullptr, 16);
    if (item[dash] == '+') {
      b += a;
    }
    if (b <= a) {
      return false;
    }
    const uint32_t start = ToPhysical(uint32_t(a));
    const uint32_t end = start + uint32_t(b - a);
    AddRange(start, end);
  }
  return true;
}

// REX_TRACE_WRITE_FOLLOW=FILE[,FILE...]: index each file's non-constant 512-byte blocks.
void LoadFollow(const char* text) {
  std::string s(text);
  size_t pos = 0;
  while (pos < s.size()) {
    size_t comma = s.find(',', pos);
    std::string path = s.substr(pos, comma == std::string::npos ? std::string::npos : comma - pos);
    pos = comma == std::string::npos ? s.size() : comma + 1;
    FILE* f = std::fopen(path.c_str(), "rb");
    if (!f) {
      REXSYS_ERROR("[write-trace] can't open follow file '{}'", path);
      continue;
    }
    std::vector<uint8_t> data;
    uint8_t buf[65536];
    size_t got;
    while ((got = std::fread(buf, 1, sizeof(buf), f)) > 0) {
      data.insert(data.end(), buf, buf + got);
    }
    std::fclose(f);
    const uint32_t blob = uint32_t(g_follow_blobs.size());
    const size_t slash = path.rfind('/');
    g_follow_blobs.push_back({slash == std::string::npos ? path : path.substr(slash + 1), uint32_t(data.size())});
    for (size_t o = 0; o + 512 <= data.size(); o += 512) {
      bool constant = true;
      for (size_t k = 1; k < 512 && constant; ++k) {
        constant = data[o + k] == data[o];
      }
      if (!constant) {
        g_follow_index.emplace(XXH3_64bits(&data[o], 512), std::make_pair(blob, uint32_t(o)));
      }
    }
    g_follow = true;
  }
}

// After a noted write: does [physical, physical + length) now hold a followed block? Then watch around it.
void FollowCheck(const char* source, uint32_t physical, uint32_t length, uint32_t guest_lr, const std::string& detail) {
  const uint32_t first = (physical + 511u) & ~511u;
  const uint64_t end = uint64_t(physical) + length;
  for (uint64_t b = first; b + 512 <= end; b += 512) {
    auto it = g_follow_index.find(XXH3_64bits(g_memory->TranslatePhysical(uint32_t(b)), 512));
    if (it == g_follow_index.end()) {
      continue;
    }
    const uint32_t at = uint32_t(b);
    const uint32_t lo = at > g_follow_span ? at - g_follow_span : 0;
    const uint32_t hi = uint32_t(std::min<uint64_t>(uint64_t(at) + g_follow_span, 0x20000000u));
    std::lock_guard<std::mutex> lock(g_mutex);
    if (AddRange(lo, hi)) {
      Line(fmt::format("t={:.3f} follow {}+0x{:X} found at {:08X} by src={} phys={:08X}+0x{:X} guest_lr={:08X} | "
                       "{} -> watching {:08X}-{:08X} (range {})",
                       NowMs(), g_follow_blobs[it->second.first].name, it->second.second, at, source, physical,
                       length, guest_lr, detail, lo & ~0xFFFu, (hi + 0xFFFu) & ~0xFFFu,
                       g_range_count.load(std::memory_order_relaxed)));
    }
    return;  // one match per write is enough
  }
}

}  // namespace

bool FollowEnabled() { return g_follow; }

bool OverlapsPhysical(uint32_t physical, uint32_t length) {
  const uint64_t end = uint64_t(physical) + length;
  const size_t n = g_range_count.load(std::memory_order_acquire);
  for (size_t i = 0; i < n; ++i) {
    if (g_ranges[i].start < end && g_ranges[i].end > physical) {
      return true;
    }
  }
  return false;
}

void NotePhysicalSlow(const char* source, uint32_t physical, uint32_t length, uint32_t guest_lr,
                      const std::string& detail) {
  if (!g_enabled.load(std::memory_order_relaxed)) {
    return;
  }
  if (g_follow && length >= 512) {
    FollowCheck(source, physical, length, guest_lr, detail);
  }
  if (!OverlapsPhysical(physical, length)) {
    return;
  }
  uint32_t clip_start = 0, clip_length = 0;
  Clip(physical, length, &clip_start, &clip_length);
  const std::string thread = ThreadName();
  const std::string describe = Describe(clip_start, clip_length);
  std::lock_guard<std::mutex> lock(g_mutex);
  DrainFaults();  // faults that happened before this note come first
  const uint64_t seq = ++g_seq;
  const std::string dump = Dump("n", seq, clip_start, clip_length);
  Line(fmt::format("t={:.3f} seq={} src={} phys={:08X}+0x{:X} watched={:08X}+0x{:X} guest_lr={:08X} thr={} | {}{}{}{}",
                   NowMs(), seq, source, physical, length, clip_start, clip_length, guest_lr, thread, detail,
                   dump.empty() ? "" : " dump=", dump, describe.empty() ? "" : " | " + describe));
  MarkTouched(clip_start, clip_length, seq);
}

void OnWriteFault(uintptr_t host_address, uint64_t host_pc, uint64_t host_lr) {
  if (!g_enabled.load(std::memory_order_relaxed)) {
    return;
  }
  const size_t n = g_range_count.load(std::memory_order_acquire);
  for (size_t i = 0; i < n * 3; ++i) {
    const Range& r = g_ranges[i / 3];
    if (i % 3 >= r.view_count) {
      continue;
    }
    const HostView& v = r.views[i % 3];
    if (host_address < v.host_start || host_address >= v.host_end) {
      continue;
    }
    const uint32_t phys = v.phys_start + uint32_t(host_address - v.host_start);
    if (!OverlapsPhysical(phys, 1)) {
      return;  // the same host page, outside the watched range
    }
    const uint64_t slot = g_fault_write.load(std::memory_order_relaxed);
    Fault& f = g_faults[slot % kFaultRing];
    if (f.ready.load(std::memory_order_acquire)) {
      g_fault_dropped.fetch_add(1, std::memory_order_relaxed);
      return;
    }
    // Several threads may fault at once: claim the slot.
    uint64_t expected = slot;
    if (!g_fault_write.compare_exchange_strong(expected, slot + 1, std::memory_order_acq_rel)) {
      g_fault_dropped.fetch_add(1, std::memory_order_relaxed);
      return;
    }
    f.host_address = host_address;
    f.host_pc = host_pc;
    f.host_lr = host_lr;
    f.phys = phys;
    f.guest_lr = 0;
    if (auto* ts = runtime::ThreadState::Get()) {
      if (auto* ctx = ts->context()) {
        f.guest_lr = uint32_t(ctx->lr);
      }
    }
    f.t_ns = uint64_t(
        std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - g_t0).count());
    pthread_threadid_np(nullptr, &f.thread);
    f.ready.store(1, std::memory_order_release);
    return;
  }
}

bool IsArmedHostPage(uintptr_t host_address) {
  if (!g_enabled.load(std::memory_order_relaxed) || !g_fault_mode) {
    return false;
  }
  const size_t n = g_range_count.load(std::memory_order_acquire);
  for (size_t ri = 0; ri < n; ++ri) {
    for (size_t i = 0; i < g_ranges[ri].view_count; ++i) {
      const HostView& v = g_ranges[ri].views[i];
      if (host_address >= (v.host_start & ~(g_host_page - 1)) && host_address < v.host_end) {
        return true;
      }
    }
  }
  return false;
}

void SetDescriber(Describer describer) { g_describer.store(describer, std::memory_order_release); }

void SetTicker(Ticker ticker) { g_ticker.store(ticker, std::memory_order_release); }

void Aux(const std::string& text) {
  std::lock_guard<std::mutex> lock(g_aux_mutex);
  if (g_aux) {
    std::fprintf(g_aux, "t=%.3f %s\n", NowMs(), text.c_str());
    std::fflush(g_aux);
  }
}

double ElapsedMs() { return NowMs(); }

namespace {
std::atomic<uint32_t> g_cpu_stamp[1u << 17];
std::atomic<uint32_t> g_cpu_lr[1u << 17];
}  // namespace

void StampPhysicalSlow(uint32_t physical, uint32_t length, uint32_t guest_lr) {
  if (length == 0) {
    return;
  }
  const uint32_t t = uint32_t(NowMs() * 4.0) + 1;
  const uint32_t first = (physical & 0x1FFFFFFFu) >> 12;
  const uint32_t last = std::min<uint32_t>(((physical & 0x1FFFFFFFu) + length - 1) >> 12, (1u << 17) - 1);
  for (uint32_t i = first; i <= last; ++i) {
    g_cpu_stamp[i].store(t, std::memory_order_relaxed);
    g_cpu_lr[i].store(guest_lr, std::memory_order_relaxed);
  }
}

double LastCpuWriteMs(uint32_t physical, uint32_t length, uint32_t* guest_lr) {
  uint32_t best = 0, lr = 0;
  if (length) {
    const uint32_t first = (physical & 0x1FFFFFFFu) >> 12;
    const uint32_t last = std::min<uint32_t>(((physical & 0x1FFFFFFFu) + length - 1) >> 12, (1u << 17) - 1);
    for (uint32_t i = first; i <= last; ++i) {
      const uint32_t t = g_cpu_stamp[i].load(std::memory_order_relaxed);
      if (t > best) {
        best = t;
        lr = g_cpu_lr[i].load(std::memory_order_relaxed);
      }
    }
  }
  if (guest_lr) {
    *guest_lr = lr;
  }
  return best ? (best - 1) / 4.0 : -1.0;
}

void Initialize(memory::Memory* memory) {
  const char* ranges = std::getenv("REX_TRACE_WRITE_PAGES");
  const char* follow = std::getenv("REX_TRACE_WRITE_FOLLOW");
  if ((!ranges || !*ranges) && (!follow || !*follow)) {
    return;
  }
  g_memory = memory;
  g_host_page = memory::page_size();
  if (const char* nf = std::getenv("REX_TRACE_WRITE_NOFAULT")) {
    g_fault_mode = !(nf[0] == '1');
  }
  if (const char* n = std::getenv("REX_TRACE_WRITE_DUMPS")) {
    g_dumps_left = uint32_t(std::strtoul(n, nullptr, 10));
  }
  if (const char* span = std::getenv("REX_TRACE_WRITE_FOLLOW_SPAN")) {
    g_follow_span = uint32_t(std::strtoul(span, nullptr, 16));
  }
  if (const char* dir = std::getenv("REX_TRACE_WRITE_DIR")) {
    g_dir = dir;
    g_log = std::fopen((g_dir + "/events.log").c_str(), "a");
    g_aux = std::fopen((g_dir + "/aux.log").c_str(), "a");

  }
  {
    std::lock_guard<std::mutex> lock(g_mutex);
    if (ranges && *ranges && !ParseRanges(ranges)) {
      REXSYS_ERROR("[write-trace] bad REX_TRACE_WRITE_PAGES '{}'", ranges);
      return;
    }
    if (follow && *follow) {
      LoadFollow(follow);
    }
    std::string text = fmt::format("t={:.3f} start ranges=", NowMs());
    const size_t n = g_range_count.load();
    size_t pages = 0;
    for (size_t i = 0; i < n; ++i) {
      text += fmt::format("{}{:08X}-{:08X}", i ? "," : "", g_ranges[i].start, g_ranges[i].end);
      pages += g_ranges[i].page_count;
    }
    text += fmt::format(" pages={} fault_mode={} host_page={} follow_blocks={} follow_span=0x{:X}", pages,
                        g_fault_mode, g_host_page, g_follow_index.size(), g_follow_span);
    for (const FollowBlob& b : g_follow_blobs) {
      text += fmt::format(" [{} {} B]", b.name, b.length);
    }
    Line(text);
  }
  g_enabled.store(true, std::memory_order_release);
  REXSYS_WARN("[write-trace] on: pages '{}' follow '{}' (dir '{}')", ranges ? ranges : "", follow ? follow : "",
              g_dir);
  std::thread(Watcher).detach();
}

}  // namespace rex::write_trace
