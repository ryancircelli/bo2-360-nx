// REX_TRACE_WRITE_PAGES (rex/system/write_trace.h): the streaming pool's view of a traced range.
//
// For every traced write the SDK asks what the app knows about the range; for BO2 that is the streaming memory pool
// (bo2zm_stream_alloc.cpp describes its tables): the blocks in use (table A, 20 bytes each: start, end, three more
// words) and the free blocks (table B: start, end) that overlap the range. Two blocks in use that overlap each other,
// or a write into a range the pool calls free, is what an allocator bug or a late write after a free looks like.
// Read without the pool's lock: a diagnostic snapshot, and the writer may be changing the tables at that moment.

#include <cstdint>
#include <map>
#include <string>
#include <utility>

#include <fmt/format.h>

#include <rex/system/kernel_state.h>
#include <rex/system/write_trace.h>
#include <rex/system/xmemory.h>

namespace {

constexpr uint32_t kPool = 0x84BA9F00;           // table A
constexpr uint32_t kCountA = kPool + 0x25900;    // 0x84BCF800
constexpr uint32_t kCountB = kPool + 0x25904;    // 0x84BCF804
constexpr uint32_t kTableB = kPool + 0x19F00;    // 0x84BC3E00
constexpr uint32_t kEntryA = 20;
constexpr uint32_t kEntryB = 8;
constexpr uint32_t kMaxA = (kTableB - kPool) / kEntryA;
constexpr uint32_t kMaxB = (kCountA - kTableB) / kEntryB;

uint32_t Load32(const uint8_t* host) {
  uint32_t v;
  __builtin_memcpy(&v, host, 4);
  return __builtin_bswap32(v);
}

void DescribePool(uint32_t physical, uint32_t length, std::string& out) {
  auto* ks = rex::system::kernel_state();
  if (!ks || !ks->memory()) {
    return;
  }
  auto* memory = ks->memory();
  const uint8_t* a = memory->TranslateVirtual(kPool);
  const uint32_t count_a = Load32(memory->TranslateVirtual(kCountA));
  const uint32_t count_b = Load32(memory->TranslateVirtual(kCountB));
  const uint64_t end = uint64_t(physical) + length;
  out += fmt::format("pool A={} B={}", count_a, count_b);
  if (count_a <= kMaxA) {
    for (uint32_t i = 0; i < count_a; ++i) {
      const uint8_t* e = a + i * kEntryA;
      const uint32_t s = Load32(e), t = Load32(e + 4);
      const uint32_t ps = rex::write_trace::ToPhysical(s), pt = rex::write_trace::ToPhysical(t);
      if (ps < end && pt > physical) {
        out += fmt::format(" used[{}]={:08X}-{:08X}/{:08X},{:08X},{:08X}", i, s, t, Load32(e + 8), Load32(e + 12),
                           Load32(e + 16));
      }
    }
  }
  if (count_b <= kMaxB) {
    const uint8_t* b = memory->TranslateVirtual(kTableB);
    for (uint32_t i = 0; i < count_b; ++i) {
      const uint32_t s = Load32(b + i * kEntryB), t = Load32(b + i * kEntryB + 4);
      const uint32_t ps = rex::write_trace::ToPhysical(s), pt = rex::write_trace::ToPhysical(t);
      if (ps < end && pt > physical) {
        out += fmt::format(" free[{}]={:08X}-{:08X}", i, s, t);
      }
    }
  }
}

// A name for a pool block's owner (word 2 of its table A entry): the first pointer in the owner's first 0x40 bytes
// that points at a printable, terminated string (the image name for texture blocks).
std::string OwnerName(rex::memory::Memory* memory, uint32_t owner) {
  if (owner < 0x82000000u || owner >= 0xA0000000u) {
    return {};
  }
  const uint8_t* o = memory->TranslateVirtual(owner);
  for (uint32_t k = 0; k < 0x40; k += 4) {
    const uint32_t p = Load32(o + k);
    if (p < 0x82000000u || p >= 0xA0000000u) {
      continue;
    }
    const char* c = reinterpret_cast<const char*>(memory->TranslateVirtual(p));
    size_t n = 0;
    while (n < 96 && c[n] >= 0x20 && c[n] < 0x7F) {
      ++n;
    }
    if (n >= 4 && c[n] == 0) {
      return fmt::format("+{:X}:{}", k, std::string(c, n));
    }
  }
  return {};
}

// Diffs table A against the last tick: every block that appears (pool+) or disappears (pool-), with its owner.
// Blocks that come and go within one tick (~0.5 ms) are missed.
void TickPool() {
  auto* ks = rex::system::kernel_state();
  if (!ks || !ks->memory()) {
    return;
  }
  auto* memory = ks->memory();
  static std::map<std::pair<uint32_t, uint32_t>, uint32_t> last;  // (start, end) -> owner
  static bool first = true;
  const uint32_t count_a = Load32(memory->TranslateVirtual(kCountA));
  if (count_a > kMaxA) {
    return;
  }
  const uint8_t* a = memory->TranslateVirtual(kPool);
  std::map<std::pair<uint32_t, uint32_t>, uint32_t> now;
  for (uint32_t i = 0; i < count_a; ++i) {
    const uint8_t* e = a + i * kEntryA;
    now[{Load32(e), Load32(e + 4)}] = Load32(e + 8);
  }
  if (!first) {
    for (const auto& [k, owner] : last) {
      auto it = now.find(k);
      if (it == now.end() || it->second != owner) {
        rex::write_trace::Aux(fmt::format("pool- {:08X}-{:08X} owner={:08X} A={}", k.first, k.second, owner, count_a));
      }
    }
    for (const auto& [k, owner] : now) {
      auto it = last.find(k);
      if (it == last.end() || it->second != owner) {
        rex::write_trace::Aux(fmt::format("pool+ {:08X}-{:08X} owner={:08X} {} A={}", k.first, k.second, owner,
                                          OwnerName(memory, owner), count_a));
      }
    }
  }
  first = false;
  last.swap(now);
}

struct Register {
  Register() {
    rex::write_trace::SetDescriber(DescribePool);
    rex::write_trace::SetTicker(TickPool);
  }
} g_register;

}  // namespace
