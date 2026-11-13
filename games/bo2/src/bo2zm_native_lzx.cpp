// A native version of the game's fastfile stream decoder (sub_827E7058, an LZX-style decompressor).
//
// The game streams assets in while the view turns, and on the Switch this decoder cost about 7 % of a core spread over
// the job workers and the Backend, against 1-2 ms per second on the PC. A localized copy of the recompiled function
// (tools/localize.py) was no faster, so it is rewritten here from its PowerPC code.
//
// What the game's function does, from its PowerPC code (r3 = state S, r4 = position, r5 = bytes wanted;
// end = r4 + r5). The state, big-endian guest words:
//   S+0 window (guest address), S+8 window mask, S+12/16/20 the repeated offsets R0 R1 R2;
//   S+24 main tree, 1,024 halfwords looked up by the top 10 bits; S+3644 its longer codes (halfword pairs);
//   S+2072 length tree, 256 halfwords by the top 8 bits; S+9020 its longer codes;
//   S+2584 main code lengths (bytes); S+3256 length code lengths;
//   S+3508 aligned tree, 128 bytes by the top 7 bits; S+3636 aligned code lengths;
//   S+11012 input pointer, S+11016 input end; S+11952 bit buffer (MSB first), S+11956 bit count (a signed byte:
//   the bits past the top 16); S+11960 the position masked, written at the end;
//   S+12004 extra bits per position slot (bytes); S+12056 position base per slot (words).
// Per symbol while position < end (signed 32-bit compare):
//   main symbol from the fast table, then the tree while negative (mask 0x200000 halving: next = tree[2*(-sym) +
//   bit]); if input >= input end, return -1 without storing anything; consume its length.
//   Consume n bits: count -= n (as a signed byte), buffer <<= n; when count <= 0, one little-endian 16-bit word w
//   from the input: buffer |= w << -count, count += 16, input += 2.
//   symbol < 256: a literal byte at window + position.
//   else m = symbol - 256: length header m & 7 (7: the length tree's symbol + 7, mask 0x800000), slot m >> 3.
//     slot > 2: extra = extra_bits[slot]; extra >= 3: (extra - 3) verbatim bits when more than 0, then the aligned
//       tree (7 top bits) gives the low 3: offset = base[slot] + (verbatim << 3) + aligned; 1-2 extra bits: offset =
//       base[slot] + those bits; 0: base[slot]. R2 = R1, R1 = R0, R0 = offset.
//     slot 0-2: offset = R[slot], R[slot] = R0, R0 = offset.
//     length + 2 bytes from window + ((position - offset) & mask) to window + position. The game copies by bytes when
//     the distance is under the length and under 16, else by 16-byte vectors with a final masked store (which
//     rewrites up to 15 bytes past the copy with their own values): both give a forward byte-by-byte copy.
// After the loop: count byte, buffer, input and (mask & position) stored; r3 = position - end (64-bit).
// The last match may run past end: the window is written up to end + 271.

#include "bo2zm_native_lzx.h"

#include <algorithm>
#include <cstring>

#include <rex/hook.h>
#include <rex/memory/utils.h>

REX_EXTERN(__imp__sub_827E7058);

