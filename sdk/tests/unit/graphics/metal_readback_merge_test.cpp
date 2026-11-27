#include "rex/graphics/metal/metal_readback_merge.h"
#include <cstdlib>
#include <iostream>
#include <vector>
using namespace rex::graphics::metal;
#define CHECK(x) do { if (!(x)) { std::cerr << "failed line " << __LINE__ << '\n'; std::abort(); } } while (0)
int main() {
  // dword 0: CPU rewrote it after the export -> CPU wins.
  // dword 1: GPU exported a signal -> copied.
  // dword 2: GPU buffer holds stale pre-upload data it never exported -> kept.
  // dword 3: unchanged everywhere.
  std::vector<uint32_t> snap = {1, 2, 3, 4};
  std::vector<uint32_t> guest = {9, 2, 3, 4};
  std::vector<uint32_t> gpu = {5, 0x7F000012, 0xDEAD, 4};
  std::vector<uint32_t> gpu_snap = {5, 0, 0xDEAD, 4};
  std::vector<size_t> copied;
  uint32_t kept = MergeMemexportReadback(guest.data(), gpu.data(), snap.data(), gpu_snap.data(), 4,
      [&](size_t d, uint32_t old_value, uint32_t v) { CHECK(old_value == 2 && v == 0x7F000012); copied.push_back(d); });
  CHECK(kept == 1);
  CHECK((guest == std::vector<uint32_t>{9, 0x7F000012, 3, 4}));
  CHECK((copied == std::vector<size_t>{1}));
  // Without a GPU snapshot every GPU-changed dword the CPU left alone is copied.
  guest = {1, 2, 3, 4};
  kept = MergeMemexportReadback(guest.data(), gpu.data(), snap.data(), nullptr, 4, [](size_t, uint32_t, uint32_t) {});
  CHECK(kept == 0);
  CHECK((guest == std::vector<uint32_t>{5, 0x7F000012, 0xDEAD, 4}));
  // The CPU wrote nothing since the export (this used to be a whole-range copy):
  // the exported dword lands, the stale GPU word does not.
  guest = snap;
  kept = MergeMemexportReadback(guest.data(), gpu.data(), snap.data(), gpu_snap.data(), 4, [](size_t, uint32_t, uint32_t) {});
  CHECK(kept == 0);
  CHECK((guest == std::vector<uint32_t>{1, 0x7F000012, 3, 4}));
  std::cout << "metal_readback_merge_tests ok\n";
}
