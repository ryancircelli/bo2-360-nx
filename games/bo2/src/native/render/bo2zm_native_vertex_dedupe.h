// Native renderer: deduplication of vertex uploads within a frame.
//
// Derived from GPL-3.0 code; modified for bo2-zm-nx in 2026.
//
// The problem, measured
// ---------------------
// Every draw copies to the upload buffer the [vmin..vmax] range of each of its vertex bindings,
// with the byte order swapped. There is no cache: the same piece of geometry is copied again in
// full every time it is drawn.
//
//   PC, with the bo2zm_native_diag_repeated_vertices diagnostic on, steady gameplay:
//       "5954.9 MB copied; 2584.2 MB repeated in the same frame and
//        3329.2 MB equal to an earlier frame"
//       -> 99.0-99.6 % of the bytes are byte-for-byte repeats, in eight reports in a row.
//          Of those, 41-49 % repeat within the same frame.
//
// What this piece does
// --------------------
// Only the safe half: the same-frame one. A table from (physical address, bytes, order) to the
// offset that copy already occupies in this frame's upload buffer. If a later draw asks for
// exactly the same range with the same order, the existing offset is bound and nothing is copied.
//
// Why it is exact and not a gamble
// --------------------------------
// On the Xbox 360 the GPU reads vertices from main memory when it executes the draw, not when
// the game records the packet. A title cannot rewrite a vertex range it has already referenced
// in this frame without first synchronizing with the GPU: if it did, the result on the original
// console would not be defined either. So between two draws of the same frame with no
// synchronization in between, the contents of that range are the same by construction.
//
// The points where the guest may have rewritten memory are the synchronizations. Forget() is
// called there and the table is cleared. With that, deduplication does not rely on any
// assumption.
//
// What it does not do (on purpose)
// --------------------------------
// The other half (the 51-59 % that is equal to an earlier frame) needs a persistent buffer and
// a content check (XXH3 of the guest bytes, as the texture cache does with hash_raw). It is
// more expensive and more delicate than this half, which is free.
//
// How it is wired in (bo2zm_native_draws.cpp)
// ---------------------------------------------
//   - UseSlot(...)      : dedupe_.NewFrame(frame_);   // after ++frame_
//       This already covers SubmitYWait: despite its name it does not wait for anything, it
//       switches work slots, and that goes through UseSlot, which resets upload_used_ and
//       bumps frame_. Old offsets die on their own.
//   - dedupe_.Forget() where the ring thread serves a guest wait for the GPU (a fence or a
//     WAIT_REG_MEM): the only point where the game can legally rewrite a range it already drew.
//
//   Beware of bo2zm_native_submit_after_shadows: when true, the shadow pass ends in one slot and
//   the scene in the next, so the scene cannot reuse the shadow copies and half the savings
//   are lost.
//   - in the bindings loop of Draw, before Reserve/EnqueueCopy:
//         VkDeviceSize offset;
//         if (dedupe_.Find(source.address, source.bytes, uint32_t(source.order), offset)) {
//           offsets_vertices[b] = offset;
//           continue;                       // already in this frame's upload buffer
//         }
//         Reserve(source.bytes, 4, offset);
//         ... (the usual copy) ...
//         dedupe_.Annotate(source.address, source.bytes, uint32_t(source.order), offset);
//
// The table has kSlots entries of 40 bytes (640 KB) and is cleared by generation, without
// memset.

#pragma once

#include <array>
#include <cstdint>

namespace bo2zm::native {

// Deduplication of vertex bindings within a frame. Single thread (the ring thread).
class DedupeVertices {
 public:
  // Number of slots. Power of 2. With ~1,244 draws per frame, each with 1 or 2 vertex bindings,
  // they fit easily in 4096 slots. The collisions() counter tells whether it falls short: if it
  // rises, double it here.
  /*
   * 16384 slots (640 KB). Compared with 4096, collisions per report drop about 3.6x (from 19,705-33,121
   * to 5,423-6,725), but the savings barely move (30 % -> 31 %, 1.83 -> 1.84 MB per frame): collisions
   * were never the limit. The real ceiling of the safe half is ~30-38 % of the bytes, not the 41-49 %
   * the diagnostic measures, because the table is cleared on every slot change (NewFrame, several times
   * per frame with SubmitYWait) and on every WAIT_REG_MEM. Those clears are exactly what makes the
   * scheme exact rather than a gamble.
   *
   * 16384 is kept because evicting live entries is wasted work; a larger table would not save more.
   */
  static constexpr size_t kSlots = 16384;