namespace bo2zm::native_lzx {
namespace {

inline uint32_t Load32(const uint8_t* p) {
  uint32_t v;
  std::memcpy(&v, p, 4);
  return __builtin_bswap32(v);
}
inline int16_t Load16s(const uint8_t* p) {
  uint16_t v;
  std::memcpy(&v, p, 2);
  return int16_t(__builtin_bswap16(v));
}
inline void Store32(uint8_t* p, uint32_t v) {
  v = __builtin_bswap32(v);
  std::memcpy(p, &v, 4);
}

// A guest range that maps to one host range (as bo2zm_native_audio.cpp's Contiguous).
inline bool Contiguous(uint64_t guest, uint64_t bytes) {
  const uint64_t end = guest + bytes;
  return end <= 0x1'0000'0000ull && !(guest < 0xE0000000u && end > 0xE0000000ull);
}

}  // namespace

void Decode(PPCContext& ctx, uint8_t* base) {
  const uint32_t s_guest = ctx.r3.u32;
  uint8_t* const s = rex::memory::GuestPtr<uint8_t*>(base, s_guest);
  const uint64_t pos0 = ctx.r4.u64;
  const uint64_t end = ctx.r4.u64 + ctx.r5.u64;  // add r28,r4,r5
  const uint32_t window = Load32(s + 0);
  const uint32_t window_mask = Load32(s + 8);
  const uint32_t in_guest = Load32(s + 11012);
  const uint32_t in_end = Load32(s + 11016);
  // Host pointers for the window (written up to end + 272, read from anywhere under mask + 1 plus a match) and the
  // input (a little past its end): else the game's own code.
  const uint64_t window_span = std::max<uint64_t>(uint64_t(uint32_t(end)), uint64_t(window_mask) + 1) + 272;
  if (int32_t(uint32_t(pos0)) >= int32_t(uint32_t(end)) || !Contiguous(uint64_t(window), window_span) ||
      in_end < in_guest ||
      !Contiguous(uint64_t(in_guest), uint64_t(in_end - in_guest) + 64)) {
    __imp__sub_827E7058(ctx, base);
    return;
  }
  uint8_t* const win = rex::memory::GuestPtr<uint8_t*>(base, window);
  const uint8_t* const in_host0 = rex::memory::GuestPtr<const uint8_t*>(base, in_guest);
  const uint8_t* const main_fast = s + 24;
  const uint8_t* const main_tree = s + 3644;
  const uint8_t* const length_fast = s + 2072;
  const uint8_t* const length_tree = s + 9020;
  const uint8_t* const main_lengths = s + 2584;
  const uint8_t* const length_lengths = s + 3256;
  const uint8_t* const aligned_fast = s + 3508;
  const uint8_t* const aligned_lengths = s + 3636;
  const uint8_t* const extra_bits = s + 12004;
  const uint8_t* const position_base = s + 12056;
  const uint32_t mask = window_mask;
  uint32_t r[3] = {Load32(s + 12), Load32(s + 16), Load32(s + 20)};
  uint32_t buffer = Load32(s + 11952);
  int8_t count = int8_t(s[11956]);
  uint32_t in = in_guest;  // guest, for the compare with the input end
  const uint8_t* in_host = in_host0;
  uint64_t pos = pos0;

  auto consume = [&](uint32_t n) {
    count = int8_t(count - int8_t(n));
    buffer = (n & 63) >= 32 ? 0 : buffer << (n & 63);
    if (count <= 0) {
      const uint32_t word = uint32_t(in_host[0]) | (uint32_t(in_host[1]) << 8);
      const uint32_t shift = uint32_t(-int32_t(count)) & 63;
      in_host += 2;
      in += 2;
      count = int8_t(count + 16);
      buffer |= shift >= 32 ? 0 : word << shift;
    }
  };

  while (int32_t(uint32_t(pos)) < int32_t(uint32_t(end))) {
    // The main symbol.
    int32_t symbol = Load16s(main_fast + (((buffer << 11) | (buffer >> 21)) & 0x7FE));
    if (symbol < 0) {
      uint32_t bit_mask = 0x200000;
      do {
        const int32_t bit = (buffer & bit_mask) != 0 ? 1 : 0;
        symbol = Load16s(main_tree + uint32_t((bit - 2 * symbol) * 2));
        bit_mask >>= 1;
      } while (symbol < 0);
    }
    if (in >= in_end) {
      // Input exhausted: the game stores nothing more, but it stored the repeated offsets at every match.
      Store32(s + 12, r[0]);
      Store32(s + 16, r[1]);
      Store32(s + 20, r[2]);
      ctx.r3.s64 = -1;
      return;
    }
    consume(main_lengths[symbol]);
    const int32_t m = symbol - 256;
    if (m < 0) {
      win[uint32_t(pos)] = uint8_t(m);
      ++pos;
      continue;
    }
    int32_t length = m & 7;
    if (length == 7) {
      int32_t symbol_length = Load16s(length_fast + (((buffer << 9) | (buffer >> 23)) & 0x1FE));
      if (symbol_length < 0) {
        uint32_t bit_mask = 0x800000;
        do {
          const int32_t bit = (buffer & bit_mask) != 0 ? 1 : 0;
          symbol_length = Load16s(length_tree + uint32_t((bit - 2 * symbol_length) * 2));
          bit_mask >>= 1;
        } while (symbol_length < 0);
      }
      consume(length_lengths[symbol_length]);
      length = symbol_length + 7;
    }
    const int32_t slot = int8_t(m >> 3);
    uint32_t offset;
    if (slot > 2) {
      const uint32_t extra = extra_bits[slot];
      if (extra >= 3) {
        uint32_t verbatim = 0;
        if (extra != 3) {
          verbatim = buffer >> ((35 - extra) & 31);
          // count = count - extra + 3, buffer <<= extra - 3, then the refill: as consume(extra - 3).
          consume(extra - 3);
        }
        const int32_t aligned = int8_t(aligned_fast[buffer >> 25]);
        const uint32_t sum = Load32(position_base + 4 * uint32_t(slot)) + (verbatim << 3);
        consume(aligned_lengths[aligned]);
        offset = sum + uint32_t(aligned);
      } else if (extra != 0) {
        const uint32_t verbatim = buffer >> (32 - extra);
        consume(extra);
        offset = Load32(position_base + 4 * uint32_t(slot)) + verbatim;
      } else {
        offset = Load32(position_base + 4 * uint32_t(slot));
      }
      r[2] = r[1];
      r[1] = r[0];
      r[0] = offset;
    } else {
      offset = r[slot];
      r[slot] = r[0];
      r[0] = offset;
    }
    // The copy: length + 2 bytes from window + ((position - offset) & mask), forward byte by byte. 8 bytes at a time
    // where that reads the same bytes (the source ahead of the destination, or 8 or more bytes behind it); most
    // matches are short, so no library call.
    const uint32_t bytes = uint32_t(length + 2);
    const uint32_t from = uint32_t(pos - offset) & mask;
    uint8_t* const to_p = win + uint32_t(pos);
    const uint8_t* const from_p = win + from;
    uint32_t i = 0;
    if (from_p > to_p || to_p - from_p >= 8) {
      for (; i + 8 <= bytes; i += 8) {
        uint64_t v;
        std::memcpy(&v, from_p + i, 8);
        std::memcpy(to_p + i, &v, 8);
      }
    }
    for (; i < bytes; ++i) {
      to_p[i] = from_p[i];
    }
    pos += bytes;
  }
  s[11956] = uint8_t(count);
  Store32(s + 11952, buffer);
  Store32(s + 11012, in);
  Store32(s + 11960, mask & uint32_t(pos));
  Store32(s + 12, r[0]);
  Store32(s + 16, r[1]);
  Store32(s + 20, r[2]);
  ctx.r3.u64 = pos - end;
}

}  // namespace bo2zm::native_lzx
