// REX_TRACE_WRITE_PAGES (rex/system/write_trace.h): the streaming pool's view of a traced range.
//
// For every traced write the SDK asks what the app knows about the range; for BO2 that is the streaming memory pool
// (bo2zm_stream_alloc.cpp describes its tables): the blocks in use (table A, 20 bytes each: start, end, three more
// words) and the free blocks (table B: start, end) that overlap the range. Two blocks in use that overlap each other,
// or a write into a range the pool calls free, is what an allocator bug or a late write after a free looks like.
// Read without the pool's lock: a diagnostic snapshot, and the writer may be changing the tables at that moment.

#include <cstdint>
#include <string>

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

struct Register {
  Register() { rex::write_trace::SetDescriber(DescribePool); }
} g_register;

}  // namespace