  // A frame starts: everything recorded before becomes invalid (the upload buffer has been reset).
  void NewFrame(uint64_t frame) {
    // frame_ can never equal kEmpty, which marks an unused entry.
    frame_ = frame == kEmpty ? 0 : frame;
  }

  // The guest may have synchronized with the GPU and rewritten memory: what was recorded
  // becomes invalid even within the same frame. O(1): it only moves the generation.
  void Forget() {
    ++generation_;
    ++forgets_;
  }

  // true if that range is already copied in this frame's upload buffer. 'offset' then holds
  // its offset and nothing needs to be copied.
  //
  // hash: 0 = no check (the normal case). Non-zero = paranoid mode: the content fingerprint
  // is stored when recording and compared when looking up; on a mismatch it is counted in
  // discrepancies() and false is returned (the data is copied as usual). It proves during real
  // play that deduplication never serves stale data, without risking a single pixel.
  bool Find(uint64_t address, uint32_t bytes, uint32_t order, uint64_t& offset,
              uint64_t hash = 0) {
    ++queries_;
    const uint64_t key = Key(address, order);
    // Two ways, the two adjacent entries of the hashed index (see Annotate).
    const size_t index = Index(key, bytes);
    const Input* found = nullptr;
    for (size_t way = 0; way < 2 && !found; ++way) {
      const Input& e = table_[index ^ way];
      if (e.frame == frame_ && e.generation == generation_ && e.key == key && e.bytes == bytes) {
        found = &e;
      }
    }
    if (!found) {
      return false;
    }
    const Input& e = *found;
    if (hash != 0 && e.hash != hash) {
      ++discrepancies_;
      return false;
    }
    ++hits_;
    bytes_saved_ += bytes;
    offset = e.offset;
    return true;
  }

  // Records a copy just made. 'hash' is only used in the paranoid mode of Find.
  void Annotate(uint64_t address, uint32_t bytes, uint32_t order, uint64_t offset,
              uint64_t hash = 0) {
    const uint64_t key = Key(address, order);
    // Two ways. The way that already holds this range, else one with no live entry, else the second.
    // Entries live long (flush waits do not clear the table), and a direct-mapped table lost ~2-3 % of its live
    // entries to collisions per report.
    const size_t index = Index(key, bytes);
    const auto live = [&](const Input& c) { return c.frame == frame_ && c.generation == generation_; };
    const auto same = [&](const Input& c) { return c.key == key && c.bytes == bytes; };
    size_t pick = index;
    if (live(table_[index]) && !same(table_[index])) {
      pick = index ^ 1;
    }
    Input& e = table_[pick];
    if (live(e) && !same(e)) {
      ++collisions_;  // both ways hold other live ranges: one loses its slot
    }
    e.key = key;
    e.bytes = bytes;
    e.offset = offset;
    e.hash = hash;
    e.frame = frame_;
    e.generation = generation_;
  }

  // Counters for the C6 report (deltas between reports, like the others).
  uint64_t queries() const { return queries_; }
  uint64_t hits() const { return hits_; }
  uint64_t bytes_saved() const { return bytes_saved_; }
  uint64_t collisions() const { return collisions_; }
  uint64_t forgets() const { return forgets_; }
  uint64_t discrepancies() const { return discrepancies_; }

 private:
  static constexpr uint64_t kEmpty = ~uint64_t(0);

