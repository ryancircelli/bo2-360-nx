// Derived from GPL-3.0 code; modified for bo2-zm-nx in 2026.
//
// The native renderer's graphics system and its command ring sink.
//
// What it does
//   Replaces the Xenos emulation plugin when the native renderer is selected (bo2zm_renderer = "native";
//   OnPreSetup in bo2zm_app.h decides):
//     - Presentation: the SDK's VulkanProvider + VulkanPresenter, same as
//       the emulation. ReXApp sees presenter() and sets up F3, settings and
//       the console the usual way (rex_app.cpp:386-398).
//     - The game's D3D keeps running unchanged and fills its command ring. A
//       "sink" consumes it right away: it answers what the game expects from
//       the GPU and hands the draws, copies and Swaps to the rest of the
//       native renderer.
//
// What the game expects from the GPU, and why each thing is needed
//   1. MMIO registers at 0x7FC80000. Without a registered range REX_MM_LOAD_U32
//      reads uninitialized garbage (mmio_handler.cpp:105-124). The same fixed
//      values as the emulation are returned (graphics_system.cpp:232-262).
//   2. The ring read pointer: the game spins in sub_82597690 watching that
//      word to know how much room it has left.
//   3. Interrupts: vblank (sub_82597960 counts vblanks) and PM4_INTERRUPT
//      packets, which signal that the GPU reached that point of the ring.
//   4. GPU writes to memory (MEM_WRITE, COND_WRITE, REG_TO_MEM and
//      EVENT_WRITE_*), with the semantics of graphics/command_processor.cpp.
//      Occlusion queries return 1000 samples, like the emulation.
//
// Where the rest is
//   bo2zm_native_targets.cpp does the copies (resolve and clear) and presents
//   the resolved texture of each Swap, bo2zm_native_shaders.cpp identifies the
//   shaders that reach the ring, and bo2zm_native_draws.cpp draws the
//   geometry. See docs/native-renderer.md.

#include "bo2zm_native_render.h"
#include "../bo2zm_native_system.h"

#include "bo2zm_hitch_waits.h"
#include "bo2zm_native_targets.h"
#include "bo2zm_native_hooks.h"
#include "bo2zm_native_shaders.h"
#include "bo2zm_native_progress.h"
#include "bo2zm_native_precompile.h"

#include <rex/cvar.h>
#include <rex/filesystem.h>
#include <rex/graphics/pipeline/shader/interpreter.h>  // memexport on the CPU
#include <rex/graphics/register_file.h>
#include <rex/graphics/registers.h>
#include <rex/graphics/xenos.h>
#include <rex/kernel/xboxkrnl/video.h>
#include <rex/logging.h>
#include <rex/memory/utils.h>
#include <rex/system/function_dispatcher.h>
#include <rex/system/kernel_state.h>
#include <rex/system/xmemory.h>
#include <rex/system/xobject.h>
#include <rex/system/xthread.h>
#include <rex/system/xtypes.h>
#include <rex/system/xvideo.h>
#include <rex/thread.h>
#include <rex/ui/presenter.h>
#include <rex/ui/vulkan/device.h>
#include <rex/ui/vulkan/presenter.h>
#include <rex/ui/vulkan/provider.h>
#include <rex/ui/windowed_app_context.h>

#if defined(__aarch64__)
#include <arm_neon.h>
#endif

#include <algorithm>
#include <array>
#include <atomic>
#include <bit>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstring>
#include <deque>
#include <fstream>
#include <functional>
#include <mutex>
#include <set>
#include <string>
#include <string_view>
#include <thread>
#include <tuple>
#include <unordered_map>
#include <unordered_set>
#include <vector>

/*
 * xxHash reads with memcpy (XXH_FORCE_MEMORY_ACCESS 0). With the method it picks for GCC (1) it reads through
 * 64- and 32-bit pointers without may_alias, and GCC may move that read ahead of the store of the data being
 * hashed (strict aliasing). With that, the texture key read keys[4] before writing it and the same texture was
 * created several times (see docs/toolchain.md). Same fingerprint values; on AArch64, the same LDR.
 */
#if defined(XXH_IMPLEM_13a8737387)
#error "xxhash.h was already included with its implementation before this point: XXH_FORCE_MEMORY_ACCESS 0 would come too late"
#endif
#undef XXH_FORCE_MEMORY_ACCESS
#define XXH_FORCE_MEMORY_ACCESS 0
#define XXH_INLINE_ALL
#include <xxhash.h>

// The shader library next to the executable. A build for another program of the game, which shares the folder, names
// its own.
#ifndef BO2ZM_SHADER_LIBRARY_FILE
#define BO2ZM_SHADER_LIBRARY_FILE "bo2zm_shaders.spvpack"
#endif

// The summary of the scenery LOD hook (a stub for BO2, bo2zm_render_stubs.cpp).
// Declared here instead of creating a header for a single function.
namespace bo2zm::scenery_lod {
std::string Summary();
}  // namespace bo2zm::scenery_lod

#if REX_PLATFORM_SWITCH
// Only for RexSwitchSetCurrentThreadPriority. That header deliberately does not include switch.h.
#include "core/threading_switch.h"  // the SDK's src/ is on the include path (CMakeLists.txt)
#endif

// Diagnostics: "ADDRESS:BYTES@SECONDS" (hex, hex, decimal) writes that much guest physical memory to
// dump_mem_<address>_<seconds>s.bin next to the executable at the first Swap after that many seconds. Useful for
// checking whether texture data (the mip chains, for instance) changes after the renderer first read it.
REXCVAR_DEFINE_STRING(bo2zm_native_dump_memory, "", "BO2",
                      "Diagnostics: ADDRESS:BYTES@SECONDS (hex:hex@s) dumps guest memory once at that time")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);

extern std::atomic<uint64_t> g_bo2zm_dump_texture;  // bo2zm_native_draws.cpp

// Diagnostics: the color-grading strip's inputs (NativeRenderSystem::DiagDropsLutBo2zm).
#if BO2ZM_NATIVE_METAL
REXCVAR_DEFINE_BOOL(bo2zm_native_metal_present, false, "BO2",
                    "Native renderer, Metal backend (work in progress): present with Metal instead of the SDK's "
                    "Vulkan presenter. The game image is not shown yet (a plain colour is), and the debug UI is "
                    "not drawn")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
REXCVAR_DEFINE_BOOL(bo2zm_native_metal_mirror, false, "BO2",
                    "Native renderer, Metal backend (work in progress): repeat the Vulkan renderer's work on Metal. "
                    "With bo2zm_native_metal_present the Metal copy of the frame is shown")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
#endif

REXCVAR_DEFINE_BOOL(bo2zm_native_diag_lut, false, "BO2",
                    "Diagnostics: log the colour-grading strip draw's constants and base table every 5 s")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);

// Vertex shaders that write memory (memexport). See NativeRenderSystem::ExecuteMemexportBo2zm.
REXCVAR_DEFINE_BOOL(bo2zm_native_memexport, true, "BO2",
                    "Native renderer: run the vertex shaders that write guest memory (memexport) on the CPU, as the "
                    "GPU does. BO2 copies streamed texture data (mip levels) this way. false = drop them, as before");
REXCVAR_DEFINE_BOOL(bo2zm_native_memexport_fast, true, "BO2",
                    "Native renderer: BO2's two memexport copy shaders (texture streaming) are done directly in C++ "
                    "instead of through the shader interpreter, with the same writes. false = interpreter");
REXCVAR_DEFINE_BOOL(bo2zm_native_memexport_verify, false, "BO2",
                    "Diagnostic: after each direct memexport copy, run the interpreter too and compare the bytes (log "
                    "'direct memexport'). Slow");

/*
 * The game's fences wait for the deferred vertex copies (bo2zm_native_upload_thread).
 *
 * The vertex copy thread guarantees that its copies are done before the work is submitted and before the read
 * pointer goes back to the game. Fences need the same guarantee: MEM_WRITE, EVENT_WRITE_SHD, REG_TO_MEM, COND_WRITE,
 * the scratch register write-back and INTERRUPT all tell the game that the GPU has got that far, and D3D reuses
 * dynamic vertex memory as soon as a fence says the GPU is past it. Without the wait, copies were still pending at
 * about a third of all fences (on a Switch Lite in Buried, up to 66 copies at once). A game waiting on such a fence
 * rewrites the vertices before they are copied, and the draw picks up the next sprite's: glows, smoke and the "?"
 * over the mystery box blinked or grew for one frame. It showed mostly on battery, where the slower GPU keeps the
 * game waiting on fences so that it rewrites right after one; with a fast GPU the game is rarely waiting.
 *
 * With the wait, no blinks were left in a long series of screenshots at the same spot. It is cheap: what remains to
 * copy after the vertex cache is 13-25 MB every 10 s, and in Town the ring ended up copying 16 MB itself in 12 ms
 * every 10 s instead of 8 MB in 8 ms, with no change in frame rate.
 */
REXCVAR_DEFINE_BOOL(bo2zm_native_fences_wait_copies, true, "BO2",
                    "Native renderer: fences, memory writes and interrupts from the ring wait for the "
                    "vertex copy thread first, so the game cannot reuse vertices that are not copied yet. false = as "
                    "before (sprites blinked on battery)")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);

namespace bo2zm::memexport {

// Receives the exports of one vertex shader invocation (ShaderInterpreter) and writes what goes to memory, with
// the rules of Xenia's memexport translation (dxbc_translator_memexport.cpp, ExportToMemory): eA has to carry the
// stream constant's fixed bits; red/blue swap; packing by color format and number format; endian swap; each eM#
// goes to element index + #, if below the stream's index count. The write happens when the next alloc starts and
// at the end of the invocation (Fin), as in the translator.
class Sink final : public rex::graphics::ShaderInterpreter::ExportSink {
 public:
  explicit Sink(uint8_t* physical) : physical_(physical) {}

  void AllocExport(rex::graphics::ucode::AllocType, uint32_t) override { Dump(); }

  void Export(rex::graphics::ucode::ExportRegister reg_index, const float* value, uint32_t mask) override {
    const uint32_t r = uint32_t(reg_index);
    float* target = r == 32 ? ea_ : (r >= 33 && r <= 37) ? em_[r - 33] : nullptr;
    if (!target) {
      return;  // oPos and interpolators: nothing to draw
    }
    for (uint32_t i = 0; i < 4; ++i) {
      if (mask & (1u << i)) {
        target[i] = value[i];
      }
    }
    if (r != 32) {
      written_ |= 1u << (r - 33);
    }
  }

  void End() { Dump(); }

  // The direct copy's path (CopyMemexportBo2zm). Four eM# whose values are 16-bit unsigned integers
  // already (a vfetch of the same format), written without going through float: for a 16_16_16_16 unsigned integer
  // stream, Whole() gives back the same integer, so the bytes are the same. Returns false if eA is not such a
  // stream, and the caller exports the values as floats as usual.
  bool Integers16(const uint32_t a[4], const uint16_t v[4][4]) {
    if ((a[0] >> 30) != 0x1 || (a[1] >> 23) != 0x96 || (a[2] >> 23) != 0x96 || (a[3] >> 23) != 0x96) {
      return false;
    }
    const uint32_t z = a[2];
    if (((z >> 8) & 0x3F) != 26 || ((z >> 16) & 0x1) || !((z >> 17) & 0x1)) {
      return false;
    }
    if (tracing && trace.size() < 1500) {
      trace += fmt::format(" [eA {:08X} {:08X} {:08X} {:08X} eMF i{}]", a[0], a[1], a[2], a[3], a[1] & 0x7FFFFF);
    }
    const bool red_blue = (z >> 19) & 0x1;
    for (uint32_t e = 0; e < 4; ++e) {
      const uint32_t x = v[e][red_blue ? 2 : 0], y = v[e][1], zz = v[e][red_blue ? 0 : 2], w = v[e][3];
      uint32_t d[4] = {x | (y << 16), zz | (w << 16), 0, 0};
      Write(a, e, d, 3);
    }
    return true;
  }

  uint64_t elements = 0;      // elements written
  uint64_t outside = 0;          // elements past the index count (not written, as on the GPU)
  uint64_t rejected = 0;     // exports whose eA was not a stream constant
  uint32_t format_odd = 0;   // last unsupported color format (0 = none)
  uint32_t dir_min = UINT32_MAX, dir_max = 0;  // written range (physical bytes), for the log
  std::string trace;  // the first exports of the draw (diagnostics)
  // Only for the draws whose trace is logged (the first two). Formatting it on every draw cost an allocating
  // fmt::format per export for the first ~25 exports of each draw, on the ring thread (it showed in console stack
  // profiles).
  bool tracing = false;

 private:
  // Float to an integer of `bits` bits, as the translator does it (NaN to 0, clamp or scale, +-0.5, truncate).
  static uint32_t Whole(float v, uint32_t bits, bool with_sign, bool whole) {
    if (v != v) {
      v = 0.0f;
    }
    if (with_sign) {
      const float max = float((1u << (bits - 1)) - 1);
      v = whole ? std::clamp(v, -max - 1.0f, max) : std::clamp(v, -1.0f, 1.0f) * max;
      return uint32_t(int32_t(v + (v < 0.0f ? -0.5f : 0.5f))) & ((bits == 32) ? ~0u : ((1u << bits) - 1));
    }
    const float max = float(bits == 32 ? 4294967295.0 : double((1u << bits) - 1));
    v = whole ? std::clamp(v, 0.0f, max) : std::clamp(v, 0.0f, 1.0f) * max;
    return uint32_t(v + 0.5f);
  }

  // Packs one eM# into d (dwords); returns log2 of the element size in bytes, or -1 for a format not done here.
  static int32_t Pack(uint32_t format, bool with_sign, bool whole, const float* v, uint32_t* d) {
    auto bits = [](float f) {
      uint32_t u;
      std::memcpy(&u, &f, sizeof(u));
      return u;
    };
    switch (format) {
      case 6:  // k_8_8_8_8
        d[0] = Whole(v[0], 8, with_sign, whole) | (Whole(v[1], 8, with_sign, whole) << 8) |
               (Whole(v[2], 8, with_sign, whole) << 16) | (Whole(v[3], 8, with_sign, whole) << 24);
        return 2;
      case 24:  // k_16
        d[0] = Whole(v[0], 16, with_sign, whole);
        return 1;
      case 25:  // k_16_16
        d[0] = Whole(v[0], 16, with_sign, whole) | (Whole(v[1], 16, with_sign, whole) << 16);
        return 2;
      case 26:  // k_16_16_16_16 (BO2's texture copies: unsigned integer)
        d[0] = Whole(v[0], 16, with_sign, whole) | (Whole(v[1], 16, with_sign, whole) << 16);
        d[1] = Whole(v[2], 16, with_sign, whole) | (Whole(v[3], 16, with_sign, whole) << 16);
        return 3;
      case 36:  // k_32_FLOAT
        d[0] = bits(v[0]);
        return 2;
      case 37:  // k_32_32_FLOAT
        d[0] = bits(v[0]);
        d[1] = bits(v[1]);
        return 3;
      case 38:  // k_32_32_32_32_FLOAT
        for (uint32_t i = 0; i < 4; ++i) {
          d[i] = bits(v[i]);
        }
        return 4;
      default:
        return -1;
    }
  }

  void Dump() {
    if (!written_) {
      return;
    }
    const uint32_t written = written_;
    written_ = 0;
    uint32_t a[4];
    std::memcpy(a, ea_, sizeof(a));
    if ((a[0] >> 30) != 0x1 || (a[1] >> 23) != 0x96 || (a[2] >> 23) != 0x96 || (a[3] >> 23) != 0x96) {
      ++rejected;
      return;
    }
    const uint32_t z = a[2];
    const uint32_t format = (z >> 8) & 0x3F;
    const bool with_sign = (z >> 16) & 0x1;
    const bool whole = (z >> 17) & 0x1;
    const bool red_blue = (z >> 19) & 0x1;
    const uint32_t index = a[1] & 0x7FFFFF;
    if (tracing && trace.size() < 1500) {
      trace += fmt::format(" [eA {:08X} {:08X} {:08X} {:08X} eM{:X} i{}]", a[0], a[1], a[2], a[3], written, index);
    }
    for (uint32_t e = 0; e < 5; ++e) {
      if (!(written & (1u << e))) {
        continue;
      }
      float v[4] = {em_[e][0], em_[e][1], em_[e][2], em_[e][3]};
      if (red_blue) {
        std::swap(v[0], v[2]);
      }
      uint32_t d[4] = {};
      const int32_t log2_sz = Pack(format, with_sign, whole, v, d);
      if (log2_sz < 0) {
        format_odd = format;
        continue;
      }
      Write(a, e, d, uint32_t(log2_sz));
    }
  }

  // Element e of the stream in eA: endian swap, the count limit, and the write.
  void Write(const uint32_t a[4], uint32_t e, uint32_t (&d)[4], uint32_t log2_sz) {
    const uint32_t endian = a[2] & 0x7;
    const uint32_t index = a[1] & 0x7FFFFF;
    const uint32_t count = a[3] & 0x7FFFFF;
    const uint32_t base = a[0] << 2;  // physical bytes: bits 0:29 hold the address in dwords
    // 8-in-64 and 8-in-128 reorder the dwords and then swap as 8-in-32.
    uint32_t endian_e = endian;
    if (endian_e == 4) {  // k8in64
      std::swap(d[0], d[1]);
      std::swap(d[2], d[3]);
      endian_e = 2;
    } else if (endian_e == 5) {  // k8in128
      std::swap(d[0], d[3]);
      std::swap(d[1], d[2]);
      endian_e = 2;
    }
    for (uint32_t& w : d) {
      if (endian_e == 1 || endian_e == 2) {  // k8in16, first half of k8in32
        w = ((w & 0x00FF00FFu) << 8) | ((w >> 8) & 0x00FF00FFu);
      }
      if (endian_e == 2 || endian_e == 3) {  // k16in32, second half of k8in32
        w = (w << 16) | (w >> 16);
      }
    }
    if (index + e >= count) {
      ++outside;
      return;
    }
    const uint32_t sz = 1u << log2_sz;
    const uint32_t address = (base + ((index + e) << log2_sz)) & 0x1FFFFFFF;
    if (address + sz > 0x20000000u) {
      ++outside;
      return;
    }
    // The GPU stores the dwords little-endian into memory (smaller elements: their low bytes).
    std::memcpy(physical_ + address, d, sz);
    ++elements;
    dir_min = std::min(dir_min, address);
    dir_max = std::max(dir_max, address + sz);
  }

  uint8_t* physical_;
  float ea_[4] = {};
  float em_[5][4] = {};
  uint32_t written_ = 0;  // eM# written since the last alloc
};

}  // namespace bo2zm::memexport

REXCVAR_DEFINE_STRING(bo2zm_c1_renderer, "xenos", "BO2",
                      "xenos = GPU emulation; native = native renderer in development (piece C1: the game runs without "
                      "emulation and the screen shows a test color)")
    .allowed({"xenos", "native"})
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);

REXCVAR_DEFINE_BOOL(bo2zm_native_vector_register_writes, true, "BO2",
                    "Native renderer: the ring's register blocks are copied 4 at a time with NEON. "
                    "The first 200,000 blocks are checked against the usual path and, if one differs, it turns itself "
                    "off. false = the usual path")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);

REXCVAR_DEFINE_BOOL(bo2zm_native_block_register_writes, true, "BO2",
                    "Native renderer: register blocks without side effects (almost always VS and PS constants) are "
                    "written in one go instead of register by register. false: as before");

/*
 * How often the diagnostic report is written.
 *
 * Writing the report from the ring thread caused real stutters. Over 170 measured gameplay stutters, the periodic
 * dump explained 33 of them and 1,267 of the 5,707 ms lost. The report runs on a timer, not on anything the game
 * does, yet 13 of 30 reports landed inside a stuttering frame where chance would give 1.5 %. Each block was 31-37
 * lines (6-7 KB) and took 14-40 ms to write.
 *
 * A longer interval means fewer dumps, though each costs more and the damage comes in clusters. Measured side by
 * side, 10 s lost twice as much time as 20 s (58 stutters and 428 ms of stutter per minute of play, against 33 and
 * 211 ms), so the default is 20 s, which still leaves plenty of reports to measure with. The real fix is to keep the
 * writing off the ring thread altogether, which the next setting does.
 */
REXCVAR_DEFINE_INT32(bo2zm_native_diag_report_s, 20, "BO2",
                     "Native renderer: seconds between dumps of the diagnostic report. Each dump is written by the "
                     "thread that feeds the GPU and causes stutters: 10 gives twice the detail and twice the stutters "
                     "from this cause")
    .range(5, 120);

/*
 * The report is written on another thread.
 *
 * Writing a report block took 29-32 ms on the ring thread, and the stutters lined up with it: of 6 stutters of
 * 60 ms or more in one play session, 3 ended 40-70 ms after a dump started. The cost is not the formatting. The log
 * is synchronous (log_async = false), and the 2-9 ms gaps between lines fall every 2-4 lines, whenever the file is
 * flushed to the SD card.
 *
 * Lines are still formatted on the ring thread, because they read its state, which is only coherent there, and then
 * queued; the "BO2 reports" thread writes them, sleeping on a condition variable in between. It avoids the two
 * problems that sank the SDK's asynchronous logger (see log_async in sdk/src/core/logging.cpp): the queue never
 * blocks the producer (when it is full the line is dropped and counted), and the thread runs at 0x3B, the only
 * time-sliced priority on Horizon. One point of contention remains: if the ring writes a line of its own while this
 * thread is writing, it waits for that one line (the log lock), not for a whole block. false = the ring writes every
 * line itself.
 */
REXCVAR_DEFINE_BOOL(bo2zm_native_deferred_reports, true, "BO2",
                    "Native renderer: the ring thread's periodic report and [hitch] lines are "
                    "written to the SD from a separate thread; the ring only formats and queues them. false = as "
                    "before, the ring writes them itself (29-32 ms every 20 s in 176)")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);

/*
 * IM_LOAD without memcmp. On every IM_LOAD the ring compared the whole microcode with guest memory (~0.6 us,
 * ~41,000 per second in gameplay). Now the D3D constructors and the fetch patcher report what they write
 * (bo2zm_microcode_versions.h, with the cross-thread protocol) and the ring only compares if someone has
 * written that microcode since the last check. The self-checking guard is in LoadShaderCached.
 */
#include "bo2zm_microcode_versions.h"
#include "../../bo2zm_preset.h"  // outside a match: the menu blur reuse
#include "../../bo2zm_benchmark.h"
#include "../../bo2zm_memcpy_callers.h"
#include "../../bo2zm_page_writes.h"
#include "../../bo2zm_pc_profiler.h"
#include "../../bo2zm_perf_log.h"
#if BO2ZM_NATIVE_METAL
#include "metal/bo2zm_gpu_metal.h"
#include "metal/bo2zm_gpu_metal_mirror.h"
#include "metal/bo2zm_metal_presenter.h"
#endif

REXCVAR_DEFINE_BOOL(bo2zm_native_im_load_without_memcmp, true, "BO2",
                    "Native renderer: an IM_LOAD whose microcode nobody has written since the last "
                    "check (the constructors and the D3D fetch patcher report it) is taken from the cache without "
                    "comparing it whole with memcmp. Starts by checking each load against the memcmp and turns itself "
                    "off at the first disagreement. false = memcmp on every IM_LOAD, as before")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);

/*
 * IM_LOAD_IMMEDIATE with an exact cache. sub_825A37D8 (D3D) copies the VS into the ring itself with the
 * fetches patched and, if needed, with the outputs the PS does not read nulled: op 2B, 4,000-8,000 per second
 * in stretches of gameplay, at 1.5-2.8 us each ("times per packet"). Now a packet whose microcode is
 * byte-identical to one already seen (memcmp against the stored raw copy) reuses its swapped copy, its
 * identification, its fingerprint and its generation. The self-checking guard is in LoadImmediate.
 */
REXCVAR_DEFINE_BOOL(bo2zm_native_im_immediate_cache, true, "BO2",
                    "Native renderer: an IM_LOAD_IMMEDIATE (the VS the D3D copies into the ring with "
                    "the patched fetches) byte-for-byte identical to one already seen reuses its flipped copy, its "
                    "identification, its hash and its generation, without flipping or identifying again. Starts by "
                    "checking each hit against the usual path and turns itself off at the first disagreement. false = "
                    "no cache, as before")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);

// Game frames, one per Swap (bo2zm_d3d_trace.cpp). Only for the AnnotateGameAhead measurement.
extern std::atomic<uint64_t> g_bo2zm_frames_game;

namespace bo2zm::native {
namespace {

// The line queue and its thread. Created with the first line; it lives until exit.
class ReportQueue {
 public:
  ~ReportQueue() {
    {
      std::lock_guard<std::mutex> lock(mutex_);
      stop_ = true;
    }
    warning_.notify_one();
    if (thread_.joinable()) {
      thread_.join();  // writes whatever is left before exiting
    }
  }

  // false if there is no thread (it could not be created, or shutdown is under way): then the caller writes
  // the line.
  bool Enqueue(std::string&& line) {
    bool warn = false;
    {
      std::lock_guard<std::mutex> lock(mutex_);
      if (stop_ || no_thread_) {
        return false;
      }
      if (!thread_.joinable()) {
        try {
          thread_ = std::thread([this] { Loop(); });  // persistent: on Horizon detach() closes the game
        } catch (...) {
          no_thread_ = true;  // no thread (Horizon thread limit): everything as before, nothing breaks
          return false;
        }
      }
      if (queue_.size() >= kMaxLines) {
        ++lost_;  // never block the producer: the line is dropped and counted
        return true;
      }
      warn = queue_.empty();  // the thread only sleeps with the queue empty: one wake-up per burst, not per line
      queue_.push_back(std::move(line));
    }
    if (warn) {
      warning_.notify_one();
    }
    return true;
  }

 private:
  static constexpr size_t kMaxLines = 4096;

  void Loop() {
    rex::thread::set_current_thread_name("BO2 reports");
#if REX_PLATFORM_SWITCH
    const bool priority = RexSwitchSetCurrentThreadPriorityOk(REX_SWITCH_PRIO_GUEST);
#else
    const bool priority = true;
#endif
    REXLOG_INFO("[native] deferred reports: thread running{}",
                priority ? "" : " (the kernel did NOT accept priority 0x3B)");
    std::deque<std::string> batch;
    for (;;) {
      uint64_t lost = 0;
      {
        std::unique_lock<std::mutex> lock(mutex_);
        warning_.wait(lock, [this] { return stop_ || !queue_.empty(); });
        if (queue_.empty()) {
          return;  // stop_ and nothing pending
        }
        batch.swap(queue_);
        lost = lost_;
        lost_ = 0;
      }
      for (const std::string& line : batch) {
        REXLOG_INFO("{}", line);
      }
      batch.clear();
      if (lost) {
        REXLOG_WARN("[native] deferred reports: {} lines lost (the queue of {} was full)", lost,
                    kMaxLines);
      }
    }
  }

  std::mutex mutex_;
  std::condition_variable warning_;
  std::deque<std::string> queue_;  // with mutex_
  std::thread thread_;              // created with mutex_ held
  bool stop_ = false;            // with mutex_
  bool no_thread_ = false;         // with mutex_: the thread could not be created
  uint64_t lost_ = 0;         // with mutex_
};

}  // namespace

void ReportDeferred(std::string line) {
  static const bool defer = REXCVAR_GET(bo2zm_native_deferred_reports);
  if (!defer) {
    REXLOG_INFO("{}", line);
    return;
  }
  static ReportQueue queue;
  if (!queue.Enqueue(std::move(line))) {
    REXLOG_INFO("{}", line);  // no report thread: as before (Enqueue has not moved it)
  }
}

}  // namespace bo2zm::native

/*
 * See LoopRing. Lowered so the presentation thread (0x2C) can preempt it; at the same priority the
 * presenter went without a core 70 % of the time.
 */
REXCVAR_DEFINE_INT32(bo2zm_native_ring_priority, 0x2D, "BO2",
                     "Switch: Horizon priority of the ring thread (0x1C-0x3B). 0x2D by default, one step below the "
                     "presenter (0x2C) and two below audio (0x2B). Raising it to 0x2C brings back an older split, in "
                     "which only half the frames reached the screen");
/*
 * Which core the ring thread prefers.
 *
 * The ring thread uses about 95 % of a core and the game's main thread about 88 %, and by default both run with
 * "preferred core -1", which on Horizon is the process's default core: the same one for both. The kernel then keeps
 * moving them, and every migration throws away the thread's L1 cache. That would not show in the average frame time
 * but in its variance, which is what matters here: with most frames between 30 and 36 ms, straddling the 33.3 ms
 * vblank, a few tenths of a millisecond decide whether a frame is shown at 33 ms or at 50.
 *
 * This only changes the preferred core and leaves the affinity mask alone, so the kernel can still move the thread
 * when it needs to and nothing can starve; that is why it is safe to have on by default. The "C6 ring" report line
 * counts migrations per second and names the core the thread is on. In practice the migrations stay the same with -1
 * and with 1 (see the next setting).
 */
REXCVAR_DEFINE_INT32(bo2zm_native_ring_core, 1, "BO2",
                     "Switch: preferred core of the ring thread (-1 = the default, which is the same as the game "
                     "thread's; 0-2 = a specific one). Not exclusive: the mask is not touched, so the kernel can still "
                     "move it");
/*
 * An exclusive core for the ring thread.
 *
 * The preferred core alone does not keep the thread in place: the kernel accepts it and still runs the thread on
 * cores 0, 1 and 2, with the same 0.24 migrations per loop wherever it is set. Migrations by themselves are cheap,
 * though: about 44 a second at roughly 12 us to refill the L1 is 0.5 ms a second, 0.05 % of the CPU. What could
 * matter is the competition the migration counter does not see: the ring thread and the main thread together want
 * about 183 % of a single core, so they preempt each other. With the mask set to one core, the ring stops competing
 * and the other two cores carry the rest, which barely fits.
 *
 * Off by default: a fixed mask can leave a thread unable to run (a preferred core cannot), and on the console pinning
 * the ring to one core made the worst frames worse (minimum 16.3 FPS against about 21 without it; see
 * docs/platform-notes.md, Threads). The log shows whether migrations drop to zero.
 */
REXCVAR_DEFINE_BOOL(bo2zm_native_ring_core_exclusive, false, "BO2",
                    "Switch: besides preferring bo2zm_native_ring_core's core, FORBIDS the others (exclusive mask). It "
                    "is the real pin. Careful: if that core saturates, the ring does not get to run; check the "
                    "migrations/s in the log, which must drop to 0");
REXCVAR_DEFINE_INT32(bo2zm_native_regmem_wait_us, 1000, "BO2",
                     "Native renderer: pause between WAIT_REG_MEM polls in microseconds (exact on the Switch; on "
                     "Windows, below 1000 it only yields)");
// How much the ring thread's own time limits the frame rate. With the benchmark A/B (bo2zm_benchmark_ab_s), the ring
// spins this many nanoseconds per draw in the arms where factor 1 is on: about what the state decoding would cost per
// draw, to estimate what removing it could gain.
REXCVAR_DEFINE_INT32(bo2zm_native_ring_spin_ns_ab, 0, "BO2",
                     "Test, with bo2zm_benchmark_ab_s: the ring thread spins this many ns per draw in the arms of A/B "
                     "factor 1 (0 = off)");
// The blur behind popups in the menus. A menu that covers another one outside a match blurs what is behind it, and
// the game does it on the GPU every frame: it draws the covered menus into EDRAM, resolves them to a 1280x720 texture,
// then runs a chain of full-screen passes, each a quad with one of three blur pixel shaders that reads one texture and
// is resolved into the other (the two textures take turns), six passes at the game's strength. The popup's backing
// then samples the last one. On a Switch Lite at the stock 307.2 MHz that chain costs about 27 ms a frame and the
// menu drops from 60 to about 25 FPS. Behind a popup the image hardly changes and is blurred anyway, so the chain
// runs on a popup's first frame and once more after the game's 250 ms fade-in (kMenuBlurSecond: the fade raises the
// pass count from four to six), then only on one frame in bo2zm_native_menu_blur_every, or never again while the popup
// stays (0, the Switch's default): on the other frames its copies and passes are not recorded, the result texture
// keeps the last chain's image, and the popup goes on drawing it. A chain is recognised by its pixel shaders (bo2zm_native_menu_blur_ps); the copies skipped are those from
// the EDRAM surface the chain drew into to the textures it used, and only in frames that follow a frame with a chain.
// Never in a match (cl_ingame). Checked on the PC with the image kept for a whole minute: the popup's background
// stays the blurred menu.
#if defined(__SWITCH__)
constexpr int32_t kMenuBlurEveryDefault = 0;
#else
constexpr int32_t kMenuBlurEveryDefault = 1;
#endif
constexpr uint32_t kMenuBlurSecond = 15;  // the popup's frame that runs the chain again, after the fade-in
REXCVAR_DEFINE_INT32(bo2zm_native_menu_blur_every, kMenuBlurEveryDefault, "BO2",
                     "Native renderer: after a popup's first frames, the blur behind it runs on one frame in N and the "
                     "others reuse it. 0 = it is kept until the popup closes (the Switch's default); 1 = every frame, "
                     "as the game does (the PC's)")
    .range(0, 1000);
REXCVAR_DEFINE_STRING(bo2zm_native_menu_blur_ps, "", "BO2",
                      "Native renderer: the menu blur's pixel shaders, as microcode hashes (hex, comma-separated). "
                      "Empty = the three the game uses");
REXCVAR_DEFINE_INT32(bo2zm_native_menu_blur_every_alternate_s, 0, "BO2",
                     "Test helper: bo2zm_native_menu_blur_every and 1 (every frame) take turns every N seconds, "
                     "logged as '[native] menu blur'. 0 = off")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
// The campaign's front end draws its 3D scene twice: a depth prepass of every opaque mesh (colour writes off, depth
// written, the stencil only tested), then the colour pass of the same meshes, which tests GEQUAL and writes depth
// itself. About 1,250 of its 2,700 draws a frame are the prepass. There, the prepass's draws are not recorded: the
// colour pass still ends with the same depth, and the ring thread and the GPU save the draws. Draws that write the
// stencil are kept.
#if defined(__SWITCH__)
constexpr bool kFrontEndNoPrepassDefault = true;
#else
constexpr bool kFrontEndNoPrepassDefault = false;
#endif
REXCVAR_DEFINE_BOOL(bo2zm_native_front_end_no_prepass, kFrontEndNoPrepassDefault, "BO2",
                    "Native renderer: in the campaign's front-end menus the 3D scene's depth prepass is skipped (on "
                    "by default on the Switch)");
// The campaign's front end, reused while its camera holds still. Everything but the menus is drawn into targets
// narrower than the 1280-wide screen: the 880x720 scene, its bloom and post-processing chain, and last the 896x720
// image that the 1280 pass stretches under the menus. When the view has not changed since the last frame drawn in
// full, none of that is recorded (no draw into a target that is not 1280 wide, no copy out of one): the textures keep
// the last frame's images and only the menus are drawn, so a still front end costs about what a 2D menu does. The
// view is read from the frame's first meshes that write depth into such a target: the transforms in their vertex
// constants (c0-c15) are compared with those of the same meshes (same index buffer, index count and vertex shader)
// in the last frame drawn in full, each float relative to its size (at least 1), and that decides the next frame, so
// a camera move shows one frame late. Which mesh comes first may change from frame to frame, so the meshes are
// matched rather than taken in order; a frame with none in common redraws. A change above
// bo2zm_native_front_end_reuse_move (a camera move or a menu's transition) redraws at once; a smaller one above
// bo2zm_native_front_end_reuse_drift (the camera's slow idle sway, under 0.03 on the console) redraws at most once in
// bo2zm_native_front_end_reuse_drift_ms; below it the frame reuses the last one. The drift threshold is the move
// threshold by default, so the sway is ignored and the scene stays as a still backdrop. Constants that change every frame
// for a long while (kSceneNoisy frames in a row: timers) are left out. Moving models do not count (a character's
// idle animation stays as it was). bo2zm_native_front_end_reuse_refresh_ms redraws anyway after that long.
#if defined(__SWITCH__)
constexpr bool kFrontEndReuseDefault = true;
#else
constexpr bool kFrontEndReuseDefault = false;
#endif
constexpr uint32_t kSceneNoisy = 64;
REXCVAR_DEFINE_BOOL(bo2zm_native_front_end_reuse, kFrontEndReuseDefault, "BO2",
                    "Native renderer: in the campaign's front-end menus the 3D scene is drawn again only when its "
                    "view changes, and reused in between (on by default on the Switch)");
REXCVAR_DEFINE_DOUBLE(bo2zm_native_front_end_reuse_move, 0.05, "BO2",
                      "Native renderer: with bo2zm_native_front_end_reuse, a view constant that changes by more than "
                      "this (relative to its size, at least 1) redraws the scene at once")
    .range(0.0, 10.0);
REXCVAR_DEFINE_DOUBLE(bo2zm_native_front_end_reuse_drift, 0.05, "BO2",
                      "Native renderer: with bo2zm_native_front_end_reuse, smaller changes of the view constants than "
                      "this are ignored; changes between this and bo2zm_native_front_end_reuse_move redraw at most "
                      "once in bo2zm_native_front_end_reuse_drift_ms")
    .range(0.0, 10.0);
REXCVAR_DEFINE_INT32(bo2zm_native_front_end_reuse_drift_ms, 3000, "BO2",
                     "Native renderer: with bo2zm_native_front_end_reuse, a slow drift of the view redraws the scene "
                     "at most this often (ms)")
    .range(0, 60000);
REXCVAR_DEFINE_INT32(bo2zm_native_front_end_reuse_settle, 30, "BO2",
                     "Native renderer: with bo2zm_native_front_end_reuse, when the front end opens (or its scene "
                     "changes entirely) the scene is drawn in full for this many still frames before it is reused, so "
                     "the effects that build up over frames on the GPU (exposure, glow) settle first")
    .range(0, 600);
REXCVAR_DEFINE_INT32(bo2zm_native_front_end_reuse_settle_ms, 8000, "BO2",
                     "Native renderer: with bo2zm_native_front_end_reuse, a new scene (the front end opening or "
                     "loading again) is not kept before this long, while its textures stream in and the landing's "
                     "blur and boot-up run")
    .range(0, 60000);
REXCVAR_DEFINE_INT32(bo2zm_native_front_end_reuse_still_ms, 2000, "BO2",
                     "Native renderer: with bo2zm_native_front_end_reuse, the scene is kept only once its view has held "
                     "still for this long after a move")
    .range(0, 60000);
REXCVAR_DEFINE_INT32(bo2zm_native_front_end_reuse_settle_move, 10, "BO2",
                     "Native renderer: with bo2zm_native_front_end_reuse, the still frames drawn in full after a camera "
                     "move (the exposure changes less than when the front end opens)")
    .range(0, 600);
REXCVAR_DEFINE_INT32(bo2zm_native_front_end_reuse_refresh_ms, 0, "BO2",
                     "Native renderer: with bo2zm_native_front_end_reuse, the scene is drawn again after this many "
                     "ms even if its view did not change (0 = only when it changes)")
    .range(0, 60000);
REXCVAR_DEFINE_INT32(bo2zm_native_diag_frame_s, 0, "BO2",
                     "Native renderer: after these seconds, logs every draw and every copy of one whole frame (0 = no; "
                     "tests only)");
REXCVAR_DEFINE_STRING(bo2zm_native_diag_vertices_ps, "", "BO2",
                      "Native renderer: in the traced frame, for draws with these PS (comma-separated numbers) logs the "
                      "VS fetches and the bytes of their first vertices and texels (tests only)");
// The game measures with an occlusion query how much of the sun is visible (sub_82225438: GetData, Issue(BEGIN),
// a draw and Issue(END)) and uses it to turn off the flare when trees or terrain cover it. With the faked count of
// 1000 samples the flare was always drawn in full: sky burned to white and blue or purple halos in the trees.
REXCVAR_DEFINE_INT32(bo2zm_native_occlusion, 1, "BO2",
                     "Native renderer: the game's occlusion queries (the sun flare). 1 = measured on the GPU, like the "
                     "Xbox 360; 0 = a fake count of 1000 samples (what was there before: the flare is never "
                     "hidden); 2 = a fake count of 0 samples (tests only: never a flare)")
    .range(0, 2)
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
REXCVAR_DEFINE_STRING(bo2zm_native_diag_constants_ps, "", "BO2",
                      "Native renderer (tests only): PS numbers (comma-separated) whose first 12 constants are logged, "
                      "at most every bo2zm_native_diag_constants_ms");
REXCVAR_DEFINE_INT32(bo2zm_native_diag_constants_ms, 250, "BO2",
                     "Native renderer (tests only): minimum interval between constant logs per PS")
    .range(0, 60000);
REXCVAR_DEFINE_INT32(bo2zm_native_occlusion_alternate_s, 0, "BO2",
                     "Native renderer (tests only): with bo2zm_native_occlusion = 1 and N > 0, alternates every N "
                     "seconds between measured queries (even spans) and the fake count of 1000 (odd spans), and logs "
                     "each switch, to compare the sun flare in the same session")
    .range(0, 600)
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
REXCVAR_DEFINE_INT32(bo2zm_native_occlusion_scale, 0, "BO2",
                     "Native renderer: samples per pixel used to count the occlusion queries (0 = those of the "
                     "antialiasing mode the game has chosen, as on the Xbox 360; 1-16 = fixed, tests only)")
    .range(0, 16)
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);

// The 30 FPS guard lives in native/bo2zm_render_stubs.cpp (a stub for BO2), which owns the switch.
namespace bo2zm::guard30 {
void Heartbeat(double ms);
void Report();
}  // namespace bo2zm::guard30

namespace bo2zm::render_targets {
uint32_t SamplesOriginalModeCurrent(const uint8_t* base);  // a stub for BO2 (bo2zm_render_stubs.cpp)
}

namespace bo2zm::native {
namespace {

namespace xenos = rex::graphics::xenos;
using rex::X_STATUS;  // X_STATUS_SUCCESS and X_STATUS_UNSUCCESSFUL need it too
using Clock = std::chrono::steady_clock;
using RampOutputContext = rex::ui::vulkan::VulkanPresenter::VulkanGuestOutputRefreshContext;

std::atomic<uint64_t> g_swaps_native{0};  // SwapsNative(), for the watchdog

/*
 * Adding without a lock in the counters only the ring writes.
 *
 * Stack sampling pointed here: 36 % of the ring thread's time is in LoopRing -> Packet -> PacketType3. And
 * every packet did a `fetch_add` on an atomic. On Cortex-A57 (ARMv8.0, without the ARMv8.1 atomic instructions)
 * a fetch_add is an ldxr/stxr loop with an exclusive reservation: ~15-20 cycles, and it also invalidates the line
 * on the other cores. At 83,000 packets per frame that is ~1.1 ms per frame spent counting packets.
 *
 * Why removing it is safe: these counters are written by a single thread, the ring thread (Packet and
 * PacketType3 are only called from LoopRing and from BufferIndirect, which hangs off it), and the only reader
 * is Report, which runs on that same thread (LoopRing calls it on every loop). The shutdown Report(true)
 * runs after thread_ring_->Wait(), that is, with the thread already dead. Without two writers there is no need
 * for an atomic read-modify-write.
 *
 * They stay std::atomic on purpose (the type does not change, nor do the readers); the only thing that changes
 * is how they are incremented: a relaxed load and store, which on 64-bit ARM64 are a plain ldr and str, without
 * exclusive reservation and without barriers. It is still correct for an outside reader (it never sees half a
 * value) and it stops costing.
 *
 * Do not use this for counters another thread touches: writes_wptr_ (written by the game thread through
 * MMIO), vblanks_ and counter_ (the vblank thread). Those keep fetch_add.
 */
template <typename T>
inline void AddOnlyRing(std::atomic<T>& counter, T how_much = 1) {
  counter.store(counter.load(std::memory_order_relaxed) + how_much, std::memory_order_relaxed);
}

constexpr uint32_t kBaseMmio = 0x7FC80000;
/*
 * From 16 to 128, because the packet count grew sixfold.
 *
 * 16 was chosen when there were ~14,000 packets per frame. Stack sampling gives the real count: 20.8 million
 * packets per interval, which at 18.1 packets per draw is ~83,000 per frame. At 1 in 16 that is 5,187 clock
 * reads per frame, and each Clock::now() is a CNTPCT_EL0 read plus the conversion.
 *
 * With 128 there are 648 samples per frame, still plenty for a mean: the relative error of a sample of 648 is
 * 4 %, and these times are read to split percentages, not for exact figures.
 */
constexpr uint64_t kStopwatchPacketsEvery = 128;  // packets per timed one (power of 2)
/*
 * From 8 to 64. At 1 in 8, this timer and above all the stage timer in bo2zm_native_draws.cpp (about 20 clock
 * reads per timed draw) added up to thousands of clock reads per frame in the alley, with 2,500-4,000 draws.
 * With 64 there are ~1,400 measured draws per second, plenty for a mean every 20 s. The "times" line keeps
 * its scale: the measured time is multiplied by this same factor and divided by all the draws.
 */
constexpr uint64_t kStopwatchDrawsEvery = 64;   // draws per timed one (power of 2)
constexpr uint32_t kMaskMmio = 0xFFFF0000;
constexpr uint32_t kSizeMmio = 0x0000FFFF;
constexpr uint32_t kNumRegisters = 0x5003;  // RegisterFile::kRegisterCount

// Registers with a fixed value or with an effect, as in graphics_system.cpp.
constexpr uint32_t kRegCpRbWptr = 0x01C5;
constexpr uint32_t kRegRbEdramTiming = 0x0F00;
constexpr uint32_t kRegRbBcControl = 0x0F01;
constexpr uint32_t kRegD1GrphPrimarySurface = 0x1844;
// Gamma ramp, from DC_LUT_RW_MODE (0x1921) to DC_LUT_WRITE_EN_MASK (0x1927) (register_table.inc).
// The mask is 0x1927 (an earlier version stopped at 0x1926), and the output applies the ramp.
constexpr uint32_t kRegRampFirst = 0x1921;
constexpr uint32_t kRegRampIndex = 0x1922;
constexpr uint32_t kRegRampSequential = 0x1923;
constexpr uint32_t kRegRamp30 = 0x1925;
constexpr uint32_t kRegRampMask = 0x1927;
constexpr uint32_t kRegRampLast = 0x1927;
constexpr uint32_t kRegD1ModeVCounter = 0x194C;
constexpr uint32_t kRegD1ModeVblankVlineStatus = 0x1951;
constexpr uint32_t kRegD1ModeViewportSize = 0x1961;

/*
 * Register generations, so that a draw does not redo work whose inputs did not change.
 *
 * Measured on the console (gameplay): the ring thread is busy 42.5 ms of every 44.2 ms frame, so it is the
 * limit, not the GPU (37.1 ms of real work). Of those 42.5 ms, draw packets (op 22) take 29.5, and inside
 * the translator the most expensive stages are "textures" (4.1 ms) and half of "pipeline", which is
 * really viewport and scissor.
 *
 * Both are pure functions of registers that almost never change between consecutive draws: the fetch
 * constants (0x4800-0x48BF) and the viewport state (viewport, scissor, clip and window offset). This
 * tracks when they really change (same technique as generation_constants_vs_/ps_: compare the value
 * before writing it), and the number travels in DrawRequest so the translator can skip the work with
 * a single uint64 comparison.
 *
 * There are 32 fetch constants of 6 words each: the first 16 are used by textures (kRegFetch in
 * bo2zm_native_draws.cpp) and vertex fetches read theirs from the same block.
 */
constexpr uint32_t kRegFetchFirst = 0x4800;  // SHADER_CONSTANT_FETCH_00_0
constexpr uint32_t kRegFetchLast = 0x48BF;   // SHADER_CONSTANT_FETCH_31_5

// Viewport state registers: PA_SC_WINDOW_OFFSET/SCISSOR_TL/BR, PA_CL_VPORT_[XYZ]SCALE/OFFSET,
// PA_CL_CLIP_CNTL, PA_SU_SC_MODE_CNTL, PA_CL_VTE_CNTL and PA_SU_VTX_CNTL. No VGT_* register is included:
// VGT_DRAW_INITIATOR (0x21FC) changes on every draw and would make the generation useless.
constexpr uint32_t kRegFramingFirst = 0x2080;
constexpr uint32_t kRegFramingLast = 0x2302;
inline bool IsFramingRegister(uint32_t index) {
  // One subtraction and one comparison: rejects everything outside the range.
  if (index - kRegFramingFirst > kRegFramingLast - kRegFramingFirst) {
    return false;
  }
  return index <= 0x2082 ||                      // PA_SC_WINDOW_OFFSET .. SCISSOR_BR
         (index >= 0x210F && index <= 0x2114) ||  // PA_CL_VPORT_XSCALE .. ZOFFSET
         (index >= 0x2204 && index <= 0x2206) ||  // PA_CL_CLIP_CNTL, PA_SU_SC_MODE_CNTL, PA_CL_VTE_CNTL
         index == 0x2302;                          // PA_SU_VTX_CNTL
}

/*
 * Applying register blocks quickly.
 *
 * Applying registers was measured at about 146 ms a second of the ring thread's time (about 4 ms per frame, with the
 * ring busy 95 % of the time), almost all of it in WriteRegistersInBlock: 94.5 million words arrived in blocks against
 * 2.2 million one at a time over 10 s. The regular loop (SpanScalar) byte-swaps each word, compares it and classifies
 * it with several branches. SpanVector does the same 4 words at a time with NEON: it splits the span at the boundaries
 * of each class (VS, PS and fetch constants and the small viewport state pieces), copies each piece byte-swapped and
 * only checks whether anything changed in the piece, which is all that was used from the classification.
 */
inline void SpanScalar(uint32_t index, uint32_t* target, const uint8_t* source, uint32_t n, bool& vs,
                         bool& ps, bool& fetch, bool& framing) {
  for (uint32_t i = 0; i < n; ++i) {
    const uint32_t new_value = rex::memory::load_and_swap<uint32_t>(source + size_t(i) * 4);
    if (target[i] != new_value) {
      const uint32_t abs = index + i;
      if (abs >= 0x4000) {
        if (abs < 0x4400) {
          vs = true;
        } else if (abs < 0x4800) {
          ps = true;
        } else if (abs <= kRegFetchLast) {
          fetch = true;
        }
      } else if (IsFramingRegister(abs)) {
        framing = true;
      }
      target[i] = new_value;
    }
  }
}

// Copies n big-endian words byte-swapped; true if any of them differed from the previous value.
inline bool CopyWithChange(uint32_t* target, const uint8_t* source, uint32_t n) {
  uint32_t i = 0;
  bool change = false;
#if defined(__aarch64__)
  uint32x4_t mismatch = vdupq_n_u32(0);
  for (; i + 4 <= n; i += 4) {
    const uint32x4_t new_value = vreinterpretq_u32_u8(vrev32q_u8(vld1q_u8(source + size_t(i) * 4)));
    mismatch = vorrq_u32(mismatch, veorq_u32(new_value, vld1q_u32(target + i)));
    vst1q_u32(target + i, new_value);
  }
  change = vmaxvq_u32(mismatch) != 0;
#endif
  for (; i < n; ++i) {
    const uint32_t new_value = rex::memory::load_and_swap<uint32_t>(source + size_t(i) * 4);
    change |= target[i] != new_value;
    target[i] = new_value;
  }
  return change;
}

inline void SpanVector(uint32_t index, uint32_t* target, const uint8_t* source, uint32_t n, bool& vs,
                           bool& ps, bool& fetch, bool& framing) {
  /*
   * Fast path. Almost every span is a block of VS or PS constants (or fetch constants) that lies entirely
   * inside its class: one CopyWithChange plus marking the class if anything changed is enough, with no
   * search for boundaries (the loop below walks all 12 for every piece). Same if the span touches no class
   * and not the viewport state range: it is only copied. Anything that crosses a boundary still takes the
   * regular loop.
   * Same result as SpanScalar: tested on the PC with 3.5 million blocks (every start from 0x1F00 to the
   * end with lengths 1 to 40, plus 2 million random ones), 0 differences. VerifyBlock still compares it
   * with the regular path for the first blocks of each run, like the rest of the vector path.
   */
  if (n != 0) {
    const uint32_t last = index + n - 1;  // WriteRegistersInBlock already checked it stays inside the table
    if (index >= 0x4000) {
      if (last < 0x4400) {
        if (CopyWithChange(target, source, n)) {
          vs = true;
        }
        return;
      }
      if (index >= 0x4400 && last < 0x4800) {
        if (CopyWithChange(target, source, n)) {
          ps = true;
        }
        return;
      }
      if (index >= 0x4800 && last <= kRegFetchLast) {
        if (CopyWithChange(target, source, n)) {
          fetch = true;
        }
        return;
      }
      if (index > kRegFetchLast) {
        CopyWithChange(target, source, n);  // booleans and loops: no class
        return;
      }
    } else if (last < kRegFramingFirst || (index > kRegFramingLast && last < 0x4000)) {
      CopyWithChange(target, source, n);  // outside every class and the viewport state range
      return;
    }
  }
  // Where a class starts or ends (see IsFramingRegister and SpanScalar).
  static constexpr uint32_t kSlices[] = {kRegFramingFirst, 0x2083, 0x210F, 0x2115, 0x2204, 0x2207,
                                         0x2302, 0x2303, 0x4000, 0x4400, 0x4800, kRegFetchLast + 1};
  uint32_t i = 0;
  while (i < n) {
    const uint32_t abs = index + i;
    uint32_t end = n;
    for (const uint32_t slice : kSlices) {
      if (slice > abs && slice - index < end) {
        end = slice - index;
      }
    }
    const bool change = CopyWithChange(target + i, source + size_t(i) * 4, end - i);
    if (change) {
      if (abs >= 0x4000) {
        if (abs < 0x4400) {
          vs = true;
        } else if (abs < 0x4800) {
          ps = true;
        } else if (abs <= kRegFetchLast) {
          fetch = true;
        }
      } else if (IsFramingRegister(abs)) {
        framing = true;
      }
    }
    i = end;
  }
}

constexpr uint32_t kSamplesOcclusion = 1000;  // query_occlusion_fake_sample_count
constexpr uint16_t kExtensionMax = 2048 >> 3;  // xenos::kTexture2DCubeMaxWidthHeight >> 3
constexpr int kMaxDepthIndirect = 4;
constexpr auto kWaitRegMemMax = std::chrono::milliseconds(200);
constexpr uint32_t kOutputWidth = 1280;
constexpr uint32_t kOutputHeight = 720;

uint64_t NanosecondsSince(std::chrono::steady_clock::time_point start) {
  return uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(
                      std::chrono::steady_clock::now() - start)
                      .count());
}

// Big-endian words of a command buffer. In the ring the position wraps around (the size is a power
// of two); in an indirect buffer it does not.
struct RingReader {
  const uint8_t* base = nullptr;
  uint32_t mask = 0;  // words - 1 in the ring; 0 in a linear buffer
  uint32_t pos = 0;
  uint32_t end = 0;

  uint32_t Pending() const { return mask ? ((end - pos) & mask) : (end - pos); }
  uint32_t Look() const { return rex::memory::load_and_swap<uint32_t>(base + size_t(pos) * 4); }
  uint32_t Read() {
    const uint32_t value = Look();
    Advance(1);
    return value;
  }
  void Advance(uint32_t words) { pos = mask ? ((pos + words) & mask) : (pos + words); }
};

bool Compares(uint32_t info, uint32_t value, uint32_t reference) {
  switch (info & 0x7) {
    case 0x0:
      return false;
    case 0x1:
      return value < reference;
    case 0x2:
      return value <= reference;
    case 0x3:
      return value == reference;
    case 0x4:
      return value != reference;
    case 0x5:
      return value >= reference;
    case 0x6:
      return value > reference;
    default:
      return true;
  }
}

class NativeRenderSystem final : public rex::system::IGraphicsSystem {
 public:
  NativeRenderSystem() : registers_(kNumRegisters, 0), seen_(kNumRegisters) {}
  ~NativeRenderSystem() override { Shutdown(); }

  X_STATUS SetupPresentation(rex::ui::WindowedAppContext* app_context) override {
    if (presenter_) {
      return X_STATUS_SUCCESS;
    }
    app_context_ = app_context;
    if (!provider_) {
      // The shader library's SPIR-V (from XenosRecomp) needs capabilities that the VkDevice only enables if they are
      // requested before it is created.
      rex::cvar::SetFlagByName("vulkan_native_shader_features", "true");
      provider_ = rex::ui::vulkan::VulkanProvider::Create(true, true);
      if (!provider_) {
        REXLOG_ERROR("[native] Could not create the Vulkan device");
        return X_STATUS_UNSUCCESSFUL;
      }
    }
#if BO2ZM_NATIVE_METAL
    // The Metal libraries made ahead of time (tools/metal) sit in metal/ next to the executable.
    if (!metal_) {
      metal_ = gpu::metal::Device::Create(rex::filesystem::GetExecutableFolder() / "metal");
    }
    // Before the renderer makes anything, so the mirror sees every object made.
    if (metal_ && REXCVAR_GET(bo2zm_native_metal_mirror)) {
      gpu::metal::Mirror::Enable(*metal_);
    }
    const bool metal_present = metal_ && REXCVAR_GET(bo2zm_native_metal_present);
#else
    const bool metal_present = false;
#endif
    auto create = [this, metal_present]() {
#if BO2ZM_NATIVE_METAL
      if (metal_present) {
        auto metal = gpu::metal::Presenter::Create(*metal_);
        metal_presenter_ = metal.get();
        presenter_ = std::move(metal);
        return;
      }
#endif
      (void)metal_present;
      presenter_ = provider_->CreatePresenter();
    };
    if (app_context_) {
      app_context_->CallInUIThreadSynchronous(create);
    } else {
      create();
    }
    if (!presenter_) {
      REXLOG_ERROR("[native] Could not create the presenter");
      return X_STATUS_UNSUCCESSFUL;
    }
    REXLOG_INFO("[native] Native graphics system (C1): the SDK's presenter, without Xenos emulation");
    return X_STATUS_SUCCESS;
  }

  X_STATUS SetupGuestGpu(rex::runtime::FunctionDispatcher* dispatcher,
                         rex::system::KernelState* kernel_state) override {
    dispatcher_ = dispatcher;
    kernel_state_ = kernel_state;
    memory_ = dispatcher->memory();
    if (!mmio_registered_) {
      mmio_registered_ = memory_->AddVirtualMappedRange(kBaseMmio, kMaskMmio, kSizeMmio, this,
                                                        &ReadMmio, &WriteMmio);
      if (!mmio_registered_) {
        REXLOG_ERROR("[native] Could not register the GPU MMIO range");
        return X_STATUS_UNSUCCESSFUL;
      }
    }
    if (active_.exchange(true)) {
      return X_STATUS_SUCCESS;
    }
#if BO2ZM_NATIVE_METAL
    // The guest's 512 MB of physical memory as one Metal buffer, without a copy, for the Metal backend's direct
    // reads of guest data. Nothing uses it yet; if it cannot be made, the backend copies like the Vulkan one.
    if (metal_ && !metal_guest_.buffer) {
      if (metal_->WrapHostMemory(memory_->TranslatePhysical(0), gpu::Size(0x20000000), metal_guest_)) {
        REXLOG_INFO("[native] Metal: guest physical memory is a shared buffer at GPU address {:#x}",
                    metal_guest_.address);
      }
    }
#endif
    // The SPIR-V shader library sits next to the executable. It is built from the player's own game data, so it is
    // never distributed. Without it the game still runs, but no shader can be identified.
    // The first start with a pack makes its index (about a minute on a Switch): a progress screen meanwhile, made
    // only if that happens.
    std::unique_ptr<ProgressScreen> progress;
    const auto show = [&](uint64_t done, uint64_t total) {
      if (!progress) {
        progress = std::make_unique<ProgressScreen>(presenter_.get(),
                                                    provider_ ? provider_->vulkan_device() : nullptr);
      }
      progress->Update(done, total);
    };
    const bool library = shaders_.Load(rex::filesystem::GetExecutableFolder() / BO2ZM_SHADER_LIBRARY_FILE,
                                       presenter_ && !metal_presenting() ? NativeShaders::IndexProgress(show)
                                                                         : nullptr);
    if (progress) {
      progress->Finish();
      progress.reset();
    }
    if (library) {
      ActivateHooks(&shaders_);  // shader objects and Draw* records
    }
    last_report_ = Clock::now();
    last_time_report_ = last_report_;
    start_system_ = last_report_;
    thread_vblank_ = rex::system::object_ref<rex::system::XHostThread>(new rex::system::XHostThread(
        kernel_state_, 128 * 1024, 0, [this]() { return LoopVblank(); }));
    thread_vblank_->set_name("GPU native VSync");
    thread_vblank_->Create();
    thread_ring_ = rex::system::object_ref<rex::system::XHostThread>(new rex::system::XHostThread(
        kernel_state_, 128 * 1024, 0, [this]() { return LoopRing(); }));
    thread_ring_->set_name("GPU native ring");
    thread_ring_->Create();
    // From here on FlushState can write the composite marker (ProcessMarker).
    ActivateConsumerMarkers(true);
    return X_STATUS_SUCCESS;
  }

  bool has_presentation() const override { return presenter_ != nullptr; }
  bool metal_presenting() const {
#if BO2ZM_NATIVE_METAL
    return metal_presenter_ != nullptr;
#else
    return false;
#endif
  }
  rex::ui::GraphicsProvider* provider() const override { return provider_.get(); }
  rex::ui::Presenter* presenter() const override { return presenter_.get(); }

  void SetInterruptCallback(uint32_t callback, uint32_t user_data) override {
    callback_data_.store(user_data, std::memory_order_release);
    callback_.store(callback, std::memory_order_release);
    REXLOG_INFO("[native] Interrupt callback {:08X} ({:08X})", callback, user_data);
  }

  void InitializeRingBuffer(uint32_t ptr, uint32_t size_log2) override {
    // As in CommandProcessor::InitializeRingBuffer: (1 << (size_log2 + 3)) bytes.
    ring_words_.store(uint32_t(1) << (size_log2 + 1), std::memory_order_release);
    ring_base_.store(ptr, std::memory_order_release);
    generation_ring_.fetch_add(1, std::memory_order_acq_rel);
    REXLOG_INFO("[native] Ring at {:08X}, {} words", ptr, uint32_t(1) << (size_log2 + 1));
  }

  void EnableReadPointerWriteBack(uint32_t ptr, uint32_t block_size_log2) override {
    (void)block_size_log2;
    read_returned_.store(ptr, std::memory_order_release);
  }

  void Shutdown() override {
    ActivateHooks(nullptr);  // the game hooks stop touching the library
    ActivateConsumerMarkers(false);  // FlushState goes back to the game's own path
    if (active_.exchange(false)) {
      {
        std::lock_guard<std::mutex> lock(ring_mutex_);
      }
      ring_cv_.notify_all();
      if (thread_ring_) {
        thread_ring_->Wait(0, 0, 0, nullptr);
        thread_ring_.reset();
      }
      if (thread_vblank_) {
        thread_vblank_->Wait(0, 0, 0, nullptr);
        thread_vblank_.reset();
      }
      Report(true);
    }
    // The framebuffers point to presenter images: destroy them before it.
    DestroyVulkan();
    if (presenter_) {
      if (app_context_) {
        app_context_->CallInUIThreadSynchronous([this]() { presenter_.reset(); });
      }
      presenter_.reset();
    }
#if BO2ZM_NATIVE_METAL
    metal_presenter_ = nullptr;
    gpu::metal::Device::Release(metal_guest_);  // before the guest memory goes away
#endif
    provider_.reset();
  }

 private:
  // --- MMIO registers -------------------------------------------------------

  static uint32_t ReadMmio(void* ppc_context, void* context, uint32_t address) {
    (void)ppc_context;
    return static_cast<NativeRenderSystem*>(context)->ReadRegisterMmio(address);
  }

  static void WriteMmio(void* ppc_context, void* context, uint32_t address, uint32_t value) {
    (void)ppc_context;
    static_cast<NativeRenderSystem*>(context)->WriteRegisterMmio(address, value);
  }

  uint32_t ReadRegisterMmio(uint32_t address) {
    const uint32_t r = (address & 0xFFFF) / 4;
    switch (r) {
      case kRegRbEdramTiming:
        return 0x08100748;
      case kRegRbBcControl:
        return 0x0000200E;
      case kRegD1ModeVCounter: {
        rex::system::X_VIDEO_MODE mode;
        rex::kernel::xboxkrnl::VdQueryVideoMode(&mode);
        return std::min(uint32_t(mode.display_height), uint32_t(0x0FFF));
      }
      case kRegD1ModeVblankVlineStatus:
        return 1;  // vblank
      case kRegD1ModeViewportSize: {
        rex::system::X_VIDEO_MODE mode;
        rex::kernel::xboxkrnl::VdQueryVideoMode(&mode);
        const uint32_t width = std::min(uint32_t(mode.display_width), uint32_t(0x0FFF));
        const uint32_t height = std::min(uint32_t(mode.display_height), uint32_t(0x0FFF));
        return (width << 16) | height;
      }
      default:
        break;
    }
    AnnotateFirstTime("read", r, 0);
    return DvarRecord(r);
  }

  void WriteRegisterMmio(uint32_t address, uint32_t value) {
    const uint32_t r = (address & 0xFFFF) / 4;
    if (r == kRegCpRbWptr) {
      g_publications_ring.fetch_add(1, std::memory_order_relaxed);  // before the ring can see the new pointer
      pointer_write_.store(value, std::memory_order_release);
      writes_wptr_.fetch_add(1, std::memory_order_relaxed);
      // Taking the lock for an instant avoids losing the wakeup if the ring
      // thread is right between checking and going to sleep.
      {
        std::lock_guard<std::mutex> lock(ring_mutex_);
      }
      ring_cv_.notify_one();
    } else if (r != kRegD1GrphPrimarySurface) {
      AnnotateFirstTime("write", r, value);
    }
    SaveRegister(r, value);  // no side effects through MMIO (graphics_system.cpp:265-281)
  }

  // The gamma ramp the game loads, with the same logic as the SDK's CommandProcessor::WriteRegister
  // (DC_LUT_SEQ_COLOR in red, green and blue; DC_LUT_30_COLOR with blue in bits 0-9). Called before the
  // value is stored. The write mask (blue at bit 0) and the index that only advances after DC_LUT_30_COLOR
  // match the SDK; every change bumps version_ramp_ and the output picks it up on the next Swap.
  void AnnotateRampGamma(uint32_t index, uint32_t value) {
    ++writes_ramp_[index - kRegRampFirst];
    if (index == kRegRampIndex) {
      component_ramp_ = 0;
    } else if (index == kRegRampMask) {
      mask_ramp_ = value & 0b111;
    } else if (index == kRegRampSequential) {
      const uint32_t i = registers_[kRegRampIndex] & 0xFF;
      if (mask_ramp_ & (UINT32_C(1) << (2 - component_ramp_))) {
        ramp_gamma_[i][component_ramp_] = uint16_t((value >> 6) & 0x3FF);
        ++version_ramp_;
      }
      if (++component_ramp_ >= 3) {
        component_ramp_ = 0;
        registers_[kRegRampIndex] = (registers_[kRegRampIndex] & ~UINT32_C(0xFF)) | ((i + 1) & 0xFF);
      }
    } else if (index == kRegRamp30) {
      const uint32_t i = registers_[kRegRampIndex] & 0xFF;
      if (mask_ramp_ & 0b001) {
        ramp_gamma_[i][2] = uint16_t(value & 0x3FF);
      }
      if (mask_ramp_ & 0b010) {
        ramp_gamma_[i][1] = uint16_t((value >> 10) & 0x3FF);
      }
      if (mask_ramp_ & 0b100) {
        ramp_gamma_[i][0] = uint16_t((value >> 20) & 0x3FF);
      }
      if (mask_ramp_) {
        ++version_ramp_;
      }
      component_ramp_ = 0;
      registers_[kRegRampIndex] = (registers_[kRegRampIndex] & ~UINT32_C(0xFF)) | ((i + 1) & 0xFF);
    }
  }

  // EVENT_WRITE_ZPD: the sample counters of an occlusion query.
  // How the game's D3D does it:
  //   - Issue(BEGIN) (sub_8258F810) marks the structure at base + 0 with 0xFFFFFEED and requests the
  //     counters at base + 32.
  //   - Issue(END) requests them at base + 0 (sub_8258EA28).
  //   - GetData (sub_8258F998) waits for base + 0 to lose the marks and returns
  //     ZPass(base + 0) - ZPass(base + 32).
  // Here (bo2zm_native_occlusion = 1): at the start, base + 32 is zeroed, and the draws up to the end are
  // counted on the GPU with host queries. At the end, base + 0 gets the last measured count of that
  // structure: one from an earlier frame, because host queries are read when their work finishes. Anything
  // that does not fit keeps the old behavior (a faked count).
  void QueryOcclusion(uint32_t address) {
    using CountSet = xenos::xe_gpu_depth_sample_counts;
    constexpr uint32_t kMark = 0xEDFEFFFF;  // 0xFFFFFEED written big-endian by D3D, read as little-endian
    const auto marked = [](const CountSet* c) {
      return uint32_t(c->ZPass_A) == kMark || uint32_t(c->ZPass_B) == kMark || uint32_t(c->ZFail_A) == kMark ||
             uint32_t(c->ZFail_B) == kMark;
    };
    auto* counts = memory_->TranslatePhysical<CountSet*>(address);
    const bool marks_here = marked(counts);
    const bool marks_before = address >= 32 && marked(memory_->TranslatePhysical<CountSet*>(address - 32));
    int32_t mode = REXCVAR_GET(bo2zm_native_occlusion);
    // Test cvar bo2zm_native_occlusion_alternate_s: odd intervals use the old faked count.
    if (const int32_t alternate_s = REXCVAR_GET(bo2zm_native_occlusion_alternate_s); mode == 1 && alternate_s > 0) {
      const auto now = Clock::now();
      if (!occlusion_test_started_) {
        occlusion_test_started_ = true;
        occlusion_test_start_ = now;
      }
      const auto seconds =
          std::chrono::duration_cast<std::chrono::seconds>(now - occlusion_test_start_).count();
      const bool fake = (seconds / alternate_s) % 2 == 1;
      if (fake != occlusion_test_fake_) {
        occlusion_test_fake_ = fake;
        if (fake && occlusion_base_ != 0 && targets_) {
          uint64_t discarded = 0;
          targets_->FinishOcclusion(occlusion_base_, discarded);
          occlusion_base_ = 0;
        }
        REXLOG_INFO("[native] occlusion test: {}", fake ? "fake (1000)" : "measured");
      }
      if (fake) {
        mode = 0;
      }
    }
    const char* what = nullptr;
    uint32_t written = UINT32_MAX;
    uint64_t measures = 0;
    bool measure = false;
    uint32_t scale = 0;
    if (mode == 1 && targets_) {
      if (occlusion_base_ != 0 && address == occlusion_base_) {
        measure = targets_->FinishOcclusion(address, measures);
        occlusion_base_ = 0;
        scale = ScaleOcclusion();
        // FinishOcclusion answers with the last measurement the GPU published for this query (a frame or so old).
        // A query seen for the first time has none yet: that answer is 0 samples (hidden), not the old faked 1000,
        // which drew a light's glow at full strength for the frames until its first measurement. Buried's lamps
        // query the scene this way, and each one flashed as it came into view (on a Switch Lite, 8 to 15 such queries
        // every 20 s while walking around: blinking lights and white glare). One frame late is what the Xbox 360's
        // own query latency looks like anyway.
        written = measure ? uint32_t(std::min<uint64_t>(measures * scale, 0xFFFFFF)) : 0;
        std::memset(counts, 0, sizeof(*counts));
        counts->ZPass_A = written;
        counts->Total_A = written;
        what = measure ? "final" : "end without a measurement";
        if (measure) {
          ++occlusion_counters_[1];
          occlusion_counters_[5] += written;
          occlusion_counters_[6] = std::max<uint64_t>(occlusion_counters_[6], written);
        } else {
          ++occlusion_counters_[2];
        }
        // By render target: the 320 one (the sun) or a scene one (640 or more, scaled by the AA mode).
        const size_t group = scale > 1 ? 1 : 0;
        ++occlusion_per_target_[group];
        if (measure) {
          occlusion_max_per_target_[group] = std::max(occlusion_max_per_target_[group], written);
        }
        if (group == 1 && warnings_occlusion_scene_ < 12) {
          ++warnings_occlusion_scene_;
          const uint32_t info = DvarRecord(rex::graphics::XE_GPU_REG_RB_SURFACE_INFO);
          REXLOG_INFO("[native] C2 occlusion on scene target {}: structure {:08X}, pitch {}, MSAA {}, scale {}, host "
                      "samples {} ({}), written {}",
                      warnings_occlusion_scene_, address, info & 0x3FFF, (info >> 16) & 0x3, scale, measures,
                      measure ? "measured" : "without a measurement", written);
        }
      } else if (address >= 32 && (marks_before || bases_occlusion_.count(address - 32))) {
        const uint32_t base = address - 32;
        if (occlusion_base_ != 0) {
          uint64_t discarded = 0;
          targets_->FinishOcclusion(occlusion_base_, discarded);  // the previous one did not reach its Issue(END)
          ++occlusion_counters_[3];
        }
        if (bases_occlusion_.size() < 64) {
          bases_occlusion_.insert(base);
        }
        targets_->BeginOcclusion(base);
        occlusion_base_ = base;
        std::memset(counts, 0, sizeof(*counts));
        what = "start";
        ++occlusion_counters_[0];
      }
    }
    if (!what) {
      // The previous behavior (also the emulated path's): a faked count at the end.
      std::memset(counts, 0, sizeof(*counts));
      if (marks_here) {
        written = mode == 2 ? 0 : kSamplesOcclusion;
        counts->ZPass_A = written;
        counts->Total_A = written;
        ++occlusion_counters_[4];
      }
      what = mode == 1 ? "without a pair" : "fake";
    }
    if (warnings_occlusion_ < 8) {  // was 48; these lines are long and the ring thread writes them
      ++warnings_occlusion_;
      const uint8_t* base_virtual = memory_->virtual_membase();
      const auto read_be = [base_virtual](uint32_t d) {
        uint32_t v = 0;
        std::memcpy(&v, base_virtual + d, sizeof(v));
        return __builtin_bswap32(v);
      };
      const uint32_t renderer = 0;  // reference-port address 0x82A2D1AC, not BO2
      const uint32_t info = DvarRecord(rex::graphics::XE_GPU_REG_RB_SURFACE_INFO);
      REXLOG_INFO("[native] C2 occlusion {}: ZPD at {:08X} ({}), marks here {} and 32 before {}; host samples {} ({}), "
                  "scale {}, written {}; bins {:016X}/{:016X}; RB_SURFACE_INFO {:08X} (pitch {}, MSAA {}), "
                  "RB_DEPTH_INFO {:08X}; game AA mode {}, quality {}",
                  warnings_occlusion_, address, what, marks_here, marks_before, measures, measure ? "measured" : "without a measurement",
                  scale, written == UINT32_MAX ? -1 : int64_t(written), bin_select_, bin_mask_, info, info & 0x3FFF,
                  (info >> 16) & 0x3, DvarRecord(rex::graphics::XE_GPU_REG_RB_DEPTH_INFO),
                  renderer ? int64_t(read_be(renderer)) : -1, 0u);  // reference-port address 0x82A2CEE4, not BO2
    }
  }

  // Samples per pixel the Xbox 360 would count in the query's render target: the MSAA samples the target
  // requests and, for scene targets (pitch of 640 or more), those of the game's AA mode before it is
  // removed (a reference-port test). The sun flare is queried on a 320 target without MSAA: 1.
  uint32_t ScaleOcclusion() const {
    const int32_t fixed = REXCVAR_GET(bo2zm_native_occlusion_scale);
    if (fixed > 0) {
      return uint32_t(fixed);
    }
    const uint32_t info = DvarRecord(rex::graphics::XE_GPU_REG_RB_SURFACE_INFO);
    const uint32_t samples_target = UINT32_C(1) << std::min<uint32_t>((info >> 16) & 0x3, 2);
    const uint32_t samples_scene =
        (info & 0x3FFF) >= 640 ? bo2zm::render_targets::SamplesOriginalModeCurrent(memory_->virtual_membase()) : 1;
    return std::max(samples_target, samples_scene);
  }

  void AnnotateFirstTime(const char* what, uint32_t index, uint32_t value) {
    if (index >= kNumRegisters || seen_[index].exchange(1, std::memory_order_acq_rel)) {
      return;
    }
    REXLOG_INFO("[native] MMIO register {:04X}: first {} ({:08X})", index, what, value);
  }

  // The registers are touched by the game thread (MMIO) and by the ring thread. As in the
  // emulated path, races on integer values are harmless.
  void SaveRegister(uint32_t index, uint32_t value) {
    if (index < kNumRegisters) {
      /*
       * This entry point is used by MMIO from the game thread and by VIZ_QUERY, and it does not go
       * through WriteRegister. Neither touches the fetch constants or the viewport state today (the
       * 360's D3D writes GPU registers through the ring), but if one ever did and the generation were
       * not bumped, the translator would keep the stale viewport or textures. It is called very rarely
       * compared with the ring, so the safeguard costs nothing.
       */
      if (registers_[index] != value) {
        if (index >= kRegFetchFirst && index <= kRegFetchLast) {
          ++generation_fetch_;
        } else if (IsFramingRegister(index)) {
          ++generation_framing_;
        }
      }
      registers_[index] = value;
    }
  }

  // A write from the ring, with the side effects it has when the GPU processes it
  // (CommandProcessor::WriteRegister). SCRATCH_REG values are copied to memory: the
  // D3D writes one and then waits with WAIT_REG_MEM until it shows up at SCRATCH_ADDR
  // (measured on the PC: without this the game stops within the first second).
  // COHER_STATUS_HOST stays marked as pending until the next poll.
  void WriteRegister(uint32_t index, uint32_t value) {
    if (index >= kNumRegisters) {
      return;
    }
    /*
     * Only when the value really changes.
     *
     * The Xbox 360 D3D dumps the constant block before every draw, and many constants are identical
     * between draws of the same material. Bumping the generation on every write made 60 % of the draws
     * re-upload their 7.7 KB of constants to the buffer (12.54 MB per frame, measured) into memory with
     * no CPU cache (2654 MB/s), on the thread that already uses 96 % of a core.
     *
     * Comparing before writing costs one read of a line that is about to be written anyway, so it is
     * already in cache. And the result is bit-for-bit identical: if the value does not change, the
     * shader reads the same thing.
     */
    if (index >= 0x4000) {
      if (index < 0x4800) {  // VS and PS constants (native draws)
        if (registers_[index] != value) {
          ++(index < 0x4400 ? generation_constants_vs_ : generation_constants_ps_);
        } else {
          ++constants_no_change_;
        }
        ++constants_written_;
      } else if (index <= kRegFetchLast) {  // fetch constants: textures and vertices
        if (registers_[index] != value) {
          ++generation_fetch_;
        } else {
          ++fetch_no_change_;
        }
        ++fetch_written_;
      }
    } else if (IsFramingRegister(index)) {
      if (registers_[index] != value) {
        ++generation_framing_;
      } else {
        ++framing_no_change_;
      }
      ++framing_written_;
    }
    if (index == rex::graphics::XE_GPU_REG_COHER_STATUS_HOST) {
      value |= UINT32_C(0x80000000);
    }
    if (index >= kRegRampFirst && index <= kRegRampLast) {
      AnnotateRampGamma(index, value);
    }
    registers_[index] = value;
    if (index >= rex::graphics::XE_GPU_REG_SCRATCH_REG0 &&
        index <= rex::graphics::XE_GPU_REG_SCRATCH_REG7) {
      const uint32_t n = index - rex::graphics::XE_GPU_REG_SCRATCH_REG0;
      if ((UINT32_C(1) << n) & registers_[rex::graphics::XE_GPU_REG_SCRATCH_UMSK]) {
        BeforeFence();
        const uint32_t address = registers_[rex::graphics::XE_GPU_REG_SCRATCH_ADDR] + n * 4;
        rex::memory::store_and_swap<uint32_t>(memory_->TranslatePhysical(address), value);
        AddOnlyRing(writes_memory_);
      }
    }
  }

  // A block of consecutive registers without side effects, in one go. Returns false (and consumes nothing)
  // if it touches COHER_STATUS_HOST or the SCRATCH_REG registers, if it goes past the table or if it is
  // disabled.
  bool WriteRegistersInBlock(uint32_t index, uint32_t count, RingReader& data) {
    if (registers_in_block_ < 0) {
      registers_in_block_ = REXCVAR_GET(bo2zm_native_block_register_writes) ? 1 : 0;
      REXLOG_INFO("[native] block register writes (bo2zm_native_block_register_writes) = {}",
                  registers_in_block_ ? "SI" : "no");
    }
    if (!registers_in_block_ || count < 2 || index >= kNumRegisters || count > kNumRegisters - index) {
      return false;
    }
    const uint32_t end = index + count;
    const auto due = [index, end](uint32_t since, uint32_t until) { return index < until && since < end; };
    if (due(rex::graphics::XE_GPU_REG_COHER_STATUS_HOST, rex::graphics::XE_GPU_REG_COHER_STATUS_HOST + 1) ||
        due(rex::graphics::XE_GPU_REG_SCRATCH_REG0, rex::graphics::XE_GPU_REG_SCRATCH_REG7 + 1) ||
        due(kRegRampFirst, kRegRampLast + 1)) {
      return false;
    }
    // Same as above, but a block does not know whether it changed until it has been copied, so
    // the generation is bumped afterwards, and only if some word in the range differs.
    const bool looks_vs = due(0x4000, 0x4400);
    const bool looks_ps = due(0x4400, 0x4800);
    // The same for the fetch constants and the viewport state. The viewport state is four small
    // separate pieces, so it is enough to check whether the block crosses the range that holds them:
    // inside the loop each word is already classified, and a generation bumped too often only makes
    // the translator do extra work, it never draws wrong.
    const bool looks_fetch = due(kRegFetchFirst, kRegFetchLast + 1);
    const bool looks_framing = due(kRegFramingFirst, kRegFramingLast + 1);
    const bool looks_values = looks_vs || looks_ps || looks_fetch || looks_framing;
    bool change_vs = false;
    bool change_ps = false;
    bool change_fetch = false;
    bool change_framing = false;
    // The vector path, with a self-checking guard (see VerifyBlock).
    if (registers_vector_ < 0) {
      registers_vector_ = REXCVAR_GET(bo2zm_native_vector_register_writes) ? 1 : 0;
      REXLOG_INFO("[native] vector register writes (bo2zm_native_vector_register_writes) = {}",
                  registers_vector_ ? "YES, checking the first blocks against the usual path"
                                         : "no");
    }
    const bool vectorial = registers_vector_ > 0;
    const bool verify = vectorial && blocks_verified_ < kBlocksAVerify;
    RingReader data_verification;
    if (verify) {
      data_verification = data;
      copy_verification_.assign(registers_.begin() + index, registers_.begin() + index + count);
    }
    uint32_t index_current = index;
    uint32_t* target = registers_.data() + index;
    uint32_t remaining = count;
    while (remaining) {
      // Contiguous span: up to the end of the ring (in a linear buffer, everything left).
      const uint32_t span = data.mask ? std::min(remaining, data.mask + 1 - data.pos) : remaining;
      const uint8_t* source = data.base + size_t(data.pos) * 4;
      if (looks_values) {
        if (vectorial) {
          SpanVector(index_current, target, source, span, change_vs, change_ps, change_fetch,
                         change_framing);
        } else {
          SpanScalar(index_current, target, source, span, change_vs, change_ps, change_fetch,
                       change_framing);
        }
        if (looks_vs || looks_ps) {
          constants_written_ += span;
        }
        if (looks_fetch) {
          fetch_written_ += span;
        }
        if (looks_framing) {
          framing_written_ += span;
        }
      } else if (vectorial) {
        CopyWithChange(target, source, span);
      } else {
        for (uint32_t i = 0; i < span; ++i) {
          target[i] = rex::memory::load_and_swap<uint32_t>(source + size_t(i) * 4);
        }
      }
      index_current += span;
      target += span;
      data.Advance(span);
      remaining -= span;
    }
    if (verify) {
      VerifyBlock(index, count, data_verification, looks_values, change_vs, change_ps, change_fetch,
                      change_framing);
    }
    if (change_vs) {
      ++generation_constants_vs_;
    }
    if (change_ps) {
      ++generation_constants_ps_;
    }
    if (change_fetch) {
      ++generation_fetch_;
    }
    if (change_framing) {
      ++generation_framing_;
    }
    if ((looks_vs && !change_vs) || (looks_ps && !change_ps)) {
      ++blocks_constants_no_change_;
    }
    if (looks_vs || looks_ps) {
      ++blocks_constants_;
    }
    return true;
  }

  /*
   * The vector path guard. For the first kBlocksAVerify blocks, the block is replayed through the
   * regular path (SpanScalar) on a copy of the previous registers, and the results are compared register
   * by register and class by class. If anything differs, the regular path's result is written, the log
   * reports it and the vector path is turned off for the rest of the run.
   */
  void VerifyBlock(uint32_t index, uint32_t count, RingReader data, bool looks_values, bool& vs, bool& ps,
                       bool& fetch, bool& framing) {
    bool ref_vs = false;
    bool ref_ps = false;
    bool ref_fetch = false;
    bool ref_framing = false;
    uint32_t done = 0;
    while (done < count) {
      const uint32_t remaining = count - done;
      const uint32_t span = data.mask ? std::min(remaining, data.mask + 1 - data.pos) : remaining;
      SpanScalar(index + done, copy_verification_.data() + done, data.base + size_t(data.pos) * 4, span,
                   ref_vs, ref_ps, ref_fetch, ref_framing);
      data.Advance(span);
      done += span;
    }
    ++blocks_verified_;
    const bool equal_values =
        std::equal(copy_verification_.begin(), copy_verification_.end(), registers_.begin() + index);
    const bool equal_classes =
        !looks_values || (ref_vs == vs && ref_ps == ps && ref_fetch == fetch && ref_framing == framing);
    if (!equal_values || !equal_classes) {
      REXLOG_ERROR("[native] vector register writes: block {:04X}+{} does NOT match the usual path (values {}, classes "
                   "VS {}/{} PS {}/{} fetch {}/{} framing {}/{}). The vector path is switched OFF.",
                   index, count, equal_values ? "equal" : "DIFFERENT", vs, ref_vs, ps, ref_ps, fetch,
                   ref_fetch, framing, ref_framing);
      std::copy(copy_verification_.begin(), copy_verification_.end(), registers_.begin() + index);
      if (looks_values) {
        vs = ref_vs;
        ps = ref_ps;
        fetch = ref_fetch;
        framing = ref_framing;
      }
      registers_vector_ = 0;
      return;
    }
    if (blocks_verified_ == kBlocksAVerify) {
      REXLOG_INFO("[native] vector register writes: {} blocks checked against the usual path, all equal. The vector "
                  "path stays.",
                  blocks_verified_);
    }
  }

  /*
   * The FlushState composite marker.
   *
   * See kMarkerMagic in bo2zm_native_hooks.h and the end of bo2zm_d3d_marker.cpp. FlushState
   * writes one NOP with all its register spans instead of ~7 type-0 packets and ~6 padding words per draw.
   *   - Apply mode: the spans are written here with the same effect as a type-0 block (values, generations
   *     and counters of WriteRegistersInBlock).
   *   - Check mode: the type-0 packets of the same dump come right before it and have already been applied.
   *     It only checks that the registers hold what the marker says, and the result goes back to the game
   *     thread.
   * A NOP without the magic belongs to the game itself and is ignored, as before.
   */
  void ProcessMarker(RingReader& data, uint32_t words) {
    if (words < 2 || (data.Look() & 0xFFFFFF00u) != kMarkerMagic) {
      return;
    }
    const uint32_t header = data.Read();
    const uint32_t mode = header & 0x0Fu;               // registers: 0 no spans, 1 apply, 2 check
    const uint32_t mode_draw = (header >> 4) & 0x0Fu;  // draw record: 0 none, kDrawApply, kDrawCheck
    const uint32_t sequence = data.Read();
    uint32_t n = words - 2;  // draw record, span headers and values
    // The spans, contiguous in memory: D3D never splits a packet; if it wraps around the ring, it is copied
    // aside.
    const uint8_t* p = data.base + size_t(data.pos) * 4;
    if (data.mask && data.pos + n > data.mask + 1) {
      marker_plane_.resize(n);
      for (uint32_t i = 0; i < n; ++i) {
        std::memcpy(&marker_plane_[i], data.base + size_t((data.pos + i) & data.mask) * 4, 4);
      }
      p = reinterpret_cast<const uint8_t*>(marker_plane_.data());
    }
    // The Draw* record of this FlushState, ahead of the spans. It is stored at the end, once the whole marker has
    // been validated.
    DrawRecord draw;
    if (mode_draw != 0) {
      if ((mode_draw != kDrawApply && mode_draw != kDrawCheck) || n < kWordsDraw ||
          !ReadMarkerDraw(p, draw)) {
        MarkerBad(mode, sequence, header);
        return;
      }
      p += size_t(kWordsDraw) * 4;
      n -= kWordsDraw;
    }
    // Validate the whole structure first: a broken marker writes nothing.
    uint32_t spans = 0;
    for (uint32_t i = 0; i < n;) {
      const uint32_t t = rex::memory::load_and_swap<uint32_t>(p + size_t(i) * 4);
      const uint32_t count = t >> 16;
      if (count >= n - i || !DumpSpan(t & 0xFFFFu, count)) {
        MarkerBad(mode, sequence, t);
        return;
      }
      i += 1 + count;
      ++spans;
    }
    if (mode > kMarkerCheck || (mode == 0 && (n != 0 || mode_draw == 0))) {
      MarkerBad(mode, sequence, header);
      return;
    }
    // For the DRAW_INDX packets that follow (PairDraw): until one accepts it or the next Draw* record
    // arrives.
    if (mode_draw != 0) {
      draw_marker_ = draw;
      draw_marker_mode_ = mode_draw;
    }
    if (mode == kMarkerCheck) {
      CheckMarker(p, n, sequence);
      return;
    }
    if (mode == 0) {
      return;  // only the draw record
    }
    // The fast path is decided once per marker: the block path and the vector path enabled and already
    // verified.
    const bool fast =
        registers_in_block_ > 0 && registers_vector_ > 0 && blocks_verified_ >= kBlocksAVerify;
    for (uint32_t i = 0; i < n;) {
      const uint32_t t = rex::memory::load_and_swap<uint32_t>(p + size_t(i) * 4);
      const uint32_t index = t & 0xFFFFu;
      const uint32_t count = t >> 16;
      if (fast && count >= 2 && marker_fast_ > 0) {
        ApplySpanFast(index, count, p + size_t(i + 1) * 4);
      } else {
        // The regular path, same as a type-0 packet with these registers.
        ++marker_spans_slow_;
        RingReader linear;
        linear.base = p;
        linear.pos = i + 1;
        linear.end = n;
        if (count < 2 || !WriteRegistersInBlock(index, count, linear)) {
          for (uint32_t k = 0; k < count; ++k) {
            WriteRegister(index + k, rex::memory::load_and_swap<uint32_t>(p + size_t(i + 1 + k) * 4));
          }
        }
      }
      i += 1 + count;
    }
    ++markers_applied_;
    marker_spans_ += spans;
    marker_words_ += n - spans;
  }

  void MarkerBad(uint32_t mode, uint32_t sequence, uint32_t word) {
    ++markers_bad_;
    if (warnings_marker_ < 8) {
      ++warnings_marker_;
      REXLOG_ERROR("[native] D3D marker {} MALFORMED (mode {}, word {:08X}): not applied, and the game thread switches "
                   "the marker path off",
                   sequence, mode, word);
    }
    AnnotateCheckMarker(false, sequence, 0, 0, word);
  }

  void CheckMarker(const uint8_t* p, uint32_t n, uint32_t sequence) {
    ++markers_checked_;
    for (uint32_t i = 0; i < n;) {
      const uint32_t t = rex::memory::load_and_swap<uint32_t>(p + size_t(i) * 4);
      const uint32_t index = t & 0xFFFFu;
      const uint32_t count = t >> 16;
      for (uint32_t k = 0; k < count; ++k) {
        const uint32_t value = rex::memory::load_and_swap<uint32_t>(p + size_t(i + 1 + k) * 4);
        if (registers_[index + k] != value) {
          ++markers_different_;
          if (warnings_marker_ < 8) {
            ++warnings_marker_;
            REXLOG_ERROR("[native] D3D marker {}: register {:04X} is {:08X} after its packets and {:08X} in the marker. "
                         "The game thread switches the marker path off",
                         sequence, index + k, registers_[index + k], value);
          }
          AnnotateCheckMarker(false, sequence, index + k, registers_[index + k], value);
          return;
        }
      }
      i += 1 + count;
    }
    AnnotateCheckMarker(true, sequence, 0, 0, 0);
  }

  // A span of 2 or more registers from a single dump group (DumpSpan). Same effect as
  // WriteRegistersInBlock with those registers (values, generations and counters), without its checks
  // and without searching for boundaries: in each group, the classes lie in a single known piece.
  void ApplySpanFast(uint32_t index, uint32_t count, const uint8_t* source) {
    uint32_t* target = registers_.data() + index;
    const bool verify = spans_fast_verified_ < kSpansFastAVerify;
    if (verify) {
      copy_marker_.assign(target, target + count);
    }
    bool vs = false;
    bool ps = false;
    bool fetch = false;
    bool framing = false;
    const uint32_t end = index + count;
    if (index >= 0x4000) {
      // VS, PS, fetch or boolean constants: the whole span belongs to a single class.
      const bool change = CopyWithChange(target, source, count);
      if (index < 0x4400) {
        vs = change;
      } else if (index < 0x4800) {
        ps = change;
      } else if (index <= kRegFetchLast) {
        fetch = change;
      }
    } else {
      // 0x2000-0x23FF: the viewport state of each group is a single piece (0x210F-0x2114,
      // 0x2204-0x2206 and 0x2302).
      uint32_t e0 = end;
      uint32_t e1 = end;
      if (index >= 0x2100 && index < 0x2180) {
        e0 = 0x210F;
        e1 = 0x2115;
      } else if (index >= 0x2200 && index < 0x2280) {
        e0 = 0x2204;
        e1 = 0x2207;
      } else if (index >= 0x2300 && index < 0x2380) {
        e0 = 0x2302;
        e1 = 0x2303;
      }
      e0 = std::min(std::max(e0, index), end);
      e1 = std::min(std::max(e1, e0), end);
      CopyWithChange(target, source, e0 - index);
      framing = CopyWithChange(target + (e0 - index), source + size_t(e0 - index) * 4, e1 - e0);
      CopyWithChange(target + (e1 - index), source + size_t(e1 - index) * 4, end - e1);
    }
    if (verify) {
      VerifySpanMarker(index, count, source, vs, ps, fetch, framing);
    }
    // The same counters and generations as WriteRegistersInBlock for a block with these registers.
    if (index >= 0x4000 && index < 0x4800) {
      constants_written_ += count;
      ++blocks_constants_;
      if (!(index < 0x4400 ? vs : ps)) {
        ++blocks_constants_no_change_;
      }
    } else if (index >= kRegFetchFirst && index <= kRegFetchLast) {
      fetch_written_ += count;
    } else if (index <= kRegFramingLast && end > kRegFramingFirst) {
      framing_written_ += count;
    }
    if (vs) {
      ++generation_constants_vs_;
    }
    if (ps) {
      ++generation_constants_ps_;
    }
    if (fetch) {
      ++generation_fetch_;
    }
    if (framing) {
      ++generation_framing_;
    }
  }

  // The fast path guard: the first kSpansFastAVerify spans are replayed through SpanScalar (the
  // regular path, word by word) on a copy of the previous registers, and values and classes are compared.
  // If anything differs, the regular path's result is kept and the fast path is turned off for the rest
  // of the run.
  void VerifySpanMarker(uint32_t index, uint32_t count, const uint8_t* source, bool& vs, bool& ps, bool& fetch,
                              bool& framing) {
    bool ref_vs = false;
    bool ref_ps = false;
    bool ref_fetch = false;
    bool ref_framing = false;
    SpanScalar(index, copy_marker_.data(), source, count, ref_vs, ref_ps, ref_fetch, ref_framing);
    ++spans_fast_verified_;
    const bool equal = std::equal(copy_marker_.begin(), copy_marker_.end(), registers_.begin() + index) &&
                         ref_vs == vs && ref_ps == ps && ref_fetch == fetch && ref_framing == framing;
    if (!equal) {
      REXLOG_ERROR("[native] D3D marker: the fast path of span {:04X}+{} does NOT match the usual one (classes VS {}/{} "
                   "PS {}/{} fetch {}/{} framing {}/{}). The fast path is switched OFF.",
                   index, count, vs, ref_vs, ps, ref_ps, fetch, ref_fetch, framing, ref_framing);
      std::copy(copy_marker_.begin(), copy_marker_.end(), registers_.begin() + index);
      vs = ref_vs;
      ps = ref_ps;
      fetch = ref_fetch;
      framing = ref_framing;
      marker_fast_ = 0;
      return;
    }
    if (spans_fast_verified_ == kSpansFastAVerify) {
      REXLOG_INFO("[native] D3D marker: {} fast-path spans checked against the usual one, all equal. The fast path "
                  "stays.",
                  spans_fast_verified_);
    }
  }

  uint32_t DvarRecord(uint32_t index) const { return index < kNumRegisters ? registers_[index] : 0; }

  // --- GPU memory (the two low bits of the address are the byte order) ----

  uint32_t ReadMemory(uint32_t address) const {
    uint32_t value;
    std::memcpy(&value, memory_->TranslatePhysical(address & ~uint32_t(0x3)), sizeof(value));
    return xenos::GpuSwap(value, static_cast<xenos::Endian>(address & 0x3));
  }

  void WriteMemory(uint32_t address, uint32_t value) {
    const uint32_t sorted = xenos::GpuSwap(value, static_cast<xenos::Endian>(address & 0x3));
    std::memcpy(memory_->TranslatePhysical(address & ~uint32_t(0x3)), &sorted, sizeof(sorted));
    AddOnlyRing(writes_memory_);
  }

  // --- Threads ------------------------------------------------------------------

  void Interrupt(uint32_t source_kind, uint32_t cpu) {
    const uint32_t callback = callback_.load(std::memory_order_acquire);
    if (!callback || !dispatcher_) {
      return;
    }
    auto* thread = rex::system::XThread::GetCurrentThread();
    if (!thread) {
      return;
    }
    thread->SetActiveCpu(uint8_t(cpu));
    uint64_t args[] = {source_kind, callback_data_.load(std::memory_order_acquire)};
    dispatcher_->ExecuteInterrupt(thread->thread_state(), callback, args, 2);
    interrupts_.fetch_add(1, std::memory_order_relaxed);  // NOTE: also called from LoopVblank (another thread)
    WarnProgressRing();  // some D3D waits also check the interrupt counter
  }

  int LoopVblank() {
    rex::system::X_VIDEO_MODE mode;
    rex::kernel::xboxkrnl::VdQueryVideoMode(&mode);
    const double hz = std::max(1.0, double(float(mode.refresh_rate)));
    const auto interval =
        std::chrono::duration_cast<Clock::duration>(std::chrono::duration<double>(1.0 / hz));
    auto next = Clock::now() + interval;
    while (active_.load(std::memory_order_acquire)) {
      const auto now = Clock::now();
      // After a long pause (debugger, loading) hundreds of vblanks are not replayed.
      if (now - next > std::chrono::milliseconds(250)) {
        next = now;
      }
      while (now >= next) {
        counter_.fetch_add(1, std::memory_order_relaxed);
        vblanks_.fetch_add(1, std::memory_order_relaxed);
        Interrupt(0, 2);
        next += interval;
      }
      rex::thread::Sleep(std::chrono::milliseconds(1));
    }
    return 0;
  }

  int LoopRing() {
    bo2zm::memcpy_callers::MarkThread();  // test build only (BO2ZM_MEMCPY_CALLERS)
    /*
     * The ring thread drops to 0x2D, because of the presenter.
     *
     * Measured: our presentation thread took 54.26 ms per iteration, 53.38 of them in "record"
     * (twenty Vulkan calls that should cost less than a millisecond). It was not work: the thread
     * spent 998 ms of wall-clock time per second but only 300 ms of CPU. The other 700 ms it was
     * ready with no core, preempted by this thread, which runs at 81 % and shared its priority
     * (0x2C). On Horizon, priorities below 0x3B are not time-sliced, so at equal priority the
     * presenter only ran when the ring thread yielded... and the ring thread had just lost exactly
     * what used to make it yield.
     *
     * Result: only 13.37 of the 26.25 frames per second reached the window.
     *
     * This thread is lowered instead of raising the presenter, to stay out of the audio priority
     * (0x2B), which was hard to get right and is settled. The presenter needs ~2 ms about 27 times
     * per second (5 % of a core) and sleeps on a condition variable as soon as it is done, so what
     * it takes from this thread is negligible.
     */
#if REX_PLATFORM_SWITCH
    {
      const int32_t priority = REXCVAR_GET(bo2zm_native_ring_priority);
      if (priority >= 0x1C && priority <= 0x3B) {
        RexSwitchSetCurrentThreadPriority(int(priority));
        REXLOG_INFO("[native] ring thread at priority {:#x} (the presenter runs at 0x2C and has to be able to preempt "
                    "it)", uint32_t(priority));
      }
      /*
       * And which core it starts on. See bo2zm_native_ring_core.
       *
       * The core is logged before and after. Without the "before" the line cannot be interpreted: if
       * the process's default core were already 1, asking for core 1 would separate nothing at all and
       * the log would look like a success.
       */
      const int core_before = RexSwitchCurrentCore();
      const int32_t core = REXCVAR_GET(bo2zm_native_ring_core);
      const bool exclusive = core >= 0 && REXCVAR_GET(bo2zm_native_ring_core_exclusive);
      if (core >= -1 && core <= 2) {
        const bool set = exclusive ? RexSwitchPinCurrentThreadToCore(int(core))
                                      : RexSwitchSetCurrentThreadCore(int(core));
        REXLOG_INFO("[native] ring thread: core {} {} ({}); it was on {} and now runs on {}",
                    core, exclusive ? "EXCLUSIVE (reduced mask)" : "preferred",
                    set ? "accepted" : "REJECTED by the kernel", core_before,
                    RexSwitchCurrentCore());
      }
      core_ring_ = RexSwitchCurrentCore();
    }
#endif
    // The pipeline precompile at start-up (bo2zm_native_precompile.h). Decided here, as the graphics system starts and
    // before the game is launched. When it runs, the renderer is prepared now instead of at the game's first
    // copy, and the game waits for the pass (Bo2zmApp::OnHoldModuleLaunch).
    if (bo2zm::native::precompile::Decide(provider_ ? provider_->vulkan_device() : nullptr,
                                          shaders_.loaded() ? &shaders_ : nullptr)) {
      if (EnsureTargets()) {
        targets_->Precompile();
      }
      bo2zm::native::precompile::End();
    }
    uint32_t read = 0;
    uint32_t generation = 0;
    while (active_.load(std::memory_order_acquire)) {
      {
        std::unique_lock<std::mutex> lock(ring_mutex_);
        const auto before_no_job = Clock::now();  // for the "[hitch]" line
        const bool with_signal = ring_cv_.wait_for(lock, std::chrono::milliseconds(4), [&] {
          return !active_.load(std::memory_order_acquire) ||
                 pointer_write_.load(std::memory_order_acquire) != read ||
                 generation_ring_.load(std::memory_order_acquire) != generation;
        });
        bo2zm::waits::Add(bo2zm::waits::kRingNoJob, NanosecondsSince(before_no_job));
        // Wakeup counters (only this thread uses them; Report runs on it).
        ++loops_ring_;
        if (!with_signal) {
          ++waits_ring_exhausted_;
        }
#if REX_PLATFORM_SWITCH
        /*
         * Counts how many times the kernel has moved this thread to another core. It is one
         * register read per loop iteration (not a real syscall), and it is the only way to know
         * whether the preferred core does anything: if -1 and 1 give the same count, it has no
         * effect here.
         */
        {
          const int now = RexSwitchCurrentCore();
          if (now != core_ring_) {
            core_ring_ = now;
            ++migrations_ring_;
          }
        }
#endif
      }
      if (!active_.load(std::memory_order_acquire)) {
        break;
      }
      Report(false);
      const uint32_t gen = generation_ring_.load(std::memory_order_acquire);
      if (gen != generation) {
        generation = gen;
        read = 0;  // InitializeRingBuffer leaves the read pointer at zero
      }
      const uint32_t base = ring_base_.load(std::memory_order_acquire);
      const uint32_t words = ring_words_.load(std::memory_order_acquire);
      if (!base || !words) {
        continue;
      }
      const uint32_t written = pointer_write_.load(std::memory_order_acquire) & (words - 1);
      if (written == read) {
        continue;
      }
      ++loops_ring_with_data_;
      RingReader reader;
      reader.base = memory_->TranslatePhysical(base);
      reader.mask = words - 1;
      reader.pos = read;
      reader.end = written;
      const auto start_ring = Clock::now();
      start_batch_ = start_ring;  // bo2zm_perf_log: Present counts the batch it is part of
      while (reader.Pending()) {
        if (!Packet(reader, 0)) {
          // Partial packet: the rest will arrive with the next CP_RB_WPTR.
          break;
        }
      }
      {
        const uint64_t ns_ring = NanosecondsSince(start_ring);
        time_ring_ns_ += ns_ring;
        AddOnlyRing(bo2zm::waits::g_ns_ring_working, ns_ring);  // without fetch_add
      }
      read = reader.pos;
      // Deferred vertex copies must be done before telling the game that the GPU has read up to here:
      // after that it may reuse those buffers (bo2zm_native_upload_thread).
      if (targets_) {
        targets_->WaitUploads();
      }
      const uint32_t to_return = read_returned_.load(std::memory_order_acquire);
      if (to_return) {
        rex::memory::store_and_swap<uint32_t>(memory_->TranslatePhysical(to_return), read);
      }
      // After returning it: the game's D3D waits sleep until this notification (bo2zm_waits.cpp).
      WarnProgressRing();
    }
    return 0;
  }

  // --- PM4 packets -----------------------------------------------------------

  // Consumes a whole packet, or leaves the position untouched if it is not complete yet.
  bool Packet(RingReader& reader, int depth) {
    const uint32_t packet = reader.Look();
    uint32_t load = 0;
    switch (packet >> 30) {
      case 0:
        load = packet ? ((packet >> 16) & 0x3FFF) + 1 : 0;
        break;
      case 1:
        load = 2;
        break;
      case 2:
        load = 0;
        break;
      default:
        load = ((packet >> 16) & 0x3FFF) + 1;
        break;
    }
    if (reader.Pending() < load + 1) {
      return false;
    }
    reader.Advance(1);
    AddOnlyRing(packets_);
    RingReader data = reader;
    reader.Advance(load);
    if (!packet) {
      return true;
    }
    // Timer on 1 of every kStopwatchPacketsEvery packets, with the time scaled up: there are ~14,000 per
    // frame, and two clock reads per packet cost more than writing the register. WAIT_REG_MEM, long and
    // infrequent, is always timed.
    const bool time = (++stopwatch_packets_ & (kStopwatchPacketsEvery - 1)) == 0;
    const auto start = time ? Clock::now() : Clock::time_point{};
    switch (packet >> 30) {
      case 0: {
        const uint32_t index = packet & 0x7FFF;
        const bool a_register = (packet >> 15) & 0x1;
        if (a_register || !WriteRegistersInBlock(index, load, data)) {
          for (uint32_t i = 0; i < load; ++i) {
            WriteRegister(a_register ? index : index + i, data.Read());
          }
          words_loose_ += load;  // Measurement only
        } else {
          words_block_ += load;
        }
        if (time) {
          time_registers_ns_ += NanosecondsSince(start) * kStopwatchPacketsEvery;
        }
        ++count_registers_;
        break;
      }
      case 1: {
        const uint32_t value1 = data.Read();
        const uint32_t value2 = data.Read();
        WriteRegister(packet & 0x7FF, value1);
        WriteRegister((packet >> 11) & 0x7FF, value2);
        if (time) {
          time_registers_ns_ += NanosecondsSince(start) * kStopwatchPacketsEvery;
        }
        ++count_registers_;
        break;
      }
      case 2:
        break;
      default: {
        const uint32_t opcode = (packet >> 8) & 0x7F;
        const bool wait = opcode == xenos::PM4_WAIT_REG_MEM;
        const auto start_op = wait && !time ? Clock::now() : start;
        PacketType3(data, packet, load, depth);
        // An indirect buffer is timed through its packets, so they are not counted twice.
        if (opcode != xenos::PM4_INDIRECT_BUFFER && opcode != xenos::PM4_INDIRECT_BUFFER_PFD) {
          if (wait) {
            time_opcode_ns_[opcode] += NanosecondsSince(start_op);
          } else if (time) {
            time_opcode_ns_[opcode] += NanosecondsSince(start) * kStopwatchPacketsEvery;
          }
          ++count_opcode_[opcode];
        }
        break;
      }
    }
    return true;
  }

  // bo2zm_native_ring_spin_ns_ab: busy time on the ring thread, in the A/B arms of factor 1 only.
  void SpinRing() {
    const int arm = bo2zm::benchmark::AbArmCached();
    if (arm < 0 || !(arm & 1)) {
      return;
    }
    const auto end = Clock::now() + std::chrono::nanoseconds(ring_spin_ns_);
    while (Clock::now() < end) {
    }
  }

  void PacketType3(RingReader& data, uint32_t packet, uint32_t words, int depth) {
    const uint32_t opcode = (packet >> 8) & 0x7F;
    AddOnlyRing(opcodes_[opcode]);
    if ((packet & 0x1) && (!(bin_select_ & bin_mask_) || opcode == xenos::PM4_XE_SWAP)) {
      return;  // predicate fails; predicated Swaps are always dropped
    }
    switch (opcode) {
      case xenos::PM4_INTERRUPT: {
        if (words < 1) break;
        BeforeFence();
        const uint32_t cpus = data.Read();
        for (uint32_t cpu = 0; cpu < 6; ++cpu) {
          if (cpus & (1u << cpu)) {
            Interrupt(1, cpu);
          }
        }
        break;
      }
      case xenos::PM4_XE_SWAP: {
        // VdSwap writes "SWAP", the physical address of the frontbuffer and its size.
        if (words >= 4) {
          data.Read();
          swap_frontbuffer_.store(data.Read(), std::memory_order_relaxed);
          swap_width_.store(data.Read(), std::memory_order_relaxed);
          swap_height_.store(data.Read(), std::memory_order_relaxed);
        }
        TraceSwap();
        Present();
        AnnotateGameAhead();  // Measurement only
        break;
      }
      case xenos::PM4_INDIRECT_BUFFER:
      case xenos::PM4_INDIRECT_BUFFER_PFD: {
        if (words < 2) break;
        const uint32_t address = data.Read() & 0x1FFFFFFF;
        const uint32_t length = data.Read() & 0xFFFFF;
        if (depth < kMaxDepthIndirect) {
          BufferIndirect(address, length, depth + 1);
        }
        break;
      }
      case xenos::PM4_WAIT_REG_MEM: {
        if (words < 5) break;
        const uint32_t info = data.Read();
        const uint32_t poll = data.Read();
        const uint32_t reference = data.Read();
        const uint32_t mask = data.Read();
        const auto start_regmem = Clock::now();  // for the "[hitch]" line
        const auto limit = start_regmem + kWaitRegMemMax;
        uint32_t loops = 0;
        for (;;) {
          if (!(info & 0x10) && poll == rex::graphics::XE_GPU_REG_COHER_STATUS_HOST &&
              (DvarRecord(poll) & UINT32_C(0x80000000))) {
            SaveRegister(poll, 0);  // MakeCoherent: there is no shared memory to synchronize
          }
          const uint32_t value = (info & 0x10) ? ReadMemory(poll) : DvarRecord(poll);
          if (Compares(info, value & mask, reference)) {
            break;
          }
          // The real GPU waits; without a GPU some conditions will never be
          // met. The wait is cut short so the ring does not hang.
          if (Clock::now() >= limit || !active_.load(std::memory_order_acquire)) {
            if (waits_exhausted_.fetch_add(1, std::memory_order_relaxed) == 0) {
              REXLOG_WARN("[native] WAIT_REG_MEM not satisfied ({} {:08X} ref {:08X} mask {:08X})",
                          (info & 0x10) ? "memory" : "register", poll, reference, mask);
            }
            break;
          }
          const int32_t pause_us = REXCVAR_GET(bo2zm_native_regmem_wait_us);
          rex::thread::Sleep(std::chrono::microseconds(pause_us > 50 ? pause_us : 50));
          ++loops;
        }
        /*
         * The guest has just waited for the GPU. It is the only point where it may legally rewrite
         * a vertex range it already referenced in this frame, so from here on upload deduplication
         * cannot reuse anything recorded so far. See bo2zm_native_vertex_dedupe.h. Relaxed is
         * enough: the same thread reads it.
         */
        // A cache flush wait is not a point where the CPU can rewrite memory (g_syncs_on_flush_waits).
        if ((info & 0x10) || poll != rex::graphics::XE_GPU_REG_COHER_STATUS_HOST ||
            g_syncs_on_flush_waits.load(std::memory_order_relaxed)) {
          g_syncs_ring.fetch_add(1, std::memory_order_relaxed);
        }
        bo2zm::waits::Add(bo2zm::waits::kRingRegMem, NanosecondsSince(start_regmem));
        const uint64_t key_wait = (uint64_t(info & 0x10) << 32) | poll;
        if (waits_.size() < 64 || waits_.count(key_wait)) {
          Wait& wait = waits_[key_wait];
          ++wait.times;
          wait.loops += loops;
          wait.reference = reference;
          wait.mask = mask;
          wait.info = info;
        }
        break;
      }
      case xenos::PM4_REG_RMW: {
        if (words < 3) break;
        const uint32_t info = data.Read();
        const uint32_t y = data.Read();
        const uint32_t o = data.Read();
        uint32_t value = DvarRecord(info & 0x1FFF);
        value &= ((info >> 31) & 0x1) ? DvarRecord(y & 0x1FFF) : y;
        value |= ((info >> 30) & 0x1) ? DvarRecord(o & 0x1FFF) : o;
        WriteRegister(info & 0x1FFF, value);
        break;
      }
      case xenos::PM4_REG_TO_MEM: {
        if (words < 2) break;
        const uint32_t reg_index = data.Read();
        const uint32_t address = data.Read();
        BeforeFence();
        WriteMemory(address, DvarRecord(reg_index));
        break;
      }
      case xenos::PM4_MEM_WRITE: {
        if (words < 1) break;
        BeforeFence();
        uint32_t address = data.Read();
        for (uint32_t i = 1; i < words; ++i) {
          WriteMemory(address, data.Read());
          address += 4;
        }
        break;
      }
      case xenos::PM4_COND_WRITE: {
        if (words < 6) break;
        const uint32_t info = data.Read();
        const uint32_t poll = data.Read();
        const uint32_t reference = data.Read();
        const uint32_t mask = data.Read();
        const uint32_t target = data.Read();
        const uint32_t datum = data.Read();
        const uint32_t value = (info & 0x10) ? ReadMemory(poll) : DvarRecord(poll);
        if (Compares(info, value & mask, reference)) {
          if (info & 0x100) {
            BeforeFence();
            WriteMemory(target, datum);
          } else {
            WriteRegister(target, datum);
          }
        }
        break;
      }
      case xenos::PM4_EVENT_WRITE:
        if (words < 1) break;
        WriteRegister(rex::graphics::XE_GPU_REG_VGT_EVENT_INITIATOR, data.Read() & 0x3F);
        break;
      case xenos::PM4_EVENT_WRITE_SHD: {
        if (words < 3) break;
        BeforeFence();
        const uint32_t initiator = data.Read();
        const uint32_t address = data.Read();
        const uint32_t value = data.Read();
        WriteRegister(rex::graphics::XE_GPU_REG_VGT_EVENT_INITIATOR, initiator & 0x3F);
        WriteMemory(address, ((initiator >> 31) & 0x1)
                                       ? counter_.load(std::memory_order_relaxed)
                                       : value);
        break;
      }
      case xenos::PM4_EVENT_WRITE_EXT: {
        if (words < 2) break;
        const uint32_t initiator = data.Read();
        const uint32_t address = data.Read();
        WriteRegister(rex::graphics::XE_GPU_REG_VGT_EVENT_INITIATOR, initiator & 0x3F);
        // Screen extent of the previous draw: the maximum is faked.
        const uint16_t extension[] = {0, kExtensionMax, 0, kExtensionMax, 0, 1};
        uint8_t* target = memory_->TranslatePhysical(address & ~uint32_t(0x3));
        for (size_t i = 0; i < std::size(extension); ++i) {
          rex::memory::store_and_swap<uint16_t>(target + i * 2, extension[i]);
        }
        break;
      }
      case xenos::PM4_EVENT_WRITE_ZPD: {
        if (words < 1) break;
        WriteRegister(rex::graphics::XE_GPU_REG_VGT_EVENT_INITIATOR, data.Read() & 0x3F);
        const uint32_t address = DvarRecord(rex::graphics::XE_GPU_REG_RB_SAMPLE_COUNT_ADDR);
        if (!address) break;
        QueryOcclusion(address);
        break;
      }
      case xenos::PM4_SET_CONSTANT: {
        if (words < 1) break;
        const uint32_t type_index = data.Read();
        uint32_t base = UINT32_MAX;
        switch ((type_index >> 16) & 0xFF) {
          case 0: base = 0x4000; break;  // ALU
          case 1: base = 0x4800; break;  // fetch
          case 2: base = 0x4900; break;  // bool
          case 3: base = 0x4908; break;  // loop
          case 4: base = 0x2000; break;  // registers
          default: break;
        }
        if (base != UINT32_MAX) {
          const uint32_t index = base + (type_index & 0x7FF);
          for (uint32_t i = 1; i < words; ++i) {
            WriteRegister(index + i - 1, data.Read());
          }
        }
        break;
      }
      case xenos::PM4_SET_BIN_MASK_LO:
        if (words < 1) break;
        bin_mask_ = (bin_mask_ & 0xFFFFFFFF00000000ull) | data.Read();
        break;
      case xenos::PM4_SET_BIN_MASK_HI:
        if (words < 1) break;
        bin_mask_ = (bin_mask_ & 0xFFFFFFFFull) | (uint64_t(data.Read()) << 32);
        break;
      case xenos::PM4_SET_BIN_SELECT_LO:
        if (words < 1) break;
        bin_select_ = (bin_select_ & 0xFFFFFFFF00000000ull) | data.Read();
        break;
      case xenos::PM4_SET_BIN_SELECT_HI:
        if (words < 1) break;
        bin_select_ = (bin_select_ & 0xFFFFFFFFull) | (uint64_t(data.Read()) << 32);
        break;
      case xenos::PM4_SET_BIN_MASK: {
        if (words < 2) break;
        const uint64_t height = data.Read();
        const uint64_t low = data.Read();
        bin_mask_ = (height << 32) | low;
        break;
      }
      case xenos::PM4_SET_BIN_SELECT: {
        if (words < 2) break;
        const uint64_t height = data.Read();
        const uint64_t low = data.Read();
        bin_select_ = (height << 32) | low;
        break;
      }
      case xenos::PM4_SET_CONSTANT2:
      case xenos::PM4_SET_SHADER_CONSTANTS: {
        // Same format: first value = register index, the rest is written consecutively.
        if (words < 1) break;
        const uint32_t index = data.Read() & 0xFFFF;
        for (uint32_t i = 1; i < words; ++i) {
          WriteRegister(index + i - 1, data.Read());
        }
        break;
      }
      case xenos::PM4_LOAD_ALU_CONSTANT: {
        // Registers loaded from guest memory instead of from the ring.
        if (words < 3) break;
        const uint32_t address = data.Read() & 0x3FFFFFFF;
        const uint32_t type_index = data.Read();
        const uint32_t amount = data.Read() & 0xFFF;
        uint32_t base = UINT32_MAX;
        switch ((type_index >> 16) & 0xFF) {
          case 0: base = 0x4000; break;  // ALU
          case 1: base = 0x4800; break;  // fetch
          case 2: base = 0x4900; break;  // bool
          case 3: base = 0x4908; break;  // loop
          case 4: base = 0x2000; break;  // registers
          default: break;
        }
        if (base != UINT32_MAX) {
          const uint8_t* source = memory_->TranslatePhysical(address);
          const uint32_t index = base + (type_index & 0x7FF);
          for (uint32_t i = 0; i < amount; ++i) {
            WriteRegister(index + i, rex::memory::load_and_swap<uint32_t>(source + size_t(i) * 4));
          }
        }
        break;
      }
      case xenos::PM4_VIZ_QUERY: {
        // Like the emulated path: when the query ends it is reported as visible,
        // in case the game reads the result.
        if (words < 1) break;
        const uint32_t datum = data.Read();
        const uint32_t id = datum & 0x3F;
        if (!(datum & 0x100)) {
          WriteRegister(rex::graphics::XE_GPU_REG_VGT_EVENT_INITIATOR, xenos::VIZQUERY_START);
        } else {
          WriteRegister(rex::graphics::XE_GPU_REG_VGT_EVENT_INITIATOR, xenos::VIZQUERY_END);
          const uint32_t reg_index = id < 32 ? uint32_t(rex::graphics::XE_GPU_REG_PA_SC_VIZ_QUERY_STATUS_0)
                                            : uint32_t(rex::graphics::XE_GPU_REG_PA_SC_VIZ_QUERY_STATUS_1);
          const uint32_t bit = UINT32_C(1) << (id < 32 ? id : id - 32);
          SaveRegister(reg_index, DvarRecord(reg_index) | bit);
        }
        break;
      }
      case xenos::PM4_DRAW_INDX:
      case xenos::PM4_DRAW_INDX_2: {
        if (ring_spin_ns_ > 0) [[unlikely]] {
          SpinRing();
        }
        // DRAW_INDX is preceded by the visibility query word.
        const uint32_t skip = opcode == xenos::PM4_DRAW_INDX ? 1 : 0;
        if (words < skip + 1) break;
        if (skip) {
          data.Read();
        }
        const uint32_t initiator = data.Read();
        WriteRegister(rex::graphics::XE_GPU_REG_VGT_DRAW_INITIATOR, initiator);
        // Indices in a buffer (DMA): the base and size come in the packet and are
        // stored in their registers, like ExecutePacketType3Draw
        // (graphics/command_processor.cpp:1379-1390). Without this, indexed draws
        // read VGT_DMA_SIZE as zero.
        if (((initiator >> 6) & 0x3) == uint32_t(xenos::SourceSelect::kDMA)) {
          if (words < skip + 3) break;
          WriteRegister(rex::graphics::XE_GPU_REG_VGT_DMA_BASE, data.Read());
          WriteRegister(rex::graphics::XE_GPU_REG_VGT_DMA_SIZE, data.Read());
        }
        AddOnlyRing(draws_);
        // No fetch_add, which on the A57 is an ldxr/stxr loop on every draw. Only this thread writes
        // g_draws, and the "[hitch]" line reads it from this same thread (Present): AddOnlyRing is
        // enough.
        AddOnlyRing(bo2zm::waits::g_draws);
        // A draw with RB_MODECONTROL in copy mode is a resolve (and/or a
        // clear): the emulated path treats it that way (vulkan/command_processor.cpp:3653).
        if ((DvarRecord(rex::graphics::XE_GPU_REG_RB_MODECONTROL) & 0x7) ==
            uint32_t(xenos::EdramMode::kCopy)) {
          AddOnlyRing(copies_);
          last_copy_target_.store(DvarRecord(rex::graphics::XE_GPU_REG_RB_COPY_DEST_BASE),
                                      std::memory_order_relaxed);
          last_copy_control_.store(DvarRecord(rex::graphics::XE_GPU_REG_RB_COPY_CONTROL),
                                      std::memory_order_relaxed);
          last_copy_info_.store(DvarRecord(rex::graphics::XE_GPU_REG_RB_COPY_DEST_INFO),
                                   std::memory_order_relaxed);
          last_copy_pitch_.store(DvarRecord(rex::graphics::XE_GPU_REG_RB_COPY_DEST_PITCH),
                                    std::memory_order_relaxed);
          bool skip_copy = MenuBlurSkipCopy();
          if (!skip_copy) {
            skip_copy = SceneReuseCopy();
          }
          if (!skip_copy) {
            DoCopy();
          }
        } else {
          CountDrawShaders();
          PairDraw();
          TraceDraw();
          if (SceneReuseDraw() || MenuBlurSkipDraw() || SkipPrepassDraw()) {
            break;
          }
          // In mode 5 (depth only) the VS is enough: Xenos does not run the PS, and D3D leaves
          // the PS object at 0 in some of the shadow draws.
          if (vs_draw_ &&
              (ps_draw_ || (DvarRecord(rex::graphics::XE_GPU_REG_RB_MODECONTROL) & 0x7) == 5)) {
            DrawNative();
          } else if (!ClearD3D() && !ExecuteMemexportBo2zm()) {  // BO2's clears and memory-writing shaders
            DiagDrawNoShader();
          }
        }
        break;
      }
      case xenos::PM4_IM_LOAD: {
        // Microcode in GPU memory; the type is in the two low bits.
        if (words < 2) break;
        const uint32_t address_type = data.Read();
        const uint32_t size = data.Read() & 0xFFFF;
        const uint8_t* source = memory_->TranslatePhysical(address_type & ~uint32_t(0x3));
        if ((address_type & 0x3) == 1) ps_microcode_bo2zm_ = size;  // diagnostics
        if (size && (address_type & 0x3) <= 1) {
          LoadShaderCached(address_type, size, source);
          break;
        }
        microcode_.resize(size);
        for (uint32_t i = 0; i < size; ++i) {
          microcode_[i] = rex::memory::load_and_swap<uint32_t>(source + size_t(i) * 4);
        }
        IdentifyShader(address_type & 0x3);
        break;
      }
      case xenos::PM4_IM_LOAD_IMMEDIATE: {
        // Microcode inside the packet itself.
        if (words < 2) break;
        const uint32_t type = data.Read();
        const uint32_t size = data.Read() & 0xFFFF;
        if (size > words - 2) break;
        if ((type & 0x3) == 1) {  // D3D's clear pixel shader, for ClearD3D's log
          ps_microcode_bo2zm_ = size;
          if (size <= ps_immediate_bo2zm_.size()) {
            RingReader copy = data;
            for (uint32_t i = 0; i < size; ++i) {
              ps_immediate_bo2zm_[i] = copy.Read();
            }
          }
        }
        LoadImmediate(data, type, size);  // with an exact cache (bo2zm_native_im_immediate_cache)
        break;
      }
      case xenos::PM4_NOP:
        // The FlushState composite marker (ProcessMarker).
        // A NOP without its magic belongs to the game itself and does nothing, as before.
        ProcessMarker(data, words);
        break;
      default:
        // Invalidations and the like: nothing is drawn yet.
        break;
    }
  }

  // Diagnostics (bo2zm_native_log_vs_misses): draws dropped for lack of shader identity, with the
  // state needed to tell what they do. BO2 clears its depth with D3D's own Clear quads, whose inline shaders are
  // not in the library.
  void DiagDrawNoShader() {
    if (!bo2zm::native::LogVertexShaderMisses() || diag_no_shader_ >= 3000) {
      return;
    }
    namespace g = rex::graphics;
    // Skip draws that cannot change anything: no color writes, no depth write, no stencil.
    const uint32_t control_z = DvarRecord(g::XE_GPU_REG_RB_DEPTHCONTROL);
    if (DvarRecord(g::XE_GPU_REG_RB_COLOR_MASK) == 0 && !(control_z & 0x4) && !(control_z & 0x1)) {
      return;
    }
    {
      const uint64_t key = (uint64_t(DvarRecord(g::XE_GPU_REG_RB_SURFACE_INFO)) << 32) ^
                             (uint64_t(control_z) << 8) ^ DvarRecord(g::XE_GPU_REG_RB_COLOR_MASK) ^
                             (uint64_t(DvarRecord(g::XE_GPU_REG_VGT_DRAW_INITIATOR) & 0xFF) << 56);
      if (!diag_no_shader_seen_.insert(key).second) {
        return;
      }
    }
    ++diag_no_shader_;
    const auto float_value = [](uint32_t bits) {
      float f;
      std::memcpy(&f, &bits, sizeof(f));
      return f;
    };
    std::string constants;
    for (uint32_t k = 0; k < 4; ++k) {
      constants += fmt::format(" vc{}=({:.4g},{:.4g},{:.4g},{:.4g})", k, float_value(DvarRecord(0x4000 + k * 4)),
                                float_value(DvarRecord(0x4001 + k * 4)), float_value(DvarRecord(0x4002 + k * 4)),
                                float_value(DvarRecord(0x4003 + k * 4)));
    }
    for (uint32_t k = 0; k < 2; ++k) {
      constants += fmt::format(" pc{}=({:.4g},{:.4g},{:.4g},{:.4g})", k, float_value(DvarRecord(0x4400 + k * 4)),
                                float_value(DvarRecord(0x4401 + k * 4)), float_value(DvarRecord(0x4402 + k * 4)),
                                float_value(DvarRecord(0x4403 + k * 4)));
    }
    const uint32_t f0 = DvarRecord(0x4800), f1 = DvarRecord(0x4801);
    std::string vertices;
    if ((f0 & 0x3) == 3) {  // vertex fetch constant in slot 0
      const uint8_t* data = memory_->TranslatePhysical(f0 & 0x1FFFFFFC);
      for (uint32_t i = 0; i < 12; ++i) {
        vertices += fmt::format(" {:.4g}", float_value(rex::memory::load_and_swap<uint32_t>(data + i * 4)));
      }
    }
    REXLOG_INFO("[native] BO2 draw without a shader: vs {} ({} words) ps {} ({} words) start {:08X} mode {} z {:08X} "
                "mask {:08X} color {:08X} depth {:08X} surface {:08X} vte {:08X} stencil {:08X} clear z {:08X} color "
                "{:08X} fetch0 {:08X} {:08X};{} | vertices:{}",
                vs_draw_ ? int(vs_draw_->number) : -1, vs_microcode_.size(),
                ps_draw_ ? int(ps_draw_->number) : -1, ps_microcode_bo2zm_,
                DvarRecord(g::XE_GPU_REG_VGT_DRAW_INITIATOR), DvarRecord(g::XE_GPU_REG_RB_MODECONTROL) & 0x7,
                DvarRecord(g::XE_GPU_REG_RB_DEPTHCONTROL), DvarRecord(g::XE_GPU_REG_RB_COLOR_MASK),
                DvarRecord(g::XE_GPU_REG_RB_COLOR_INFO), DvarRecord(g::XE_GPU_REG_RB_DEPTH_INFO),
                DvarRecord(g::XE_GPU_REG_RB_SURFACE_INFO), DvarRecord(g::XE_GPU_REG_PA_CL_VTE_CNTL),
                DvarRecord(g::XE_GPU_REG_RB_STENCILREFMASK), DvarRecord(g::XE_GPU_REG_RB_DEPTH_CLEAR),
                DvarRecord(g::XE_GPU_REG_RB_COLOR_CLEAR), f0, f1, constants, vertices);
  }
  uint32_t diag_no_shader_ = 0;
  std::set<uint64_t> diag_no_shader_seen_;  // one line per kind of draw

  // Diagnostics (bo2zm_native_log_vs_misses): dropped draws whose vertex shader writes to memory
  // (memexport: a control flow ALLOC of type 3, kMemory in Xenia's ucode.h). Xenos/Xenia run these; a draw
  // that only writes memory has no color or depth writes, so DiagDrawNoShader skips it. One line per vertex
  // shader (by its hash) and a running count.
  bool VsExportsMemory() const {
    bool exports = false;
    for (size_t i = 0; i + 2 < vs_microcode_.size() && i < 3 * 64 && !exports; i += 3) {
      const uint32_t w0 = vs_microcode_[i], w1 = vs_microcode_[i + 1], w2 = vs_microcode_[i + 2];
      (void)w0;
      const uint32_t high[2] = {w1 & 0xFFFF, w2 >> 16};
      bool end = false;
      for (uint32_t height : high) {
        const uint32_t opcode = (height >> 12) & 0xF;
        if (opcode == 12 && ((height >> 9) & 0x3) == 3) {
          exports = true;
        }
        end |= opcode == 2 || opcode == 4;  // exec_end / cond_exec_end
      }
      if (end) {
        break;
      }
    }
    return exports;
  }

  // Draws whose vertex shader writes guest memory (memexport). BO2 copies streamed texture data this way: two vertex
  // shaders that live in the game code (not in the fastfiles, so not in the library) read 8-byte blocks through vertex
  // fetch constant 0 as 16_16_16_16 integers and write them through eA/eM0-3 to the stream in c0 (in a Xenia shader
  // dump they are A4827826E2589916 and 5B9B7484417FB9B6).
  // Under Xenos these run as GPU point-list draws. The native renderer dropped them, so the mip levels of
  // streamed textures kept another texture's bytes (noisy lava, garbage Pack-a-Punch and fence, green splotches).
  // They run here on the CPU, in ring order, with the SDK's ShaderInterpreter: one invocation per vertex, its
  // exports written by bo2zm::memexport::Sink. The texture cache sees the new bytes at its next content check.
  // Returns false if the vertex shader does not write memory (the draw is still dropped).
  bool ExecuteMemexportBo2zm() {
    if (vs_microcode_.empty() || !VsExportsMemory()) {
      return false;
    }
    DiagMemexportBo2zm();
    static const bool active = REXCVAR_GET(bo2zm_native_memexport);
    if (!active) {
      return true;
    }
    namespace g = rex::graphics;
    // The two copy shaders (see CopyMemexportBo2zm) skip the interpreter.
    static const bool direct_active = REXCVAR_GET(bo2zm_native_memexport_fast);
    static const bool verify = REXCVAR_GET(bo2zm_native_memexport_verify);
    const uint64_t hash = XXH3_64bits(vs_microcode_.data(), vs_microcode_.size() * sizeof(uint32_t));
    const uint32_t copy = !direct_active                     ? 0
                           : hash == kMemexportCopy64Bytes  ? 1
                           : hash == kMemexportCopy128Bytes ? 2
                                                               : 0;
    const auto start = std::chrono::steady_clock::now();
    if (!copy || verify) {
      if (!interpreter_bo2zm_) {
        registers_interpreter_bo2zm_ = std::make_unique<g::RegisterFile>();
        interpreter_bo2zm_ = std::make_unique<g::ShaderInterpreter>(*registers_interpreter_bo2zm_, *memory_);
      }
      std::memcpy(registers_interpreter_bo2zm_->values, registers_.data(),
                  std::min(registers_.size(), g::RegisterFile::kRegisterCount) * sizeof(uint32_t));
      interpreter_bo2zm_->SetShader(g::xenos::ShaderType::kVertex, vs_microcode_.data());
    }
    bo2zm::memexport::Sink sink(memory_->TranslatePhysical(0));
    sink.tracing = memexport_done_ < 2;  // the "memexport trace" line below
    const uint32_t initiator = DvarRecord(g::XE_GPU_REG_VGT_DRAW_INITIATOR);
    const uint32_t num_indices = initiator >> 16;
    const uint32_t source_kind = (initiator >> 6) & 0x3;  // 0 = DMA, 1 = immediate, 2 = auto
    const uint32_t displacement = DvarRecord(g::XE_GPU_REG_VGT_INDX_OFFSET) & 0xFFFFFF;
    const uint32_t min = DvarRecord(g::XE_GPU_REG_VGT_MIN_VTX_INDX) & 0xFFFFFF;
    const uint32_t max = DvarRecord(g::XE_GPU_REG_VGT_MAX_VTX_INDX) & 0xFFFFFF;
    // Indices from memory, as DrawExtentEstimator reads them (util/draw_extent_estimator.cpp).
    const uint32_t sz_dma = DvarRecord(g::XE_GPU_REG_VGT_DMA_SIZE);
    const bool indices32 = (initiator >> 11) & 0x1;
    auto endian_indices = g::xenos::Endian(sz_dma >> 30);
    if (!indices32) {
      endian_indices = endian_indices == g::xenos::Endian::k8in32    ? g::xenos::Endian::k8in16
                       : endian_indices == g::xenos::Endian::k16in32 ? g::xenos::Endian::kNone
                                                                      : endian_indices;
    }
    const uint32_t base_dma = DvarRecord(g::XE_GPU_REG_VGT_DMA_BASE) & ~uint32_t(indices32 ? 3 : 1);
    const uint8_t* indices = source_kind == 0 ? memory_->TranslatePhysical(base_dma) : nullptr;
    // The vertex index of draw vertex i, or false for index sources not handled here.
    const auto index_of = [&](uint32_t i, uint32_t& index) {
      index = i;
      if (source_kind == 0) {
        if (i >= (sz_dma & 0xFFFFFF)) {
          index = 0;
        } else if (indices32) {
          uint32_t v;
          std::memcpy(&v, indices + size_t(i) * 4, 4);
          index = g::xenos::GpuSwap(v, endian_indices) & 0xFFFFFF;
        } else {
          uint16_t v;
          std::memcpy(&v, indices + size_t(i) * 2, 2);
          index = g::xenos::GpuSwap(uint32_t(v), endian_indices) & 0xFFFF;
        }
      } else if (source_kind != 2) {
        return false;  // immediate indices: not seen in BO2
      }
      index = std::min(max, std::max(min, (index + displacement) & 0xFFFFFF));
      return true;
    };
    const auto interpret = [&](bo2zm::memexport::Sink& target) {
      interpreter_bo2zm_->SetExportSink(&target);
      uint32_t index;
      for (uint32_t i = 0; i < num_indices && index_of(i, index); ++i) {
        interpreter_bo2zm_->temp_registers()[0] = float(index);
        interpreter_bo2zm_->Execute();
        target.End();
      }
      interpreter_bo2zm_->SetExportSink(nullptr);
    };
    if (!copy) {
      interpret(sink);
    } else {
      uint32_t index;
      for (uint32_t i = 0; i < num_indices && index_of(i, index); ++i) {
        CopyMemexportBo2zm(copy == 2, index, sink);
      }
      ++memexport_direct_;
      if (verify) {
        VerifyMemexportBo2zm(sink, interpret);
      }
    }
    ++memexport_done_;
    // Memory was written in mid-frame: what the upload dedupe recorded may no longer match.
    g_syncs_ring.fetch_add(1, std::memory_order_relaxed);
    // And stamped for the vertex reuse (bo2zm_page_writes.h).
    if (sink.dir_min < sink.dir_max && bo2zm::page_writes::g_enabled.load(std::memory_order_relaxed)) {
      bo2zm::page_writes::MarkPhysical(sink.dir_min, sink.dir_max - sink.dir_min);
    }
    memexport_elements_ += sink.elements;
    memexport_ns_ += uint64_t(
        std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - start).count());
    if (memexport_done_ <= 2) {
      REXLOG_INFO("[native] BO2 memexport trace: c255 {:08X} {:08X} {:08X} {:08X} c254 {:08X} {:08X};{}",
                  DvarRecord(0x4000 + 255 * 4), DvarRecord(0x4001 + 255 * 4), DvarRecord(0x4002 + 255 * 4),
                  DvarRecord(0x4003 + 255 * 4), DvarRecord(0x4000 + 254 * 4), DvarRecord(0x4001 + 254 * 4),
                  sink.trace);
    }
    if (memexport_done_ <= 3 || memexport_done_ % 2000 == 0 || sink.rejected || sink.format_odd) {
      REXLOG_INFO("[native] BO2 memexport done on the CPU: draw {} ({} vertices, source {}): {} elements at "
                  "{:08X}-{:08X}, {} outside the count, {} eA rejected, format not handled {}; in total {} elements, "
                  "{:.1f} ms ({} without the interpreter)",
                  memexport_done_, num_indices, source_kind, sink.elements,
                  sink.dir_min == UINT32_MAX ? 0 : sink.dir_min, sink.dir_max, sink.outside,
                  sink.rejected, sink.format_odd, memexport_elements_, double(memexport_ns_) / 1e6,
                  memexport_direct_);
    }
    return true;
  }
  std::unique_ptr<rex::graphics::RegisterFile> registers_interpreter_bo2zm_;
  std::unique_ptr<rex::graphics::ShaderInterpreter> interpreter_bo2zm_;
  uint64_t memexport_done_ = 0;
  uint64_t memexport_direct_ = 0;
  uint64_t memexport_verified_ = 0;
  uint64_t memexport_different_ = 0;

  // BO2's two memexport copy shaders, by the XXH3 of their microcode words (the hash DiagMemexportBo2zm logs; a Xenia
  // shader dump names them A4827826E2589916 and 5B9B7484417FB9B6). Through the interpreter they took about 0.66 us per
  // 8-byte element on the Switch: 11 s of the renderer thread over 3.5 minutes in Town, in bursts while textures
  // stream.
  static constexpr uint64_t kMemexportCopy64Bytes = 0x682419C7A651D9A2ull;   // 81 words: 64 bytes per vertex
  static constexpr uint64_t kMemexportCopy128Bytes = 0x77B7DDF40EBDA6CBull;  // 150 words: 128 bytes per vertex

  // One vertex of those shaders, without the interpreter. Each reads 8-byte blocks through vertex fetch constant 95
  // (vfetch 16_16_16_16 unsigned integer, destination swizzle yxwz, stride 16 or 64 dwords) and exports them in
  // groups of four (eM0-3) to element indices made from c254/c255, with eA = index * c255.xyxx + c0:
  //   64 bytes:  r = c255.w * vertex; groups at r and c255.z + r (blocks 0-3 and 4-7)
  //   128 bytes: r = vertex * c254.x; groups at r, c255.z + r, c255.w + r and c254.y + r (blocks 0-15)
  // The values go through the same Sink as the interpreter's exports, so packing, endian and the count limit
  // are unchanged. bo2zm_native_memexport_verify compares the bytes against the interpreter.
  void CopyMemexportBo2zm(bool of_128, uint32_t index, bo2zm::memexport::Sink& sink) {
    namespace g = rex::graphics;
    // Vertex fetch constant 95 (const_index 31, select 2 in both shaders' vfetch words; Xenia's disassembly prints
    // it as vf0), where D3D binds stream 0.
    g::xenos::xe_gpu_vertex_fetch_t vf;
    vf.dword_0 = DvarRecord(g::XE_GPU_REG_SHADER_CONSTANT_FETCH_00_0 + 2 * 95);
    vf.dword_1 = DvarRecord(g::XE_GPU_REG_SHADER_CONSTANT_FETCH_00_0 + 2 * 95 + 1);
    const auto constant = [this](uint32_t c, uint32_t k) {
      const uint32_t bits = DvarRecord(0x4000 + c * 4 + k);
      float f;
      std::memcpy(&f, &bits, sizeof(f));
      return f;
    };
    const uint32_t* guest_memory = reinterpret_cast<const uint32_t*>(memory_->physical_membase());
    const uint32_t blocks = of_128 ? 16 : 8;
    const uint32_t first = (of_128 ? 64 : 16) * index + vf.address;
    const uint32_t end = vf.address + vf.size;
    uint16_t r[16][4];
    for (uint32_t k = 0; k < blocks; ++k) {
      uint32_t d[2];
      for (uint32_t j = 0; j < 2; ++j) {
        const uint32_t a = first + 2 * k + j;
        d[j] = a >= vf.address && a < end ? g::xenos::GpuSwap(guest_memory[a], vf.endian) : 0;
      }
      r[k][0] = uint16_t(d[0] >> 16);  // .y
      r[k][1] = uint16_t(d[0]);        // .x
      r[k][2] = uint16_t(d[1] >> 16);  // .w
      r[k][3] = uint16_t(d[1]);        // .z
    }
    const float vertex = float(index);
    float groups[4];
    uint32_t n = 2;
    if (of_128) {
      const float base = vertex * constant(254, 0);
      groups[0] = base;
      groups[1] = constant(255, 2) + base;
      groups[2] = constant(255, 3) + base;
      groups[3] = constant(254, 1) + base;
      n = 4;
    } else {
      groups[0] = constant(255, 3) * vertex;
      groups[1] = constant(255, 2) + groups[0];
    }
    const float scale[4] = {constant(255, 0), constant(255, 1), constant(255, 0), constant(255, 0)};
    const float c0[4] = {constant(0, 0), constant(0, 1), constant(0, 2), constant(0, 3)};
    for (uint32_t group = 0; group < n; ++group) {
      sink.AllocExport(g::ucode::AllocType::kMemory, 2);
      float ea[4];
      for (uint32_t i = 0; i < 4; ++i) {
        ea[i] = groups[group] * scale[i] + c0[i];
      }
      uint32_t a[4];
      std::memcpy(a, ea, sizeof(a));
      if (sink.Integers16(a, &r[group * 4])) {
        continue;
      }
      sink.Export(g::ucode::ExportRegister(32), ea, 0xF);
      for (uint32_t e = 0; e < 4; ++e) {
        const uint16_t* b = r[group * 4 + e];
        const float values[4] = {float(b[0]), float(b[1]), float(b[2]), float(b[3])};
        sink.Export(g::ucode::ExportRegister(33 + e), values, 0xF);
      }
    }
    sink.End();
  }

  // bo2zm_native_memexport_verify: runs the interpreter over the same draw and compares what both wrote.
  template <typename Interpret>
  void VerifyMemexportBo2zm(const bo2zm::memexport::Sink& direct, Interpret&& interpret) {
    const uint8_t* physical = memory_->TranslatePhysical(0);
    std::vector<uint8_t> copy;
    if (direct.dir_min < direct.dir_max) {
      copy.assign(physical + direct.dir_min, physical + direct.dir_max);
    }
    bo2zm::memexport::Sink reference(memory_->TranslatePhysical(0));
    interpret(reference);
    ++memexport_verified_;
    const bool same_range = reference.dir_min == direct.dir_min && reference.dir_max == direct.dir_max &&
                             reference.elements == direct.elements && reference.outside == direct.outside;
    const bool same_bytes = copy.empty() || std::memcmp(copy.data(), physical + direct.dir_min, copy.size()) == 0;
    if ((!same_range || !same_bytes) && ++memexport_different_ <= 8) {
      REXLOG_ERROR("[native] BO2 direct memexport: MISMATCH with the interpreter in draw {}: range {:08X}-{:08X} and {} "
                   "elements ({} outside) versus {:08X}-{:08X} and {} ({} outside); bytes {}",
                   memexport_done_ + 1, direct.dir_min, direct.dir_max, direct.elements, direct.outside,
                   reference.dir_min, reference.dir_max, reference.elements, reference.outside,
                   same_bytes ? "equal" : "different");
    }
    if (memexport_verified_ % 500 == 0) {
      REXLOG_INFO("[native] BO2 direct memexport: {} draws compared with the interpreter, {} with differences",
                  memexport_verified_, memexport_different_);
    }
  }
  uint64_t memexport_elements_ = 0;
  uint64_t memexport_ns_ = 0;

  // One log line per memexport vertex shader (bo2zm_native_log_vs_misses), and a running count.
  void DiagMemexportBo2zm() {
    if (!bo2zm::native::LogVertexShaderMisses()) {
      return;
    }
    ++memexport_draws_;
    const uint64_t hash = XXH3_64bits(vs_microcode_.data(), vs_microcode_.size() * sizeof(uint32_t));
    if (!memexport_seen_.insert(hash).second || memexport_seen_.size() > 64) {
      if (memexport_draws_ % 2000 == 0) {
        REXLOG_INFO("[native] BO2 memexport: {} draws not handled ({} different VS)", memexport_draws_,
                    memexport_seen_.size());
      }
      return;
    }
    namespace g = rex::graphics;
    const auto float_value = [](uint32_t bits) {
      float f;
      std::memcpy(&f, &bits, sizeof(f));
      return f;
    };
    std::string constants;
    for (uint32_t k = 0; k < 4; ++k) {
      constants += fmt::format(" c{}=({:08X} {:08X} {:08X} {:08X} | {:.4g})", k, DvarRecord(0x4000 + k * 4),
                                DvarRecord(0x4001 + k * 4), DvarRecord(0x4002 + k * 4), DvarRecord(0x4003 + k * 4),
                                float_value(DvarRecord(0x4000 + k * 4)));
    }
    REXLOG_INFO("[native] BO2 memexport: VS {:016X} ({} words, in the library: {}) ps {} start {:08X} indices {} mode "
                "{} mask {:08X} fetch0 {:08X} {:08X};{}",
                hash, vs_microcode_.size(), vs_draw_ ? "si" : "no", ps_draw_ ? int(ps_draw_->number) : -1,
                DvarRecord(g::XE_GPU_REG_VGT_DRAW_INITIATOR), DvarRecord(g::XE_GPU_REG_VGT_DRAW_INITIATOR) >> 16,
                DvarRecord(g::XE_GPU_REG_RB_MODECONTROL) & 0x7, DvarRecord(g::XE_GPU_REG_RB_COLOR_MASK),
                DvarRecord(0x4800), DvarRecord(0x4801), constants);
  }
  uint64_t memexport_draws_ = 0;
  std::set<uint64_t> memexport_seen_;
  size_t ps_microcode_bo2zm_ = 0;  // words of the last pixel shader load (diagnostics)
  std::array<uint32_t, 16> ps_immediate_bo2zm_{};  // and its words, if it came inline and is that short

  // BO2's Direct3D Clear() is not a copy-mode clear. D3D draws a screen-space rectangle (RECTLIST, 3 vertices, no
  // viewport transform) with its own shaders, loaded with IM_LOAD_IMMEDIATE (a 27-word VS and a 9-word PS) and absent
  // from the shader library, so DrawNative cannot draw it. Left alone, the scene depth is never cleared: a RenderDoc
  // capture showed it stuck at its initial 1.0, where every GEQUAL test of the reversed-Z world fails, and the world
  // came out black. This turns the rectangle into a clear of the render targets it writes (ClearAsDraw in
  // bo2zm_native_targets.cpp). Returns false if the draw is not such a clear.
  //
  // What is written, from the draw state:
  //   depth    RB_DEPTHCONTROL z enable + z write with func ALWAYS; the value is the vertices' z (no viewport
  //            transform: PA_CL_VTE_CNTL 0x300 takes x, y and z as they come).
  //   stencil  stencil enable, func ALWAYS, zpass REPLACE, write mask FF; the value is the reference.
  //   color    only with RB_MODECONTROL 4 (color + depth) and RT0's mask; the value is the vertices' color.
  //            D3D's 27-word VS fetches each vertex as position (x, y, z) followed by an RGBA color (four
  //            floats at offset 3) and passes the color through interpolator 0; the 9-word PS has a single ALU
  //            instruction that exports that register (r0) to oC0. It reads no constant: PS c0 holds whatever
  //            the game last put there (the render target size, 880, 720, 1/880, 1/720, which clamps to
  //            yellow and showed as a yellow screen whenever nothing was drawn over the clear).
  // D3D often clears through an alias: the same EDRAM base with another pitch and MSAA mode (the 880-wide 2X
  // scene is cleared as a 440-wide 4X surface; the 1040-wide 1X shadow map as 520-wide 4X). The rectangle is
  // moved to the surface last drawn at that base when both have the same samples per row.
  bool ClearD3D() {
    namespace g = rex::graphics;
    if (!bo2zm::native::ClearQuads()) {
      return false;
    }
    const uint32_t initiator = DvarRecord(g::XE_GPU_REG_VGT_DRAW_INITIATOR);
    if ((initiator & 0x3F) != uint32_t(xenos::PrimitiveType::kRectangleList) ||
        ((initiator >> 6) & 0x3) != uint32_t(xenos::SourceSelect::kAutoIndex) || (initiator >> 16) != 3 ||
        DvarRecord(g::XE_GPU_REG_PA_CL_VTE_CNTL) != 0x300 || vs_microcode_.size() != 27 ||
        ps_microcode_bo2zm_ != 9) {
      return false;
    }
    if (!EnsureTargets()) {
      return true;
    }
    const auto no_is_clear = [this](const char* reason) {
      if (clears_d3d_rejected_++ < 16) {
        REXLOG_WARN("[native] BO2 D3D clear not applied ({}): z {:08X} stencil {:08X} mask {:08X} mode {}",
                    reason, DvarRecord(rex::graphics::XE_GPU_REG_RB_DEPTHCONTROL),
                    DvarRecord(rex::graphics::XE_GPU_REG_RB_STENCILREFMASK),
                    DvarRecord(rex::graphics::XE_GPU_REG_RB_COLOR_MASK),
                    DvarRecord(rex::graphics::XE_GPU_REG_RB_MODECONTROL) & 0x7);
      }
      return true;  // it is D3D's clear: it does not go to the shader diagnostics
    };
    const auto float_value = [](uint32_t bits) {
      float f;
      std::memcpy(&f, &bits, sizeof(f));
      return f;
    };
    bo2zm::native::DrawClear b;
    const uint32_t mode = DvarRecord(g::XE_GPU_REG_RB_MODECONTROL) & 0x7;
    const uint32_t control_z = DvarRecord(g::XE_GPU_REG_RB_DEPTHCONTROL);
    const uint32_t stencil = DvarRecord(g::XE_GPU_REG_RB_STENCILREFMASK);
    if (control_z & 0x2) {  // z enable
      if (((control_z >> 4) & 0x7) != 7) {
        return no_is_clear("z test other than ALWAYS");
      }
      b.depth = (control_z & 0x4) != 0;
    }
    if ((control_z & 0x1) && ((stencil >> 16) & 0xFF)) {  // stencil enable and something written
      if (((control_z >> 8) & 0x7) != 7 || ((control_z >> 14) & 0x7) != 2 || ((stencil >> 16) & 0xFF) != 0xFF) {
        return no_is_clear("stencil that is not a full replace");
      }
      b.stencil = true;
      b.value_stencil = stencil & 0xFF;
    }
    const uint32_t mask = DvarRecord(g::XE_GPU_REG_RB_COLOR_MASK) & 0xF;
    if (mode == 4 && mask) {
      if (mask != 0xF) {
        return no_is_clear("partial color mask");
      }
      b.color = true;  // the value comes from the vertices, below
    }
    if (!b.depth && !b.stencil && !b.color) {
      return true;  // writes nothing
    }
    // The three corners, from vertex fetch constant 0: 3 vertices with x, y, z first.
    const uint32_t f0 = DvarRecord(0x4800), f1 = DvarRecord(0x4801);
    const uint32_t words = (f1 >> 2) & 0xFFFFFF;
    if ((f0 & 0x3) != 3 || (f1 & 0x3) != 2 || words < 9 || words % 3 || words > 48) {
      return no_is_clear("vertices that are not understood");
    }
    const uint32_t step = words / 3;
    const uint8_t* data = memory_->TranslatePhysical(f0 & 0x1FFFFFFC);
    float v[3][3];
    for (uint32_t i = 0; i < 3; ++i) {
      for (uint32_t c = 0; c < 3; ++c) {
        v[i][c] = float_value(rex::memory::load_and_swap<uint32_t>(data + (size_t(i) * step + c) * 4));
      }
    }
    if (b.color) {
      // The color follows the position in each vertex (the VS fetches it at offset 3); all three carry the same.
      if (step < 7) {
        return no_is_clear("color clear without a color in the vertices");
      }
      for (uint32_t i = 0; i < 4; ++i) {
        b.rgba[i] = float_value(rex::memory::load_and_swap<uint32_t>(data + (3 + size_t(i)) * 4));
      }
    }
    // Pixel centers at integers (D3D9): a pixel is covered when min <= center < max.
    const auto edge = [](float x) { return int32_t(std::ceil(x - 1.0f / 1024.0f)); };
    b.x0 = std::max(0, edge(std::min({v[0][0], v[1][0], v[2][0]})));
    b.x1 = std::max(0, edge(std::max({v[0][0], v[1][0], v[2][0]})));
    b.y0 = std::max(0, edge(std::min({v[0][1], v[1][1], v[2][1]})));
    b.y1 = std::max(0, edge(std::max({v[0][1], v[1][1], v[2][1]})));
    b.z = v[0][2];
    const uint32_t surface = DvarRecord(g::XE_GPU_REG_RB_SURFACE_INFO);
    const uint32_t info_z = DvarRecord(g::XE_GPU_REG_RB_DEPTH_INFO);
    const uint32_t info_color = DvarRecord(g::XE_GPU_REG_RB_COLOR_INFO);
    b.rb_depth_info = info_z;
    b.rb_color_info = info_color;
    // Depth/stencil and color can each be an alias of a different surface: one clear for each.
    std::string detail;
    for (uint32_t part = 0; part < 2; ++part) {
      bo2zm::native::DrawClear p = b;
      if (part == 0) {
        p.color = false;
        if (!p.depth && !p.stencil) continue;
      } else {
        p.depth = p.stencil = false;
        if (!p.color) continue;
      }
      const uint32_t key = part == 0 ? (info_z & 0xFFF) : (0x1000 | (info_color & 0xFFF));
      p.rb_surface_info = surface;
      const auto it = surfaces_bo2zm_.find(key);
      if (it != surfaces_bo2zm_.end() && (it->second & 0x33FFF) != (surface & 0x33FFF)) {
        FromAlias(surface, it->second, p);
      }
      const bool applied = targets_->ClearAsDraw(p);
      detail += fmt::format(" {} sup {:08X} [{},{})x[{},{}){}", part == 0 ? "z/stencil" : "color", p.rb_surface_info,
                             p.x0, p.x1, p.y0, p.y1, applied ? "" : " (no image)");
    }
    ++clears_d3d_;
    // One line per kind of clear, so the decoding can be checked against the guest state.
    const uint64_t type = (uint64_t(surface) << 32) ^ (uint64_t(info_z) << 12) ^ info_color ^
                          (uint64_t(control_z) << 20) ^ (uint64_t(b.x1) << 48) ^ (uint64_t(b.color) << 63);
    if (clears_d3d_seen_.size() < 64 && clears_d3d_seen_.insert(type).second) {
      std::string raw;
      for (uint32_t i = 0; i < words; ++i) {
        raw += fmt::format(" {:.4g}", float_value(rex::memory::load_and_swap<uint32_t>(data + size_t(i) * 4)));
      }
      REXLOG_INFO("[native] BO2 D3D clear: surface {:08X} z {:08X} stencil {:08X} depth {:08X} color {:08X} mode {} -> "
                  "z {} ({:.6g}) stencil {} ({}) color {} ({:.3g},{:.3g},{:.3g},{:.3g});{} | vertices:{}",
                  surface, control_z, stencil, info_z, info_color, mode, b.depth, b.z, b.stencil,
                  b.value_stencil, b.color, b.rgba[0], b.rgba[1], b.rgba[2], b.rgba[3], detail, raw);
      if (clears_d3d_seen_.size() == 1) {
        std::string vs, ps;
        for (uint32_t w : vs_microcode_) vs += fmt::format(" {:08X}", w);
        for (uint32_t i = 0; i < 9; ++i) ps += fmt::format(" {:08X}", ps_immediate_bo2zm_[i]);
        REXLOG_INFO("[native] BO2 D3D clear: VS{} | PS{}", vs, ps);
      }
    }
    return true;
  }

  // Whether two surfaces (RB_SURFACE_INFO) lay out the same samples per EDRAM row.
  static bool SameSamplesPerRow(uint32_t a, uint32_t b) {
    const auto in_x = [](uint32_t sup) { return ((sup >> 16) & 0x3) >= 2 ? 2u : 1u; };
    return (a & 0x3FFF) * in_x(a) == (b & 0x3FFF) * in_x(b);
  }

  // The clear's rectangle, from D3D's alias surface to the real one: same samples per row. 4X MSAA has 2x2
  // samples per pixel, 2X has 1x2 (two rows), 1X one.
  static void FromAlias(uint32_t alias, uint32_t real, bo2zm::native::DrawClear& b) {
    const auto in_x = [](uint32_t sup) { return ((sup >> 16) & 0x3) >= 2 ? 2 : 1; };
    const auto in_y = [](uint32_t sup) { return ((sup >> 16) & 0x3) >= 1 ? 2 : 1; };
    if (!SameSamplesPerRow(alias, real)) {
      return;  // not the same rows of samples: left as it is
    }
    b.x0 = b.x0 * in_x(alias) / in_x(real);
    b.x1 = b.x1 * in_x(alias) / in_x(real);
    b.y0 = b.y0 * in_y(alias) / in_y(real);
    b.y1 = b.y1 * in_y(alias) / in_y(real);
    b.rb_surface_info = real;
  }

  // The surface (RB_SURFACE_INFO) each EDRAM base was last drawn with, for ClearD3D. Depth when
  // the draw tests or writes depth or stencil; color for RT0 when it writes color.
  void AnnotateSurfaceBo2zm() {
    namespace g = rex::graphics;
    const uint32_t surface = DvarRecord(g::XE_GPU_REG_RB_SURFACE_INFO);
    const uint32_t info_z = DvarRecord(g::XE_GPU_REG_RB_DEPTH_INFO);
    const uint32_t info_color = DvarRecord(g::XE_GPU_REG_RB_COLOR_INFO);
    const uint32_t control_z = DvarRecord(g::XE_GPU_REG_RB_DEPTHCONTROL) & 0x7;
    const uint32_t color = (DvarRecord(g::XE_GPU_REG_RB_MODECONTROL) & 0x7) == 4 &&
                           (DvarRecord(g::XE_GPU_REG_RB_COLOR_MASK) & 0xF);
    if (surface == annotated_sup_ && info_z == annotated_z_ && info_color == annotated_color_ &&
        control_z == annotated_control_z_ && color == annotated_writes_color_) [[likely]] {
      return;
    }
    annotated_sup_ = surface;
    annotated_z_ = info_z;
    annotated_color_ = info_color;
    annotated_control_z_ = control_z;
    annotated_writes_color_ = color;
    if (control_z) {
      surfaces_bo2zm_[info_z & 0xFFF] = surface;
    }
    if (color) {
      surfaces_bo2zm_[0x1000 | (info_color & 0xFFF)] = surface;
    }
  }
  std::unordered_map<uint32_t, uint32_t> surfaces_bo2zm_;
  uint32_t annotated_sup_ = 0, annotated_z_ = 0, annotated_color_ = 0, annotated_control_z_ = 0, annotated_writes_color_ = 0;
  uint64_t clears_d3d_ = 0;
  uint32_t clears_d3d_rejected_ = 0;
  std::set<uint64_t> clears_d3d_seen_;

  // The render target side (bo2zm_native_targets.cpp), created on first use: a draw in copy mode is a resolve and/or
  // a clear.
  bool EnsureTargets() {
    if (!targets_ && !targets_failed_) {
      targets_ = NativeTargets::Create(provider_ ? provider_->vulkan_device() : nullptr, memory_);
      if (!targets_) {
        targets_failed_ = true;
        REXLOG_ERROR("[native] Could not create the native targets (C2)");
      }
    }
    return targets_ != nullptr;
  }

  // Diagnostic bo2zm_native_diag_constants_ps: the first constants of the requested PS, at most once
  // every bo2zm_native_diag_constants_ms per PS (exposure and brightness of the visual treatment).
  void AnnotateConstantsPs() {
    if (!ps_draw_) {
      return;
    }
    const std::string& list = REXCVAR_GET(bo2zm_native_diag_constants_ps);
    if (list.empty()) {
      return;
    }
    if (list != constants_list_text_) {
      constants_list_text_ = list;
      constants_list_.clear();
      size_t start = 0;
      while (start <= list.size()) {
        const size_t end = std::min(list.find(',', start), list.size());
        if (end > start) {
          constants_list_.insert(uint32_t(std::strtoul(list.substr(start, end - start).c_str(), nullptr, 10)));
        }
        start = end + 1;
      }
    }
    if (!constants_list_.count(ps_draw_->number)) {
      return;
    }
    const auto now = Clock::now();
    auto& last = constants_last_[ps_draw_->number];
    if (now - last < std::chrono::milliseconds(REXCVAR_GET(bo2zm_native_diag_constants_ms))) {
      return;
    }
    last = now;
    std::string values;
    // BO2's scene shaders scale their output by hdrControl0 (c58); and where the constant windows
    // are (SQ_VS_CONST / SQ_PS_CONST: base and size).
    values += fmt::format(" SQ_VS_CONST={:08X} SQ_PS_CONST={:08X}", DvarRecord(rex::graphics::XE_GPU_REG_SQ_VS_CONST),
                           DvarRecord(rex::graphics::XE_GPU_REG_SQ_PS_CONST));
    for (uint32_t k : {0u, 1u, 2u, 3u, 4u, 5u, 6u, 7u, 8u, 9u, 10u, 11u, 56u, 57u, 58u, 59u}) {
      values += fmt::format(" c{}=(", k);
      for (uint32_t c = 0; c < 4; ++c) {
        float f;
        const uint32_t bits = registers_[0x4400 + k * 4 + c];
        std::memcpy(&f, &bits, sizeof(f));
        values += fmt::format("{}{:.4g}", c ? "," : "", f);
      }
      values += ")";
    }
    REXLOG_INFO("[native] PS n{} constants (Swap {}):{}", ps_draw_->number, swaps_.load(), values);
    // And the vertex shader's (in BO2: view-projection c0-c3, world c4-c7, fog c64-c68).
    std::string vs_values;
    for (uint32_t k : {0u, 1u, 2u, 3u, 4u, 5u, 6u, 7u, 64u, 65u, 66u, 67u, 68u}) {
      vs_values += fmt::format(" c{}=(", k);
      for (uint32_t c = 0; c < 4; ++c) {
        float f;
        const uint32_t bits = registers_[0x4000 + k * 4 + c];
        std::memcpy(&f, &bits, sizeof(f));
        vs_values += fmt::format("{}{:.4g}", c ? "," : "", f);
      }
      vs_values += ")";
    }
    REXLOG_INFO("[native] VS n{} constants with PS n{}:{}", vs_draw_ ? int(vs_draw_->number) : -1,
                ps_draw_->number, vs_values);
  }

  // The draw, with its VS and PS identified, goes to Vulkan (bo2zm_native_draws.cpp).
  /*
   * Measurement only. A fence or an interrupt the game can see: how many vertex copies are still
   * pending at that moment (the ring only waits for them before returning the read pointer). Ring
   * thread only.
   */
  /*
   * Measurement only. When the ring finishes a frame (its Swap, already counted in swaps_), how many
   * more Swaps the game has written: 0 or less = the ring is caught up; 1 = the next frame is already
   * entirely in the ring; 2 or more = two or more. The game counts after writing the Swap, so a
   * momentary -1 is normal. The first difference is recorded separately in case the two counters do
   * not start together. Ring thread only.
   */
  void AnnotateGameAhead() {
    const int64_t mismatch = int64_t(::g_bo2zm_frames_game.load(std::memory_order_relaxed)) -
                               int64_t(swaps_.load(std::memory_order_relaxed));
    if (!game_ahead_first_valid_) {
      game_ahead_first_valid_ = true;
      game_ahead_first_ = mismatch;
    }
    ++game_ahead_[mismatch <= 0 ? 0 : mismatch == 1 ? 1 : 2];
    game_ahead_max_ = std::max(game_ahead_max_, mismatch);
  }

  void AnnotateFenceCopies() {
    ++fences_total_;
    if (targets_) {
      const size_t pending = targets_->CopiesPending();
      if (pending) {
        ++fences_with_copies_;
        fences_copies_max_ = std::max(fences_copies_max_, pending);
      }
    }
  }

  // Before the ring tells the game the GPU got this far (bo2zm_native_fences_wait_copies).
  void BeforeFence() {
    AnnotateFenceCopies();  // Measurement only: what was pending before the wait
    static const bool wait = REXCVAR_GET(bo2zm_native_fences_wait_copies);
    if (wait && targets_) {
      targets_->WaitUploads();
    }
  }

  // Diagnostics (bo2zm_native_diag_lut): BO2's color-grading strip (the draw with a 32x1024 viewport) came out black
  // on the Switch at one point, while on the PC it was colored. Its pixel shader reads the base table (fetch slot 0, a
  // 64x1024 8888 texture from memory) and the constants c72-c74 (colorMatrixR/G/B) and c236-c250 (postFxControl0-E).
  // At most every 5 s: those constants, the slot 0 fetch constant and the mean of the table's first 4 KB in memory.
  void DiagDropsLutBo2zm() {
    static const bool active = REXCVAR_GET(bo2zm_native_diag_lut);
    if (!active) [[likely]] {
      return;
    }
    namespace g = rex::graphics;
    const auto float_value = [](uint32_t bits) {
      float f;
      std::memcpy(&f, &bits, sizeof(f));
      return f;
    };
    const float scale_x = float_value(DvarRecord(g::XE_GPU_REG_PA_CL_VPORT_XSCALE));
    const float scale_y = float_value(DvarRecord(g::XE_GPU_REG_PA_CL_VPORT_YSCALE));
    if (std::abs(scale_x) != 16.0f || std::abs(scale_y) != 512.0f) {
      return;
    }
    const auto now = Clock::now();
    if (now - diag_lut_last_ < std::chrono::seconds(5)) {
      return;
    }
    diag_lut_last_ = now;
    std::string constants;
    for (uint32_t c : {72u, 73u, 74u, 236u, 237u, 238u, 240u, 245u, 247u, 248u, 249u, 250u}) {
      constants += fmt::format(" c{}=({:.3g},{:.3g},{:.3g},{:.3g})", c, float_value(DvarRecord(0x4400 + c * 4)),
                                float_value(DvarRecord(0x4401 + c * 4)), float_value(DvarRecord(0x4402 + c * 4)),
                                float_value(DvarRecord(0x4403 + c * 4)));
    }
    const uint32_t f0 = g::XE_GPU_REG_SHADER_CONSTANT_FETCH_00_0;
    const uint32_t base = DvarRecord(f0 + 1) & 0x1FFFF000;
    uint64_t sum = 0;
    if (base && base + 4096 <= 0x20000000u) {
      const uint8_t* data = memory_->TranslatePhysical(base);
      for (uint32_t i = 0; i < 4096; ++i) {
        sum += data[i];
      }
    }
    REXLOG_INFO("[native] BO2 LUT strip: PS n{} viewport {}x{}; fetch 0 {:08X} {:08X} {:08X} {:08X} {:08X} {:08X}, "
                "average of the table's 4 KB {:.1f};{}",
                ps_draw_ ? int(ps_draw_->number) : -1, std::abs(scale_x) * 2, std::abs(scale_y) * 2,
                DvarRecord(f0), DvarRecord(f0 + 1), DvarRecord(f0 + 2), DvarRecord(f0 + 3), DvarRecord(f0 + 4),
                DvarRecord(f0 + 5), double(sum) / 4096.0, constants);
  }
  Clock::time_point diag_lut_last_{};

  void DrawNative() {
    if (!EnsureTargets()) {
      return;
    }
    AnnotateSurfaceBo2zm();
    DiagDropsLutBo2zm();
    // AnnotateConstantsPs used to read the text cvar on every draw, and REXCVAR_GET of a string
    // is a call with an initialization guard (a load with acquire) that cannot be folded. The
    // flag is refreshed once per Swap: the diagnostic takes at most one frame to turn on.
    if (diag_constants_active_) {
      AnnotateConstantsPs();
    }
    DrawRequest request;
    request.registers = registers_.data();
    request.vs = vs_draw_;
    request.ps = ps_draw_;
    request.vs_microcode = vs_microcode_;
    request.generation_vs = generation_vs_;
    request.generation_constants_vs = generation_constants_vs_;
    request.generation_constants_ps = generation_constants_ps_;
    request.generation_fetch = generation_fetch_;
    request.generation_framing = generation_framing_;
    request.vegetation_game = vegetation_game_;  // bo2zm_d3d_game_vegetation
    // Timer on 1 of every kStopwatchDrawsEvery draws, with the time scaled up (as in Packet).
    // The phase comes from a counter that is not reset on each report (draws_measured_ is), offset to
    // the middle of the batch: counting from 1, draws 33, 97, 161... are timed here, and the stage timer
    // of VulkanDrawsImpl::Draw runs on 64, 128, 192... Those pay ~20 extra clock reads; previously,
    // after each report, 1 time in 8 they landed exactly on the draws timed here and inflated
    // "draws us/draw".
    if ((phase_stopwatch_draws_++ & (kStopwatchDrawsEvery - 1)) == kStopwatchDrawsEvery / 2) {
      const auto start = Clock::now();
      targets_->Draw(request);
      time_draws_ns_ += NanosecondsSince(start) * kStopwatchDrawsEvery;
    } else {
      targets_->Draw(request);
    }
    ++draws_measured_;
  }

  void DoCopy() {
    if (!EnsureTargets()) {
      return;
    }
    namespace g = rex::graphics;
    CopyRegisters r;
    r.rb_surface_info = DvarRecord(g::XE_GPU_REG_RB_SURFACE_INFO);
    r.rb_color_info[0] = DvarRecord(g::XE_GPU_REG_RB_COLOR_INFO);
    r.rb_color_info[1] = DvarRecord(g::XE_GPU_REG_RB_COLOR1_INFO);
    r.rb_color_info[2] = DvarRecord(g::XE_GPU_REG_RB_COLOR2_INFO);
    r.rb_color_info[3] = DvarRecord(g::XE_GPU_REG_RB_COLOR3_INFO);
    r.rb_depth_info = DvarRecord(g::XE_GPU_REG_RB_DEPTH_INFO);
    r.rb_copy_control = DvarRecord(g::XE_GPU_REG_RB_COPY_CONTROL);
    r.rb_copy_dest_base = DvarRecord(g::XE_GPU_REG_RB_COPY_DEST_BASE);
    r.rb_copy_dest_pitch = DvarRecord(g::XE_GPU_REG_RB_COPY_DEST_PITCH);
    r.rb_copy_dest_info = DvarRecord(g::XE_GPU_REG_RB_COPY_DEST_INFO);
    r.rb_color_clear = DvarRecord(g::XE_GPU_REG_RB_COLOR_CLEAR);
    r.rb_color_clear_lo = DvarRecord(g::XE_GPU_REG_RB_COLOR_CLEAR_LO);
    r.rb_depth_clear = DvarRecord(g::XE_GPU_REG_RB_DEPTH_CLEAR);
    r.pa_sc_window_offset = DvarRecord(g::XE_GPU_REG_PA_SC_WINDOW_OFFSET);
    r.pa_sc_window_scissor_tl = DvarRecord(g::XE_GPU_REG_PA_SC_WINDOW_SCISSOR_TL);
    r.pa_sc_window_scissor_br = DvarRecord(g::XE_GPU_REG_PA_SC_WINDOW_SCISSOR_BR);
    r.pa_su_sc_mode_cntl = DvarRecord(g::XE_GPU_REG_PA_SU_SC_MODE_CNTL);
    r.pa_su_vtx_cntl = DvarRecord(g::XE_GPU_REG_PA_SU_VTX_CNTL);
    r.fetch_vertices[0] = DvarRecord(g::XE_GPU_REG_SHADER_CONSTANT_FETCH_00_0);
    r.fetch_vertices[1] = DvarRecord(g::XE_GPU_REG_SHADER_CONSTANT_FETCH_00_1);
    // A resolve through an alias, as with D3D's clears (ClearD3D, FromAlias). In Town, while the player is burning,
    // BO2 draws its post-processed 880x720 image (1X) and resolves the same EDRAM as a 440-wide 4X surface: the 2x2
    // samples of each 4X pixel are a 2x2 block of the 1X image, so the 440x360 result is the image at half size, the
    // source of a full-screen blur. The target code keeps one image per pitch, so without this it copied from an empty
    // 440-wide image, and the blur covered the frame with black (a RenderDoc capture showed the copy reading an image
    // that was never drawn into the 448x360 texture the blur samples).
    if (const uint32_t source = r.rb_copy_control & 0x7; source < 4) {
      const auto it = surfaces_bo2zm_.find(0x1000 | (r.rb_color_info[source] & 0xFFF));
      if (it != surfaces_bo2zm_.end() && (it->second & 0x33FFF) != (r.rb_surface_info & 0x33FFF) &&
          SameSamplesPerRow(r.rb_surface_info, it->second)) {
        r.surface_real = it->second;
      }
    }
    TraceCopy(r);
    const auto start = Clock::now();
    targets_->Copy(r);
    time_copies_ns_ += NanosecondsSince(start);
    ++copies_measures_;
  }

  // Diagnostic (bo2zm_native_diag_frame_s): every draw and every copy of one whole
  // frame, from the first Swap after that many seconds until the next one.
  // Diagnostics (bo2zm_native_dump_memory).
  void DumpMemoryBo2zm() {
    if (memory_dumped_bo2zm_) [[likely]] {
      return;
    }
    const std::string& text = REXCVAR_GET(bo2zm_native_dump_memory);
    const size_t two_points = text.find(':'), at = text.find('@');
    if (text.empty() || two_points == std::string::npos || at == std::string::npos) {
      memory_dumped_bo2zm_ = true;
      return;
    }
    const uint32_t seconds = uint32_t(std::strtoul(text.c_str() + at + 1, nullptr, 10));
    if (Clock::now() - start_system_ < std::chrono::seconds(seconds)) {
      return;
    }
    uint32_t address = uint32_t(std::strtoul(text.c_str(), nullptr, 16)) & 0x1FFFFFFF;
    uint32_t bytes = uint32_t(std::strtoul(text.c_str() + two_points + 1, nullptr, 16));
    if (text.rfind("auto", 0) == 0) {  // "auto:0@S": the texture bo2zm_native_dump_texture dumped, again
      const uint64_t dump = g_bo2zm_dump_texture.load(std::memory_order_relaxed);
      if (!dump) {
        return;  // not dumped yet
      }
      address = uint32_t(dump >> 32);
      bytes = uint32_t(dump);
    }
    memory_dumped_bo2zm_ = true;
    if (!bytes || uint64_t(address) + bytes > 0x20000000) {
      return;
    }
    const std::filesystem::path path =
        rex::filesystem::GetExecutableFolder() / fmt::format("dump_mem_{:08X}_{}s.bin", address, seconds);
    std::ofstream output(path, std::ios::binary);
    output.write(reinterpret_cast<const char*>(memory_->TranslatePhysical(address)), std::streamsize(bytes));
    REXLOG_INFO("[native] BO2: memory {:08X}+{:X} dumped to {}", address, bytes, path.string());
  }
  bool memory_dumped_bo2zm_ = false;

  // The campaign front end's reuse (bo2zm_native_front_end_reuse, see there).
  static uint32_t SurfacePitch(uint32_t surface_info) { return surface_info & 0x3FFF; }

  static constexpr uint32_t kSceneSamplesEach = 24;  // meshes and post-processing draws compared per frame
  static constexpr uint32_t kSceneSamples = kSceneSamplesEach * 2;
  static constexpr uint32_t kSceneRegisters = 16;    // their constants c0-c15 (transforms; colour grading)
  static constexpr uint32_t kSceneSlots = kSceneRegisters * 2;  // vertex constants of meshes, then pixel of post
  struct SceneSample {
    uint64_t key = 0;   // the draw: its index buffer or target, index count and shader
    uint32_t bank = 0;  // 0: a mesh's vertex constants; 1: a post-processing draw's pixel constants
    std::array<uint32_t, kSceneRegisters * 4> constants{};
  };

  bool SceneReuseDraw() {
    if (!scene_reuse_frame_) {
      return false;
    }
    namespace g = rex::graphics;
    const uint32_t pitch = SurfacePitch(DvarRecord(g::XE_GPU_REG_RB_SURFACE_INFO));
    if (pitch == 0 || pitch == 1280) {
      return false;
    }
    // The first meshes that write depth into such a target: their transforms tell whether the view moved. And the
    // first draws that do not (post-processing): their pixel constants change while a vision set or a blur fades.
    const bool depth = (DvarRecord(g::XE_GPU_REG_RB_DEPTHCONTROL) & 0x6) == 0x6;
    if (scene_samples_count_ < kSceneSamples && (depth ? vs_draw_ != nullptr : ps_draw_ != nullptr) &&
        (depth ? scene_mesh_samples_ : scene_post_samples_) < kSceneSamplesEach) {
      ++(depth ? scene_mesh_samples_ : scene_post_samples_);
      SceneSample& sample = scene_samples_[scene_samples_count_++];
      sample.bank = depth ? 0 : 1;
      sample.key = depth ? (uint64_t(DvarRecord(g::XE_GPU_REG_VGT_DMA_BASE)) << 32) ^
                               (uint64_t(DvarRecord(g::XE_GPU_REG_VGT_DRAW_INITIATOR) >> 16) << 8) ^ vs_draw_->hash
                         : (uint64_t(pitch) << 48) ^ (uint64_t(DvarRecord(g::XE_GPU_REG_RB_COLOR_INFO)) << 24) ^
                               ps_draw_->hash ^ 1;
      const uint32_t first = g::XE_GPU_REG_SHADER_CONSTANT_000_X + (depth ? 0 : 256 * 4);
      for (uint32_t i = 0; i < kSceneRegisters * 4; ++i) {
        sample.constants[i] = DvarRecord(first + i);
      }
    }
    if (scene_skip_) {
      ++scene_skipped_draws_;
    }
    return scene_skip_;
  }

  bool SceneReuseCopy() {
    if (!scene_reuse_frame_ || !scene_skip_) {
      return false;
    }
    const uint32_t pitch = SurfacePitch(DvarRecord(rex::graphics::XE_GPU_REG_RB_SURFACE_INFO));
    if (pitch == 0 || pitch == 1280) {
      return false;
    }
    ++scene_skipped_copies_;
    return true;
  }

  static float AsFloat(uint32_t bits) {
    float value;
    std::memcpy(&value, &bits, sizeof(value));
    return value;
  }

  // At the end of a frame: compares its meshes' transforms with the same meshes in the last frame drawn in full (full:
  // this frame is one, and becomes the reference). True if the next frame can reuse the scene.
  bool CheckSceneView(bool full) {
    std::array<float, kSceneSlots> change{};  // the largest relative change of each register
    uint32_t matched = 0;
    for (uint32_t s = 0; s < scene_samples_count_; ++s) {
      const SceneSample& sample = scene_samples_[s];
      for (uint32_t t = 0; t < scene_reference_count_; ++t) {
        const SceneSample& reference = scene_reference_[t];
        if (reference.key != sample.key) {
          continue;
        }
        ++matched;
        for (uint32_t i = 0; i < kSceneRegisters * 4; ++i) {
          if (sample.constants[i] == reference.constants[i]) {
            continue;
          }
          const float before = AsFloat(reference.constants[i]);
          float c = std::abs(AsFloat(sample.constants[i]) - before) / std::max(1.0f, std::abs(before));
          if (!(c == c)) {
            c = 1e9f;
          }
          change[sample.bank * kSceneRegisters + i / 4] = std::max(change[sample.bank * kSceneRegisters + i / 4], c);
        }
        break;
      }
    }
    // Registers that change in every frame for a long while (timers) are left out.
    const float drift_limit = float(REXCVAR_GET(bo2zm_native_front_end_reuse_drift));
    float largest = 0.0f;
    uint32_t largest_register = 0;
    uint32_t noisy = 0;
    for (uint32_t r = 0; r < kSceneSlots; ++r) {
      scene_streak_[r] = change[r] > drift_limit ? std::min(scene_streak_[r] + 1, kSceneNoisy) : 0;
      if (scene_streak_[r] >= kSceneNoisy) {
        ++noisy;
        continue;
      }
      if (change[r] > largest) {
        largest = change[r];
        largest_register = r;
      }
    }
    scene_noisy_ = noisy;
    scene_largest_ = std::max(scene_largest_, largest);
    if (!matched) {
      ++scene_unmatched_;
    }
    const auto now = Clock::now();
    const int32_t refresh_ms = REXCVAR_GET(bo2zm_native_front_end_reuse_refresh_ms);
    const bool refresh = refresh_ms > 0 && now - scene_drawn_ >= std::chrono::milliseconds(refresh_ms);
    const bool moved = !matched || largest > float(REXCVAR_GET(bo2zm_native_front_end_reuse_move));
    const bool drifted = largest > drift_limit &&
                         now - scene_drawn_ >=
                             std::chrono::milliseconds(REXCVAR_GET(bo2zm_native_front_end_reuse_drift_ms));
    const bool had_reference = scene_reference_count_ > 0;
    if (had_reference && (moved || drifted)) {
      if (matched) {
        ++scene_redraws_by_register_[largest_register];
      }
      ++(moved ? scene_redraws_moved_ : scene_redraws_drifted_);
    }
    // When the front end opens, and after a move, the scene is drawn in full until it has held still for a while:
    // effects that build up over frames on the GPU (the exposure, the glow) settle first, as they would in a still
    // view.
    if (!had_reference || !matched || moved) {
      scene_last_move_ = now;
    }
    if ((!had_reference || !matched) && !full) {
      scene_force_full_ = true;  // a new scene seen in a frame not drawn: the next one is drawn to take it in
    } else if (!had_reference || !matched) {
      // A new scene (the front end opening, or loading again): textures still stream in for a while too.
      scene_kept_ = false;
      scene_settle_ = uint32_t(REXCVAR_GET(bo2zm_native_front_end_reuse_settle));
      scene_settle_until_ = now + std::chrono::milliseconds(REXCVAR_GET(bo2zm_native_front_end_reuse_settle_ms));
    } else if (moved) {
      scene_settle_ = std::max(scene_settle_, uint32_t(REXCVAR_GET(bo2zm_native_front_end_reuse_settle_move)));
    } else if (full && scene_settle_) {
      --scene_settle_;
    }
    if (full) {
      scene_force_full_ = false;
      scene_reference_ = scene_samples_;
      scene_reference_count_ = scene_samples_count_;
      scene_drawn_ = now;
    }
    // And the view has held still for a while: effects that run while the camera moves (the landing's blur) are over.
    const bool still =
        now - scene_last_move_ >= std::chrono::milliseconds(REXCVAR_GET(bo2zm_native_front_end_reuse_still_ms));
    return had_reference && !moved && !drifted && !refresh && !scene_settle_ && now >= scene_settle_until_ && still;
  }

  void SceneReuseSwap() {
    const bool on = REXCVAR_GET(bo2zm_native_front_end_reuse) && bo2zm::preset::FrontEnd();
    bool next = false;
    if (scene_reuse_frame_ && scene_samples_count_) {
      next = CheckSceneView(!scene_skip_);
      if (scene_skip_) {
        ++scene_reused_frames_;
      } else {
        ++scene_drawn_frames_;
      }
    }
    if (!on && scene_reuse_frame_) {
      scene_reference_count_ = 0;
      scene_streak_.fill(0);
    }
    if (!on) {
      scene_kept_ = false;
    }
    // The scene is drawn in every frame until it is kept, even while the menus hide it behind their veil: drawn on
    // some frames only, the kept image came out wrong (darker, with parts of the scene missing), as the effects that
    // build up over frames on the GPU and the scene's own set-up need every frame.
    if (on && next) {
      scene_kept_ = true;
    }
    bo2zm::preset::SetFrontEndSceneKept(on && scene_kept_);
    // The next frame reuses the scene if this one found the view unchanged.
    scene_reuse_frame_ = on;
    scene_skip_ = on && next;
    scene_samples_count_ = 0;
    scene_mesh_samples_ = scene_post_samples_ = 0;
    if (Clock::now() - scene_report_ >= std::chrono::seconds(10)) {
      scene_report_ = Clock::now();
      if (scene_reused_frames_ || scene_drawn_frames_) {
        // The registers that caused the most redraws.
        std::string top;
        for (int k = 0; k < 4; ++k) {
          uint32_t best = 0, count = 0;
          for (uint32_t r = 0; r < kSceneSlots; ++r) {
            if (scene_redraws_by_register_[r] > count) {
              count = scene_redraws_by_register_[r];
              best = r;
            }
          }
          if (!count) {
            break;
          }
          top += fmt::format("{}{}c{} x{}", top.empty() ? "" : ", ", best < kSceneRegisters ? "VS " : "PS ",
                             best % kSceneRegisters, count);
          scene_redraws_by_register_[best] = 0;
        }
        REXLOG_INFO("[native] front end, last 10 s: the scene drawn in {} frames ({} for a move, {} for a drift, {} "
                    "with no mesh in common) and reused in {} ({} draws and {} copies not recorded); largest change "
                    "{:.4f}; redraws by constant: {}; {} constants left out as always changing",
                    scene_drawn_frames_, scene_redraws_moved_, scene_redraws_drifted_, scene_unmatched_,
                    scene_reused_frames_, scene_skipped_draws_, scene_skipped_copies_, double(scene_largest_),
                    top.empty() ? "none" : top, scene_noisy_);
      }
      scene_drawn_frames_ = scene_reused_frames_ = scene_skipped_draws_ = scene_skipped_copies_ = 0;
      scene_redraws_moved_ = scene_redraws_drifted_ = scene_unmatched_ = 0;
      scene_redraws_by_register_.fill(0);
      scene_largest_ = 0.0f;
    }
  }
  std::array<SceneSample, kSceneSamples> scene_samples_{};
  std::array<SceneSample, kSceneSamples> scene_reference_{};
  uint32_t scene_samples_count_ = 0;
  uint32_t scene_reference_count_ = 0;  // 0: no frame drawn in full yet
  uint32_t scene_settle_ = 0;           // full frames still to draw before reusing
  bool scene_kept_ = false;             // the scene has been kept since it last changed entirely
  bool scene_force_full_ = false;       // draw the next frame (a new scene was seen in a frame not drawn)
  Clock::time_point scene_last_move_{};
  Clock::time_point scene_settle_until_{};  // and the time before which a new scene is not reused
  std::array<uint32_t, kSceneSlots> scene_streak_{};
  uint32_t scene_mesh_samples_ = 0;
  uint32_t scene_post_samples_ = 0;
  bool scene_reuse_frame_ = false;  // this frame: reuse on and the front end loaded
  bool scene_skip_ = false;         // this frame reuses the scene
  uint32_t scene_noisy_ = 0;
  float scene_largest_ = 0.0f;
  uint32_t scene_redraws_moved_ = 0;
  uint32_t scene_redraws_drifted_ = 0;
  uint32_t scene_unmatched_ = 0;
  std::array<uint32_t, kSceneSlots> scene_redraws_by_register_{};
  uint32_t scene_reused_frames_ = 0;
  uint32_t scene_drawn_frames_ = 0;
  uint32_t scene_skipped_draws_ = 0;
  uint32_t scene_skipped_copies_ = 0;
  Clock::time_point scene_drawn_{};
  Clock::time_point scene_report_{};

  // The campaign front end's depth prepass (bo2zm_native_front_end_no_prepass, see there): colour writes off, depth
  // test and write on, and no stencil write.
  bool SkipPrepassDraw() {
    if (!prepass_skip_frame_) {
      return false;
    }
    namespace g = rex::graphics;
    if (DvarRecord(g::XE_GPU_REG_RB_COLOR_MASK) != 0) {
      return false;
    }
    const uint32_t depth = DvarRecord(g::XE_GPU_REG_RB_DEPTHCONTROL);
    // Depth test and write on, and not ALWAYS (a depth clear drawn as a quad), with a vertex shader of its own.
    if ((depth & 0x6) != 0x6 || ((depth >> 4) & 0x7) == 0x7 || !vs_draw_) {
      return false;
    }
    if ((depth & 0x1) && ((depth >> 11) & 0x1FF) != 0) {  // front face: fail, zpass, zfail
      return false;
    }
    if ((depth & 0x81) == 0x81 && ((depth >> 23) & 0x1FF) != 0) {  // back face
      return false;
    }
    ++prepass_skipped_;
    return true;
  }
  bool prepass_skip_frame_ = false;  // this frame: bo2zm_native_front_end_no_prepass and the front end is loaded
  uint32_t prepass_skipped_ = 0;
  Clock::time_point prepass_report_{};

  // The menu blur (bo2zm_native_menu_blur_every, see there).
  bool IsMenuBlurPass() const {
    if (!ps_draw_) {
      return false;
    }
    for (const uint64_t hash : menu_blur_ps_) {
      if (ps_draw_->hash == hash) {
        return true;
      }
    }
    return false;
  }

  // A draw: true if it is one of the chain's passes and this frame reuses the last chain.
  bool MenuBlurSkipDraw() {
    if (menu_blur_ps_.empty() || !IsMenuBlurPass()) {
      return false;
    }
    namespace g = rex::graphics;
    menu_blur_seen_ = true;
    menu_blur_surface_ = DvarRecord(g::XE_GPU_REG_RB_COLOR_INFO) & 0xFFF;
    // The texture it reads is one of the chain's (the first pass reads the covered menus' copy).
    const uint32_t texture = DvarRecord(g::XE_GPU_REG_SHADER_CONSTANT_FETCH_00_0 + 1) & 0xFFFFF000;
    AddMenuBlurTexture(texture);
    menu_blur_pending_copy_ = true;
    if (menu_blur_reuse_) {
      ++menu_blur_skipped_draws_;
      return true;
    }
    return false;
  }

  void AddMenuBlurTexture(uint32_t texture) {
    if (!texture) {
      return;
    }
    for (uint32_t& t : menu_blur_textures_) {
      if (t == texture) {
        return;
      }
      if (t == 0) {
        t = texture;
        return;
      }
    }
  }

  // A copy: true if this frame reuses the last chain and the copy writes one of its textures from its surface.
  bool MenuBlurSkipCopy() {
    namespace g = rex::graphics;
    const uint32_t target = DvarRecord(g::XE_GPU_REG_RB_COPY_DEST_BASE) & 0xFFFFF000;
    const uint32_t source = DvarRecord(g::XE_GPU_REG_RB_COLOR_INFO) & 0xFFF;
    if (menu_blur_pending_copy_) {
      // The copy right after a pass: its destination is one of the chain's textures.
      menu_blur_pending_copy_ = false;
      AddMenuBlurTexture(target);
    }
    if (!menu_blur_reuse_ || source != menu_blur_surface_) {
      return false;
    }
    for (const uint32_t t : menu_blur_textures_) {
      if (t && t == target) {
        ++menu_blur_skipped_copies_;
        return true;
      }
    }
    return false;
  }

  void MenuBlurSwap() {
    static bool parsed = false;
    if (!parsed) {
      parsed = true;
      std::string text = REXCVAR_GET(bo2zm_native_menu_blur_ps);
      if (text.empty()) {
        text = "96DABEACA2BE010F,8BEE95B21E191C1E,ED9222BFCE1FD6D9";
      }
      for (size_t at = 0; at < text.size();) {
        const size_t comma = std::min(text.find(',', at), text.size());
        const std::string word = text.substr(at, comma - at);
        if (!word.empty()) {
          menu_blur_ps_.push_back(std::strtoull(word.c_str(), nullptr, 16));
        }
        at = comma + 1;
      }
    }
    int32_t every = REXCVAR_GET(bo2zm_native_menu_blur_every);
    if (const int32_t alternate = REXCVAR_GET(bo2zm_native_menu_blur_every_alternate_s); alternate > 0) {
      const int64_t period =
          std::chrono::duration_cast<std::chrono::seconds>(Clock::now() - start_system_).count() / alternate;
      if (period != menu_blur_period_) {
        menu_blur_period_ = period;
        REXLOG_INFO("[native] menu blur: every {} for the next {} s (0 = kept)", period % 2 == 1 ? 1 : every,
                    alternate);
      }
      if (period % 2 == 1) {
        every = 1;
      }
    }
    // The next frame reuses the chain only if this one had a chain, and not on every Nth frame.
    const bool had_chain = menu_blur_seen_;
    menu_blur_seen_ = false;
    menu_blur_pending_copy_ = false;
    if (had_chain) {
      ++menu_blur_frames_;
    } else {
      menu_blur_textures_.fill(0);
      menu_blur_frames_ = 0;
    }
    // Only outside a match: in one, the world behind the menus moves, and other effects may share the shaders.
    menu_blur_reuse_ = had_chain && every != 1 && menu_blur_frames_ != kMenuBlurSecond &&
                       (every == 0 || menu_blur_frames_ % uint32_t(every) != 0) && !bo2zm::preset::ClientInGame();
    if (Clock::now() - menu_blur_report_ >= std::chrono::seconds(10)) {
      menu_blur_report_ = Clock::now();
      if (menu_blur_skipped_draws_ || menu_blur_skipped_copies_) {
        REXLOG_INFO("[native] menu blur, last 10 s: {} passes and {} copies reused (every {}); textures "
                    "{:08X} {:08X} {:08X}, surface {:03X}", menu_blur_skipped_draws_, menu_blur_skipped_copies_,
                    every, menu_blur_textures_[0], menu_blur_textures_[1], menu_blur_textures_[2],
                    menu_blur_surface_);
      }
      menu_blur_skipped_draws_ = menu_blur_skipped_copies_ = 0;
    }
  }
  std::vector<uint64_t> menu_blur_ps_;
  std::array<uint32_t, 4> menu_blur_textures_{};  // the chain's textures (0 = free slot)
  uint32_t menu_blur_surface_ = 0xFFFFFFFF;        // the EDRAM base the chain draws into
  uint32_t menu_blur_frames_ = 0;                 // consecutive frames with a chain
  uint32_t menu_blur_skipped_draws_ = 0;
  uint32_t menu_blur_skipped_copies_ = 0;
  int64_t menu_blur_period_ = -1;
  bool menu_blur_seen_ = false;          // this frame had a chain pass
  bool menu_blur_pending_copy_ = false;  // a pass was just seen: the next copy is the chain's
  bool menu_blur_reuse_ = false;         // this frame reuses the last chain
  Clock::time_point menu_blur_report_{};

  void TraceSwap() {
    MenuBlurSwap();
    SceneReuseSwap();
    prepass_skip_frame_ = REXCVAR_GET(bo2zm_native_front_end_no_prepass) && bo2zm::preset::FrontEnd();
    if (Clock::now() - prepass_report_ >= std::chrono::seconds(10)) {
      prepass_report_ = Clock::now();
      if (prepass_skipped_) {
        REXLOG_INFO("[native] front end, last 10 s: {} depth prepass draws skipped", prepass_skipped_);
      }
      prepass_skipped_ = 0;
    }
    DumpMemoryBo2zm();
    // Once per Swap, not once per draw (see DrawNative).
    diag_constants_active_ = !REXCVAR_GET(bo2zm_native_diag_constants_ps).empty();
    if (tracing_) {
      tracing_ = false;
      trace_done_ = true;
      if (targets_) targets_->TraceFrontFrame(false);
      REXLOG_INFO("[trace] end of frame: {} lines", traces_);
      return;
    }
    const int32_t seconds = REXCVAR_GET(bo2zm_native_diag_frame_s);
    if (!trace_done_ && seconds > 0 &&
        Clock::now() - start_system_ >= std::chrono::seconds(seconds)) {
      tracing_ = true;
      traces_ = 0;
      if (targets_) targets_->TraceFrontFrame(true);
      REXLOG_INFO("[trace] full frame after Swap {}", swaps_.load());
    }
  }

  // One line per draw: shaders, render targets, state and the textures of the PS samplers
  // (address / format / dimension; ! if the fetch constant is not a texture).
  void TraceDraw() {
    if (!tracing_ || traces_ >= 20000) {
      return;
    }
    ++traces_;
    namespace g = rex::graphics;
    std::string textures;
    if (ps_draw_) {
      for (const SamplerShader& s : ps_draw_->samplers) {
        if (s.reg_index >= 16) {
          continue;
        }
        const uint32_t base = g::XE_GPU_REG_SHADER_CONSTANT_FETCH_00_0 + uint32_t(s.reg_index) * 6;
        const uint32_t d0 = DvarRecord(base);
        const uint32_t d1 = DvarRecord(base + 1);
        const uint32_t d3 = DvarRecord(base + 3);
        const uint32_t d5 = DvarRecord(base + 5);
        // Also the size word and the shader's sampler type (D3DXPARAMETER_TYPE: 13 = 3D).
        textures += fmt::format(" t{}={:08X}/f{}/d{}/s{:03X}/e{}/z{:08X}/k{}/w0{:08X}/w3{:08X}{}", s.reg_index,
                                d1 & 0xFFFFF000, d1 & 0x3F, (d5 >> 9) & 0x3, (d3 >> 1) & 0xFFF, (d1 >> 6) & 0x3,
                                DvarRecord(base + 2), s.type, d0, d3, (d0 & 0x3) == 2 ? "" : "!");
      }
    }
    if (!vs_draw_ || !ps_draw_) {
      static constexpr const char* kReasons[] = {"record with unknown shaders",
                                                 "no record",
                                                 "record with shaders different from the IM_LOAD"};
      textures += fmt::format(" | {}: VS objects {:08X} PS {:08X}; IM_LOAD PS n{} VS n{}; pending:",
                              kReasons[reason_paired_ % 3], object_vs_, object_ps_,
                              ps_current_ ? int(ps_current_->number) : -1,
                              vs_current_ ? int(vs_current_->number) : -1);
      for (size_t i = 0; i < pending_.size() && i < 4; ++i) {
        const DrawRecord& pending = pending_[i];
        const ShaderInput* vs = CachedObjectShader(pending.vs);
        const ShaderInput* ps = CachedObjectShader(pending.ps);
        textures += fmt::format(" [function {} type {} count {} VS n{} PS n{}]",
                                int(pending.function), pending.args[0],
                                RegisterCount(pending), vs ? int(vs->number) : -1,
                                ps ? int(ps->number) : -1);
      }
    }
    const uint32_t initiator = DvarRecord(g::XE_GPU_REG_VGT_DRAW_INITIATOR);
    if (ps_draw_) {
      textures += fmt::format(" ps_hash {:016X}", ps_draw_->hash);
    }
    REXLOG_INFO("[trace] draw VS n{} PS n{} type {} count {} surface {:08X} rt0 {:08X} rt1 {:08X} mask {:08X} blend "
                "{:08X} colorctl {:08X} depth {:08X} stencil {:08X} mode {:08X}{}",
                vs_draw_ ? int(vs_draw_->number) : -1, ps_draw_ ? int(ps_draw_->number) : -1,
                initiator & 0x3F, initiator >> 16, DvarRecord(g::XE_GPU_REG_RB_SURFACE_INFO),
                DvarRecord(g::XE_GPU_REG_RB_COLOR_INFO), DvarRecord(g::XE_GPU_REG_RB_COLOR1_INFO),
                DvarRecord(g::XE_GPU_REG_RB_COLOR_MASK), DvarRecord(g::XE_GPU_REG_RB_BLENDCONTROL0),
                DvarRecord(g::XE_GPU_REG_RB_COLORCONTROL), DvarRecord(g::XE_GPU_REG_RB_DEPTHCONTROL),
                DvarRecord(g::XE_GPU_REG_RB_STENCILREFMASK), DvarRecord(g::XE_GPU_REG_RB_MODECONTROL),
                textures);
    TraceVertices(initiator);
  }

  // Diagnostic (bo2zm_native_diag_vertices_ps): the fetch of each VS element, original and
  // patched, the bytes of the draw's first vertices and the center texels of its 8888
  // textures, as they are in guest memory.
  void TraceVertices(uint32_t initiator) {
    namespace g = rex::graphics;
    if (!vs_draw_ || !ps_draw_ || traces_ >= 20000 ||
        vs_microcode_.size() != vs_draw_->microcode.size()) {
      return;
    }
    const std::string list = REXCVAR_GET(bo2zm_native_diag_vertices_ps);
    bool in_list = false;
    uint32_t value = 0;
    bool has = false;
    for (char c : list + ",") {
      if (c >= '0' && c <= '9') {
        value = value * 10 + uint32_t(c - '0');
        has = true;
      } else if (has) {
        in_list = in_list || value == ps_draw_->number;
        value = 0;
        has = false;
      }
    }
    if (!in_list) {
      return;
    }
    ++traces_;
    std::string detail;
    uint32_t slot = UINT32_MAX;
    uint32_t stride = 0;
    for (const VertexElement& element : vs_draw_->elements) {
      const size_t i = size_t(element.instruction) * 3;
      const uint32_t o0 = vs_draw_->microcode[i], o1 = vs_draw_->microcode[i + 1];
      const uint32_t p0 = vs_microcode_[i], p1 = vs_microcode_[i + 1], p2 = vs_microcode_[i + 2];
      detail += fmt::format(" {}{}@{}: original r{} s{:03X} fmt{}; patched r{} op{} s{:03X} fmt{} f{} z{} o{} ({:08X} "
                             "{:08X} {:08X});",
                             NameUse(element.use), element.index_use, element.instruction,
                             (o0 >> 12) & 0x3F, o1 & 0xFFF, (o1 >> 16) & 0x3F, (p0 >> 12) & 0x3F,
                             p0 & 0x1F, p1 & 0xFFF, (p1 >> 16) & 0x3F,
                             ((p0 >> 20) & 0x1F) * 3 + ((p0 >> 25) & 0x3), p2 & 0xFF,
                             int32_t(p2 << 1) >> 9, p0, p1, p2);
      if (slot == UINT32_MAX && (p0 & 0x1F) == 0 && !((p1 >> 30) & 0x1)) {
        slot = ((p0 >> 20) & 0x1F) * 3 + ((p0 >> 25) & 0x3);
        stride = (p2 & 0xFF) * 4;
      }
    }
    if (slot < 96 && stride && stride <= 64) {
      const uint32_t f0 = DvarRecord(g::XE_GPU_REG_SHADER_CONSTANT_FETCH_00_0 + slot * 2);
      const uint32_t f1 = DvarRecord(g::XE_GPU_REG_SHADER_CONSTANT_FETCH_00_0 + slot * 2 + 1);
      const uint32_t source_kind = (initiator >> 6) & 0x3;
      uint32_t first = DvarRecord(g::XE_GPU_REG_VGT_INDX_OFFSET) & 0xFFFFFF;
      if (source_kind == uint32_t(xenos::SourceSelect::kDMA)) {
        const bool of_32 = (initiator >> 11) & 0x1;
        const uint32_t base_indices =
            DvarRecord(g::XE_GPU_REG_VGT_DMA_BASE) & (of_32 ? 0x1FFFFFFC : 0x1FFFFFFE);
        const auto order_indices =
            static_cast<xenos::Endian>(DvarRecord(g::XE_GPU_REG_VGT_DMA_SIZE) >> 30);
        if (of_32) {
          uint32_t raw;
          std::memcpy(&raw, memory_->TranslatePhysical(base_indices), 4);
          first = (first + (xenos::GpuSwap(raw, order_indices) & 0xFFFFFF)) & 0xFFFFFF;
        } else {
          uint16_t raw;
          std::memcpy(&raw, memory_->TranslatePhysical(base_indices), 2);
          first = (first + xenos::GpuSwap(raw, order_indices)) & 0xFFFFFF;
        }
      }
      const uint64_t address = uint64_t(f0 & 0x1FFFFFFC) + uint64_t(first) * stride;
      std::string bytes;
      if (address + uint64_t(4) * stride <= 0x20000000) {
        const uint8_t* data = memory_->TranslatePhysical(uint32_t(address));
        for (uint32_t b = 0; b < 4 * stride; ++b) {
          bytes += fmt::format("{}{:02X}", b % stride == 0 ? " | " : (b % 4 == 0 ? " " : ""),
                               data[b]);
        }
      }
      detail += fmt::format(" fetch f{} {:08X} {:08X} (order {}) source {} first {} stride {}:{}",
                             slot, f0, f1, f1 & 0x3, source_kind, first, stride, bytes);
    }
    for (const SamplerShader& s : ps_draw_->samplers) {
      if (s.reg_index >= 16) {
        continue;
      }
      const uint32_t base = g::XE_GPU_REG_SHADER_CONSTANT_FETCH_00_0 + uint32_t(s.reg_index) * 6;
      const uint32_t t0 = DvarRecord(base), t1 = DvarRecord(base + 1), t2 = DvarRecord(base + 2);
      if ((t0 & 0x3) != 2 || (t1 & 0x3F) != 6) {
        continue;
      }
      const uint32_t width = (t2 & 0x1FFF) + 1, height = ((t2 >> 13) & 0x1FFF) + 1;
      const uint32_t pitch = std::max<uint32_t>(((t0 >> 22) & 0x1FF) << 5, 1);
      const bool tiled = (t0 >> 31) & 0x1;
      const uint32_t y = height / 2;
      std::string texels;
      for (uint32_t k = 0; k < 8; ++k) {
        const uint32_t x = std::min(width - 1, (width > 8 ? width / 2 - 4 : 0) + k);
        const int64_t displacement = tiled ? OffsetTiledDiag(int32_t(x), int32_t(y), pitch, 2)
                                               : int64_t(y) * pitch * 4 + int64_t(x) * 4;
        const uint64_t d = uint64_t(t1 & 0x1FFFF000) + uint64_t(displacement);
        if (displacement < 0 || d + 4 > 0x20000000) {
          break;
        }
        const uint8_t* texel = memory_->TranslatePhysical(uint32_t(d));
        texels += fmt::format(" {:02X}{:02X}{:02X}{:02X}", texel[0], texel[1], texel[2], texel[3]);
      }
      detail += fmt::format("; t{} {}x{} {} pitch {} swizzle {:03X} order {} signs {:02X} row {}:{}",
                             s.reg_index, width, height, tiled ? "tiled" : "linear", pitch,
                             (DvarRecord(base + 3) >> 1) & 0xFFF, (t1 >> 6) & 0x3, (t0 >> 2) & 0xFF,
                             y, texels);
      const uint32_t address_texture = t1 & 0x1FFFF000;
      if (textures_dumped_.size() < 8 && textures_dumped_.insert(address_texture).second) {
        std::vector<uint8_t> dump(size_t(width) * height * 4);
        bool full = true;
        for (uint32_t yy = 0; yy < height && full; ++yy) {
          for (uint32_t xx = 0; xx < width; ++xx) {
            const int64_t dt = tiled ? OffsetTiledDiag(int32_t(xx), int32_t(yy), pitch, 2)
                                       : int64_t(yy) * pitch * 4 + int64_t(xx) * 4;
            const uint64_t texel = uint64_t(address_texture) + uint64_t(dt);
            if (dt < 0 || texel + 4 > 0x20000000) {
              full = false;
              break;
            }
            std::memcpy(dump.data() + (size_t(yy) * width + xx) * 4,
                        memory_->TranslatePhysical(uint32_t(texel)), 4);
          }
        }
        const auto path = rex::filesystem::GetExecutableFolder() /
                          fmt::format("diag_texture_{:08X}_{}x{}.bin", address_texture, width, height);
        std::ofstream file(path, std::ios::binary | std::ios::trunc);
        file.write(reinterpret_cast<const char*>(dump.data()), std::streamsize(dump.size()));
        detail += fmt::format(" (dumped to {}{})", path.filename().string(),
                               full ? "" : ", incomplete");
      }
    }
    detail += "; VS constants";
    for (uint32_t c = 0; c < 16; ++c) {
      const uint32_t b = 0x4000 + c * 4;
      detail += fmt::format(" c{}=({:.5g} {:.5g} {:.5g} {:.5g})", c, std::bit_cast<float>(DvarRecord(b)),
                             std::bit_cast<float>(DvarRecord(b + 1)), std::bit_cast<float>(DvarRecord(b + 2)),
                             std::bit_cast<float>(DvarRecord(b + 3)));
    }
    detail += "; PS constants";
    for (uint32_t c = 0; c < 4; ++c) {
      const uint32_t b = 0x4400 + c * 4;
      detail += fmt::format(" c{}=({:.5g} {:.5g} {:.5g} {:.5g})", c, std::bit_cast<float>(DvarRecord(b)),
                             std::bit_cast<float>(DvarRecord(b + 1)), std::bit_cast<float>(DvarRecord(b + 2)),
                             std::bit_cast<float>(DvarRecord(b + 3)));
    }
    REXLOG_INFO("[trace] vertices PS n{} VS n{}:{}", ps_draw_->number, vs_draw_->number,
                detail);
  }

  // GetTiledOffset2D (graphics/pipeline/texture/util.cpp), same as OffsetTiled2D in
  // bo2zm_native_draws.cpp; here only for the diagnostic. pitch in blocks.
  static int32_t OffsetTiledDiag(int32_t x, int32_t y, uint32_t pitch, uint32_t log2_bytes) {
    pitch = (pitch + 31) & ~uint32_t(31);
    const int32_t macro = ((x >> 5) + (y >> 5) * int32_t(pitch >> 5)) << (log2_bytes + 7);
    const int32_t micro = ((x & 7) + ((y & 0xE) << 2)) << log2_bytes;
    const int32_t offset = macro + ((micro & ~0xF) << 1) + (micro & 0xF) + ((y & 1) << 4);
    return ((offset & ~0x1FF) << 3) + ((y & 16) << 7) + ((offset & 0x1C0) << 2) +
           (((((y & 8) >> 2) + (x >> 3)) & 3) << 6) + (offset & 0x3F);
  }

  void TraceCopy(const CopyRegisters& r) {
    if (!tracing_ || traces_ >= 20000) {
      return;
    }
    ++traces_;
    REXLOG_INFO("[trace] copy control {:08X} target {:08X} info {:08X} pitch {:08X} surface {:08X} rt0 {:08X} depth "
                "{:08X} clear {:08X}/{:08X} depth_clear {:08X}",
                r.rb_copy_control, r.rb_copy_dest_base, r.rb_copy_dest_info, r.rb_copy_dest_pitch,
                r.rb_surface_info, r.rb_color_info[0], r.rb_depth_info, r.rb_color_clear,
                r.rb_color_clear_lo, r.rb_depth_clear);
  }

  // The last VS (type 0) or PS (type 1) uploaded to the ring.
  // IM_LOAD with a cache. The game reloads the same shaders on almost every draw: in gameplay on the PC,
  // 70,000-97,000 loads per second of about 82 distinct microcodes. Each load byte-swapped the data,
  // computed the XXH3 to identify it and copied the VS, and each new VS then repeated the XXH3 in
  // HashVs and in AnnotateInputVertices. If the address, size and bytes match an earlier load, its
  // swapped copy, identification, fingerprint and generation are reused. The bytes must be compared:
  // D3D patches VS microcode in place, so the address alone is not enough. Same microcode, same
  // generation: whatever is cached per generation (vertex input, fingerprint, coherence) stays valid.
  //
  // IM_LOAD without memcmp (bo2zm_native_im_load_without_memcmp). The memcmp above cost ~0.6 us per IM_LOAD
  // (~41,000 per second in gameplay). Whoever writes microcode announces it (the D3D constructors and the
  // fetch patcher; the list and the cross-thread protocol are in bo2zm_microcode_versions.h). Each way
  // records the versions of its slot it was checked against guest memory with; if they are unchanged,
  // nobody has written that microcode since, and it is used without reading it.
  // Self-checking guard:
  //   - watching: always the regular memcmp and, if the shortcut had a candidate, it is compared with the
  //     memcmp result. After kImShortcutALook agreements, no disagreement and at least one in-place patch
  //     seen (proof that the sub_825A2FB8 hook runs: without it, a patch would not bump any version), it
  //     moves to applying;
  //   - applying: the candidate is used without memcmp, except 1 in every kImShortcutCheckEvery, which is
  //     still compared;
  //   - a disagreement (the candidate no longer matches memory) or a patcher call that is neither of the
  //     two known ones: "MISMATCH" in the log and the shortcut turned off for the run. That load and
  //     the following ones use memcmp.
  void LoadShaderCached(uint32_t address_type, uint32_t size, const uint8_t* source) {
    const uint32_t type = address_type & 0x3;
    const size_t bytes = size_t(size) * sizeof(uint32_t);
    auto& ways = loads_cache_[type][size_t((uint64_t(address_type) * 0x9E3779B97F4A7C15ull) >> 57)];
    if (im_shortcut_phase_ == kImShortcutNoBegin) {
      BeginImShortcut();
    }
    const uint32_t slot = microcode::SlotOf(address_type & ~uint32_t(0x3));
    // 1. The shortcut: a way with the same contents, checked with the current versions.
    ShaderLoad* candidate = nullptr;
    if (im_shortcut_phase_ != kImShortcutOff) {
      const uint32_t global = microcode::GlobalRelaxed();
      for (ShaderLoad& via : ways) {
        if (!via.validated || via.address_type != address_type || via.size != size) {
          continue;
        }
        if (via.version_global != global) {
          // Someone has written microcode since it was checked: if not in its slot, it is still valid.
          if (via.version_slot != microcode::StartAfterGlobal(slot)) {
            via.validated = false;
            ++im_i_version_changed_;
            continue;
          }
          if (microcode::g_patches_others.load(std::memory_order_relaxed) != 0) {
            TurnOffImShortcutPerOthers();
            break;
          }
          via.version_global = global;
        }
        candidate = &via;
        break;
      }
    }
    ShaderLoad* load = nullptr;
    if (candidate && im_shortcut_phase_ == kImShortcutApplying &&
        (++im_shortcut_turn_ & (kImShortcutCheckEvery - 1)) != 0) {
      // 2. Without reading the guest microcode.
      load = candidate;
      ++loads_cached_;
      ++im_i_no_memcmp_;
    } else {
      // 3. The regular path, with the versions read before and after reading guest memory.
      const bool validate = im_shortcut_phase_ != kImShortcutOff;
      microcode::Readback read;
      if (validate) {
        read = microcode::BeforeRead(slot);
      }
      for (ShaderLoad& via : ways) {
        if (via.address_type == address_type && via.size == size &&
            std::memcmp(via.raw.data(), source, bytes) == 0) {
          load = &via;
          break;
        }
      }
      if (validate && candidate) {
        if (load == candidate) {
          ++im_shortcut_checked_;
          ++im_i_checked_;
          if (im_shortcut_phase_ == kImShortcutObserving && im_shortcut_checked_ >= kImShortcutALook &&
              microcode::g_patches_in_room.load(std::memory_order_relaxed) != 0) {
            im_shortcut_phase_ = kImShortcutApplying;
            BO2ZM_REPORT_RING("[native] C5a IM_LOAD without memcmp: {} loads checked against the memcmp, 0 "
                                 "disagreements and {} in-place patches seen: APPLYING, the memcmp is skipped if the "
                                 "version has not changed (1 in {} is still checked)",
                                 im_shortcut_checked_,
                                 microcode::g_patches_in_room.load(std::memory_order_relaxed),
                                 kImShortcutCheckEvery);
          }
        } else {
          TurnOffImShortcut(address_type, size, candidate->raw, candidate->version_slot,
                        candidate->version_global, load != nullptr, source, read);
        }
      }
      if (load) {
        ++loads_cached_;
      } else {
        load = ways[0].use <= ways[1].use ? &ways[0] : &ways[1];
        load->address_type = address_type;
        load->size = size;
        load->raw.resize(size);
        std::memcpy(load->raw.data(), source, bytes);
        load->host.resize(size);
        // The fresh copy is byte-swapped without reading game memory again: raw and host hold the same
        // contents, the ones the versions certify (with the guest idle, the same as before).
        const uint8_t* copy = reinterpret_cast<const uint8_t*>(load->raw.data());
        for (uint32_t i = 0; i < size; ++i) {
          load->host[i] = rex::memory::load_and_swap<uint32_t>(copy + size_t(i) * 4);
        }
        load->input = shaders_.loaded() ? shaders_.Identify(type == 0, load->host) : nullptr;
        load->hash = XXH3_64bits(load->host.data(), bytes);
        load->generation = ++generations_microcode_;
        load->validated = false;
      }
      if (validate && im_shortcut_phase_ != kImShortcutOff) {
        ++im_i_with_memcmp_;
        if (microcode::AfterRead(slot, read)) {
          load->validated = true;
          load->version_slot = read.start;
          load->version_global = read.global;
        } else {
          load->validated = false;  // someone wrote to its slot during the read: it will be compared again
          ++im_i_cancelled_;
        }
      }
    }
    load->use = ++loads_tic_;
    (type == 0 ? vs_current_ : ps_current_) = load->input;
    if (tracing_ && traces_ < 20000) {
      ++traces_;
      REXLOG_INFO("[trace] IM_LOAD {} n{} ({} words, hash {:016X})", type == 0 ? "VS" : "PS",
                  load->input ? int(load->input->number) : -1, load->host.size(), load->hash);
    }
    if (type == 0) {
      vs_microcode_ = load->host;  // patched: the vertex input comes from here
      generation_vs_ = load->generation;
      hash_vs_generation_ = load->generation;
      hash_vs_ = load->hash;
    }
  }

  // IM_LOAD without memcmp. The first load reads the cvar.
  void BeginImShortcut() {
    const bool requested = REXCVAR_GET(bo2zm_native_im_load_without_memcmp);
    im_shortcut_phase_ = requested && microcode::kCreationsWatched ? kImShortcutObserving : kImShortcutOff;
    BO2ZM_REPORT_RING("[native] C5a IM_LOAD without memcmp (bo2zm_native_im_load_without_memcmp) = {}",
                         !requested ? "no (off by the cvar): memcmp on every IM_LOAD"
                         : !microcode::kCreationsWatched
                             ? "no: with BO2ZM_NATIVE_SHADER_LIBRARY the constructors do not report"
                             : "YES, observing: each load is also compared with memcmp until there is proof");
  }

  const char* NamePhaseImShortcut() const {
    switch (im_shortcut_phase_) {
      case kImShortcutObserving:
        return "observing";
      case kImShortcutApplying:
        return "applying";
      case kImShortcutOff:
        return "OFF";
      default:
        return "not started";
    }
  }

  // The shortcut's candidate does not match guest memory: someone wrote microcode without announcing it.
  // (Its fields are passed separately: ShaderLoad is declared further down and cannot appear in the
  // signature.)
  void TurnOffImShortcut(uint32_t address_type, uint32_t size, const std::vector<uint32_t>& raw,
                     uint32_t version_slot, uint32_t version_global, bool other_via, const uint8_t* source,
                     const microcode::Readback& read) {
    uint32_t first = 0;
    while (first < size && std::memcmp(raw.data() + first, source + size_t(first) * 4, 4) == 0) {
      ++first;
    }
    REXLOG_ERROR("[native] C5a IM_LOAD without memcmp: MISMATCH. The shortcut gave the copy of {} {:08X} ({} words) "
                 "checked with slot version {} and global {}, and the guest memory is already different (first "
                 "different word: {}; the memcmp {}). Current versions: start {} end {} global {}; phase {} with {} "
                 "loads checked. Reported writes: {} in-place patches, {} in the ring copy, {} others, {} creations. "
                 "Someone writes microcode without reporting it: shortcut OFF for the rest of the session (every load "
                 "with memcmp, as before 184)",
                 (address_type & 0x3) == 0 ? "VS" : "PS", address_type & ~uint32_t(0x3), size,
                 version_slot, version_global, first,
                 other_via ? "finds another way" : "has to copy it again", read.start, read.end,
                 read.global, NamePhaseImShortcut(), im_shortcut_checked_,
                 microcode::g_patches_in_room.load(std::memory_order_relaxed),
                 microcode::g_patches_in_copy.load(std::memory_order_relaxed),
                 microcode::g_patches_others.load(std::memory_order_relaxed),
                 microcode::g_creations.load(std::memory_order_relaxed));
    im_shortcut_phase_ = kImShortcutOff;
  }

  // The fetch patcher was called from outside its two known call sites.
  void TurnOffImShortcutPerOthers() {
    REXLOG_ERROR("[native] C5a IM_LOAD without memcmp: MISMATCH. The fetch patcher (sub_825A2FB8) has been called {} "
                 "times from outside its two known calls (return {:08X}, target {:08X}): it may write microcode some "
                 "other way. Shortcut OFF for the rest of the session (every load with memcmp, as before 184)",
                 microcode::g_patches_others.load(std::memory_order_relaxed),
                 microcode::g_other_return.load(std::memory_order_relaxed),
                 microcode::g_other_target.load(std::memory_order_relaxed));
    im_shortcut_phase_ = kImShortcutOff;
  }

  // IM_LOAD_IMMEDIATE (and unusual IM_LOAD loads): no cache.
  void IdentifyShader(uint32_t type) {
    if (type > 1) {
      return;
    }
    const ShaderInput* input =
        shaders_.loaded() ? shaders_.Identify(type == 0, microcode_) : nullptr;
    (type == 0 ? vs_current_ : ps_current_) = input;
    if (tracing_ && traces_ < 20000) {
      ++traces_;
      REXLOG_INFO("[trace] IM_LOAD {} n{} ({} words, hash {:016X})", type == 0 ? "VS" : "PS",
                  input ? int(input->number) : -1, microcode_.size(),
                  XXH3_64bits(microcode_.data(), microcode_.size() * sizeof(uint32_t)));
    }
    if (type == 0) {
      vs_immediate_ = microcode_;  // patched: the vertex input comes from here
      vs_microcode_ = vs_immediate_;
      generation_vs_ = ++generations_microcode_;
    }
  }

  // IM_LOAD_IMMEDIATE with an exact cache (bo2zm_native_im_immediate_cache).
  //
  // Who sends them: sub_825A37D8, the D3D copy path. If the GPU has not yet passed the fence of a VS's
  // last IM_LOAD, or outputs the PS does not read must be disabled (sub_825A36A8), it copies the microcode
  // at [VS+40] ([VS+600] bytes) into the ring itself and patches it there (sub_825A2FB8 on the copy).
  // Always type 0 (VS).
  // In gameplay there are stretches with 4,000-8,000 per second at 1.5-2.8 us each ("times per
  // packet", op 2B): the regular path byte-swaps word by word, computes the XXH3 in Identify, copies
  // the VS to vs_immediate_ and gives it a new generation, so the next draw redoes the fingerprint
  // (another XXH3 in HashVs), the fetch coherence and the vertex input key. And the contents repeat:
  // over a whole run there are 82 distinct microcodes between IM_LOAD and immediate loads.
  //   - Set chosen by a cheap signature (size and four words, not swapped). Within the set, the way with
  //     the same signature, the same size and the same bytes (memcmp against its raw copy): equality is
  //     exact; the signature only chooses where to look.
  //   - Hit: the way's swapped copy, identification, fingerprint and generation. Same microcode, same
  //     generation, as in the IM_LOAD cache (LoadShaderCached): whatever is cached per generation
  //     stays valid.
  //   - Miss, or a packet that wraps around the ring (not contiguous): the regular path; the miss is
  //     stored.
  // Self-checking guard:
  //   - watching: every load takes the regular path and, if the cache had a way, it is compared with what
  //     the cache would give (swapped copy and identification). After kImmALook agreements and no
  //     disagreement it moves to applying;
  //   - applying: hits neither swap nor identify; 1 in every kImmCheckEvery is still compared;
  //   - a disagreement, or a new way whose swapped raw copy is not what the regular path read (the ring
  //     position arithmetic): "MISMATCH" in the log and the cache turned off for the run. That load
  //     already uses the regular result.
  struct ImmediateLoad {
    uint64_t signature = 0;  // 0 = empty way
    uint32_t size = 0;
    std::vector<uint32_t> raw;  // as it is in the ring (big-endian)
    std::vector<uint32_t> host;   // swapped: what gets identified and what the vertex input reads
    const ShaderInput* input = nullptr;
    uint64_t hash = 0;      // host XXH3, the same one HashVs would compute
    uint64_t generation = 0;  // from generations_microcode_, when this content was stored
    uint64_t use = 0;         // the least recently used way is the one replaced
  };

  void LoadImmediate(RingReader data, uint32_t type, uint32_t size) {
    if (imm_phase_ == kImmNoBegin) {
      BeginImmediate();
    }
    const bool back = data.mask != 0 && size_t(data.pos) + size > size_t(data.mask) + 1;
    if (imm_phase_ == kImmOff || type > 1 || size == 0 || back) {
      if (back) {
        ++imm_i_back_;
      } else {
        ++imm_i_no_cache_;
      }
      LoadImmediateNoCache(data, type, size);
      return;
    }
    const uint8_t* raw = data.base + size_t(data.pos) * 4;
    const size_t bytes = size_t(size) * sizeof(uint32_t);
    const uint64_t signature = SignatureImmediate(raw, size);
    const size_t index = size_t(signature >> (64 - kImmBitsSet));
    auto& set = imm_cache_[type][index];
    uint8_t& last_way = imm_last_way_[type][index];
    ImmediateLoad* via = nullptr;
    for (uint32_t k = 0; k < kImmWays; ++k) {  // first the way of the set's last hit
      const uint32_t v = (last_way + k) % kImmWays;
      ImmediateLoad& candidate = set[v];
      if (candidate.signature == signature && candidate.size == size &&
          std::memcmp(candidate.raw.data(), raw, bytes) == 0) {
        via = &candidate;
        last_way = uint8_t(v);
        break;
      }
    }
    if (via && imm_phase_ == kImmApplying && (++imm_turn_ & (kImmCheckEvery - 1)) != 0) {
      // Hit: no byte swap, no identification, no copy, no new generation.
      via->use = ++imm_tic_;
      UseImmediate(type, *via);
      ++imm_i_hits_;
      ++loads_cached_;
      if (tracing_ && traces_ < 20000) {
        ++traces_;
        REXLOG_INFO("[trace] IM_LOAD {} n{} ({} words, hash {:016X}; IM_LOAD_IMMEDIATE from the cache)",
                    type == 0 ? "VS" : "PS", via->input ? int(via->input->number) : -1, via->host.size(),
                    via->hash);
      }
      return;
    }
    // The regular path: swaps, identifies and leaves its result in use
    // (vs_current_ or ps_current_, vs_immediate_...).
    LoadImmediateNoCache(data, type, size);
    const ShaderInput* input = type == 0 ? vs_current_ : ps_current_;
    if (via) {
      // Check: what the cache would have given against what the regular path just gave.
      uint32_t first = 0;
      while (first < size && first < via->host.size() && via->host[first] == microcode_[first]) {
        ++first;
      }
      if (first != size || via->host.size() != size || via->input != input) {
        TurnOffImmediate("a cache hit does not give the same as the usual path", type, size, signature, first,
                        via->input, input);
        return;
      }
      ++imm_checked_;
      ++imm_i_checked_;
      via->use = ++imm_tic_;
      if (imm_phase_ == kImmObserving && imm_checked_ >= kImmALook) {
        imm_phase_ = kImmApplying;
        BO2ZM_REPORT_RING("[native] C5a IM_LOAD_IMMEDIATE with a cache: {} hits checked against the usual path and 0 "
                             "disagreements: APPLYING, hits are no longer flipped or identified (1 in {} is still "
                             "checked)",
                             imm_checked_, kImmCheckEvery);
      }
      if (imm_phase_ == kImmApplying) {
        UseImmediate(type, *via);  // the same generation as the unchecked hits
      }
      return;
    }
    // Miss: into the least recently used way of the set.
    ++imm_i_misses_;
    uint32_t room = 0;
    for (uint32_t k = 1; k < kImmWays; ++k) {
      if (set[k].use < set[room].use) {
        room = k;
      }
    }
    ImmediateLoad& fresh = set[room];
    fresh.signature = signature;
    fresh.size = size;
    fresh.raw.resize(size);
    std::memcpy(fresh.raw.data(), raw, bytes);
    fresh.host.assign(microcode_.begin(), microcode_.end());
    fresh.input = input;
    fresh.hash = XXH3_64bits(fresh.host.data(), bytes);
    fresh.generation = ++generations_microcode_;
    fresh.use = ++imm_tic_;
    last_way = uint8_t(room);
    // The raw copy comes from the packet's position in the ring: once swapped it must equal what the
    // regular path read word by word. Only on misses (a few dozen per run).
    const uint8_t* copy = reinterpret_cast<const uint8_t*>(fresh.raw.data());
    for (uint32_t i = 0; i < size; ++i) {
      if (rex::memory::load_and_swap<uint32_t>(copy + size_t(i) * 4) != fresh.host[i]) {
        fresh.signature = 0;  // empty way: cannot come out again
        fresh.use = 0;
        TurnOffImmediate("the raw copy of a new way is not what the usual path read", type, size, signature,
                        i, input, input);
        return;
      }
    }
    if (imm_phase_ == kImmApplying) {
      UseImmediate(type, fresh);
    }
  }

  // The regular IM_LOAD_IMMEDIATE path (formerly inline in PacketType3).
  void LoadImmediateNoCache(RingReader data, uint32_t type, uint32_t size) {
    microcode_.resize(size);
    for (uint32_t i = 0; i < size; ++i) {
      microcode_[i] = data.Read();
    }
    IdentifyShader(type);
  }

  // The same state IdentifyShader leaves, taken from the way (without swapping, identifying or
  // hashing).
  void UseImmediate(uint32_t type, const ImmediateLoad& via) {
    (type == 0 ? vs_current_ : ps_current_) = via.input;
    if (type == 0) {
      vs_microcode_ = via.host;  // patched: the vertex input comes from here
      generation_vs_ = via.generation;
      hash_vs_generation_ = via.generation;
      hash_vs_ = via.hash;
    }
  }

  // Cheap signature of an unswapped packet: picks the set and rejects other shaders' ways before the
  // memcmp.
  static uint64_t SignatureImmediate(const uint8_t* raw, uint32_t size) {
    const size_t positions[4] = {0, size_t(size) / 3, size_t(size) * 2 / 3, size_t(size) - 1};
    uint64_t h = uint64_t(size) * 0x9E3779B97F4A7C15ull;
    for (const size_t p : positions) {
      uint32_t word;
      std::memcpy(&word, raw + p * 4, sizeof(word));
      h = (h ^ word) * 0xBF58476D1CE4E5B9ull;
      h ^= h >> 29;
    }
    return h | 1;  // never 0: 0 means an empty way
  }

  void BeginImmediate() {
    const bool requested = REXCVAR_GET(bo2zm_native_im_immediate_cache);
    imm_phase_ = requested ? kImmObserving : kImmOff;
    BO2ZM_REPORT_RING("[native] C5a IM_LOAD_IMMEDIATE with a cache (bo2zm_native_im_immediate_cache) = {}",
                         requested ? "YES, observing: each hit is compared with the usual path until there is proof"
                                : "no (off by the cvar): each IM_LOAD_IMMEDIATE is flipped and identified, as before");
  }

  const char* NamePhaseImmediate() const {
    switch (imm_phase_) {
      case kImmObserving:
        return "observing";
      case kImmApplying:
        return "applying";
      case kImmOff:
        return "OFF";
      default:
        return "not started";
    }
  }

  // The cache does not match the regular path: it is turned off for the rest of the run.
  void TurnOffImmediate(const char* reason, uint32_t type, uint32_t size, uint64_t signature, uint32_t first,
                       const ShaderInput* of_cache, const ShaderInput* of_always) {
    REXLOG_ERROR("[native] C5a IM_LOAD_IMMEDIATE with a cache: MISMATCH, {}: {} of {} words (signature {:016X}), first "
                 "different word {}, shader from the cache n{} and from the usual path n{}; phase {} with {} hits "
                 "checked. Cache OFF for the rest of the session (everything by the usual path, as before 184); this "
                 "load already goes by the usual path",
                 reason, type == 0 ? "VS" : "PS", size, signature, first,
                 of_cache ? int(of_cache->number) : -1, of_always ? int(of_always->number) : -1,
                 NamePhaseImmediate(), imm_checked_);
    imm_phase_ = kImmOff;
  }

  void CountDrawShaders() {
    if (!shaders_.loaded()) {
      return;
    }
    if (!vs_current_) {
      ++draws_no_vs_;
    } else if (!ps_current_) {
      ++draws_no_ps_;
    } else {
      ++draws_identified_;
    }
    const uint64_t pair = (uint64_t(vs_current_ ? vs_current_->number + 1 : 0) << 32) |
                         (ps_current_ ? ps_current_->number + 1 : 0);
    // In gameplay there are 40 distinct pairs and ~2,200 draws per frame, mostly runs of the same
    // material. Remembering the previous pair avoids the set insert (a hash and a possible cache
    // miss) on every draw.
    if (pair != last_pair_shaders_) {
      last_pair_shaders_ = pair;
      // Open addressing in a flat table, with no allocation.
      if (pairs_count_ < kPairSlots / 2) {
        for (uint32_t i = uint32_t((pair * 0x9E3779B97F4A7C15ull) >> 52);; i = (i + 1) & (kPairSlots - 1)) {
          if (pairs_[i] == pair) {
            break;
          }
          if (pairs_[i] == 0) {
            pairs_[i] = pair;
            ++pairs_count_;
            break;
          }
        }
      }
    }
  }

  // The game thread's Draw* record that corresponds to this ring draw. Records arrive in the same order as the draws;
  // the primitive type and count are checked, and at most 8 records without a draw are skipped.
  // If the last marker carries its Draw* record and this draw accepts it (the usual acceptance: type, count and
  // shaders consistent with the IM_LOAD packets), apply mode uses it without the queue or the search. If it is not
  // accepted, it is kept for the next draw (like the head of the queue) and the usual search runs. In check mode the
  // usual search runs and the results of the two paths are compared (CompareDrawMarker).
  void PairDraw() {
    vs_draw_ = nullptr;
    ps_draw_ = nullptr;
    reason_paired_ = 0;
    object_vs_ = object_ps_ = 0;
    vegetation_game_ = 0;  // without a record there is no game verdict to compare
    if (!shaders_.loaded()) {
      return;
    }
    RefreshObjects();  // a single acquire load per draw (formerly four)
    const uint32_t initiator = DvarRecord(rex::graphics::XE_GPU_REG_VGT_DRAW_INITIATOR);
    const uint32_t type = initiator & 0x3F;
    const uint32_t count = initiator >> 16;
    const uint32_t mode_marker = draw_marker_mode_;
    bool marker_fine = false;
    if (mode_marker != 0) {
      marker_fine = draw_marker_.args[0] == type && RegisterCount(draw_marker_) == count &&
                      ShadersCoherent(draw_marker_);
      if (!marker_fine) {
        ++marker_rejected_registers_;
      } else {
        draw_marker_mode_ = 0;  // accepted: valid for this draw and no other
        if (mode_marker == kDrawApply) {
          ++draws_with_marker_register_;
          UseDrawRegister(draw_marker_);
          return;
        }
      }
    }
    DrawRecord reg_index;
    while (pending_.size() < 64 && ExtractDraw(reg_index)) {
      pending_.push_back(reg_index);
    }
    size_t found = SIZE_MAX;
    bool incoherent = false;
    // The size of a deque is not a field, it is computed by subtracting iterators. It was
    // queried on every loop iteration, and there are ~2,200 draws per frame.
    const size_t candidates = std::min<size_t>(pending_.size(), 8);
    for (size_t i = 0; i < candidates; ++i) {
      if (pending_[i].args[0] != type || RegisterCount(pending_[i]) != count) {
        continue;
      }
      // Type and count are not enough: in a series of identical draws, a record
      // without a draw in the ring shifts the matching. The record's VS and PS
      // must be those of the last IM_LOAD packets (observed: a VS matched with
      // another shader's microcode).
      if (!ShadersCoherent(pending_[i])) {
        ++candidates_incoherent_;
        incoherent = true;
        continue;
      }
      found = i;
      break;
    }
    if (mode_marker == kDrawCheck && marker_fine) {
      CompareDrawMarker(found == SIZE_MAX ? nullptr : &pending_[found], type, count);
    }
    if (found == SIZE_MAX) {
      ++draws_no_register_;
      draws_incoherent_ += incoherent;
      reason_paired_ = incoherent ? 2 : 1;
      if (warnings_c5b_ < 16) {
        ++warnings_c5b_;
        std::string first;
        if (!pending_.empty()) {
          const DrawRecord& r = pending_.front();
          first = fmt::format("; the first pending one is function {} type {} count {}",
                                int(r.function), r.args[0], RegisterCount(r));
        }
        REXLOG_WARN("[native] C5b: ring draw without a Draw* record: type {} count {} ({} pending{})",
                    type, count, pending_.size(), first);
      }
      UseRingIdentity();
      return;
    }
    registers_skipped_ += found;
    const DrawRecord r = pending_[found];
    pending_.erase(pending_.begin(), pending_.begin() + std::ptrdiff_t(found + 1));
    UseDrawRegister(r);
  }

  // The usual work with the matched record, whether it came from the queue or from the marker.
  void UseDrawRegister(const DrawRecord& r) {
    if (r.shadow) {
      CompareShadow(r.shadow, r);  // the D3D shadow
    }
    object_vs_ = r.vs;
    object_ps_ = r.ps;
    ++draws_paired_;
    vs_draw_ = CachedObjectShader(r.vs);
    ps_draw_ = CachedObjectShader(r.ps);
    // The game's vegetation verdict is only compared when drawing with this record's shaders; not with
    // the ring identity (UseRingIdentity, below) (bo2zm_d3d_game_vegetation).
    vegetation_game_ = IdentityForVegetation(r.vegetation, vs_draw_ != nullptr && ps_draw_ != nullptr);
    if (vs_draw_ && ps_draw_) {
      // Only when the VS or its microcode changes: it used to be one map write per draw.
      if ((vs_draw_ != mapped_vs_ || generation_vs_ != mapped_generation_) &&
          vs_per_microcode_.size() < 4096) {
        vs_per_microcode_[HashVs()] = vs_draw_;
        mapped_vs_ = vs_draw_;
        mapped_generation_ = generation_vs_;
      }
    } else {
      UseRingIdentity();
    }
    draws_with_vs_ += vs_draw_ != nullptr;
    draws_with_ps_ += ps_draw_ != nullptr;
    if (vs_draw_ && (vs_draw_ != annotated_vs_ || generation_vs_ != annotated_generation_)) {
      annotated_vs_ = vs_draw_;
      annotated_generation_ = generation_vs_;
      AnnotateInputVertices(*vs_draw_);
    }
  }

  // The VS and PS that would be used to draw with this record (null: no record), without changing anything: what
  // UseDrawRegister and UseRingIdentity would do.
  std::pair<const ShaderInput*, const ShaderInput*> ShadersWithRegister(const DrawRecord* r) {
    const ShaderInput* vs = r ? CachedObjectShader(r->vs) : nullptr;
    const ShaderInput* ps = r ? CachedObjectShader(r->ps) : nullptr;
    // UseRingIdentity also identifies a depth-only draw (RB_MODECONTROL 5) without a PS, so the same is done here;
    // otherwise every depth-only draw of BO2 (PS 0 bound) would compare as different (about a quarter of all checks).
    const bool only_depth = (DvarRecord(rex::graphics::XE_GPU_REG_RB_MODECONTROL) & 0x7) == 5;
    if ((vs && ps) || (!ps_current_ && !only_depth)) {
      return {vs, ps};
    }
    const ShaderInput* identity = vs_current_;
    if (!identity) {
      const auto it = vs_per_microcode_.find(HashVs());
      if (it == vs_per_microcode_.end()) {
        return {vs, ps};
      }
      identity = it->second;
    }
    return {identity, ps_current_};
  }

  /*
   * The comparison made while observing (and for 1 in every 1,024 draws afterwards). Only when the
   * draw accepts the marker's record: if it does not, the marker path ends in the same search and gives
   * the same result by construction. What decides the draw is compared: the VS and PS it would be drawn
   * with using the record from the search (or the ring identity if none is found) and using the marker's
   * record. A different record with the same shaders is only counted.
   */
  void CompareDrawMarker(const DrawRecord* lookup, uint32_t type, uint32_t count) {
    const auto with_lookup = ShadersWithRegister(lookup);
    const auto with_marker = ShadersWithRegister(&draw_marker_);
    const DrawRecord& m = draw_marker_;
    uint32_t kind = 0;  // 0 equal; 1 different shaders; 3 search finds no record and the identity is not valid
    if (with_lookup != with_marker) {
      kind = lookup ? 1u : 3u;
    } else if (!lookup || lookup->function != m.function || lookup->vs != m.vs || lookup->ps != m.ps ||
               lookup->args[1] != m.args[1] || lookup->args[2] != m.args[2] || lookup->args[3] != m.args[3]) {
      ++checks_draw_other_register_;  // another record (or the identity), same shaders
    }
    ++checks_draw_;
    if (kind != 0) {
      ++checks_draw_different_;
      if (warnings_draw_marker_ < 8) {
        ++warnings_draw_marker_;
        const auto number = [](const ShaderInput* e) { return e ? int(e->number) : -1; };
        REXLOG_ERROR("[native] draw record: the lookup and the marker do not agree (case {}) in a draw of "
                     "type {} count {}: lookup {} (VS {:08X} PS {:08X}) -> VS n{} PS n{}; marker (VS {:08X} PS {:08X}) "
                     "-> VS n{} PS n{}. The game thread switches the draw records off",
                     kind, type, count, lookup ? "with a record" : "without a record", lookup ? lookup->vs : 0u,
                     lookup ? lookup->ps : 0u, number(with_lookup.first), number(with_lookup.second), m.vs,
                     m.ps, number(with_marker.first), number(with_marker.second));
      }
    }
    AnnotateCheckDraw(kind == 0, kind, lookup, &m);
  }

  // The kWordsDraw words of the Draw* record (written by BeforeDraw and WriteMarker in bo2zm_d3d_marker.cpp). false if
  // the function is not a Draw* one.
  static bool ReadMarkerDraw(const uint8_t* p, DrawRecord& r) {
    // The low byte is the function and the 16 bits above it the game's vegetation verdict
    // (DrawRecord::vegetation, bo2zm_d3d_game_vegetation); the high byte must be 0.
    const uint32_t word = rex::memory::load_and_swap<uint32_t>(p);
    const uint32_t function = word & 0xFFu;
    if (function > uint32_t(DrawFunction::kIndexedUP) || (word >> 24) != 0) {
      return false;
    }
    r.function = DrawFunction(function);
    r.vegetation = uint16_t(word >> 8);
    r.vs = rex::memory::load_and_swap<uint32_t>(p + 4);
    r.ps = rex::memory::load_and_swap<uint32_t>(p + 8);
    for (uint32_t i = 0; i < 4; ++i) {
      r.args[i] = rex::memory::load_and_swap<uint32_t>(p + 12 + 4 * i);
    }
    r.shadow = (uint64_t(rex::memory::load_and_swap<uint32_t>(p + 28)) << 32) |
               rex::memory::load_and_swap<uint32_t>(p + 32);
    return true;
  }

  /*
   * The D3D shadow (docs/native-renderer.md).
   *
   * The snapshot of the device mirror taken in this draw's Draw* call, against the state the ring has
   * read from the packets right when it reaches this draw. Everything that matches is state the renderer
   * can read from the mirror without packets; anything else has to come through another path.
   */
  void CompareShadow(uint64_t sequence, const DrawRecord& record) {
    if (!ReadSnapshot(sequence, shadow_shot_)) {
      ++shadow_lost_;
      return;
    }
    ++shadow_draws_;
    const MirrorSnapshot& f = shadow_shot_;
    for (uint32_t g = 0; g < kGroupsMirror; ++g) {
      const uint64_t m = f.mask[g];
      for (uint32_t i = 0; m && i < 64; ++i) {
        if (!((m >> (63 - i)) & 1)) {
          continue;
        }
        const uint32_t reg = f.base_register[g] + i;
        ++shadow_state_compared_;
        if (DvarRecord(reg) != f.state[g][i]) {
          ++shadow_state_different_;
          ++shadow_per_register_[reg];
        }
      }
    }
    for (uint32_t i = 0; i < 192; ++i) {
      ++shadow_fetch_compared_;
      if (DvarRecord(0x4800 + i) != f.fetch[i]) {
        ++shadow_fetch_different_;
        ++shadow_per_register_[0x4800 + i];
      }
    }
    // The float4 registers the draw's shaders read (ShaderInput::constants_runs, xenia's microcode
    // analysis; without valid runs, every register below its constants_bytes; none for a missing PS), so the
    // differences that matter to a draw are counted apart from leftovers no shader of the draw reads.
    uint64_t read[8] = {};  // VS registers 0-255, then PS 256-511
    const auto mark = [&read](const ShaderInput* s, uint32_t offset) {
      if (!s) {
        return;
      }
      if (s->constants_runs_valid) {
        for (const auto& [first, n] : s->constants_runs) {
          for (uint32_t r = first; r < uint32_t(first) + n && r < 256; ++r) {
            read[(offset + r) >> 6] |= uint64_t(1) << ((offset + r) & 63);
          }
        }
      } else {
        for (uint32_t r = 0; r < 256 && r * 16 < s->constants_bytes; ++r) {
          read[(offset + r) >> 6] |= uint64_t(1) << ((offset + r) & 63);
        }
      }
    };
    mark(CachedObjectShader(record.vs), 0);
    mark(CachedObjectShader(record.ps), 256);
    for (uint32_t i = 0; i < 2048; ++i) {
      ++shadow_constants_compared_;
      const uint32_t ring = DvarRecord(0x4000 + i);
      const bool reads = (read[(i >> 2) >> 6] >> ((i >> 2) & 63)) & 1;
      shadow_constants_read_compared_ += reads;
      if (ring != f.constants[i]) {
        ++shadow_constants_different_;
        shadow_constants_read_different_ += reads;
        shadow_constants_read_unknown_ += reads && f.constants_from[i] == kFromUnknown;
        ++shadow_per_register_[0x4000 + i];
        if (reads) {
          ++shadow_per_register_read_[0x4000 + i];
        }
        // By where the shadow's word came from, and whether the ring has the mirror's own value (then
        // the overlay was stale or out of order) or another one (a path the shadow does not see).
        const uint32_t from = f.constants_from[i] < 5 ? f.constants_from[i] : 4;
        ++shadow_from_[from][ring == f.constants_mirror[i] ? 1 : 0];
      }
    }
  }

  // Whether the VS and PS of a Draw* record are the ones the ring has loaded.
  // The PS arrives unpatched: its microcode is compared. For the patched VS, the only
  // possible check is that its fetches write to the original's registers.
  bool ShadersCoherent(const DrawRecord& reg_index) {
    const ShaderInput* ps = CachedObjectShader(reg_index.ps);
    if (ps && ps_current_ && ps->hash != ps_current_->hash) {
      return false;
    }
    const ShaderInput* vs = CachedObjectShader(reg_index.vs);
    if (!vs) {
      return true;
    }
    if (vs != coherence_vs_ || generation_vs_ != coherence_generation_) {
      coherence_vs_ = vs;
      coherence_generation_ = generation_vs_;
      coherence_ = FetchCoherent(*vs, vs_microcode_);
    }
    return coherence_;
  }

  // No usable Draw* record (in the menu, the final composite has no record of its own
  // and only the ring carries its shaders): the PS of the last IM_LOAD, which arrives
  // unpatched; the VS of the IM_LOAD if it was fully identified, and otherwise the one
  // already seen matched with this same patched microcode.
  void UseRingIdentity() {
    // A depth-only draw (RB_MODECONTROL 5) runs no pixel shader (Xenia: IsPixelShaderNeededWithRasterization,
    // util/draw.cpp), so it only needs its VS. BO2's depth prepass draws with D3D's own 9-word pixel shader, which is
    // not in the library: without this the whole prepass was dropped, and with it the stencil 0x80 it writes under
    // the first-person gun and hands, which keeps the world from painting over them (confirmed by comparing RenderDoc
    // captures of the Xenos emulation and the native renderer).
    const bool only_depth = (DvarRecord(rex::graphics::XE_GPU_REG_RB_MODECONTROL) & 0x7) == 5;
    if (!ps_current_ && !only_depth) {
      return;
    }
    const ShaderInput* vs = vs_current_;
    if (!vs) {
      const auto it = vs_per_microcode_.find(HashVs());
      if (it == vs_per_microcode_.end()) {
        return;
      }
      vs = it->second;
    }
    vs_draw_ = vs;
    ps_draw_ = ps_current_;
    ++draws_per_im_load_;
  }

  uint64_t HashVs() {
    if (hash_vs_generation_ != generation_vs_) {
      hash_vs_generation_ = generation_vs_;
      hash_vs_ = XXH3_64bits(vs_microcode_.data(), vs_microcode_.size() * sizeof(uint32_t));
    }
    return hash_vs_;
  }

  static uint32_t RegisterCount(const DrawRecord& r) {
    switch (r.function) {
      case DrawFunction::kVertices:
        return r.args[2];  // r6: vertex count
      case DrawFunction::kIndexed:
        return r.args[3];  // r7: index count
      case DrawFunction::kVerticesUP:
        return r.args[1];  // r5: vertex count
      case DrawFunction::kIndexedUP:
      default:
        return r.args[3];  // r7: index count (unconfirmed)
    }
  }

  /*
   * The object generation used to be read four times per draw (twice in ShadersCoherent and
   * twice in PairDraw), and it is an atomic load with acquire: on the Switch's A57 that
   * is an ldar, which drains the load buffer. It is now read once, on entering the matching;
   * everything after that in the draw (including the trace) already has it.
   */
  void RefreshObjects() {
    const uint64_t generation = GenerationObjects();
    if (generation != generation_objects_) {
      objects_.clear();
      memo_objects_.fill(ObjectMemo{});
      generation_objects_ = generation;
    }
  }

  const ShaderInput* CachedObjectShader(uint32_t object) {
    /*
     * A 16-slot direct-mapped cache in front of the map: the draw's VS and PS are looked up several
     * times (candidate coherence and matching), and between consecutive draws they are almost
     * always the same. It saves the hash and the cache miss of the unordered_map, which with
     * ~2,200 draws per frame were ~9,000 lookups. RefreshObjects has already validated the
     * generation: when it changes, both caches are cleared together.
     */
    ObjectMemo& memo = memo_objects_[size_t((object * UINT32_C(2654435761)) >> 28)];
    if (memo.object == object) {
      return memo.input;
    }
    const ShaderInput* input;
    if (const auto it = objects_.find(object); it != objects_.end()) {
      input = it->second;
    } else {
      input = ObjectShader(object);
      objects_.emplace(object, input);
    }
    memo.object = object;
    memo.input = input;
    return input;
  }

  // Vertex input of the patched VS. Each element of the container fetches into
  // a temporary register; the ring microcode is searched, among the fetch
  // instructions, for the one that writes to that register (D3D reorders them),
  // and it yields the fetch constant, format, stride and offset. Logged once per
  // variant, for validation.
  void AnnotateInputVertices(const ShaderInput& vs) {
    if (vs_microcode_.size() != vs.microcode.size()) {
      ++variants_length_different_;
      return;
    }
    if (variants_views_.size() >= 256) {
      return;
    }
    const uint64_t key = (uint64_t(vs.number) << 48) ^ HashVs();  // the same XXH3, already computed
    if (!variants_views_.insert(key).second) {
      return;
    }
    std::string detail;
    for (const VertexElement& element : vs.elements) {
      const uint32_t reg_index = (vs.microcode[size_t(element.instruction) * 3] >> 12) & 0x3F;
      size_t p = SIZE_MAX;
      for (const VertexElement& other : vs.elements) {
        const size_t q = size_t(other.instruction) * 3;
        if (((vs_microcode_[q] >> 12) & 0x3F) == reg_index && (vs_microcode_[q] & 0x1F) == 0) {
          p = q;
          break;
        }
      }
      if (p == SIZE_MAX) {
        detail += fmt::format(" {}{}:?", NameUse(element.use), element.index_use);
        ++elements_no_fetch_;
        continue;
      }
      const uint32_t d0 = vs_microcode_[p], d1 = vs_microcode_[p + 1], d2 = vs_microcode_[p + 2];
      // exp_adjust (signed bits 24-29) and sign mode (bit 14): the emulated path multiplies the
      // value by 2^exp_adjust (spirv_translator_fetch.cpp:445-449); this path does not.
      const int32_t exponent = int32_t(d1 << 2) >> 26;
      detail += fmt::format(" {}{}:f{}/fmt{}/z{}/o{}/s{:03X}>{:03X}{}{}{}{}", NameUse(element.use),
                             element.index_use, ((d0 >> 20) & 0x1F) * 3 + ((d0 >> 25) & 0x3),
                             (d1 >> 16) & 0x3F, d2 & 0xFF, int32_t(d2 << 1) >> 9,
                             vs.microcode[size_t(element.instruction) * 3 + 1] & 0xFFF,
                             d1 & 0xFFF, ((d1 >> 30) & 0x1) ? "/mini" : "",
                             ((d1 >> 12) & 0x1) ? "/sign" : "",
                             exponent ? fmt::format("/exp{}", exponent) : std::string(),
                             ((d1 >> 14) & 0x1) ? "/rf1" : "");
    }
    if (variants_views_.size() <= 24) {  // was 200; the ring thread writes it
      REXLOG_INFO("[native] C5b: VS n{} (variant {:016X}): vertex input{}", vs.number,
                  key, detail);
    }
  }

  // Fetch constant 0, which VdSwap writes right before the Swap packet.
  SwapTexture GetSwapTexture() const {
    SwapTexture texture;
    for (uint32_t i = 0; i < 6; ++i) {
      texture.dword[i] = DvarRecord(rex::graphics::XE_GPU_REG_SHADER_CONSTANT_FETCH_00_0 + i);
    }
    return texture;
  }

  void BufferIndirect(uint32_t address, uint32_t words, int depth) {
    AddOnlyRing(indirect_);
    RingReader reader;
    reader.base = memory_->TranslatePhysical(address);
    reader.pos = 0;
    reader.end = words;
    while (reader.Pending()) {
      if (!Packet(reader, depth)) {
        break;
      }
    }
  }

  // --- Test presentation -------------------------------------------------

  // bo2zm_perf_log: this frame's cumulative counters (bo2zm_perf_log.cpp says which number goes in which column).
  // g_ns_ring_working is only added once a batch of packets ends, and this Swap is in the middle of one, so the
  // batch's time so far is added here: otherwise each frame's last batch would count in the next frame.
  void LogFrame() {
    bo2zm::perf_log::Counters c;
    c.ns_working = bo2zm::waits::g_ns_ring_working.load(std::memory_order_relaxed) + NanosecondsSince(start_batch_);
    c.ns_waiting_guest = bo2zm::waits::g_ns[bo2zm::waits::kRingRegMem].load(std::memory_order_relaxed);
    uint64_t cost[12] = {};
    targets_->CostPresent(cost);
    uint64_t waits_slot = 0, ns_waits_slot = 0;
    targets_->WaitsGpu(waits_slot, ns_waits_slot);
    // The output fence, the SDK's part of RefreshGuestOutput (swapchain acquire and present) and the slot fences.
    c.ns_present_wait = cost[1] + cost[9] + cost[11] + ns_waits_slot;
    c.draws = draws_.load(std::memory_order_relaxed);
    c.jobs_submitted = targets_->JobsSubmitted();
    c.width = swap_width_.load(std::memory_order_relaxed);
    c.height = swap_height_.load(std::memory_order_relaxed);
    bo2zm::perf_log::OnPresent(c);
  }

  void Present() {
    swaps_.fetch_add(1, std::memory_order_relaxed);
    bo2zm::benchmark::AnnotateFrame();  // bo2zm_benchmark's frame times
    bo2zm::pc_profiler::NoteFrame();    // bo2zm_pc_profile_stalls (PC test helper)
    g_swaps_native.fetch_add(1, std::memory_order_relaxed);
    counter_.fetch_add(1, std::memory_order_relaxed);
    if (!presenter_) {
      return;
    }
#if BO2ZM_NATIVE_METAL
    if (metal_presenter_ && targets_ && gpu::metal::Mirror::Get()) {
      // The frame the mirror drew on Metal (the Vulkan renderer's work, repeated); the clear below if none.
      if (version_ramp_submitted_ != version_ramp_) {
        version_ramp_submitted_ = version_ramp_;
        targets_->RampGamma(ramp_gamma_);
      }
      if (targets_->Present(nullptr, GetSwapTexture(), swap_width_.load(std::memory_order_relaxed),
                            swap_height_.load(std::memory_order_relaxed))) {
        uint32_t w = 0;
        uint32_t h = 0;
        if (void* image = gpu::metal::Mirror::Get()->Shown(w, h)) {
          metal_presenter_->RefreshWithImage(image, w, h);
          return;
        }
      }
    }
    if (metal_presenter_) {
      // The first Metal-drawn frames: a clear whose colour moves (so a frozen paint shows), until the renderer
      // draws on Metal. The Vulkan renderer still draws, off screen.
      const float t = float(counter_.load(std::memory_order_relaxed) % 120) / 120.0f;
      const float rgba[4] = {0.05f, 0.15f + 0.35f * t, 0.35f, 1.0f};
      metal_presenter_->RefreshWithClear(kOutputWidth, kOutputHeight, rgba);
      return;
    }
#endif
    // The game image, if this Swap has a resolved texture.
    if (targets_) {
      // The gamma ramp, if the game has changed it (same thread as AnnotateRampGamma).
      if (version_ramp_submitted_ != version_ramp_) {
        version_ramp_submitted_ = version_ramp_;
        targets_->RampGamma(ramp_gamma_);
      }
      const auto start = Clock::now();
      const bool presented = targets_->Present(presenter_.get(), GetSwapTexture(),
                                                   swap_width_.load(std::memory_order_relaxed),
                                                   swap_height_.load(std::memory_order_relaxed));
      time_present_ns_ += NanosecondsSince(start);
      ++presents_measures_;
      if (presented) {
        if (bo2zm::perf_log::Enabled()) {
          LogFrame();
        }
        return;
      }
    }
    presenter_->RefreshGuestOutput(
        kOutputWidth, kOutputHeight, kOutputWidth, kOutputHeight,
        [this](rex::ui::Presenter::GuestOutputRefreshContext& context) {
          return CleanOutput(static_cast<RampOutputContext&>(context));
        });
  }

  bool CleanOutput(RampOutputContext& context) {
    const rex::ui::vulkan::VulkanDevice* vulkan_device = provider_->vulkan_device();
    const auto& dfn = vulkan_device->functions();
    const VkDevice device = vulkan_device->device();

    if (render_pass_ == VK_NULL_HANDLE) {
      VkAttachmentDescription attachment{};
      attachment.format = rex::ui::vulkan::VulkanPresenter::kGuestOutputFormat;
      attachment.samples = VK_SAMPLE_COUNT_1_BIT;
      attachment.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
      attachment.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
      attachment.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
      attachment.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
      attachment.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
      attachment.finalLayout = rex::ui::vulkan::VulkanPresenter::kGuestOutputInternalLayout;
      VkAttachmentReference reference{0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
      VkSubpassDescription subpass{};
      subpass.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
      subpass.colorAttachmentCount = 1;
      subpass.pColorAttachments = &reference;
      VkRenderPassCreateInfo info{};
      info.sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO;
      info.attachmentCount = 1;
      info.pAttachments = &attachment;
      info.subpassCount = 1;
      info.pSubpasses = &subpass;
      if (dfn.vkCreateRenderPass(device, &info, nullptr, &render_pass_) != VK_SUCCESS) {
        render_pass_ = VK_NULL_HANDLE;
        return false;
      }
    }

    if (pool_ == VK_NULL_HANDLE) {
      VkCommandPoolCreateInfo info{};
      info.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
      info.queueFamilyIndex = vulkan_device->queue_family_graphics_compute();
      if (dfn.vkCreateCommandPool(device, &info, nullptr, &pool_) != VK_SUCCESS) {
        pool_ = VK_NULL_HANDLE;
        return false;
      }
      VkCommandBufferAllocateInfo reserve{};
      reserve.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
      reserve.commandPool = pool_;
      reserve.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
      reserve.commandBufferCount = 1;
      if (dfn.vkAllocateCommandBuffers(device, &reserve, &commands_) != VK_SUCCESS) {
        commands_ = VK_NULL_HANDLE;
      }
      VkFenceCreateInfo info_fence{};
      info_fence.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
      if (dfn.vkCreateFence(device, &info_fence, nullptr, &fence_) != VK_SUCCESS) {
        fence_ = VK_NULL_HANDLE;
      }
    }
    if (commands_ == VK_NULL_HANDLE || fence_ == VK_NULL_HANDLE) {
      return false;
    }

    // A single command buffer: wait for the previous one before reusing it.
    if (fence_pending_) {
      dfn.vkWaitForFences(device, 1, &fence_, VK_TRUE, UINT64_MAX);
      dfn.vkResetFences(device, 1, &fence_);
      fence_pending_ = false;
    }
    dfn.vkResetCommandPool(device, pool_, 0);

    VkFramebuffer framebuffer = VK_NULL_HANDLE;
    for (const Framebuffer& f : framebuffers_) {
      if (f.framebuffer != VK_NULL_HANDLE && f.version == context.image_version()) {
        framebuffer = f.framebuffer;
      }
    }
    if (framebuffer == VK_NULL_HANDLE) {
      Framebuffer& f = framebuffers_[next_framebuffer_];
      next_framebuffer_ = (next_framebuffer_ + 1) % framebuffers_.size();
      if (f.framebuffer != VK_NULL_HANDLE) {
        dfn.vkDestroyFramebuffer(device, f.framebuffer, nullptr);
        f.framebuffer = VK_NULL_HANDLE;
      }
      VkImageView view = context.image_view();
      VkFramebufferCreateInfo info{};
      info.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
      info.renderPass = render_pass_;
      info.attachmentCount = 1;
      info.pAttachments = &view;
      info.width = kOutputWidth;
      info.height = kOutputHeight;
      info.layers = 1;
      if (dfn.vkCreateFramebuffer(device, &info, nullptr, &f.framebuffer) != VK_SUCCESS) {
        f.framebuffer = VK_NULL_HANDLE;
        return false;
      }
      f.version = context.image_version();
      framebuffer = f.framebuffer;
    }

    VkCommandBufferBeginInfo start{};
    start.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    start.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    if (dfn.vkBeginCommandBuffer(commands_, &start) != VK_SUCCESS) {
      return false;
    }
    // Swaps without a resolved image stay black until the game can present one.
    VkClearValue color{};
    color.color.float32[3] = 1.0f;
    VkRenderPassBeginInfo pass{};
    pass.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
    pass.renderPass = render_pass_;
    pass.framebuffer = framebuffer;
    pass.renderArea.extent.width = kOutputWidth;
    pass.renderArea.extent.height = kOutputHeight;
    pass.clearValueCount = 1;
    pass.pClearValues = &color;
    dfn.vkCmdBeginRenderPass(commands_, &pass, VK_SUBPASS_CONTENTS_INLINE);
    dfn.vkCmdEndRenderPass(commands_);
    if (dfn.vkEndCommandBuffer(commands_) != VK_SUCCESS) {
      return false;
    }
    VkSubmitInfo submit{};
    submit.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    submit.commandBufferCount = 1;
    submit.pCommandBuffers = &commands_;
    {
      const auto queue = vulkan_device->AcquireQueue(vulkan_device->queue_family_graphics_compute(), 0);
      if (dfn.vkQueueSubmit(queue.queue(), 1, &submit, fence_) != VK_SUCCESS) {
        return false;
      }
    }
    fence_pending_ = true;
    context.SetIs8bpc(true);
    return true;
  }

  void DestroyVulkan() {
    targets_.reset();  // uses the device: before everything else
    if (!provider_ || !provider_->vulkan_device()) {
      return;
    }
    const rex::ui::vulkan::VulkanDevice* vulkan_device = provider_->vulkan_device();
    const auto& dfn = vulkan_device->functions();
    const VkDevice device = vulkan_device->device();
    if (fence_ != VK_NULL_HANDLE) {
      if (fence_pending_) {
        dfn.vkWaitForFences(device, 1, &fence_, VK_TRUE, UINT64_MAX);
        fence_pending_ = false;
      }
      dfn.vkDestroyFence(device, fence_, nullptr);
      fence_ = VK_NULL_HANDLE;
    }
    for (Framebuffer& f : framebuffers_) {
      if (f.framebuffer != VK_NULL_HANDLE) {
        dfn.vkDestroyFramebuffer(device, f.framebuffer, nullptr);
        f = Framebuffer{};
      }
    }
    if (pool_ != VK_NULL_HANDLE) {
      dfn.vkDestroyCommandPool(device, pool_, nullptr);
      pool_ = VK_NULL_HANDLE;
      commands_ = VK_NULL_HANDLE;
    }
    if (render_pass_ != VK_NULL_HANDLE) {
      dfn.vkDestroyRenderPass(device, render_pass_, nullptr);
      render_pass_ = VK_NULL_HANDLE;
    }
  }

  // Report lines go to the report thread (BO2ZM_REPORT_RING, see ReportDeferred), except the
  // shutdown ones (force), which are written right here so they are not lost.
#define BO2ZM_REPORT_BY(defer, ...) do { if (defer) { BO2ZM_REPORT_RING(__VA_ARGS__); } else { REXLOG_INFO(__VA_ARGS__); } } while (0)
  void Report(bool force) {
    bo2zm::memcpy_callers::Report();  // test build only (BO2ZM_MEMCPY_CALLERS)
    const auto now = Clock::now();
    if (!force &&
        now - last_report_ < std::chrono::seconds(REXCVAR_GET(bo2zm_native_diag_report_s))) {
      return;
    }
    const bool defer_report = !force;
    last_report_ = now;
    BO2ZM_REPORT_BY(defer_report,
        "[native] swaps={} vblanks={} packets={} interrupts={} indirect={} memory_writes={} timed_out_waits={} draws={} "
        "copies={}",
        swaps_.load(), vblanks_.load(), packets_.load(), interrupts_.load(),
        indirect_.load(), writes_memory_.load(), waits_exhausted_.load(), draws_.load(),
        copies_.load());
    // Ring thread cost since the previous report. "ring" includes the
    // WAIT_REG_MEM waits and the wait for the GPU when presenting.
    const double seconds = std::chrono::duration<double>(now - last_time_report_).count();
    last_time_report_ = now;
    if (seconds > 0.0) {
      BO2ZM_REPORT_BY(defer_report,
          "[native] times: ring {:.1f} ms/s; draws {:.1f} us/draw ({}); copies {:.1f} us/copy; present {:.2f} ms/Swap "
          "with the wait for the GPU",
          double(time_ring_ns_) / 1e6 / seconds,
          draws_measured_ ? double(time_draws_ns_) / 1e3 / double(draws_measured_) : 0.0,
          draws_measured_,
          copies_measures_ ? double(time_copies_ns_) / 1e3 / double(copies_measures_) : 0.0,
          presents_measures_
              ? double(time_present_ns_) / 1e6 / double(presents_measures_)
              : 0.0);
      // Ring thread wakeups: how many CP_RB_WPTR writes the game makes and how many iterations this
      // thread runs to serve them.
      {
        const uint64_t writes = writes_wptr_.load(std::memory_order_relaxed);
        const uint64_t d_writes = writes - writes_wptr_previous_;
        const uint64_t d_loops = loops_ring_ - loops_ring_previous_;
        const uint64_t d_with_data = loops_ring_with_data_ - loops_ring_with_data_previous_;
        const uint64_t d_exhausted = waits_ring_exhausted_ - waits_ring_exhausted_previous_;
        /*
         * Core migrations go on this same line on purpose, next to the timed-out waits: both
         * measure the same thing (the ring not working when it should) and are best read
         * together. If bo2zm_native_ring_core at -1 and at 1 gives the same migrations/s,
         * the preferred core has no effect here.
         */
        const uint64_t d_migrations = migrations_ring_ - migrations_ring_previous_;
        BO2ZM_REPORT_BY(defer_report, "[native] ring wake-ups: {:.0f} CP_RB_WPTR writes/s, {:.0f} loops/s, {:.0f} with "
                    "data/s ({:.2f} writes per loop with data), {:.0f} 4 ms waits timed out/s; core {} with {:.0f} "
                    "migrations/s ({:.2f} per loop)",
                    double(d_writes) / seconds, double(d_loops) / seconds, double(d_with_data) / seconds,
                    d_with_data ? double(d_writes) / double(d_with_data) : 0.0, double(d_exhausted) / seconds,
                    core_ring_, double(d_migrations) / seconds,
                    d_loops ? double(d_migrations) / double(d_loops) : 0.0);
        migrations_ring_previous_ = migrations_ring_;
        writes_wptr_previous_ = writes;
        loops_ring_previous_ = loops_ring_;
        loops_ring_with_data_previous_ = loops_ring_with_data_;
        waits_ring_exhausted_previous_ = waits_ring_exhausted_;
      }
      // Breakdown per packet, largest first (top 8).
      std::vector<std::tuple<uint64_t, int, uint64_t>> op_costs;
      if (time_registers_ns_) {
        op_costs.emplace_back(time_registers_ns_, -1, count_registers_);
      }
      for (int op = 0; op < int(time_opcode_ns_.size()); ++op) {
        if (time_opcode_ns_[op]) {
          op_costs.emplace_back(time_opcode_ns_[op], op, count_opcode_[op]);
        }
      }
      std::sort(op_costs.begin(), op_costs.end(), std::greater<>());
      std::string split;
      for (size_t i = 0; i < op_costs.size() && i < 8; ++i) {
        const auto& [ns, op, n] = op_costs[i];
        split += op < 0 ? fmt::format(" registers {:.1f} ms/s ({})", double(ns) / 1e6 / seconds, n)
                          : fmt::format(" op {:02X} {:.1f} ms/s ({})", op,
                                        double(ns) / 1e6 / seconds, n);
      }
      BO2ZM_REPORT_BY(defer_report, "[native] times per packet:{}", split);
      // How many register words take the block path and how many go one at a time.
      BO2ZM_REPORT_BY(defer_report, "[native] registers by path: {} words in blocks, {} one at a time ({:.1f} words per "
                  "type 0/1 packet)",
                  words_block_, words_loose_,
                  count_registers_ ? double(words_block_ + words_loose_) / double(count_registers_) : 0.0);
      words_block_ = words_loose_ = 0;
      // The FlushState markers (ProcessMarker). Their time shows up as "op 10".
      if (markers_applied_ || markers_checked_ || markers_bad_) {
        BO2ZM_REPORT_BY(defer_report, "[native] D3D markers: {} applied ({:.1f} spans and {:.1f} words each; "
                    "{} spans by the usual path), {} checked against their packets ({} different), {} malformed; fast "
                    "path {} ({} spans checked)",
                    markers_applied_,
                    markers_applied_ ? double(marker_spans_) / double(markers_applied_) : 0.0,
                    markers_applied_ ? double(marker_words_) / double(markers_applied_) : 0.0,
                    marker_spans_slow_, markers_checked_, markers_different_, markers_bad_,
                    marker_fast_ > 0 ? "on" : "OFF", spans_fast_verified_);
        markers_applied_ = markers_checked_ = markers_different_ = markers_bad_ = 0;
        marker_spans_ = marker_spans_slow_ = marker_words_ = 0;
      }
      // The draw record in the marker (PairDraw).
      if (draws_with_marker_register_ || marker_rejected_registers_ || checks_draw_) {
        BO2ZM_REPORT_BY(defer_report, "[native] draw record in the marker: {} draws without a lookup, {} "
                    "marker records not accepted (type, count or shaders: looked up as always), {} checked against the "
                    "lookup ({} different, {} with another record and the same shaders)",
                    draws_with_marker_register_, marker_rejected_registers_, checks_draw_,
                    checks_draw_different_, checks_draw_other_register_);
        draws_with_marker_register_ = marker_rejected_registers_ = 0;
        checks_draw_ = checks_draw_different_ = checks_draw_other_register_ = 0;
      }
      // The D3D shadow.
      if (shadow_draws_ || shadow_lost_) {
        // Consecutive registers with the same count as one range, the constants (0x4000-0x47FF) and the
        // rest (state, fetch) listed apart, so one line shows both.
        std::vector<std::pair<uint32_t, uint64_t>> sorted(shadow_per_register_.begin(), shadow_per_register_.end());
        std::sort(sorted.begin(), sorted.end());
        struct Range {
          uint64_t n;
          uint32_t first, last;
        };
        std::vector<Range> ranges;
        for (const auto& [reg, n] : sorted) {
          if (!ranges.empty() && ranges.back().last + 1 == reg && ranges.back().n == n) {
            ranges.back().last = reg;
          } else {
            ranges.push_back({n, reg, reg});
          }
        }
        std::sort(ranges.begin(), ranges.end(), [](const Range& a, const Range& b) {
          return a.n * (a.last - a.first + 1) > b.n * (b.last - b.first + 1);
        });
        std::string list, list_state;
        size_t in_list = 0, in_state = 0;
        for (const Range& r : ranges) {
          const bool constant = r.first >= 0x4000 && r.first < 0x4800;
          std::string& out = constant ? list : list_state;
          size_t& count = constant ? in_list : in_state;
          if (count++ >= 8) {
            continue;
          }
          out += r.first == r.last ? fmt::format(" 0x{:04X}x{}", r.first, r.n)
                                   : fmt::format(" 0x{:04X}-0x{:04X}x{}", r.first, r.last, r.n);
        }
        if (!list_state.empty()) {
          list += " | state:" + list_state;
        }
        const size_t worst_size = sorted.size();
        size_t groups_seen = 0;
        for (uint32_t g = 0; g < kGroupsMirror; ++g) {
          groups_seen += shadow_shot_.mask[g] != 0;
        }
        BO2ZM_REPORT_BY(defer_report, "[native] D3D shadow: {} draws compared ({} snapshots lost; {} of {} mirror "
                    "groups learned) | state {} different of {} | fetch {} of {} | constants {} of {} | registers with "
                    "differences: {}{}",
                    shadow_draws_, shadow_lost_, groups_seen, kGroupsMirror, shadow_state_different_,
                    shadow_state_compared_, shadow_fetch_different_, shadow_fetch_compared_,
                    shadow_constants_different_, shadow_constants_compared_, worst_size,
                    list.empty() ? "" : " (the most:" + list + ")");
        shadow_draws_ = shadow_lost_ = 0;
        shadow_state_compared_ = shadow_state_different_ = 0;
        shadow_fetch_compared_ = shadow_fetch_different_ = 0;
        shadow_constants_compared_ = shadow_constants_different_ = 0;
        shadow_per_register_.clear();
        std::string list_read;
        {
          std::vector<std::pair<uint64_t, uint32_t>> worst;
          for (const auto& [reg, n] : shadow_per_register_read_) {
            worst.emplace_back(n, reg);
          }
          std::sort(worst.begin(), worst.end(), std::greater<>());
          for (size_t i = 0; i < worst.size() && i < 8; ++i) {
            list_read += fmt::format(" 0x{:04X}x{}", worst[i].second, worst[i].first);
          }
        }
        BO2ZM_REPORT_BY(defer_report, "[native] D3D shadow constants in registers the draw's shaders read: {} "
                    "different of {} ({} of them not given by the device's stream since its buffer began){}",
                    shadow_constants_read_different_, shadow_constants_read_compared_, shadow_constants_read_unknown_,
                    list_read.empty() ? "" : " (the most:" + list_read + ")");
        shadow_constants_read_compared_ = shadow_constants_read_different_ = shadow_constants_read_unknown_ = 0;
        shadow_per_register_read_.clear();
        BO2ZM_REPORT_BY(defer_report, "[native] D3D shadow constants by source (different; of them the ring has the "
                    "mirror's own value): mirror {} ({}), shader literal {} ({}), game-written {} ({}), command "
                    "buffer {} ({}), not given by the device's stream since its buffer began {} ({}); {}",
                    shadow_from_[0][0] + shadow_from_[0][1], shadow_from_[0][1],
                    shadow_from_[1][0] + shadow_from_[1][1], shadow_from_[1][1],
                    shadow_from_[2][0] + shadow_from_[2][1], shadow_from_[2][1],
                    shadow_from_[3][0] + shadow_from_[3][1], shadow_from_[3][1],
                    shadow_from_[4][0] + shadow_from_[4][1], shadow_from_[4][1], StreamConstantsReport());
        std::memset(shadow_from_, 0, sizeof(shadow_from_));
      }
      // What WAIT_REG_MEM waits for: the 4 with the most 1 ms iterations.
      std::vector<std::pair<uint64_t, Wait>> waits(waits_.begin(), waits_.end());
      std::sort(waits.begin(), waits.end(), [](const auto& a, const auto& b) {
        return a.second.loops > b.second.loops;
      });
      std::string detail_waits;
      for (size_t i = 0; i < waits.size() && i < 4; ++i) {
        const auto& [key, e] = waits[i];
        detail_waits += fmt::format(
            " {} {:08X} (function {} ref {:08X} mask {:08X}): {} waits, {} loops;",
            (key >> 32) ? "memory" : "register", uint32_t(key), e.info & 0x7, e.reference,
            e.mask, e.times, e.loops);
      }
      if (!detail_waits.empty()) {
        BO2ZM_REPORT_BY(defer_report, "[native] WAIT_REG_MEM:{}", detail_waits);
      }
    }
    time_ring_ns_ = time_draws_ns_ = time_copies_ns_ = time_present_ns_ = 0;
    draws_measured_ = copies_measures_ = presents_measures_ = 0;
    time_registers_ns_ = count_registers_ = 0;
    time_opcode_ns_.fill(0);
    count_opcode_.fill(0);
    waits_.clear();
    const uint32_t frontbuffer = swap_frontbuffer_.load();
    const uint32_t target = last_copy_target_.load();
    BO2ZM_REPORT_BY(defer_report,
        "[native] last swap: frontbuffer={:08X} {}x{}; last copy: target={:08X} control={:08X} info={:08X} "
        "pitch={:08X}; match={}",
        frontbuffer, swap_width_.load(), swap_height_.load(), target, last_copy_control_.load(),
        last_copy_info_.load(), last_copy_pitch_.load(),
        (frontbuffer & 0x1FFFFFFF) == (target & 0x1FFFFFFF));
    if (targets_) {
      uint64_t copies = 0, clears = 0, presented = 0, rejections = 0;
      targets_->Stats(copies, clears, presented, rejections);
      uint64_t gpu_ns = 0, gpu_jobs = 0;
      targets_->TimeGpu(gpu_ns, gpu_jobs);
      const uint64_t presented_interval = presented - presented_previous_gpu_;
      BO2ZM_REPORT_BY(defer_report, "[native] C2: copies={} (of depth {}) clears={} (of depth {}) presented={} "
                  "rejected={}; GPU {:.2f} ms per Swap ({} jobs measured)",
                  copies, targets_->CopiesDepth(), clears,
                  targets_->ClearsDepth(), presented, rejections,
                  presented_interval
                      ? double(gpu_ns - gpu_ns_previous_) / 1e6 / double(presented_interval)
                      : 0.0,
                  gpu_jobs - gpu_jobs_previous_);
      // Occlusion queries since the previous report.
      {
        const auto d = [&](size_t i) { return occlusion_counters_[i] - occlusion_counters_previous_[i]; };
        uint64_t oc[5] = {};
        targets_->StatsOcclusion(oc);
        if (d(0) || d(1) || d(2) || d(4)) {
          BO2ZM_REPORT_BY(defer_report, "[native] C2 occlusion: mode {}; game queries begun {}, ended with a "
                      "measurement {} and without a measurement yet {}, without their end {}, faked without a pair {}; "
                      "written measurements: average {:.1f}, historical maximum {}; ended by target: small (scale 1) {} "
                      "(maximum {}), scene (AA mode scale) {} (maximum {}); host: spans {} (without room {}), queries "
                      "published {}, samples {} (historical maximum per query {})",
                      REXCVAR_GET(bo2zm_native_occlusion), d(0), d(1), d(2), d(3), d(4),
                      d(1) ? double(d(5)) / double(d(1)) : 0.0, occlusion_counters_[6], occlusion_per_target_[0],
                      occlusion_max_per_target_[0], occlusion_per_target_[1], occlusion_max_per_target_[1],
                      oc[0] - occlusion_host_previous_[0], oc[1] - occlusion_host_previous_[1],
                      oc[2] - occlusion_host_previous_[2], oc[3] - occlusion_host_previous_[3], oc[4]);
        }
        occlusion_per_target_ = {};
        occlusion_counters_previous_ = occlusion_counters_;
        std::copy(std::begin(oc), std::end(oc), occlusion_host_previous_);
      }
      {
        uint64_t waits = 0, ns_waits = 0;
        targets_->WaitsGpu(waits, ns_waits);
        const double ms_waits = double(ns_waits - ns_waits_gpu_previous_) / 1e6;
        BO2ZM_REPORT_BY(defer_report, "[native] C2: ring waits for the GPU: {} ({:.1f} ms; {:.2f} ms per Swap)",
                    waits - waits_gpu_previous_, ms_waits,
                    presented_interval ? ms_waits / double(presented_interval) : 0.0);
        waits_gpu_previous_ = waits;
        ns_waits_gpu_previous_ = ns_waits;
        // And the real duration of the jobs, to check it against the sum of the timestamps.
        uint64_t jobs = 0, ns_jobs = 0;
        targets_->DurationJobsGpu(jobs, ns_jobs);
        const uint64_t d_jobs = jobs - jobs_gpu_previous_;
        const double ms_jobs = double(ns_jobs - ns_job_gpu_previous_) / 1e6;
        BO2ZM_REPORT_BY(defer_report, "[native] C2: GPU jobs by wall clock: {} ({:.1f} ms; {:.2f} ms each; {:.2f} ms "
                    "per Swap)",
                    d_jobs, ms_jobs, d_jobs ? ms_jobs / double(d_jobs) : 0.0,
                    presented_interval ? ms_jobs / double(presented_interval) : 0.0);
        jobs_gpu_previous_ = jobs;
        ns_job_gpu_previous_ = ns_jobs;
      }
      {
        uint64_t cost[6] = {};
        targets_->CostRecord(cost);
        uint64_t dc[6] = {};
        for (size_t i = 0; i < 6; ++i) {
          dc[i] = cost[i] - cost_record_previous_[i];
          cost_record_previous_[i] = cost[i];
        }
        BO2ZM_REPORT_BY(defer_report, "[native] C2: command buffers begun {} ({:.1f} ms in Record, {:.1f} ms resetting "
                    "pools); readbacks {} ({:.2f} M texels, {:.1f} ms writing them)",
                    dc[0], double(dc[1]) / 1e6, double(dc[2]) / 1e6, dc[3], double(dc[4]) / 1e6,
                    double(dc[5]) / 1e6);
      }
      std::array<uint64_t, kGpuCategories> categories{};
      targets_->TimeGpuPerCategory(categories);
      if (presented_interval) {
        const auto ms = [&](uint32_t c) {
          return double(categories[c] - gpu_categories_previous_[c]) / 1e6 /
                 double(presented_interval);
        };
        BO2ZM_REPORT_BY(defer_report, "[native] C2: GPU per Swap: shadows {:.2f} ms, scene {:.2f}, reflection {:.2f}, "
                    "320 (cube and blur) {:.2f}, minor {:.2f}, copies {:.2f}, clears {:.2f}, rest {:.2f}, scene without "
                    "depth {:.2f}, gap between jobs {:.2f}",
                    ms(kGpuShadows), ms(kGpuScene), ms(kGpuReflection), ms(kGpu320),
                    ms(kGpuSmaller), ms(kGpuCopies), ms(kGpuClears), ms(kGpuOthers),
                    ms(kGpuSceneNoDepth), ms(kGpuGapBetweenJobs));
      }
      gpu_categories_previous_ = categories;
      // The size of the remaining copies.
      if (presented_interval) {
        std::array<uint64_t, 4> copies_t{}, pixels_t{};
        targets_->CopiesPerSize(copies_t, pixels_t);
        static constexpr const char* kBuckets[] = {"<=64x64", "<=320x320", "<=1024x1024", "larger"};
        std::string line;
        for (uint32_t c = 0; c < 4; ++c) {
          const uint64_t n = copies_t[c] - copies_bucket_previous_[c];
          const uint64_t px = pixels_t[c] - pixels_bucket_previous_[c];
          if (n) {
            line += fmt::format(" {} {:.1f} copies and {:.2f} Mpixels", kBuckets[c],
                                 double(n) / double(presented_interval),
                                 double(px) / 1e6 / double(presented_interval));
          }
        }
        if (!line.empty()) {
          BO2ZM_REPORT_BY(defer_report, "[native] C2 copies per frame and size:{}", line);
        }
        copies_bucket_previous_ = copies_t;
        pixels_bucket_previous_ = pixels_t;
      }
      // Actual fragments and vertices of each pass, to separate per-pixel cost from geometry cost.
      // The counts do not depend on the machine: what is measured on the PC holds for the console.
      {
        std::array<uint64_t, kGpuCategories> frag{}, vert{}, prim{};
        targets_->StatsPipeline(frag, vert, prim);
        static constexpr const char* kTypesStat[] = {"rest",  "shadows", "scene",  "reflection",
                                                      "cube",   "minor", "copies",  "clears",
                                                      "scene without depth", "gap"};
        std::string line;
        for (uint32_t c = 0; presented_interval && c < kGpuCategories; ++c) {
          const uint64_t f = frag[c] - fragments_category_previous_[c];
          const uint64_t v = vert[c] - vertices_category_previous_[c];
          const uint64_t p = prim[c] - primitives_category_previous_[c];
          if (f || v) {
            line += fmt::format(" {} {:.2f} M fragments, {:.0f} k vertices, {:.0f} k primitives;",
                                 kTypesStat[c], double(f) / 1e6 / double(presented_interval),
                                 double(v) / 1e3 / double(presented_interval),
                                 double(p) / 1e3 / double(presented_interval));
          }
        }
        if (!line.empty()) {
          BO2ZM_REPORT_BY(defer_report, "[native] C2 per frame and pass type:{}", line);
        }
        fragments_category_previous_ = frag;
        vertices_category_previous_ = vert;
        primitives_category_previous_ = prim;
      }
      // Which pixel shaders fill the screen, from the diagnostic frames
      // (bo2zm_native_per_draw_stats_s). Accumulated from the start, not per interval.
      {
        std::vector<uint64_t> frag_ps, draw_ps;
        uint64_t frames = 0;
        targets_->StatsPerShader(frag_ps, draw_ps, frames);
        if (frames > frames_diagnostic_previous_) {
          frames_diagnostic_previous_ = frames;
          static constexpr const char* kTypesPs[] = {"rest",  "shadows", "scene",  "reflection",
                                                     "cube",   "minor", "copies",  "clears",
                                                     "scene without depth", "gap"};
          const size_t per_category = frag_ps.size() / kGpuCategories;
          for (uint32_t c = 0; c < kGpuCategories; ++c) {
            uint64_t total = 0;
            std::vector<uint32_t> order;
            for (size_t i = 0; i < per_category; ++i) {
              const size_t e = size_t(c) * per_category + i;
              total += frag_ps[e];
              if (frag_ps[e]) {
                order.push_back(uint32_t(e));
              }
            }
            uint64_t draws = 0;
            for (size_t i = 0; i < per_category; ++i) {
              draws += draw_ps[size_t(c) * per_category + i];
            }
            if (!total && !draws) {
              continue;  // that category was not measured
            }
            std::sort(order.begin(), order.end(),
                      [&](uint32_t a, uint32_t b) { return frag_ps[a] > frag_ps[b]; });
            std::string line;
            for (size_t i = 0; i < order.size() && i < 8; ++i) {
              const uint32_t e = order[i];
              const uint32_t ps = uint32_t(e % per_category);
              line += fmt::format(" PS {} {:.2f} M ({:.0f} %, {:.1f} draws)",
                                   ps ? fmt::format("n{}", ps - 1) : std::string("none"),
                                   double(frag_ps[e]) / 1e6 / double(frames),
                                   100.0 * double(frag_ps[e]) / double(total),
                                   double(draw_ps[e]) / double(frames));
            }
            BO2ZM_REPORT_BY(defer_report, "[native] C2 {} fragments per pixel shader ({} frames, {:.2f} M and {:.0f} "
                        "draws per frame):{}",
                        kTypesPs[c], frames, double(total) / 1e6 / double(frames),
                        double(draws) / double(frames), line);
          }
        }
      }
      // Real scale of the GPU timestamps. Between two reports, the end timestamp of the last timed job
      // must advance as much as the clock (with a lag of one or two frames at each end). On the console
      // NVK declares 1 ns per unit and the ratio comes out at 1.628 in every interval measured, menus
      // included: the GPU ms in the reports must be multiplied by the accumulated scale.
      {
        const uint64_t mark = targets_->MarkGpuFinalNs();
        const Clock::time_point now = Clock::now();
        if (mark != 0 && mark != mark_gpu_previous_ns_) {
          if (mark_gpu_previous_ns_ != 0 && mark > mark_gpu_previous_ns_) {
            const double real_ns = double(
                std::chrono::duration_cast<std::chrono::nanoseconds>(now - clock_mark_previous_).count());
            const double advance_ns = double(mark - mark_gpu_previous_ns_);
            scale_real_ns_ += real_ns;
            scale_marks_ns_ += advance_ns;
            const double scale = scale_real_ns_ / scale_marks_ns_;
            BO2ZM_REPORT_BY(defer_report, "[native] C2: GPU mark scale {:.3f} in this interval ({:.3f} accumulated; "
                        "real time between reports / advance of the marks); real GPU per Swap {:.2f} ms",
                        real_ns / advance_ns, scale,
                        presented_interval
                            ? double(gpu_ns - gpu_ns_previous_) / 1e6 / double(presented_interval) * scale
                            : 0.0);
          }
          mark_gpu_previous_ns_ = mark;
          clock_mark_previous_ = now;
        }
      }
      // Mode of the intermediate timestamps of the jobs timed in the interval.
      {
        const uint64_t precise = targets_->JobsMarksPrecise();
        BO2ZM_REPORT_BY(defer_report, "[native] C2: precise marks in {} of {} measured "
                                      "jobs", precise - jobs_precise_previous_,
                    gpu_jobs - gpu_jobs_previous_);
        jobs_precise_previous_ = precise;
      }
      // Intervals between Swaps and GPU overlaps.
      {
        std::array<uint64_t, kBucketsSwap> buckets{};
        uint64_t overlaps = 0;
        double worst_ms = 0.0;
        targets_->IntervalsBetweenSwaps(buckets, overlaps, worst_ms);
        uint64_t total = 0;
        for (uint32_t i = 0; i < kBucketsSwap; ++i) {
          total += buckets[i] - buckets_swap_previous_[i];
        }
        if (total) {
          const auto c = [&](uint32_t i) { return buckets[i] - buckets_swap_previous_[i]; };
          BO2ZM_REPORT_BY(defer_report, "[native] C2: intervals between Swaps: <15 ms {} | 15-18 {} | 18-25 {} | 25-30 "
                      "{} | 30-36 {} | 36-50 {} | 50-60 {} | 60-75 {} | 75-100 {} | 100-150 {} | >=150 {}; worst frame "
                      "{:.1f} ms; overlapping jobs on the GPU {}",
                      c(0), c(1), c(2), c(3), c(4), c(5), c(6), c(7), c(8), c(9), c(10), worst_ms,
                      overlaps - overlaps_gpu_previous_);
        }
        buckets_swap_previous_ = buckets;
        bo2zm::guard30::Report();  // which step the 30 FPS guard was on
        {
          // How much of the game's constant dump is redundant.
          static uint64_t writes_prev = 0, same_prev = 0, blocks_prev = 0, blocks_same_prev = 0;
          const uint64_t writes = constants_written_ - writes_prev;
          const uint64_t same = constants_no_change_ - same_prev;
          const uint64_t blocks = blocks_constants_ - blocks_prev;
          const uint64_t blocks_same = blocks_constants_no_change_ - blocks_same_prev;
          writes_prev = constants_written_; same_prev = constants_no_change_;
          blocks_prev = blocks_constants_; blocks_same_prev = blocks_constants_no_change_;
          if (writes > 0 || blocks > 0) {
            BO2ZM_REPORT_BY(defer_report, "[native] C6 game constants: {} single writes ({} unchanged, {:.1f} %) | {} "
                        "blocks ({} unchanged, {:.1f} %)",
                        writes, same, writes ? 100.0 * double(same) / double(writes) : 0.0,
                        blocks, blocks_same, blocks ? 100.0 * double(blocks_same) / double(blocks) : 0.0);
          }
        }
        {
          /*
           * How often the fetch constants and the viewport state really change. This
           * measure tells how much the translator can skip: if changes per frame are far
           * fewer than draws per frame, the "textures" stage and the viewport/scissor
           * are almost entirely repeated work.
           */
          static uint64_t fe_prev = 0, fs_prev = 0, fg_prev = 0;
          static uint64_t ee_prev = 0, is_prev = 0, eg_prev = 0, draw_prev = 0;
          const uint64_t fe = fetch_written_ - fe_prev, fs = fetch_no_change_ - fs_prev;
          const uint64_t fg = generation_fetch_ - fg_prev;
          const uint64_t ee = framing_written_ - ee_prev, es = framing_no_change_ - is_prev;
          const uint64_t eg = generation_framing_ - eg_prev;
          // draws_measured_ was already reset above in this same report: the right count is
          // the ring's accumulated counter.
          const uint64_t draw_total = draws_.load(std::memory_order_relaxed);
          const uint64_t draw = draw_total - draw_prev;
          fe_prev = fetch_written_; fs_prev = fetch_no_change_; fg_prev = generation_fetch_;
          ee_prev = framing_written_; is_prev = framing_no_change_; eg_prev = generation_framing_;
          draw_prev = draw_total;
          if (fe > 0 || ee > 0) {
            BO2ZM_REPORT_BY(defer_report, "[native] C6 generations: fetch {} writes ({} unchanged), {} changes ({:.2f} "
                        "per draw) | framing {} writes ({} unchanged), {} changes ({:.2f} per draw); draws {}",
                        fe, fs, fg, draw ? double(fg) / double(draw) : 0.0,
                        ee, es, eg, draw ? double(eg) / double(draw) : 0.0, draw);
          }
        }
        overlaps_gpu_previous_ = overlaps;
      }
      // Breakdown of the time spent in Present.
      {
        uint64_t c[12] = {};
        targets_->CostPresent(c);
        const auto d = [&](int i) { return c[i] - cost_present_previous_[i]; };
        const auto average_of = [&](int ns, int times) { return d(times) ? double(d(ns)) / 1e6 / double(d(times)) : 0.0; };
        if (d(0) || d(2) || d(5) || d(8)) {
          BO2ZM_REPORT_BY(defer_report, "[native] C2: present (ms per call): wait for the previous output {:.2f} ({}); "
                      "work submit: queue lock {:.2f}, vkQueueSubmit {:.2f} ({}); output submit: lock {:.2f}, "
                      "vkQueueSubmit {:.2f} ({}); RefreshGuestOutput: before the call {:.2f}, inside {:.2f}, after "
                      "{:.2f} ({})",
                      average_of(1, 0), d(0), average_of(3, 2), average_of(4, 2), d(2), average_of(6, 5), average_of(7, 5), d(5),
                      average_of(9, 8), average_of(10, 8), average_of(11, 8), d(8));
        }
        std::copy(c, c + 12, cost_present_previous_);
      }
      gpu_ns_previous_ = gpu_ns;
      gpu_jobs_previous_ = gpu_jobs;
      presented_previous_gpu_ = presented;
      const DrawStats d = targets_->DrawsStats();
      std::string causes;
      for (const auto& [cause, n] : d.causes) {
        causes += fmt::format(" {}={}", cause, n);
      }
      // Per-stage cost of the draws recorded since the previous report, measured on the timed ones.
      const uint64_t recorded = d.drawn - drawn_previous_;
      const uint64_t timed = d.drawn_timed - timed_previous_;
      if (recorded && timed) {
        static constexpr const char* kStages[] = {"state",  "indices",  "textures",
                                                  "pass",    "uploads",  "pipeline",
                                                  "record",  "(of the pass stage, the pass change)",
                                                  "[of the pass change, close the previous one]",
                                                  "[targets]", "[render pass and framebuffer]",
                                                  "[open the pass]"};
        std::string stages;
        for (size_t i = 0; i < d.stages_ns.size(); ++i) {
          stages += fmt::format(" {} {:.1f}", kStages[i],
                                double(d.stages_ns[i] - stages_previous_[i]) / 1e3 / double(timed));
        }
        /*
         * The divisor is the number of recorded draws, not the number of incoming draws.
         * `drawn_timed_` is incremented in bo2zm_native_draws.cpp:2440, right after
         * vkCmdDraw/vkCmdDrawIndexed: it only counts recorded draws, never rejected ones.
         * Arithmetic check on a log (1 in 8 timed): sample 107,457 and recorded 859,614, and
         * 859,614/8 = 107,452 (exact). With the incoming draws it would have been 147,852.
         *
         * In practice: to turn these us/draw into ms per frame, multiply by the recorded draws
         * (~1,395/frame), not by the incoming ones (~1,957/frame). Getting it wrong inflates every
         * stage by 40 %. Also, these samples pay for 12 clock reads, so they come out ~3 us above
         * "draws us/draw".
         */
        // The ratio comes from the counts themselves (it used to be a hard-coded 8; the stage timer
        // now runs on 1 of every 64 draws).
        BO2ZM_REPORT_BY(defer_report, "[native] C6 stages (us per RECORDED draw, sample of {} = 1 in {:.0f} recorded; "
                    "recorded {}):{}",
                    timed, double(recorded) / double(timed), recorded, stages);
      }
      // The scenery LOD hook (a stub for BO2). If "forced" does not resemble "what the game draws", the hook is
      // not reaching the objects and the popping will remain (this has happened before).
      BO2ZM_REPORT_BY(defer_report, "[native] C6 {}", bo2zm::scenery_lod::Summary());
      // The early vegetation rejection has its own counter. Without this line there is no way to
      // check whether the shortcut works (the number would have to be deduced by subtracting
      // recorded draws from incoming ones).
      {
        const uint64_t soon = d.vegetation_soon - vegetation_soon_previous_;
        vegetation_soon_previous_ = d.vegetation_soon;
        if (presented_interval) {
          BO2ZM_REPORT_BY(defer_report, "[native] C6 shadow vegetation dropped early: {:.0f} per frame ({} in the "
                      "interval); saves the indices, textures, upload and pass of each one",
                      double(soon) / double(presented_interval), soon);
        }
      }
      {
        // How much of the scene forces shading before the depth test.
        const uint64_t with = d.scene_with_discard, no = d.scene_no_discard;
        const uint64_t d_with = with - discard_with_previous_, d_without = no - discard_no_previous_;
        discard_with_previous_ = with;
        discard_no_previous_ = no;
        if (d_with + d_without) {
          BO2ZM_REPORT_BY(defer_report, "[native] C6 early discard in the scene: {} draws allow it, {} prevent it "
                      "(alpha test, kill or depth) = {:.0f} %",
                      d_without, d_with, 100.0 * double(d_with) / double(d_with + d_without));
        }
      }
      drawn_previous_ = d.drawn;
      timed_previous_ = d.drawn_timed;
      stages_previous_ = d.stages_ns;
      {
        const std::array<uint64_t, 20> counters = {d.passes, d.submits_full,
                                                      d.ns_submits_full, d.bytes_vertices,
                                                      d.bytes_indices, d.samplers,
                                                      d.samplers_cache, d.ns_passes,
                                                      d.ns_vertices, d.inputs_computed,
                                                      d.ns_inputs, d.inputs_reused,
                                                      d.passes_per_generation, d.passes_per_target,
                                                      d.passes_resumed, d.ns_render_pass,
                                                      d.bytes_repeated_frame,
                                                      d.bytes_equal_earlier, d.ns_hash_vertices,
                                                      d.texels_passes};
        // Deduplication of vertex uploads within the frame.
        const std::array<uint64_t, 3> dedupe = {d.dedupe_hits, d.dedupe_bytes,
                                                d.dedupe_collisions};
        const auto ddelta = [&](size_t i) { return dedupe[i] - dedupe_previous_[i]; };
        const auto delta = [&](size_t i) { return counters[i] - counters_previous_[i]; };
        BO2ZM_REPORT_BY(defer_report, "[native] C6 counters: passes {} submits because the upload buffer was full {} "
                    "({:.1f} ms) vertices {:.1f} MB indices {:.1f} MB samplers {} (cache {}); {:.1f} ms in pass changes "
                    "({:.1f} us each), {:.1f} ms copying vertices",
                    delta(0), delta(1), double(delta(2)) / 1e6, double(delta(3)) / 1048576.0,
                    double(delta(4)) / 1048576.0, delta(5), delta(6), double(delta(7)) / 1e6,
                    delta(0) ? double(delta(7)) / 1e3 / double(delta(0)) : 0.0,
                    timed ? double(delta(8)) / 1e6 * double(recorded) / double(timed) : 0.0);
        if (presented_interval) {
          BO2ZM_REPORT_BY(defer_report, "[native] C6 deduplicated vertices: {} bindings reused, {:.2f} MB NOT copied "
                      "({:.2f} MB per frame, {:.0f} % of those uploaded){}",
                      ddelta(0), double(ddelta(1)) / 1048576.0,
                      double(ddelta(1)) / 1048576.0 / double(presented_interval),
                      delta(3) + ddelta(1)
                          ? 100.0 * double(ddelta(1)) / double(delta(3) + ddelta(1))
                          : 0.0,
                      ddelta(2) ? fmt::format("; *** {} COLLISIONS: the table is too small ***",
                                              ddelta(2))
                                : "");
        }
        dedupe_previous_ = dedupe;
        BO2ZM_REPORT_BY(defer_report, "[native] C6 counters: vertex inputs computed {} ({:.1f} ms, {:.1f} us each), "
                    "reused {}",
                    delta(9), double(delta(10)) / 1e6,
                    delta(9) ? double(delta(10)) / 1e3 / double(delta(9)) : 0.0, delta(11));
        BO2ZM_REPORT_BY(defer_report, "[native] C6 passes: because of a new command buffer {}, because of targets {}, "
                    "resumed after a copy or clear {}; {:.1f} ms in vkCmdBegin/EndRenderPass; open area {:.1f} Mtexels "
                    "({:.2f} Mtexels per frame)",
                    delta(12), delta(13), delta(14), double(delta(15)) / 1e6,
                    double(delta(19)) / 1e6,
                    presented_interval ? double(delta(19)) / 1e6 / double(presented_interval)
                                          : 0.0);
        if (presented_interval) {
          static constexpr const char* kTypes[] = {"rest",  "shadows", "scene",  "reflection",
                                                   "cube",   "minor", "copies",  "clears",
                                                   "scene without depth", "gap"};
          std::string area;
          for (uint32_t c = 0; c < kGpuCategories; ++c) {
            const uint64_t t = d.texels_per_category[c] - texels_category_previous_[c];
            const uint64_t n = d.passes_per_category[c] - passes_category_previous_[c];
            if (!t && !n) {
              continue;
            }
            const uint64_t draw = d.draws_per_category[c] - draws_category_previous_[c];
            const uint64_t tri = d.triangles_per_category[c] - triangles_category_previous_[c];
            area += fmt::format(" {} {:.2f} Mtexels in {:.1f} passes ({:.0f} draws, {:.0f} k triangles)",
                                kTypes[c], double(t) / 1e6 / double(presented_interval),
                                double(n) / double(presented_interval),
                                double(draw) / double(presented_interval),
                                double(tri) / 1e3 / double(presented_interval));
          }
          BO2ZM_REPORT_BY(defer_report, "[native] C6 open area per frame and target type:{}", area);
        }
        if (presented_interval) {
          // Draws without color, with and without a required pixel shader.
          BO2ZM_REPORT_BY(defer_report, "[native] C6 colorless draws per frame: {:.0f} with a pixel shader to spare, "
                      "{:.0f} that need it (alpha test, kill or depth); in the shadow map {:.0f} with the alpha test "
                      "set and {:.0f} without it",
                      double(d.draws_ps_useless - draws_ps_useless_previous_) /
                          double(presented_interval),
                      double(d.draws_ps_needed - draws_ps_needed_previous_) /
                          double(presented_interval),
                      double(d.shadows_alpha_active - shadows_alpha_active_previous_) /
                          double(presented_interval),
                      double(d.shadows_alpha_off - shadows_alpha_off_previous_) /
                          double(presented_interval));
        }
        shadows_alpha_active_previous_ = d.shadows_alpha_active;
        shadows_alpha_off_previous_ = d.shadows_alpha_off;
        draws_ps_useless_previous_ = d.draws_ps_useless;
        draws_ps_needed_previous_ = d.draws_ps_needed;
        texels_category_previous_ = d.texels_per_category;
        passes_category_previous_ = d.passes_per_category;
        draws_category_previous_ = d.draws_per_category;
        triangles_category_previous_ = d.triangles_per_category;
        // Which constants mode each interval used (test cvar bo2zm_native_constants_ubo_alternate_s).
        BO2ZM_REPORT_BY(defer_report, "[native] C6 constants by UBO: {} of {} submits", d.submits_ubo - submits_ubo_previous_,
                    d.submits - submits_previous_);
        {
          const uint64_t observed = d.shared_observed - shared_observed_previous_;
          const uint64_t changed = d.shared_changed - shared_changed_previous_;
          shared_observed_previous_ = d.shared_observed;
          shared_changed_previous_ = d.shared_changed;
          if (observed > 0) {
            BO2ZM_REPORT_BY(defer_report, "[native] C6 shared constants: {} of {} draws change the 488 bytes ({:.1f} "
                        "%); the others only pay for the memcmp",
                        changed, observed, 100.0 * double(changed) / double(observed));
          }
        }
        submits_ubo_previous_ = d.submits_ubo;
        submits_previous_ = d.submits;
        // Diagnostic: whether the game has touched the gamma ramp since the previous report.
        {
          uint64_t total = 0;
          for (uint64_t n : writes_ramp_) {
            total += n;
          }
          if (total != writes_ramp_previous_) {
            writes_ramp_previous_ = total;
            uint32_t different = 0;
            for (uint32_t i = 0; i < 256; ++i) {
              const uint16_t identity = uint16_t(i * 0x3FF / 0xFF);
              if (ramp_gamma_[i][0] != identity || ramp_gamma_[i][1] != identity ||
                  ramp_gamma_[i][2] != identity) {
                ++different;
              }
            }
            const auto sample = [&](uint32_t i) {
              return fmt::format("[{}]={}/{}/{}", i, ramp_gamma_[i][0], ramp_gamma_[i][1], ramp_gamma_[i][2]);
            };
            BO2ZM_REPORT_BY(defer_report, "[native] C2 gamma ramp: writes mode {} index {} sequential {} PWL {} 30 bits "
                        "{} 1926 {} mask {} (value {}); entries of the 256 table different from the identity {}; {} {} "
                        "{} {} {} (identity: i*1023/255); version {}",
                        writes_ramp_[0], writes_ramp_[1], writes_ramp_[2], writes_ramp_[3],
                        writes_ramp_[4], writes_ramp_[5], writes_ramp_[6], mask_ramp_, different,
                        sample(0), sample(64), sample(128), sample(192), sample(255), version_ramp_);
            // The whole table, to compare the image with the Xbox 360 one outside the game.
            bool channels_equal = true;
            for (const auto& input : ramp_gamma_) {
              channels_equal = channels_equal && input[0] == input[1] && input[0] == input[2];
            }
            static constexpr const char* kNamesChannel[3] = {"red", "green", "blue"};
            for (uint32_t c = 0; c < (channels_equal ? 1u : 3u); ++c) {
              std::string list;
              list.reserve(256 * 5);
              for (uint32_t i = 0; i < 256; ++i) {
                list += fmt::format("{}{}", i ? " " : "", ramp_gamma_[i][c]);
              }
              BO2ZM_REPORT_BY(defer_report, "[native] C2 gamma ramp, table {}: {}",
                          channels_equal ? "of the three channels" : kNamesChannel[c], list);
            }
          }
        }
        if (delta(16) || delta(17)) {
          BO2ZM_REPORT_BY(defer_report, "[native] C6 repeated vertices: {:.1f} MB copied; {:.1f} MB repeated in the "
                      "same frame and {:.1f} MB equal to an earlier frame; {:.1f} ms in hashes",
                      double(delta(3)) / 1048576.0, double(delta(16)) / 1048576.0,
                      double(delta(17)) / 1048576.0, double(delta(18)) / 1e6);
        }
        counters_previous_ = counters;
      }
      BO2ZM_REPORT_BY(defer_report, "[native] C6: drawn={} rejected={} pipelines={} ({} ms creating) textures={} ({} "
                  "MB) texture_uploads={} MB_uploaded={}; rejections by cause:{}",
                  d.drawn, d.rejected, d.pipelines, d.ms_pipelines, d.textures,
                  d.megs_textures, d.uploads_texture, d.megs_uploaded, causes);
    }
    if (shaders_.loaded()) {
      const NativeShaderStats e = shaders_.Stats();
      BO2ZM_REPORT_BY(defer_report, "[native] C5a: loads={} distinct={} identified={} no_identify={} ambiguous={}; "
                  "draws with VS and PS={} without VS={} without PS={}; VS/PS pairs={}",
                  e.loads + loads_cached_, e.different, e.identified, e.no_identify, e.ambiguous,
                  draws_identified_, draws_no_vs_, draws_no_ps_, pairs_count_);
      // IM_LOAD without memcmp (LoadShaderCached).
      {
        const uint64_t no_memcmp = im_i_no_memcmp_;
        const uint64_t with_memcmp = im_i_with_memcmp_;
        BO2ZM_REPORT_BY(defer_report,
                            "[native] C5a IM_LOAD without memcmp: phase {}; in the interval {} loads "
                            "without reading the microcode ({:.1f} %) and {} with memcmp ({} compared with the shortcut "
                            "and equal, {} because of a write to their slot, {} not recorded because of a half-done "
                            "write); checked to apply {} of {}{} | reported writes since boot: {} in-place patches, {} "
                            "in the ring copy, {} others, {} creations",
                            NamePhaseImShortcut(), no_memcmp,
                            no_memcmp + with_memcmp
                                ? 100.0 * double(no_memcmp) / double(no_memcmp + with_memcmp)
                                : 0.0,
                            with_memcmp, im_i_checked_, im_i_version_changed_, im_i_cancelled_,
                            std::min(im_shortcut_checked_, kImShortcutALook), kImShortcutALook,
                            im_shortcut_phase_ == kImShortcutObserving && im_shortcut_checked_ >= kImShortcutALook
                                ? " (no in-place patch seen: the sub_825A2FB8 hook does not run)"
                                : "",
                            microcode::g_patches_in_room.load(std::memory_order_relaxed),
                            microcode::g_patches_in_copy.load(std::memory_order_relaxed),
                            microcode::g_patches_others.load(std::memory_order_relaxed),
                            microcode::g_creations.load(std::memory_order_relaxed));
        im_i_no_memcmp_ = im_i_with_memcmp_ = im_i_checked_ = im_i_version_changed_ = im_i_cancelled_ = 0;
      }
      // IM_LOAD_IMMEDIATE with an exact cache (LoadImmediate).
      if (imm_phase_ != kImmNoBegin) {
        const uint64_t total =
            imm_i_hits_ + imm_i_checked_ + imm_i_misses_ + imm_i_back_ + imm_i_no_cache_;
        uint32_t occupied = 0;
        for (const auto& per_type : imm_cache_) {
          for (const auto& set : per_type) {
            for (const ImmediateLoad& via : set) {
              occupied += via.signature != 0 ? 1 : 0;
            }
          }
        }
        BO2ZM_REPORT_BY(defer_report,
                            "[native] C5a IM_LOAD_IMMEDIATE with a cache: phase {}; in the interval {} "
                            "loads: {} hits without flipping or identifying ({:.1f} %), {} hits checked against the "
                            "usual path, {} misses (new ways), {} because the ring wrapped around and {} without a "
                            "cache; ways in use {} of {}; hits checked since boot {}",
                            NamePhaseImmediate(), total, imm_i_hits_,
                            total ? 100.0 * double(imm_i_hits_) / double(total) : 0.0, imm_i_checked_,
                            imm_i_misses_, imm_i_back_, imm_i_no_cache_, occupied,
                            2 * kImmWays * (uint32_t(1) << kImmBitsSet), imm_checked_);
        imm_i_hits_ = imm_i_checked_ = imm_i_misses_ = imm_i_back_ = imm_i_no_cache_ = 0;
      }
      // Measurement only: fences with pending vertex copies (AnnotateFenceCopies).
      BO2ZM_REPORT_BY(defer_report,
                          "[native] C6 fences with pending vertex copies (measurement only): {} of {} fences "
                          "and interrupts in the interval (at most {} pending copies)",
                          fences_with_copies_, fences_total_, fences_copies_max_);
      fences_with_copies_ = fences_total_ = 0;
      fences_copies_max_ = 0;
      // Measurement only (AnnotateGameAhead).
      BO2ZM_REPORT_BY(defer_report,
                          "[native] C6 game ahead of the ring (measurement only): when the ring finished a "
                          "frame, the game was 0 Swaps ahead in {}, 1 in {} and 2 or more in {} (at most {}; difference "
                          "at the first frame {})",
                          game_ahead_[0], game_ahead_[1], game_ahead_[2], game_ahead_max_,
                          game_ahead_first_);
      game_ahead_[0] = game_ahead_[1] = game_ahead_[2] = 0;
      game_ahead_max_ = 0;
      const HookStats g = HooksStats();
      BO2ZM_REPORT_BY(defer_report, "[native] C5b: shaders created VS={} (known {}) PS={} (known {}); draws paired={} "
                  "without_record={} records skipped={} lost={}; with VS={} with PS={}; records with shaders different "
                  "from the IM_LOADs={} (draws left without a record because of it={}); draws with the ring's "
                  "identity={}; VS variants={} (different length {}, elements without a fetch {})",
                  g.created_vs, g.known_vs, g.created_ps, g.known_ps,
                  draws_paired_, draws_no_register_, registers_skipped_, g.lost,
                  draws_with_vs_, draws_with_ps_, candidates_incoherent_,
                  draws_incoherent_, draws_per_im_load_, variants_views_.size(),
                  variants_length_different_, elements_no_fetch_);
    }
    if (!histogram_annotated_ && swaps_.load() >= 60) {
      histogram_annotated_ = true;
      std::string lines;
      for (uint32_t op = 0; op < opcodes_.size(); ++op) {
        const uint64_t n = opcodes_[op].load();
        if (n) {
          lines += fmt::format(" {:02X}={}", op, n);
        }
      }
      BO2ZM_REPORT_BY(defer_report, "[native] type 3 packets by opcode (hex=count):{}", lines);
    }
  }

#undef BO2ZM_REPORT_BY

  struct Framebuffer {
    uint64_t version = UINT64_MAX;
    VkFramebuffer framebuffer = VK_NULL_HANDLE;
  };

  rex::ui::WindowedAppContext* app_context_ = nullptr;
  std::unique_ptr<rex::ui::vulkan::VulkanProvider> provider_;
#if BO2ZM_NATIVE_METAL
  // The Metal backend while it is built up: its device and shader libraries are opened next to the Vulkan one,
  // which still does all the drawing.
  std::unique_ptr<gpu::metal::Device> metal_;
  // presenter_, when bo2zm_native_metal_present made it a Metal presenter.
  gpu::metal::Presenter* metal_presenter_ = nullptr;
  // The guest's physical memory as a buffer (SetupGuestGpu); empty if Metal could not wrap it.
  gpu::metal::HostBuffer metal_guest_;
#endif
  std::unique_ptr<rex::ui::Presenter> presenter_;

  rex::memory::Memory* memory_ = nullptr;
  rex::runtime::FunctionDispatcher* dispatcher_ = nullptr;
  rex::system::KernelState* kernel_state_ = nullptr;
  bool mmio_registered_ = false;

  std::vector<uint32_t> registers_;
  std::vector<std::atomic<uint8_t>> seen_;

  std::atomic<uint32_t> callback_{0};
  std::atomic<uint32_t> callback_data_{0};
  std::atomic<uint32_t> ring_base_{0};
  std::atomic<uint32_t> ring_words_{0};
  std::atomic<uint32_t> generation_ring_{0};
  std::atomic<uint32_t> read_returned_{0};
  std::atomic<uint32_t> pointer_write_{0};
  // Ring wakeups: the game thread counts the writes; the ring thread counts the rest.
  std::atomic<uint64_t> writes_wptr_{0};
  uint64_t writes_wptr_previous_ = 0;
  // Which core the ring thread runs on and how many times it has been moved to another one.
  int core_ring_ = -1;
  uint64_t migrations_ring_ = 0;
  uint64_t migrations_ring_previous_ = 0;
  uint64_t loops_ring_ = 0;
  uint64_t loops_ring_previous_ = 0;
  uint64_t loops_ring_with_data_ = 0;
  uint64_t loops_ring_with_data_previous_ = 0;
  uint64_t waits_ring_exhausted_ = 0;
  uint64_t waits_ring_exhausted_previous_ = 0;
  std::atomic<uint32_t> counter_{0};
  uint64_t bin_mask_ = 0xFFFFFFFFull;
  uint64_t bin_select_ = 0xFFFFFFFFull;

  std::atomic<bool> active_{false};
  std::mutex ring_mutex_;
  std::condition_variable ring_cv_;
  rex::system::object_ref<rex::system::XHostThread> thread_vblank_;
  rex::system::object_ref<rex::system::XHostThread> thread_ring_;

  std::atomic<uint64_t> swaps_{0};
  std::atomic<uint64_t> vblanks_{0};
  std::atomic<uint64_t> packets_{0};
  std::atomic<uint64_t> interrupts_{0};
  uint64_t fences_total_ = 0;       // AnnotateFenceCopies, ring thread only
  uint64_t fences_with_copies_ = 0;
  size_t fences_copies_max_ = 0;
  uint64_t game_ahead_[3] = {};  // AnnotateGameAhead, ring thread only
  int64_t game_ahead_max_ = 0;
  int64_t game_ahead_first_ = 0;
  bool game_ahead_first_valid_ = false;
  std::atomic<uint64_t> indirect_{0};
  std::atomic<uint64_t> writes_memory_{0};
  std::atomic<uint64_t> waits_exhausted_{0};
  std::atomic<uint64_t> draws_{0};
  std::atomic<uint64_t> copies_{0};
  std::atomic<uint32_t> last_copy_target_{0};
  std::atomic<uint32_t> last_copy_control_{0};
  std::atomic<uint32_t> last_copy_info_{0};
  std::atomic<uint32_t> last_copy_pitch_{0};
  std::atomic<uint32_t> swap_frontbuffer_{0};
  std::atomic<uint32_t> swap_width_{0};
  std::atomic<uint32_t> swap_height_{0};
  std::array<std::atomic<uint64_t>, 128> opcodes_{};
  bool histogram_annotated_ = false;  // only the ring thread touches it
  // Render targets, copies and presentation (ring thread only).
  std::unique_ptr<NativeTargets> targets_;
  bool targets_failed_ = false;
  // Identified shaders (ring thread only).
  NativeShaders shaders_;
  const ShaderInput* vs_current_ = nullptr;
  const ShaderInput* ps_current_ = nullptr;
  std::vector<uint32_t> microcode_;
  uint64_t draws_identified_ = 0;
  uint64_t draws_no_vs_ = 0;
  uint64_t draws_no_ps_ = 0;
  static constexpr uint32_t kPairSlots = 4096;  // (VS number + 1) << 32 | (PS number + 1): never 0
  std::array<uint64_t, kPairSlots> pairs_{};
  uint32_t pairs_count_ = 0;
  // Matching with the game's Draw* calls (ring thread only).
  std::deque<DrawRecord> pending_;
  std::unordered_map<uint32_t, const ShaderInput*> objects_;
  uint64_t generation_objects_ = UINT64_MAX;
  // direct cache in front of objects_ (CachedObjectShader).
  struct ObjectMemo {
    uint32_t object = UINT32_MAX;  // UINT32_MAX = empty slot: not a guest object
    const ShaderInput* input = nullptr;
  };
  std::array<ObjectMemo, 16> memo_objects_{};
  uint64_t last_pair_shaders_ = UINT64_MAX;  // CountDrawShaders: the previous draw's pair
  std::span<const uint32_t> vs_microcode_;  // from the IM_LOAD cache or from vs_immediate_
  const ShaderInput* vs_draw_ = nullptr;
  const ShaderInput* ps_draw_ = nullptr;
  uint64_t draws_paired_ = 0;
  uint64_t draws_no_register_ = 0;
  uint64_t registers_skipped_ = 0;
  uint64_t draws_with_vs_ = 0;
  uint64_t draws_with_ps_ = 0;
  uint64_t candidates_incoherent_ = 0;
  uint64_t draws_incoherent_ = 0;
  const ShaderInput* coherence_vs_ = nullptr;
  uint64_t coherence_generation_ = UINT64_MAX;
  bool coherence_ = false;
  const ShaderInput* annotated_vs_ = nullptr;
  uint64_t annotated_generation_ = UINT64_MAX;
  uint32_t warnings_c5b_ = 0;
  uint64_t variants_length_different_ = 0;
  uint64_t elements_no_fetch_ = 0;
  std::unordered_set<uint64_t> variants_views_;
  // Generations, so unchanged data is not uploaded again.
  uint64_t generation_vs_ = 0;  // one per distinct VS microcode (LoadShaderCached)
  // IM_LOAD cache (LoadShaderCached): 2 types x 128 sets x 2 ways.
  struct ShaderLoad {
    uint32_t address_type = 0;
    uint32_t size = 0;
    std::vector<uint32_t> raw;  // as it is in game memory
    std::vector<uint32_t> host;   // swapped: what gets identified and what the vertex input reads
    const ShaderInput* input = nullptr;
    uint64_t hash = 0;      // host XXH3
    uint64_t generation = 0;  // from generations_microcode_, when this content was loaded
    uint64_t use = 0;         // the least recently used way is the one replaced
    // The versions of its slot this content was checked against guest memory with
    // (microcode::BeforeRead and AfterRead). validated = false: it must be compared with memcmp again.
    bool validated = false;
    uint32_t version_slot = 0;
    uint32_t version_global = 0;
  };
  std::array<std::array<std::array<ShaderLoad, 2>, 128>, 2> loads_cache_{};
  uint64_t loads_tic_ = 0;
  uint64_t loads_cached_ = 0;
  uint64_t generations_microcode_ = 0;
  // IM_LOAD without memcmp (LoadShaderCached). Ring thread only.
  static constexpr int kImShortcutNoBegin = -1;
  static constexpr int kImShortcutObserving = 0;
  static constexpr int kImShortcutApplying = 1;
  static constexpr int kImShortcutOff = 2;
  static constexpr uint64_t kImShortcutALook = 200000;
  static constexpr uint64_t kImShortcutCheckEvery = 4096;  // power of 2
  int im_shortcut_phase_ = kImShortcutNoBegin;
  uint64_t im_shortcut_turn_ = 0;
  uint64_t im_shortcut_checked_ = 0;  // agreements with the memcmp since startup
  // Since the last report.
  uint64_t im_i_no_memcmp_ = 0;
  uint64_t im_i_with_memcmp_ = 0;
  uint64_t im_i_checked_ = 0;
  uint64_t im_i_version_changed_ = 0;
  uint64_t im_i_cancelled_ = 0;
  std::vector<uint32_t> vs_immediate_;  // VS from IM_LOAD_IMMEDIATE, uncached
  // Exact IM_LOAD_IMMEDIATE cache (LoadImmediate): 2 types x 32 sets x 8 ways. Ring thread only.
  static constexpr uint32_t kImmBitsSet = 5;
  static constexpr uint32_t kImmWays = 8;
  static constexpr int kImmNoBegin = -1;
  static constexpr int kImmObserving = 0;
  static constexpr int kImmApplying = 1;
  static constexpr int kImmOff = 2;
  static constexpr uint64_t kImmALook = 20000;
  static constexpr uint64_t kImmCheckEvery = 1024;  // power of 2
  std::array<std::array<std::array<ImmediateLoad, kImmWays>, (size_t(1) << kImmBitsSet)>, 2> imm_cache_{};
  std::array<std::array<uint8_t, (size_t(1) << kImmBitsSet)>, 2> imm_last_way_{};  // way of the last hit
  int imm_phase_ = kImmNoBegin;
  uint64_t imm_turn_ = 0;
  uint64_t imm_tic_ = 0;
  uint64_t imm_checked_ = 0;  // hits checked and equal since startup
  // Since the last report.
  uint64_t imm_i_hits_ = 0;
  uint64_t imm_i_checked_ = 0;
  uint64_t imm_i_misses_ = 0;
  uint64_t imm_i_back_ = 0;
  uint64_t imm_i_no_cache_ = 0;
  const ShaderInput* mapped_vs_ = nullptr;
  uint64_t mapped_generation_ = UINT64_MAX;
  // How many constant writes do not change the value. Tells whether comparing before bumping
  // the generation pays off, and by how much.
  uint64_t constants_written_ = 0;
  uint64_t constants_no_change_ = 0;
  uint64_t blocks_constants_ = 0;
  uint64_t blocks_constants_no_change_ = 0;
  uint64_t generation_constants_vs_ = 0;
  int registers_in_block_ = -1;  // -1 = cvar not read yet
  uint64_t generation_constants_ps_ = 0;
  // Generations of the fetch constants and the viewport state (viewport, scissor, clip). They
  // travel in DrawRequest so the translator does not redo per draw what has not changed.
  uint64_t generation_fetch_ = 0;
  uint64_t generation_framing_ = 0;
  uint64_t fetch_written_ = 0;
  uint64_t fetch_no_change_ = 0;
  uint64_t framing_written_ = 0;
  uint64_t framing_no_change_ = 0;
  // Ring thread times since the last report (that thread only).
  uint64_t time_ring_ns_ = 0;
  uint64_t time_draws_ns_ = 0;
  uint64_t time_copies_ns_ = 0;
  uint64_t time_present_ns_ = 0;
  Clock::time_point start_batch_{};  // start of the packet batch being executed (bo2zm_perf_log)
  uint64_t draws_measured_ = 0;
  uint64_t phase_stopwatch_draws_ = 0;  // phase of the DrawNative timer, never reset
  uint64_t copies_measures_ = 0;
  uint64_t presents_measures_ = 0;
  // Ring breakdown per packet: registers (types 0 and 1) and each type-3 opcode.
  uint64_t time_registers_ns_ = 0;
  uint64_t count_registers_ = 0;
  uint64_t words_block_ = 0;   // Measurement only
  // Vector path of WriteRegistersInBlock and its guard.
  static constexpr uint64_t kBlocksAVerify = 200000;
  int registers_vector_ = -1;
  uint64_t blocks_verified_ = 0;
  std::vector<uint32_t> copy_verification_;
  // The D3D shadow (CompareShadow).
  MirrorSnapshot shadow_shot_;
  uint64_t shadow_draws_ = 0;
  uint64_t shadow_lost_ = 0;
  uint64_t shadow_state_compared_ = 0;
  uint64_t shadow_state_different_ = 0;
  uint64_t shadow_fetch_compared_ = 0;
  uint64_t shadow_fetch_different_ = 0;
  uint64_t shadow_constants_compared_ = 0;
  uint64_t shadow_constants_different_ = 0;
  const int32_t ring_spin_ns_ = REXCVAR_GET(bo2zm_native_ring_spin_ns_ab);
  uint64_t shadow_constants_read_compared_ = 0;  // in registers the draw's shaders read (CompareShadow)
  uint64_t shadow_constants_read_different_ = 0;
  uint64_t shadow_constants_read_unknown_ = 0;  // of them, words the device's stream had not given (kFromUnknown)
  std::unordered_map<uint32_t, uint64_t> shadow_per_register_;
  std::unordered_map<uint32_t, uint64_t> shadow_per_register_read_;
  uint64_t shadow_from_[5][2] = {};  // [kFrom*][the ring has the mirror's own value]
  // The FlushState markers (ProcessMarker). Ring thread only.
  static constexpr uint64_t kSpansFastAVerify = 200000;
  int marker_fast_ = 1;  // 0: the fast path was turned off by a mismatch (VerifySpanMarker)
  uint64_t spans_fast_verified_ = 0;
  std::vector<uint32_t> copy_marker_;
  std::vector<uint32_t> marker_plane_;  // a marker that wrapped around the ring, made contiguous (should not happen)
  uint64_t markers_applied_ = 0;
  uint64_t markers_checked_ = 0;
  uint64_t markers_different_ = 0;
  uint64_t markers_bad_ = 0;
  uint64_t marker_spans_ = 0;
  uint64_t marker_spans_slow_ = 0;  // through the regular path (WriteRegistersInBlock or one at a time)
  uint64_t marker_words_ = 0;
  uint32_t warnings_marker_ = 0;
  // The Draw* record carried by the last marker, for the DRAW_INDX that follows. It stays
  // until a draw accepts it or the next Draw* record arrives, like the head of the queue.
  DrawRecord draw_marker_;
  uint32_t draw_marker_mode_ = 0;  // 0 none, kDrawApply or kDrawCheck
  uint16_t vegetation_game_ = 0;  // kVeg* flags of the record used to draw (0: none)
  uint64_t draws_with_marker_register_ = 0;
  uint64_t marker_rejected_registers_ = 0;
  uint64_t checks_draw_ = 0;
  uint64_t checks_draw_different_ = 0;
  uint64_t checks_draw_other_register_ = 0;
  uint32_t warnings_draw_marker_ = 0;
  uint64_t words_loose_ = 0;
  uint64_t stopwatch_packets_ = 0;
  std::array<uint64_t, 128> time_opcode_ns_{};
  std::array<uint64_t, 128> count_opcode_{};
  // WAIT_REG_MEM per polled register or address since the last report.
  struct Wait {
    uint64_t times = 0;
    uint64_t loops = 0;  // failed polls, each with a 1 ms wait
    uint32_t reference = 0;
    uint32_t mask = 0;
    uint32_t info = 0;
  };
  std::unordered_map<uint64_t, Wait> waits_;
  // Draw stages at the previous report (for the deltas).
  uint64_t drawn_previous_ = 0;
  uint64_t timed_previous_ = 0;
  uint64_t vegetation_soon_previous_ = 0;
  std::array<uint64_t, kStagesDraw> stages_previous_{};
  uint64_t discard_with_previous_ = 0;
  uint64_t discard_no_previous_ = 0;
  std::array<uint64_t, 20> counters_previous_{};
  std::array<uint64_t, 3> dedupe_previous_{};  // "C6 counters"
  std::array<uint64_t, kGpuCategories> texels_category_previous_{};  // "C6 open area"
  std::array<uint64_t, kGpuCategories> passes_category_previous_{};
  uint64_t draws_ps_useless_previous_ = 0;
  uint64_t shadows_alpha_active_previous_ = 0;
  uint64_t shadows_alpha_off_previous_ = 0;
  uint64_t draws_ps_needed_previous_ = 0;
  std::array<uint64_t, kGpuCategories> draws_category_previous_{};
  std::array<uint64_t, kGpuCategories> triangles_category_previous_{};
  uint64_t submits_ubo_previous_ = 0;  // "C6 constants by UBO"
  uint64_t shared_observed_previous_ = 0;  // "C6 shared constants"
  uint64_t shared_changed_previous_ = 0;
  std::array<uint64_t, kBucketsSwap> buckets_swap_previous_{};  // "C2: intervals between Swaps"
  uint64_t overlaps_gpu_previous_ = 0;
  uint64_t cost_present_previous_[12] = {};  // "C2: present"
  uint64_t submits_previous_ = 0;
  // Gamma ramp loaded by the game (AnnotateRampGamma), identity at startup. The output applies it.
  std::array<std::array<uint16_t, 3>, 256> ramp_gamma_ = [] {
    std::array<std::array<uint16_t, 3>, 256> t{};
    for (uint32_t i = 0; i < 256; ++i) {
      t[i].fill(uint16_t(i * 0x3FF / 0xFF));
    }
    return t;
  }();
  uint32_t component_ramp_ = 0;
  uint32_t mask_ramp_ = 0b111;  // default DC_LUT_WRITE_EN_MASK (register_table.inc)
  uint64_t version_ramp_ = 0;
  uint64_t version_ramp_submitted_ = 0;  // the identity is already in the output
  std::array<uint64_t, kRegRampLast - kRegRampFirst + 1> writes_ramp_{};
  uint64_t writes_ramp_previous_ = 0;
  // Occlusion queries (QueryOcclusion), ring thread only.
  uint32_t occlusion_base_ = 0;                    // structure of the open query (0 = none)
  std::unordered_set<uint32_t> bases_occlusion_;   // structures seen in an Issue(BEGIN)
  uint32_t warnings_occlusion_ = 0;                  // details of the first ones in the log
  // Finished queries per render target kind (0: under 640 or without MSAA on the Xbox 360; 1: scene,
  // scaled) in the interval, and the largest count written for each since the start.
  std::array<uint64_t, 2> occlusion_per_target_{};
  std::array<uint32_t, 2> occlusion_max_per_target_{};
  uint32_t warnings_occlusion_scene_ = 0;
  // Diagnostic bo2zm_native_diag_constants_ps.
  std::string constants_list_text_;
  std::unordered_set<uint32_t> constants_list_;
  std::unordered_map<uint32_t, Clock::time_point> constants_last_;
  // Test bo2zm_native_occlusion_alternate_s.
  bool occlusion_test_started_ = false;
  bool occlusion_test_fake_ = false;
  Clock::time_point occlusion_test_start_{};
  // Started, finished with a measurement, finished without a measurement yet, started without their
  // end, written with the faked count outside a pair, sum of the written measurements and the
  // largest one written.
  std::array<uint64_t, 7> occlusion_counters_{};
  std::array<uint64_t, 7> occlusion_counters_previous_{};
  uint64_t occlusion_host_previous_[5] = {};  // NativeTargets::StatsOcclusion at the previous report
  // GPU time at the previous report ("C2" lines).
  uint64_t gpu_ns_previous_ = 0;
  uint64_t waits_gpu_previous_ = 0;     // C2 report: ring waits for the GPU
  uint64_t ns_waits_gpu_previous_ = 0;
  uint64_t jobs_gpu_previous_ = 0;
  uint64_t ns_job_gpu_previous_ = 0;
  uint64_t cost_record_previous_[6] = {};  // C2 report: Record and readbacks
  uint64_t gpu_jobs_previous_ = 0;
  uint64_t presented_previous_gpu_ = 0;
  std::array<uint64_t, kGpuCategories> gpu_categories_previous_{};
  std::array<uint64_t, kGpuCategories> fragments_category_previous_{};
  std::array<uint64_t, kGpuCategories> vertices_category_previous_{};
  std::array<uint64_t, kGpuCategories> primitives_category_previous_{};
  uint64_t frames_diagnostic_previous_ = 0;
  std::array<uint64_t, 4> copies_bucket_previous_{};
  std::array<uint64_t, 4> pixels_bucket_previous_{};
  // Real scale of the GPU timestamps (C2 report).
  uint64_t mark_gpu_previous_ns_ = 0;
  uint64_t jobs_precise_previous_ = 0;
  Clock::time_point clock_mark_previous_{};
  double scale_real_ns_ = 0.0;
  double scale_marks_ns_ = 0.0;
  Clock::time_point last_time_report_{};
  Clock::time_point last_report_{};
  // Trace of one frame (bo2zm_native_diag_frame_s).
  Clock::time_point start_system_{};
  bool tracing_ = false;
  bool trace_done_ = false;
  // bo2zm_native_diag_constants_ps is not empty. Refreshed on every Swap.
  bool diag_constants_active_ = false;
  std::unordered_set<uint32_t> textures_dumped_;  // bo2zm_native_diag_vertices_ps
  uint32_t traces_ = 0;
  uint32_t reason_paired_ = 0;  // 0 paired, 1 without a record, 2 inconsistent record
  uint32_t object_vs_ = 0;           // objects of the paired record
  uint32_t object_ps_ = 0;
  // Ring identity for draws without a usable record (UseRingIdentity).
  std::unordered_map<uint64_t, const ShaderInput*> vs_per_microcode_;
  uint64_t hash_vs_ = 0;
  uint64_t hash_vs_generation_ = UINT64_MAX;
  uint64_t draws_per_im_load_ = 0;

  // Vulkan objects for the test image: only the ring thread touches them.
  VkRenderPass render_pass_ = VK_NULL_HANDLE;
  VkCommandPool pool_ = VK_NULL_HANDLE;
  VkCommandBuffer commands_ = VK_NULL_HANDLE;
  VkFence fence_ = VK_NULL_HANDLE;
  bool fence_pending_ = false;
  std::array<Framebuffer, rex::ui::vulkan::VulkanPresenter::kMaxActiveGuestOutputImageVersions>
      framebuffers_{};
  size_t next_framebuffer_ = 0;
};

}  // namespace

uint64_t SwapsNative() {
  return g_swaps_native.load(std::memory_order_relaxed);
}

bool IsActive() {
  // Selected by bo2zm_renderer = "native" (bo2zm_native_system.cpp); the older bo2zm_c1_renderer is not read.
  return bo2zm::native::PortedRendererActive();
}

std::unique_ptr<rex::system::IGraphicsSystem> CreateSystemGraphics() {
  return std::make_unique<NativeRenderSystem>();
}

}  // namespace bo2zm::native