  struct Input {
    uint64_t key = 0;           // (address << 2) | order
    uint64_t offset = 0;          // offset inside the upload buffer
    uint64_t hash = 0;          // paranoid mode only
    uint64_t frame = kEmpty;  // kEmpty = never used
    uint32_t bytes = 0;
    uint32_t generation = 0;      // incremented by every Forget()
  };
  static_assert(sizeof(Input) == 40, "the entry has to fit in 40 bytes");

  static uint64_t Key(uint64_t address, uint32_t order) {
    return (address << 2) | (uint64_t(order) & 0x3);
  }

  // Multiply-and-shift mix (splitmix). Cheap, and it spreads aligned addresses well, which is
  // exactly what arrives here.
  static size_t Index(uint64_t key, uint32_t bytes) {
    uint64_t x = key ^ (uint64_t(bytes) * 0x9E3779B97F4A7C15ull);
    x ^= x >> 30;
    x *= 0xBF58476D1CE4E5B9ull;
    x ^= x >> 27;
    return size_t(x) & (kSlots - 1);
  }

  std::array<Input, kSlots> table_{};
  uint64_t frame_ = 0;
  uint32_t generation_ = 0;

  uint64_t queries_ = 0;
  uint64_t hits_ = 0;
  uint64_t bytes_saved_ = 0;
  uint64_t collisions_ = 0;
  uint64_t forgets_ = 0;
  uint64_t discrepancies_ = 0;
};

// The same for the common 16-bit index ranges (bo2zm_native_dedupe_indices). A draw whose index range
// (physical address, count, byte order) was already scanned and uploaded in this frame's upload buffer reuses the
// scan's minimum and maximum and the uploaded copy: no scan, no copy. The same rules make it exact: entries live
// until the upload buffer is reset (NewFrame) or the guest waits for the GPU (Forget). A mesh drawn in several
// passes of one frame (shadows, scene, reflection) is the usual repeat. Single thread (the ring thread).
class DedupeIndices {
 public:
  static constexpr size_t kSlots = 8192;

  struct Entry {
    uint64_t key = 0;       // (address << 2) | order
    uint64_t offset = 0;    // of the uploaded copy, in the upload buffer of 'frame'
    uint64_t frame = ~uint64_t(0);
    uint64_t hash = 0;      // XXH3 of the guest's index bytes, only while the reuses are being checked (else 0)
    uint32_t count = 0;
    uint32_t generation = 0;
    uint16_t min = 0;
    uint16_t max = 0;
  };

  void NewFrame(uint64_t frame) { frame_ = frame == ~uint64_t(0) ? 0 : frame; }
  void Forget() { ++generation_; }

  // The entry of that range in this frame, or null.
  const Entry* Find(uint32_t address, uint32_t count, uint32_t order) const {
    const Entry& e = table_[Index(Key(address, order), count)];
    if (e.frame != frame_ || e.generation != generation_ || e.key != Key(address, order) || e.count != count) {
      return nullptr;
    }
    return &e;
  }

  void Annotate(uint32_t address, uint32_t count, uint32_t order, uint16_t min, uint16_t max, uint64_t offset,
                uint64_t hash) {
    Entry& e = table_[Index(Key(address, order), count)];
    e.hash = hash;
    e.key = Key(address, order);
    e.count = count;
    e.min = min;
    e.max = max;
    e.offset = offset;
    e.frame = frame_;
    e.generation = generation_;
  }

  uint64_t frame() const { return frame_; }

 private:
  static uint64_t Key(uint32_t address, uint32_t order) { return (uint64_t(address) << 2) | (order & 0x3); }
  static size_t Index(uint64_t key, uint32_t count) {
    uint64_t x = key ^ (uint64_t(count) * 0x9E3779B97F4A7C15ull);
    x ^= x >> 30;
    x *= 0xBF58476D1CE4E5B9ull;
    x ^= x >> 27;
    return size_t(x) & (kSlots - 1);
  }

  std::array<Entry, kSlots> table_{};
  uint64_t frame_ = 0;
  uint32_t generation_ = 0;
};

}  // namespace bo2zm::native
