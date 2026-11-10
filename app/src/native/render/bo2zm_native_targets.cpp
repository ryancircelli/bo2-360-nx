// Derived from GPL-3.0 code; modified for bo2-zm-nx in 2026.
//
// Render targets, resolves and clears, and presentation of the game's image (see bo2zm_native_targets.h).
//
// What it covers
//   - Color and depth render targets, kept as host images of pitch x max(720, pitch) rows, capped at 2048 (narrow
//     targets get all 2048 rows; see HeightTarget). Most color formats are stored as RGBA8; 16-bit float targets and
//     16-bit resolve destinations get 16-bit images (FormatHostTarget, FormatHostResolved).
//   - Resolves: a rectangle of a render target copied to the texture at RB_COPY_DEST_BASE, with the rectangle and the
//     base computed as GetResolveInfo does (graphics/util/draw.cpp), including resolves through EDRAM aliases.
//   - MSAA render targets, used with a single sample (MSAA hangs NVK on the Switch).
//   - Clears from RB_COLOR_CLEAR, converted as the emulation does (vulkan/render_target_cache.cpp), and Direct3D
//     clears that the game draws as rectangles (ClearAsDraw).
//   - Presentation: the resolved texture that fetch constant 0 of the Swap points to is drawn into the presenter's
//     output, by default through the gamma ramp the game loads, as the Xbox 360's display does
//     (shaders/bo2zm_output_gamma_ramp.frag).
//
// Draws (bo2zm_native_draws.cpp)
//   This class lends them the render targets (color and depth), the frame's command buffer and an upload command
//   buffer that is submitted right before it.
//
// Each copy and clear optimization below that could change the image checks itself and turns itself off at the first
// disagreement. Their notes describe the mechanism; a figure that does not name BO2 only illustrates the size of the
// effect and is not a BO2 measurement.
//
// Everything here runs on the native system's PM4 ring thread only, so there are no locks.

#include "bo2zm_native_targets.h"
#include "bo2zm_native_frontbuffer.h"
#include "bo2zm_output_crosshairs.h"
#include "../bo2zm_render_formats.h"
#include "bo2zm_hitch_waits.h"
#include "bo2zm_native_deferred.h"  // the job buffer's calls made by a recorder thread
#include "bo2zm_native_shaders.h"  // Samplers of the PS (bo2zm_native_diag_readers_s)
#include "bo2zm_reflection_demand.h"  // reflection only when it is read

#include "bo2zm_graphics_settings.h"
#include "../../bo2zm_cheats.h"  // the third-person crosshair on the output
#include "../../bo2zm_preset.h"  // the preset note on the output
#include "../../bo2zm_credits.h"  // the home screen's credits line on the output
#include "../../bo2zm_perf_log.h"  // bo2zm_perf_log: GPU time per frame, GPU name

#include <rex/cvar.h>
#include <rex/graphics/xenos.h>
#include <rex/logging.h>
#include <rex/math.h>
#include <rex/graphics/registers.h>
#include <rex/system/xmemory.h>
#include <rex/ui/presenter.h>
#include <rex/ui/vulkan/device.h>
#include <rex/ui/vulkan/presenter.h>
#include <rex/ui/vulkan/util.h>

#include "bo2zm_native_draws.h"  // g_probe_vertex_address
#include "bo2zm_native_depth_resolve.h"
#if defined(__SWITCH__)
extern "C" void armDCacheFlush(void* address, size_t size);  // libnx
#endif

#include <algorithm>
#include <array>
#include <chrono>

#if REX_PLATFORM_SWITCH
// Counter 0 of the console profiler (game FPS), the same one IssueSwap of the emulated path uses.
extern "C" void RexSwitchPerfCount(unsigned id);
// Interval of a long frame for stack sampling (the SDK profiler's section on stutters).
extern "C" void RexSwitchPerfHitch(uint64_t start, uint64_t end);
#endif
#include <cmath>
#include <cstring>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

REXCVAR_DEFINE_INT32(bo2zm_native_resolve_without_copy_alternate_s, 0, "BO2",
                     "Native renderer (test): with N > 0 alternates copying and swapping every N seconds, to "
                     "compare captures of the same spot with the game paused");
REXCVAR_DEFINE_INT32(bo2zm_native_shadow_scale, 100, "BO2",
                     "Native renderer: draws 1600x1600 depth targets (shadow maps) at this percentage and "
                     "scales them up when resolving them. BO2's shadow target is 560x720, so this does not act in "
                     "BO2. The value is taken when the first map is created and does not change while running")
    .range(50, 100);
REXCVAR_DEFINE_BOOL(bo2zm_native_resolve_without_copy, true, "BO2",
                    "Native renderer (on by default): when resolving a whole target to a texture of the same "
                    "size and format, swaps the images "
                    "instead of copying the pixels. The image does not change; if the game draws to that target again "
                    "without clearing it first, the copy is restored");
/*
 * Resolving by swapping images even when the command does not clear the render target
 * (bo2zm_native_swap_without_clear).
 *
 * A shadow map resolved twice per frame, once per cascade, is the typical case. The first command clears the render
 * target and therefore swaps images for free; the second does not clear, so without this it copies the whole map.
 *
 * Requiring a clear was a precaution, not a necessity: if the game draws into the render target again without
 * clearing it, RestoreContent brings the image back from the resolved texture, so the worst case is paying the same
 * copy later. In practice that is what happens, exactly one restore per swap (see below), so this saves nothing on
 * its own and costs nothing either. The restores are what the next settings deal with. The
 * report shows both counts: "C2 resolves without a copy" and "restorations".
 */
/*
 * Swapping color render targets too, not only depth (bo2zm_native_swap_color, off).
 *
 * Resolving by swapping images pays off for depth. For color it could never apply: a color render target is created
 * max(720, pitch) rows high (880x880 for BO2's 880-wide scene) while its resolved texture is 880x720, and
 * SwapWithResolved requires the same size, so full-screen color resolves stay copies.
 *
 * Both ways of making it work failed. Without requiring a clear there were as many restores as swaps: the game draws on
 * top again without clearing, so the copy is still paid, later and as two operations. Creating the render target only
 * 720 rows high, so that the sizes match, broke the scene (a yellow smear across the screen). The mechanism itself is
 * correct and the code stays, switched off; what defeats it is that the game reuses its color targets without clearing
 * them.
 *
 * Color render targets do get the same safety net as depth: TargetColor calls RestoreContent, as TargetDepth does,
 * so the first frame in which the game draws on top without clearing never sees the previous frame's content.
 */
REXCVAR_DEFINE_BOOL(bo2zm_native_swap_color, false, "BO2",
                    "Native renderer: when resolving the whole color target to a texture of the same size, swap "
                    "the images instead of copying the pixels. The image does not change; if the game draws on top "
                    "without clearing, it is restored");

REXCVAR_DEFINE_BOOL(bo2zm_native_swap_without_clear, true, "BO2",
                    "Native renderer: swap the image also when the resolve command does not clear the target. "
                    "It takes out the copy of a shadow map resolved a second time without a clear. If the game "
                    "draws on top "
                    "without clearing, it is restored: worst case, the same as now. Watch 'restorations' in the log: it "
                    "has to stay at 0");
/*
 * Bringing a target's content back before resolving it again (bo2zm_native_resolve_valid_content).
 *
 * A resolve done by swapping leaves the content in the texture and gives the render target the texture's old image
 * (content_invalid). That image used to be fixed only when something was drawn on top (TargetDepth), never before
 * another resolve. When a shadow map is resolved twice in a row with nothing drawn in between (once without a clear
 * to one texture, once with a clear to another), the second resolve reads the previous frame's map:
 *     draw into A -> resolve without a clear: the texture gets A (correct), the render target gets B (old)
 *                 -> resolve with a clear: the texture gets B, the previous frame's map
 * The scenery then sampled a map one frame old with the current frame's light matrix. While the shadow framing holds
 * still that is invisible; as soon as it jumps (a camera coming in while rotating), the old map lands shifted and
 * part of the scene goes dark for one frame.
 *
 * Now, if the render target about to be resolved has its content in another texture, it is copied back first and
 * the resolve goes on as usual. That is the exact behavior: on the Xbox 360 both resolves read the same EDRAM. It
 * costs a copy only where it happens (one full-size copy of the map). Report line:
 * "C2 restorations per frame", the "to resolve" figure. false = the old behavior.
 */
REXCVAR_DEFINE_BOOL(bo2zm_native_resolve_valid_content, true, "BO2",
                    "Native renderer: before resolving a depth target whose content went away in a "
                    "swap, bring it back. Fixes the flicker of the menu shadows (the scene sampled the previous frame's "
                    "map). false = the old behavior");
/*
 * What the restores cost.
 *
 * Swaps without a clear and restores rise by exactly the same amounts: one restore per swap. So swapping without a
 * clear saves no pixels: the full-size copy is paid later, as two operations instead of one.
 *
 * Restores were also the missing piece of the frame's copy budget, and showed up in no bucket of the report: the
 * recorded copies and the scaling blit alone fell well short of the measured copy time, and the restores (one
 * full-size copy per swap without a clear) made up the difference. They are now recorded (AnnotateCopy in
 * RestoreContent), so the budget adds up by itself.
 *
 * Turning swap_without_clear off would only move the copy back to the resolve. What has to go is the restore, which
 * is what this setting (copying only the rows in use) and the next one are for.
 */
REXCVAR_DEFINE_BOOL(bo2zm_native_restore_useful_area, true, "BO2",
                    "Native renderer: when bringing back the content of a swapped target, copy only the "
                    "rows the game really resolves instead of the whole image. The targets are created with height = "
                    "max(720, pitch) (the scene is 880x880 to draw 880x720), and what is below the useful area is "
                    "never drawn or read");
/*
 * Restoring by swapping the images back instead of copying (bo2zm_native_restore_by_swap, off).
 *
 * The render target lost its content in a swap: it is in the resolved texture R. Normally R is copied back into the
 * render target (a full-size copy of the shadow map every frame), but the content does not have to be duplicated,
 * only put back, and swapping the two images again does that for free. The catch is that R keeps the render target's
 * old image until the game resolves to that address again, so anything that samples R in between reads stale
 * content. "loaned reads" in the C2 report counts the times a resolved texture was requested while lent out.
 *
 * When R is read while lent out once per frame, every frame, that can look harmless at first, since the frame-time
 * variance improves more than with any other change. The "C2 loaned reads, by address" line names what is read
 * while lent.
 *
 * But the read was not harmless, and this setting is off for good. A shadow pass can resolve the same render target
 * twice: after the base geometry without a clear (texture[1]) and, after the overlay is drawn on top, with a clear
 * (texture[0]). The overlay's own materials sample texture[1] (the map without the overlay, so they do not shadow
 * themselves) and the rest of the scene samples texture[0]. Between the two resolves the render target was restored
 * by swapping, so texture[1] got its old image back and stayed lent out until the next frame's resolve; the scene read
 * it before then (the loaned reads matched the reads of texture[1] one for one), and since it always got the same
 * image back, everything that samples texture[1] saw a map frozen since the start of play: objects lost their
 * shadows. The saving was not real. With false, "loaned reads" and "by swap" must stay at 0.
 */
REXCVAR_DEFINE_BOOL(bo2zm_native_restore_by_swap, false, "BO2",
                    "Native renderer (off by default): bring back the content of a swapped target by "
                    "swapping the images again instead of copying them. DO NOT TURN ON: it leaves texture[1] of the "
                    "shadow map on loan, the one the overlay samples, and it stops receiving shadows. Watch "
                    "'loaned reads' in the log: it has to be 0");
/*
 * Depth that nobody samples is not copied (bo2zm_native_lazy_depth).
 *
 * A game can resolve the scene depth to a texture every frame whose only reader is the final composite, where it only
 * feeds a blur factor. With bo2zm_native_no_blur (on by default) that sampling is dead: a specialization constant cuts
 * the blend and the compiler removes the read, yet the copy is still paid.
 *
 * So the copy is deferred, and only for an address the composite (without blur) requested in the last kLazyFrames
 * frames and that no draw really sampled. It is kept pending (source, destination, rectangle):
 *   - if a draw really samples it (another shader, the composite with depth of field, or with the blur on), it is
 *     copied right before that draw. The source cannot have changed: every write to it goes through BeforeWriteDepth
 *     first (pass, clear, restore, swap and destruction);
 *   - if the source is about to be written in a later frame and the composite already requested it, nobody really
 *     read it, and it is dropped without copying (the normal case in gameplay: the next frame's scene clear);
 *   - if the source is about to be written in the same frame, or before the composite requested it, it is copied
 *     (exact);
 *   - if another resolve arrives at the same address while the copy is pending, it is copied first (exact).
 * GUARD: if a draw samples an address whose copy was dropped (a late read of a previous frame's depth after the next
 * frame has started), MISMATCH in the log and it turns off for the session. Report line: "C2 lazy depth".
 * false = always copy, as before.
 */
REXCVAR_DEFINE_BOOL(bo2zm_native_lazy_depth, true, "BO2",
                    "Native renderer: the depth the game resolves for the final composite is only "
                    "copied if a draw really samples it (without the blur, none does). The image does not change. "
                    "Checks itself. false = always copy, as before");
/*
 * The front buffer is drawn from its render target, without copying it (bo2zm_native_lazy_front).
 *
 * At the end of each frame a game resolves its output to one of its front buffers, and the Swap draws it with
 * PaintOutput. When no draw samples a front buffer ("C2 faces resolved_list"), only the Swap reads it, and the copy
 * is pure overhead.
 *
 * If the address is a front buffer (a Swap drew it in the last kFrontFrames and no draw sampled it)
 * and the copy is 1 to 1, of the whole texture and from the corner of the render target, it is deferred:
 *   - in the Swap it is drawn from the image that holds the content with the exact variant of the output
 *     (texelFetch of the pixel, output of the front buffer's size and without FXAA, which is the usual
 *     one): texel (x, y) of that image is the same byte the copy would have left at (x, y) of the texture.
 *     With FXAA, without the ramp or with another output, the copy is recorded before submitting the work
 *     and the texture is drawn, as always;
 *   - if the game clears the whole render target (which it does when the next frame starts), the render
 *     target gets a spare image of the same size and the one with the content is kept for the front
 *     buffer: the clear does not need what was there, so nothing is copied. There are at most
 *     kFrontImagesMax spares (~4 MB each);
 *   - if the render target is written some other way first (a pass, a restore, a swap), if a draw samples
 *     the front buffer or if another resolve arrives that does not cover it entirely, the copy is recorded
 *     first (exact);
 *   - if another resolve arrives that covers it entirely, the deferred copy is unnecessary and dropped.
 * The content is never lost: the image is the same by construction. GUARD: if something requests a front
 * buffer whose copy can no longer be made, MISMATCH in the log and it turns off for the session. Report
 * line: "C2 lazy front buffer". false = always copy, as before.
 */
REXCVAR_DEFINE_BOOL(bo2zm_native_probe_host_import, false, "BO2",
                    "Test: once, after 3,000 frames, imports guest memory around a recent vertex range as GPU memory "
                    "(VK_EXT_external_memory_host), has the GPU copy 1 MB of it and compares that with the CPU's view "
                    "(\"host import probe\" lines in the log)")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
REXCVAR_DEFINE_BOOL(bo2zm_native_lazy_front, true, "BO2",
                    "Native renderer: the Swap paints the front buffer from its render target (or a "
                    "retained image) instead of copying it to the texture first: one front buffer copy less per "
                    "frame. Same "
                    "image; with FXAA it is copied as always. Checks itself. false = always copy, as before");
REXCVAR_DEFINE_BOOL(bo2zm_native_front_partial_clear, true, "BO2",
                    "Native renderer: initialize a Swap-only front buffer source before its first resolved-color "
                    "composition pass. Keeps unwritten borders black; false retains the previous contents");
REXCVAR_DEFINE_BOOL(bo2zm_native_diag_front, false, "BO2",
                    "Diagnostic: log front composition ownership and guard decisions during the one frame "
                    "selected by bo2zm_native_diag_frame_s, up to 512 records. Does not enable clearing");
/*
 * The shadow map without the full-size copy (bo2zm_native_shadow_min).
 *
 * Inactive in BO2: no BO2 pixel shader samples SHADOWMAP_SAMPLER, so the guard never leaves WATCHING (the "C5a"
 * report line says 0 pixel shaders sample the shadow map, on the console and on the PC). It is for a shadow pass that
 * draws the base geometry into its render target, resolves it without a clear to texture[1], draws the overlay on
 * top and resolves it with a clear to texture[0]. Since we resolve by swapping images, after the first resolve the
 * render target no longer holds the base map and it has to be brought back before the overlay: a full-size copy,
 * which can be the most expensive copy in the frame.
 *
 * With this setting the render target is cleared to 1.0 instead of copied and the overlay is drawn alone.
 * texture[0] ends up with the overlay only, and the draws that sample it through their SHADOWMAP_SAMPLER
 * (tfetch2DShadowMin in the library) get min(texture[0], texture[1]): the 3D index word of that register
 * carries texture[1] and the pipeline carries the SPEC_CONSTANT_SHADOW_MIN bit (23). This is exact, not
 * an approximation: with Z writes and a LESS or LEQUAL test, what a draw leaves on a buffer is the minimum
 * of what was there and of its fragments, and min(base, min(1, overlay)) = min(base, overlay) because no
 * texel exceeds 1.0. The map is point-sampled and both views carry the same swizzle: the result is the same
 * value the copy would have left. The depth bias is the same on both paths (same pipelines). With the cheap
 * PCF there is one extra read per fragment that samples the shadow; with the 9-sample PCF it would be nine
 * and it does not pay off: without bo2zm_native_cheap_pcf it copies as always.
 *
 * It costs a clear of the map and that read, both much cheaper than the copy it saves.
 *
 * GUARD, self-checking:
 *   - WATCHING: it copies as always and watches the whole cycle (resolve without a clear by swapping, overlay
 *     pass on the same render target, resolve with a clear by swapping), that every draw of the overlay pass
 *     is exact (pass without color, without stencil, without an occlusion query and, if it writes Z, a NEVER,
 *     LESS or LEQUAL test) and that every read of texture[0] comes from a shader with tfetch2DShadowMin in
 *     that register and outside the window between the two resolves. Those reads already use the pipeline
 *     with the bit (minimum with itself: the same texel), so switching to applying does not create pipelines
 *     in the middle of play. After kShadowMinCycles clean cycles in a row it moves to
 *   - APPLYING: clears instead of copying. If anything watched fails, MISMATCH in the log (REXLOG_ERROR) and
 *   - OFF for the session: it copies again. A library without tfetch2DShadowMin never leaves watching.
 * Report line: "C2 shadow by minimum". false = always copy, as before.
 */
REXCVAR_DEFINE_BOOL(bo2zm_native_shadow_min, true, "BO2",
                    "Native renderer: the base shadow map is no longer restored by a "
                    "full-size copy; the overlay is drawn onto the cleared target and the scene samples the minimum of "
                    "the two textures. Same image. Needs the library with tfetch2DShadowMin. Checks itself "
                    "(inactive in BO2: no shader samples SHADOWMAP_SAMPLER). false = copy as before");
REXCVAR_DEFINE_INT32(bo2zm_native_shadow_min_alternate_s, 0, "BO2",
                     "Native renderer (test): with N > 0 and the guard already applying, alternates "
                     "every N seconds between the minimum (even spans) and the usual copy (odd spans), to measure the "
                     "net gain on the console with 'C2: GPU per Swap'. 0 = no alternation");
/*
 * Repeated clears.
 *
 * vkCmdClearColorImage and vkCmdClearDepthStencilImage clear the whole image, and the game requests some
 * clears on a render target that is already cleared to that same value and has not been drawn to since.
 * That clear does not change a single bit.
 *
 * The condition is deliberately conservative: it is skipped only if (a) the last clear of that render
 * target used the same value, (b) the global draw counter has not gone up since then (meaning nothing has
 * been drawn anywhere) and (c) the render target has not changed image through a swap nor received a
 * restore. With that it is impossible to skip a clear that is needed.
 */
REXCVAR_DEFINE_BOOL(bo2zm_native_skip_repeated_clears, true, "BO2",
                    "Native renderer: skip a clear when the target is already cleared to that same value "
                    "and nothing has been drawn since. Does not change a single pixel; 'clears skipped' in the C2 "
                    "report says how many are saved");
/*
 * How much of each clear is used (bo2zm_native_diag_clears).
 *
 * Clears all clear the whole image, which is larger than the area drawn: 880x880 for BO2's 880x720 scene, and
 * 2048 rows for a narrow target (see HeightTarget). To
 * clear less, it must first be known, per render target, which part is really used until the next clear:
 * what the passes load and store (their renderArea, the useful area), the rectangles that are resolved,
 * what is restored and what is swapped (whole). Measurement only: it does not change the image. One line
 * every 20 s: "C2 clears per target". false = not measured.
 */
// false by default. Measured: the clip to the useful area that needs it saved almost no GPU time, and the
// tracking runs on the PM4 ring thread, which is now the bottleneck.
REXCVAR_DEFINE_BOOL(bo2zm_native_diag_clears, false, "BO2",
                    "Native renderer (diagnostic): per target, the cleared area versus the area used "
                    "until the next clear (passes, resolves, restorations and swaps). One line every 20 s ('C2 clears "
                    "per target'). Does not change the image");
/*
 * Color clears only over the area in use (bo2zm_native_clear_useful_area).
 *
 * vkCmdClearColorImage clears the whole image: 880x880 for BO2's 880x720 scene, 2048 rows for a narrow
 * target. With the measurement of bo2zm_native_diag_clears (the
 * largest height used by passes, resolves, restores and swaps of that render target, plus one row of margin,
 * rounded up to the next multiple of 64), once a render target has kAreaUsefulCycles measured clears only that
 * top band is cleared, with a loadOp = CLEAR pass (the same thing NVK does internally, over fewer pixels).
 * The bottom band is recorded with its color and cleared before any use that reaches it: the result is the
 * same as clearing it all. Draws do not touch it: NVK clips each pass to its renderArea (SET_SURFACE_CLIP)
 * and the pass that reaches the band completes it before it opens. If a render target needs that 3 times,
 * it stops being clipped (its useful area is not stable). Color only: depth without TRANSFER_DST is cleared
 * per pass and that pass clears the whole ZCULL region (NVK), so it is not clipped. GUARD: if a band cannot
 * be completed, MISMATCH in the log and it turns off for the session. false = clear the whole image, as
 * before.
 */
// false by default. Measured: almost no GPU time saved per frame: not worth it with the
// PM4 ring as the bottleneck.
REXCVAR_DEFINE_BOOL(bo2zm_native_clear_useful_area, false, "BO2",
                    "Native renderer: color clears only clear the rows really used; the rest is "
                    "cleared before anything uses it. Same image. Needs bo2zm_native_diag_clears. false = the whole "
                    "image, as before");
// Defined in bo2zm_native_draws.cpp; here it is only read so as not to open two queries of the same type
// at once.
REXCVAR_DECLARE(int32_t, bo2zm_native_per_draw_stats_s);
// bo2zm_native_shadow_min only pays off with the single-sample PCF (defined in bo2zm_native_draws.cpp).
REXCVAR_DECLARE(bool, bo2zm_native_cheap_pcf);
REXCVAR_DEFINE_BOOL(bo2zm_native_pipeline_stats, false, "BO2",
                    "Native renderer: counts shaded fragments, vertex invocations and clipped "
                    "primitives per pass type (C2 report). The image does not change, but the queries cost GPU time: "
                    "for measuring only");
REXCVAR_DEFINE_BOOL(bo2zm_native_diag_clear, false, "BO2",
                    "Native renderer: clear each render target with its own color instead of the game's color (tests "
                    "only: checks copy, clear and presentation)");
REXCVAR_DEFINE_BOOL(bo2zm_native_diag_resolved, false, "BO2",
                    "Native renderer: present the resolved textures of each frame as a mosaic, in the order of their "
                    "copies (tests only)");
/*
 * Who reads each resolved texture (bo2zm_native_diag_readers_s, measurement only).
 *
 * "C2 faces resolved_list" counts copies and reads per address, but a read is a call to TextureResolved and only
 * happens when the sampler caches of VulkanDraws miss (per frame and per fetch constant): it does not say which shader
 * reads nor after which copy. For a texture written twice per frame (for example the blit of the scene and, after the
 * composition, the composited scene 1 to 1) and read twice, that leaves the question open: if both reads come from
 * the first write, nobody reads the second one before the next frame covers it entirely, and it is unnecessary.
 *
 * Every N seconds two whole frames are examined: every logical write to a resolved texture (the color or
 * depth resolve, even if the real copy is deferred or is a swap), every draw that samples it according to
 * the fetch constants of its pixel shader, bypassing the caches, and the Swap that draws the front buffer.
 * At the end, one line per address: for each write, who reads it until the next one (PS nN, sampler, draws
 * and the render target being drawn into) and UNREAD if nobody reads it and the next one covers it entirely.
 * What is read before the first write of the window goes to the last one before it ("[before]"). The
 * per-address lines are only printed if their pattern changes (the first three windows, all of them); the
 * summary line always is.
 * A write that comes out as UNREAD stays watched in every frame (menus, pause, rain, cutscenes...),
 * separating the reads before its source is written again (a deferred copy covers them: it would be recorded
 * right before that draw; this is the case of raindrops drawn on the screen) from the ones after
 * (with a single one, dropping the copy when the source is written would not be exact). The first of each
 * kind is reported; the counts, in every summary. It is the evidence needed before deferring or dropping a
 * copy.
 * Cost: outside the window, one if per draw and per copy (and, with something watched, comparing each
 * sampler's address with 1-4 addresses); in the two frames of the window, one lookup in the resolved map
 * per sampler. 0 = off.
 */
// 0 by default. Measured: its watching only served the lazy composite copy, which saves nothing.
REXCVAR_DEFINE_INT32(bo2zm_native_diag_readers_s, 0, "BO2",
                     "Native renderer: every this many seconds two whole frames are looked at and, "
                     "for each resolved texture, it writes which pixel shaders read each write before the next one (C2 "
                     "readers lines); what comes out as SPARE is watched afterwards in every frame. Measures only. 0 = "
                     "off")
    .range(0, 3600);
/*
 * The composited scene is only copied if someone reads it (bo2zm_native_lazy_composite).
 * A scene texture can be written twice per frame: the blit of the scene and, after the composition, the composited
 * scene 1 to 1 from the output, with the second write read only by a later effect drawn before the HUD, if at all.
 * It is deferred: it is recorded right before the first draw that samples it without its source having been written
 * again (exact), dropped if another write covers it entirely (exact) and dropped when its source (the HUD) is written
 * if nobody has read it. It only applies to the
 * (address, source) pair that the watching of bo2zm_native_diag_readers_s has seen kCompositeALook times with no read
 * after the source was written, and it turns itself off if a draw samples the texture after its copy was dropped
 * (MISMATCH in the log). Without the diagnostic (bo2zm_native_diag_readers_s = 0) it never applies. false = always
 * copy, as before.
 */
// false by default. Measured: nearly every copy was recorded anyway because a draw reads it almost
// every frame: nothing saved.
REXCVAR_DEFINE_BOOL(bo2zm_native_lazy_composite, false, "BO2",
                    "Native renderer: the 1:1 copy of the composite scene to its texture (before "
                    "the HUD) is postponed: it is recorded if a draw samples it before its source is written "
                    "again (the raindrops) and dropped if not. Turns on after the bo2zm_native_diag_readers_s watch and "
                    "turns itself off at the first disagreement. false = always copied");
REXCVAR_DEFINE_INT32(bo2zm_native_read_resolved_texels, 4096, "BO2",
                     "Native renderer: resolved textures of up to this many texels are also copied to guest memory, "
                     "which the game reads for its exposure (0 = none; 4096 = up to 64x64)");
REXCVAR_DEFINE_BOOL(bo2zm_native_invalidate_textures_every_copy, false, "BO2",
                    "Native renderer: drop the texture caches on every copy (what was done before). Now "
                    "they are only dropped when a resolved texture is created, remade, prepared or changes its channel "
                    "order");
/*
 * Painting each output without waiting for the previous one (bo2zm_native_output_without_wait, below).
 *
 * Without it, the ring thread waited almost a whole 60 Hz vsync per frame for the previous presentation to finish,
 * which pushes a frame that should land on one vsync onto the next.
 *
 * The SDK already keeps a mailbox of three output images for exactly this (kGuestOutputMailboxSize = 3: presenting
 * without long interlocking between refreshing the guest output and painting it), and hands out a different image on
 * each refresh. Painting the next one while the previous one is still being presented is the intended use, and the
 * image does not change.
 */
/*
 * How many work slots (frames of work in flight) there are.
 *
 * With 2 the CPU can only be one frame ahead of the GPU; on the console that left the ring thread waiting inside
 * Record() for the GPU 4.8 s out of every 10. With 3 it can be two frames ahead. Each slot costs a 64 MB upload
 * buffer. 2 gives the original behavior.
 *
 * A fourth slot looked worthwhile: where the GPU was the limit, the CPU still arrived first and waited several ms
 * per frame for a slot. But four slots break the image (flicker and odd colors), for a known reason. Work slots and
 * output slots are two different things, and only the first is configurable: kSlotsOutput, which governs the pass
 * that draws the game image on screen (its pools, descriptors, fences_output_, outputs_pending_ and the rotation
 * modulo kSlotsOutput), is fixed at 3. With the work three frames ahead, an output slot whose image is still on
 * screen gets reused. Raising this setting needs kSlotsOutput raised with it and all of those places reviewed.
 */
REXCVAR_DEFINE_INT32(bo2zm_native_work_slots, 3, "BO2",
                     "Native renderer: work slots (2 to 4). With more, the CPU runs more frames ahead of the GPU and "
                     "stalls less; each one costs 64 MB. 2 = the original behavior")
    .range(2, 4)
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);

// ZCULL (hierarchical depth culling).
//
// It is enabled by removing TRANSFER_DST from the game's depth buffers, which makes them eligible for a ZCULL plane in
// the driver. The scene and the reflection cubemap end up under ZCULL; the shadow map keeps TRANSFER_DST and does
// not. It saves GPU time in the scene. It was once suspected of darkening a model's material from time
// to time, but that turned out to be the environment cubemap refreshing one face every six frames, so the model's
// lighting lagged and then jumped.
REXCVAR_DEFINE_BOOL(bo2zm_native_zcull, true, "BO2",
                    "Hierarchical depth culling (ZCULL): removes TRANSFER_DST from the game's depth targets so the "
                    "driver gives them a ZCULL plane");

REXCVAR_DEFINE_BOOL(bo2zm_native_output_without_wait, true, "BO2",
                    "Native renderer: paints the output of each Swap rotating 3 slots instead of one, so as not to wait "
                    "for the GPU to finish the previous output (the ring waited there almost one vsync per Swap). "
                    "Does not change the image; false goes back to the old behavior");
// In NVK, TOP_OF_PIPE is PIPELINE_LOCATION_NONE: the timestamp is released when the GPU reads the command,
// not when the previous work finishes, and the per-category breakdown is approximate (a copy gets charged
// with the draw of the previous pass). BOTTOM_OF_PIPE is PIPELINE_LOCATION_ALL: it is released when all
// previous work finishes.
/*
 * On by default, so the measurement does not depend on a test configuration remembering to enable it.
 */
REXCVAR_DEFINE_BOOL(bo2zm_native_precise_marks, true, "BO2",
                    "Native renderer (measurement): GPU timestamps between categories with BOTTOM_OF_PIPE (released "
                    "when the previous work finishes) instead of TOP_OF_PIPE (when the command is read): exact split by "
                    "category. Does not change the image");
REXCVAR_DEFINE_INT32(bo2zm_native_precise_marks_alternate_s, 0, "BO2",
                     "Native renderer (measurement): with N > 0 alternates normal marks (even spans) and precise marks "
                     "(odd spans) every N seconds and annotates each switch, to compare in the same run");
REXCVAR_DEFINE_INT32(bo2zm_native_reads_every, 1, "BO2",
                     "Native renderer: of each target, one in N small copies is read back and written to guest memory "
                     "(1 = all); the game uses them for its exposure");
// The Xbox 360 passes the image through the gamma ramp the game loads, which need not be the identity.
REXCVAR_DEFINE_BOOL(bo2zm_show_fps, false, "BO2",
                    "Shows the frames per second at the top left (in the output pass with the gamma ramp; measured "
                    "every half second)");
REXCVAR_DEFINE_BOOL(bo2zm_native_gamma_ramp, true, "BO2",
                    "Native renderer: applies at the output the gamma ramp the game loads, like the Xbox 360's screen "
                    "(without it, the midtones and shadows come out darker). false: the image as it is")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);

// The 30 FPS guard lives in native/bo2zm_render_stubs.cpp (a stub for BO2), which owns the lever it pulls.
namespace bo2zm::guard30 {
void Heartbeat(double ms);
void Report();
}  // namespace bo2zm::guard30

namespace bo2zm::native {
namespace shaders {
// The same SPIR-V the SDK presenter uses to draw the game image
// (vulkan_presenter.cpp:128-141).
#include "vulkan_spirv/guest_output_bilinear_ps.h"
#include "vulkan_spirv/guest_output_triangle_strip_rect_vs.h"
// The same sampling as guest_output_bilinear_ps with the game's gamma ramp
// (shaders/bo2zm_output_gamma_ramp.frag).
#include "shaders/bo2zm_output_gamma_ramp_ps.h"
#include "shaders/bo2zm_credits_ps.h"  // The home screen's credits line (bo2zm_credits.cpp)
#include "../shaders/bo2zm_resolve_copy_ps.h"
}  // namespace shaders

// 16_16_16_16_FLOAT render targets as half-float images (experimental, see FormatHostTarget).
REXCVAR_DEFINE_BOOL(bo2zm_native_park_resolves, true, "BO2",
                    "Native renderer: when a resolve address changes size or format, keep the old image for the next "
                    "resolve of that shape instead of destroying it after a full GPU wait. false = destroy, as before")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
REXCVAR_DEFINE_BOOL(bo2zm_native_float_targets, true, "BO2",
                    "Native renderer: 16_16_16_16_FLOAT render targets as real half-float images (experimental)")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);

namespace {

namespace xenos = rex::graphics::xenos;
using rex::ui::vulkan::VulkanDevice;
using rex::ui::vulkan::VulkanPresenter;

constexpr VkFormat kFormatColor = VK_FORMAT_R8G8B8A8_UNORM;
constexpr uint32_t kHeightMaxTarget = 2048;

// A render target image's height, from its pitch (at least 720 rows). BO2 also draws narrow, tall strips: its 32x1024 color-grading table (pitch 80) lost rows 720-1023,
// the last slices of the table, and the tonemap turned some colors pure green. Narrow targets get all 2048 rows.
uint32_t HeightTarget(uint32_t pitch) {
  if (pitch <= 256) {
    return kHeightMaxTarget;
  }
  return std::min(kHeightMaxTarget, std::max<uint32_t>(720, (pitch + 15) & ~15u));
}

// The host format of a color render target, by its storage format. The 16-bit formats keep
// their range in a half-float image (Xenos 16_16_16_16 is fixed point in [-32, 32]; FLOAT is half float): BO2's
// luminance chain writes values around 0.001 that RGBA8 rounded to 0, so the auto exposure saw a black scene.
// The rest stay RGBA8 (stage 1, bo2zm_render_formats.h).
VkFormat FormatHostTarget(uint32_t format_storage) {
  // On by default since the fetch scales (exp_adjust, integer reads) are honored: the chain matches and the image has
  // Xenos's brightness without blowouts. bo2zm_native_float_targets=false keeps them RGBA8.
  static const bool floats = REXCVAR_GET(bo2zm_native_float_targets);
  if (!floats) {
    return kFormatColor;
  }
  switch (xenos::ColorRenderTargetFormat(format_storage)) {
    case xenos::ColorRenderTargetFormat::k_16_16_16_16_FLOAT:
      return VK_FORMAT_R16G16B16A16_SFLOAT;
    default:
      return kFormatColor;
  }
}

// The host format of a resolved texture, by RB_COPY_DEST_INFO: the destination's own format,
// so the draws sample what the console would and a read-back gets the guest's bits.
VkFormat FormatHostResolved(uint32_t info_target) {
  const uint32_t format = (info_target >> 7) & 0x3F;
  const uint32_t number = (info_target >> 13) & 0x7;
  if (format == uint32_t(xenos::ColorFormat::k_16_16_16_16)) {
    return (number == 1 || number == 3) ? VK_FORMAT_R16G16B16A16_SNORM : VK_FORMAT_R16G16B16A16_UNORM;
  }
  if (format == uint32_t(xenos::ColorFormat::k_16_16_16_16_FLOAT)) {
    return VK_FORMAT_R16G16B16A16_SFLOAT;
  }
  return kFormatColor;
}

bool Is16BitsPerChannel(VkFormat format) {
  return format == VK_FORMAT_R16G16B16A16_SFLOAT || format == VK_FORMAT_R16G16B16A16_UNORM ||
         format == VK_FORMAT_R16G16B16A16_SNORM;
}
constexpr VkImageSubresourceRange kRangeColor = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
constexpr VkImageSubresourceRange kRangeDepth = {
    VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT, 0, 1, 0, 1};
using FnCopyImage = void(VKAPI_PTR*)(VkCommandBuffer, VkImage, VkImageLayout, VkImage,
                                        VkImageLayout, uint32_t, const VkImageCopy*);
using FnClearDepth = void(VKAPI_PTR*)(VkCommandBuffer, VkImage, VkImageLayout,
                                             const VkClearDepthStencilValue*, uint32_t,
                                             const VkImageSubresourceRange*);
using FnBlit = void(VKAPI_PTR*)(VkCommandBuffer, VkImage, VkImageLayout, VkImage, VkImageLayout,
                                uint32_t, const VkImageBlit*, VkFilter);

// Direct3D 11 16.8 fixed point with rounding, like ui::FloatToD3D11Fixed16p8.
int32_t Fixed16p8(float value) {
  if (!(std::abs(value) >= 1.0f / 512.0f)) {
    return 0;
  }
  const double scaled = std::clamp(double(value) * 256.0, -2147483392.0, 2147483392.0);
  return int32_t(std::lround(scaled));
}

// Address of a texel in a 32x32-tiled texture (pipeline/texture/util.cpp:424-436).
int32_t OffsetTiled2D(int32_t x, int32_t y, uint32_t pitch, uint32_t log2_bytes) {
  pitch = (pitch + 31) & ~uint32_t(31);
  const int32_t macro = ((x >> 5) + (y >> 5) * int32_t(pitch >> 5)) << (log2_bytes + 7);
  const int32_t micro = ((x & 7) + ((y & 0xE) << 2)) << log2_bytes;
  const int32_t offset = macro + ((micro & ~0xF) << 1) + (micro & 0xF) + ((y & 1) << 4);
  return ((offset & ~0x1FF) << 3) + ((y & 16) << 7) + ((offset & 0x1C0) << 2) +
         (((((y & 8) >> 2) + (x >> 3)) & 3) << 6) + (offset & 0x3F);
}

// The same for a 3D texture tiled in 32x32x4 blocks (GetTiledOffset3D,
// graphics/pipeline/texture/util.cpp:438-459; the same copy as in bo2zm_native_draws.cpp). For TextureResolved3D.
int32_t OffsetTiled3D(int32_t x, int32_t y, int32_t z, uint32_t pitch, uint32_t height,
                                uint32_t log2_bytes) {
  pitch = (pitch + 31) & ~uint32_t(31);
  height = (height + 31) & ~uint32_t(31);
  const int32_t macro_exterior = ((y >> 4) + (z >> 2) * int32_t(height >> 4)) * int32_t(pitch >> 5);
  const int32_t macro = ((((x >> 5) + macro_exterior) << (log2_bytes + 6)) & 0xFFFFFFF) << 1;
  const int32_t micro = (((x & 7) + ((y & 6) << 2)) << (log2_bytes + 6)) >> 6;
  const int32_t exterior = ((y >> 3) + (z >> 2)) & 1;
  const int32_t offset1 = exterior + ((((x >> 3) + (exterior << 1)) & 3) << 1);
  const int32_t offset2 = ((macro + (micro & ~15)) << 1) + (micro & 15) +
                          ((z & 3) << (log2_bytes + 6)) + ((y & 1) << 4);
  int32_t address = (offset1 & 1) << 3;
  address += (offset2 >> 6) & 7;
  address <<= 3;
  address += offset1 & ~1;
  address <<= 2;
  address += offset2 & ~511;
  address <<= 3;
  address += offset2 & 63;
  return address;
}

// With 4-byte texels, OffsetTiled2D(x, y, pitch, 2) is
// (((x >> 5) + (y >> 5) * (pitch aligned to 32 >> 5)) << 12) + table[(y & 31) * 32 + (x & 31)]:
// the part inside the 32x32 tile does not depend on the pitch (WriteReads).
const std::array<uint16_t, 1024>& TableTiled2DTexel4() {
  static const std::array<uint16_t, 1024> table = [] {
    std::array<uint16_t, 1024> t{};
    for (int32_t y = 0; y < 32; ++y) {
      for (int32_t x = 0; x < 32; ++x) {
        t[size_t(y) * 32 + size_t(x)] = uint16_t(OffsetTiled2D(x, y, 32, 2));
      }
    }
    return t;
  }();
  return table;
}

int32_t ExtenderSign15(uint32_t value) {
  return int32_t(value << 17) >> 17;
}

// Fixed color per render target with bo2zm_native_diag_clear: the
// capture shows which render target reached the screen.
VkClearColorValue ColorDiagnostic(uint32_t base, uint32_t format, uint32_t pitch) {
  uint32_t h = base * 2654435761u ^ pitch * 2246822519u ^ format * 3266489917u;
  h ^= h >> 15;
  h *= 2246822519u;
  h ^= h >> 13;
  VkClearColorValue color{};
  for (uint32_t j = 0; j < 3; ++j) {
    color.float32[j] = float((h >> (j * 8)) & 0xFF) * (1.0f / 255.0f);
  }
  color.float32[3] = 1.0f;
  return color;
}

using OwnedImage = NativeImage;  // prepared = already in GENERAL and cleared

struct DepthTarget : OwnedImage {
  uint64_t draw_frame = UINT64_MAX;
  uint64_t draw_order = 0;
};

struct Resolved {
  OwnedImage image;
  uint32_t format_guest = 0;
  bool swap_rb = false;
  // Times the draws have requested this address since the last report. It lives here and not in the report
  // map because TextureResolved is on the hot path (one call per texture and draw, thousands per frame) and the
  // lookup of the resolved texture is done anyway.
  uint64_t reads = 0;
};

// C2 report of readbacks per render target (base and size) and cadence of bo2zm_native_reads_every.
struct ReadTarget {
  uint32_t base = 0;
  uint32_t width = 0;
  uint32_t height = 0;
  uint64_t copies = 0;    // small copies seen in the interval
  uint64_t skipped = 0;  // of those, the ones not read because of bo2zm_native_reads_every
};

// Reflection diagnostic: copies to a square resolved texture (AnnotateCopy).
// And of all the others too, with their size and how many times each one is read. That answers whether a
// copy is needed: a resolved texture that is copied every frame and never requested is wasted bandwidth.
struct TargetCopy {
  uint32_t width = 0;
  uint32_t height = 0;
  uint64_t copies = 0;
  uint64_t with_draws = 0;  // copies with some draw since the previous copy
  uint64_t draws = 0;
  uint64_t pixels = 0;
};

// What is known about a render target between game commands. It serves two purposes:
//  - skipping a clear that changes nothing (bo2zm_native_skip_repeated_clears);
//  - restoring only the rows the game really uses (bo2zm_native_restore_useful_area).
// It lives in a separate map and not in NativeImage because that structure belongs to another file.
struct TargetState {
  bool clear_clean = false;    // the content is exactly the last clear, with nothing on top
  uint64_t value_clear = 0;     // packed color, or depth+stencil
  uint64_t draws_on_clear = 0; // global draw counter at that moment
  uint32_t height_used = 0;        // the largest y1 the game has resolved from this render target
};

// Readback of a small resolved texture (bo2zm_native_read_resolved_texels):
// host-visible buffer it is copied to and what is needed to write it to the guest.
struct Readback {
  VkBuffer buffer = VK_NULL_HANDLE;
  VkDeviceMemory guest_memory = VK_NULL_HANDLE;
  uint8_t* data = nullptr;
  VkDeviceSize bytes = 0;
  bool coherent = true;
};
struct PendingRead {
  Readback* read;
  uint32_t base;          // RB_COPY_DEST_BASE
  int32_t x0;             // texel of the texture where the rectangle starts
  int32_t y0;
  uint32_t width;
  uint32_t height;
  uint32_t pitch;         // RB_COPY_DEST_PITCH
  uint32_t height_target;
  uint32_t info;          // RB_COPY_DEST_INFO
  VkFormat format_host = VK_FORMAT_R8G8B8A8_UNORM;  // the format of the image read back
};

// Output slots with bo2zm_native_output_without_wait (without it, only slot 0 is used).
constexpr uint32_t kSlotsOutput = 3;
// GPU timestamps per slot (the last one is kept for the final timestamp).
constexpr uint32_t kMarksPerSlot = 128;
constexpr uint8_t kGpuEnd = 0xFF;
// Host occlusion queries per work unit (draw spans of the game's queries).
constexpr uint32_t kOcclusionsPerSlot = 32;
// Reflection visibility. Own occlusion queries per work unit: the draws that sample the reflection and
// the witness (the final composition). The ones that do not fit are assumed visible.
constexpr uint32_t kVisibilityPerSlot = 16;
constexpr uint8_t kVisibilityWater = 0;
constexpr uint8_t kVisibilityWitness = 1;
// Passes measured per work unit. In gameplay there are ~16 per frame including the resumed ones.
constexpr uint32_t kStatsPerSlot = 64;
constexpr uint32_t kCountersStat = 3;  // vertices, clipped primitives and fragments
// Draws measured in a diagnostic frame (the scene has ~1200).
constexpr uint32_t kStatsDrawPerSlot = 2048;
constexpr uint32_t kLabelsShader = 512;
// Buckets of the copy breakdown by size (pixels of the copy).
constexpr uint32_t kBucketsCopy = 4;
constexpr uint32_t kPixelsBucket[kBucketsCopy] = {64 * 64, 320 * 320, 1024 * 1024, 0xFFFFFFFFu};

// One of the work slots: while a frame is being recorded, the previous ones can still be
// on the GPU with the other one.
struct WorkSlot {
  VkCommandPool pool_job = VK_NULL_HANDLE;
  VkCommandPool pool_upload = VK_NULL_HANDLE;
  VkCommandBuffer job = VK_NULL_HANDLE;
  VkCommandBuffer upload = VK_NULL_HANDLE;
  VkFence fence = VK_NULL_HANDLE;
  bool pending = false;
  uint64_t order = 0;                        // submission number
  // Wall-clock time of the work, from vkQueueSubmit to the signaled fence. The GPU timestamps can add up to
  // much less than the frame while the hardware's own counter says the GPU is almost fully loaded: either there is
  // work we do not mark or the timestamp scale is wrong. This bounds the truth from above (the CPU sees
  // the fence a little late) and the timestamps from below.
  std::chrono::steady_clock::time_point submitted{};
  std::vector<PendingRead> reads;  // readbacks submitted with the work
  std::vector<uint8_t> categories;  // GPU category of each timestamp written
  bool marks_precise = false;      // intermediate marks with BOTTOM_OF_PIPE
  // Occlusion queries recorded in the work, in order from the slot's first one, with the number of the game
  // query they add to.
  std::vector<std::pair<uint32_t, uint64_t>> occlusions;
  // Statistics queries recorded, with the category of each one's pass.
  std::vector<std::pair<uint32_t, uint8_t>> stats;
  // Per-draw queries, with the pixel shader number of each one.
  std::vector<std::pair<uint32_t, uint16_t>> stats_draw;
  // Reflection visibility queries, with their type (kVisibilityWater or kVisibilityWitness).
  std::vector<std::pair<uint32_t, uint8_t>> visibility;
};

// A game occlusion query (from its Issue(BEGIN) to its Issue(END)) while its spans are being counted on the
// GPU.
struct GameOcclusionQuery {
  uint32_t base = 0;              // D3D counter structure
  uint64_t samples = 0;          // sum of the spans read
  uint32_t spans_pending = 0;  // recorded and not yet read
  bool finished = false;         // its Issue(END) has already arrived
  bool failed = false;           // some span without room or not read: not published
};

class VulkanTargets final : public NativeTargets, public TargetContext {
 public:
  VulkanTargets(const VulkanDevice* vulkan_device, rex::memory::Memory* guest_memory)
      : vulkan_device_(vulkan_device),
        dfn_(deferred::Table(*vulkan_device)),  // the device's, with the vkCmd* calls queued (deferred.h)
        device_(vulkan_device->device()),
        memory_(guest_memory),
        family_(vulkan_device->queue_family_graphics_compute()) {}

  ~VulkanTargets() override {
    WaitGpu();
    draws_.reset();  // their framebuffers and views point to these images
    for (auto& [key, image] : depths_) {
      Destroy(image);
    }
    for (auto& [key, image] : targets_) {
      Destroy(image);
    }
    for (auto& [key, resolved] : resolved_) {
      Destroy(resolved.image);
    }
    for (auto& [key, resolved] : parked_) {
      Destroy(resolved.image);
    }
    for (auto& [key, volume] : volumes_) {
      Destroy(volume.image);
    }
    Destroy(tiled_);
    for (FrontImage& spare : fronts_images_) {  // bo2zm_native_lazy_front
      Destroy(spare.image);
    }
    reads_pending_.clear();
    for (WorkSlot& slot : slots_) {
      slot.reads.clear();
    }
    for (auto& [key, read] : reads_) {
      DestroyRead(read);
    }
    for (auto& f : framebuffers_) {
      if (f.framebuffer != VK_NULL_HANDLE) {
        dfn_.vkDestroyFramebuffer(device_, f.framebuffer, nullptr);
      }
    }
    if (pipeline_ != VK_NULL_HANDLE) dfn_.vkDestroyPipeline(device_, pipeline_, nullptr);

    for (auto& [view, fb] : conv_framebuffers_) {
      if (fb != VK_NULL_HANDLE) dfn_.vkDestroyFramebuffer(device_, fb, nullptr);
    }
    for (auto& [format, p] : conv_pipelines_) {
      if (p != VK_NULL_HANDLE) dfn_.vkDestroyPipeline(device_, p, nullptr);
    }
    for (auto& [format, rp] : conv_passes_) {
      if (rp != VK_NULL_HANDLE) dfn_.vkDestroyRenderPass(device_, rp, nullptr);
    }
    if (conv_pool_ != VK_NULL_HANDLE) dfn_.vkDestroyDescriptorPool(device_, conv_pool_, nullptr);
    if (conv_fs_ != VK_NULL_HANDLE) dfn_.vkDestroyShaderModule(device_, conv_fs_, nullptr);
    for (VkPipeline p : pipelines_ramp_) {
      if (p != VK_NULL_HANDLE) dfn_.vkDestroyPipeline(device_, p, nullptr);
    }
    if (fs_ramp_ != VK_NULL_HANDLE) dfn_.vkDestroyShaderModule(device_, fs_ramp_, nullptr);
    // The credits line
    if (credits_.pipeline != VK_NULL_HANDLE) dfn_.vkDestroyPipeline(device_, credits_.pipeline, nullptr);
    if (credits_.fs != VK_NULL_HANDLE) dfn_.vkDestroyShaderModule(device_, credits_.fs, nullptr);
    if (credits_.pool != VK_NULL_HANDLE) dfn_.vkDestroyDescriptorPool(device_, credits_.pool, nullptr);
    if (credits_.view != VK_NULL_HANDLE) dfn_.vkDestroyImageView(device_, credits_.view, nullptr);
    if (credits_.image != VK_NULL_HANDLE) dfn_.vkDestroyImage(device_, credits_.image, nullptr);
    if (credits_.memory != VK_NULL_HANDLE) dfn_.vkFreeMemory(device_, credits_.memory, nullptr);
    if (credits_.staging != VK_NULL_HANDLE) dfn_.vkDestroyBuffer(device_, credits_.staging, nullptr);
    if (credits_.staging_memory != VK_NULL_HANDLE) dfn_.vkFreeMemory(device_, credits_.staging_memory, nullptr);
    for (OutputRamp& ramp : ramps_output_) {
      if (ramp.buffer != VK_NULL_HANDLE) dfn_.vkDestroyBuffer(device_, ramp.buffer, nullptr);
      if (ramp.guest_memory != VK_NULL_HANDLE) dfn_.vkFreeMemory(device_, ramp.guest_memory, nullptr);
    }
    if (layout_pipeline_ != VK_NULL_HANDLE)
      dfn_.vkDestroyPipelineLayout(device_, layout_pipeline_, nullptr);
    if (pool_descriptors_ != VK_NULL_HANDLE)
      dfn_.vkDestroyDescriptorPool(device_, pool_descriptors_, nullptr);
    if (layout_descriptors_ != VK_NULL_HANDLE)
      dfn_.vkDestroyDescriptorSetLayout(device_, layout_descriptors_, nullptr);
    if (sampler_ != VK_NULL_HANDLE) dfn_.vkDestroySampler(device_, sampler_, nullptr);
    if (vs_ != VK_NULL_HANDLE) dfn_.vkDestroyShaderModule(device_, vs_, nullptr);
    if (fs_ != VK_NULL_HANDLE) dfn_.vkDestroyShaderModule(device_, fs_, nullptr);
    if (render_pass_output_ != VK_NULL_HANDLE)
      dfn_.vkDestroyRenderPass(device_, render_pass_output_, nullptr);
    for (WorkSlot& slot : slots_) {
      if (slot.fence != VK_NULL_HANDLE) dfn_.vkDestroyFence(device_, slot.fence, nullptr);
      if (slot.pool_job != VK_NULL_HANDLE)
        dfn_.vkDestroyCommandPool(device_, slot.pool_job, nullptr);
      if (slot.pool_upload != VK_NULL_HANDLE)
        dfn_.vkDestroyCommandPool(device_, slot.pool_upload, nullptr);
    }
    if (queries_ != VK_NULL_HANDLE) dfn_.vkDestroyQueryPool(device_, queries_, nullptr);
    if (occlusions_ != VK_NULL_HANDLE) dfn_.vkDestroyQueryPool(device_, occlusions_, nullptr);
    if (visibility_ != VK_NULL_HANDLE) dfn_.vkDestroyQueryPool(device_, visibility_, nullptr);
    for (uint32_t i = 0; i < kSlotsOutput; ++i) {
      if (fences_output_[i] != VK_NULL_HANDLE) dfn_.vkDestroyFence(device_, fences_output_[i], nullptr);
      if (pools_output_[i] != VK_NULL_HANDLE) dfn_.vkDestroyCommandPool(device_, pools_output_[i], nullptr);
    }
  }

  bool Init() {
    // vkCmdCopyImage is not in the SDK's function table: it is requested from the driver. These three and
    // vkCmdWriteTimestamp go through deferred::Wrap (queued on the job buffer, bo2zm_native_deferred.h).
    copy_image_ = reinterpret_cast<FnCopyImage>(deferred::Wrap(
        "vkCmdCopyImage",
        vulkan_device_->vulkan_instance()->functions().vkGetDeviceProcAddr(device_, "vkCmdCopyImage")));
    if (!copy_image_) {
      REXLOG_ERROR("[native] C2: the driver does not provide vkCmdCopyImage");
      return false;
    }
    clear_depth_ = reinterpret_cast<FnClearDepth>(deferred::Wrap(
        "vkCmdClearDepthStencilImage",
        vulkan_device_->vulkan_instance()->functions().vkGetDeviceProcAddr(device_,
                                                                          "vkCmdClearDepthStencilImage")));
    blit_ = reinterpret_cast<FnBlit>(deferred::Wrap(
        "vkCmdBlitImage",
        vulkan_device_->vulkan_instance()->functions().vkGetDeviceProcAddr(device_, "vkCmdBlitImage")));
    // Attachment, copy and clear source and destination, and sampling of the resolved ones (shadows).
    const VkFormatFeatureFlags kUsesDepth =
        VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT | VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT |
        VK_FORMAT_FEATURE_TRANSFER_SRC_BIT | VK_FORMAT_FEATURE_TRANSFER_DST_BIT;
    // To draw the shadow map smaller it has to be scaled up when resolving it, and that is a vkCmdBlitImage on
    // depth. If the driver does not provide it, nothing is scaled.
    const VkFormatFeatureFlags kUsesScaled =
        VK_FORMAT_FEATURE_BLIT_SRC_BIT | VK_FORMAT_FEATURE_BLIT_DST_BIT;
    VkFormatFeatureFlags uses_chosen = 0;
    for (const VkFormat candidate : {VK_FORMAT_D24_UNORM_S8_UINT, VK_FORMAT_D32_SFLOAT_S8_UINT}) {
      VkFormatProperties properties{};
      vulkan_device_->vulkan_instance()->functions().vkGetPhysicalDeviceFormatProperties(
          vulkan_device_->physical_device(), candidate, &properties);
      const VkFormatFeatureFlags uses = properties.optimalTilingFeatures & kUsesDepth;
      if (!(uses & VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT)) {
        continue;
      }
      if (format_depth_ == VK_FORMAT_UNDEFINED || uses == kUsesDepth) {
        format_depth_ = candidate;
        uses_chosen = uses;
        depth_scalable_ =
            (properties.optimalTilingFeatures & kUsesScaled) == kUsesScaled;
      }
      if (uses == kUsesDepth) {
        break;
      }
    }
    if (format_depth_ != VK_FORMAT_UNDEFINED) {
      const VkFormatFeatureFlags missing = kUsesDepth & ~uses_chosen;
      REXLOG_INFO("[native] C2: depth format {}{}{}{}",
                  format_depth_ == VK_FORMAT_D24_UNORM_S8_UINT ? "D24S8" : "D32S8",
                  (missing & VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT) ? ", without sampling" : "",
                  (missing & VK_FORMAT_FEATURE_TRANSFER_SRC_BIT) ? ", without copy source" : "",
                  (missing & VK_FORMAT_FEATURE_TRANSFER_DST_BIT) ? ", without copy target" : "");
      if (!depth_scalable_) {
        REXLOG_INFO("[native] C2: the driver does not scale depth: the shadow map stays at its size");
      }
    }
    // One pool per command buffer: the SDK table does not include
    // vkResetCommandBuffer either, so the whole pool is reset.
    VkCommandPoolCreateInfo info_pool{};
    info_pool.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
    info_pool.queueFamilyIndex = family_;
    VkCommandBufferAllocateInfo reserve{};
    reserve.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    reserve.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    reserve.commandBufferCount = 1;
    VkFenceCreateInfo info_fence{};
    info_fence.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
    for (WorkSlot& slot : slots_) {
      if (dfn_.vkCreateCommandPool(device_, &info_pool, nullptr, &slot.pool_job) !=
              VK_SUCCESS ||
          dfn_.vkCreateCommandPool(device_, &info_pool, nullptr, &slot.pool_upload) !=
              VK_SUCCESS ||
          dfn_.vkCreateFence(device_, &info_fence, nullptr, &slot.fence) != VK_SUCCESS) {
        return false;
      }
      reserve.commandPool = slot.pool_job;
      if (dfn_.vkAllocateCommandBuffers(device_, &reserve, &slot.job) != VK_SUCCESS) {
        return false;
      }
      reserve.commandPool = slot.pool_upload;
      if (dfn_.vkAllocateCommandBuffers(device_, &reserve, &slot.upload) != VK_SUCCESS) {
        return false;
      }
    }
    for (uint32_t i = 0; i < kSlotsOutput; ++i) {
      if (dfn_.vkCreateCommandPool(device_, &info_pool, nullptr, &pools_output_[i]) != VK_SUCCESS) {
        return false;
      }
      reserve.commandPool = pools_output_[i];
      if (dfn_.vkAllocateCommandBuffers(device_, &reserve, &commands_output_[i]) != VK_SUCCESS) {
        return false;
      }
      if (dfn_.vkCreateFence(device_, &info_fence, nullptr, &fences_output_[i]) != VK_SUCCESS) {
        return false;
      }
    }
    slots_used_ = uint32_t(std::clamp(REXCVAR_GET(bo2zm_native_work_slots), 2,
                                          int32_t(slots_.size())));
    REXLOG_INFO("[native] C2: work slots (bo2zm_native_work_slots) = {} of {}", slots_used_,
                slots_.size());
    output_no_wait_ = REXCVAR_GET(bo2zm_native_output_without_wait);
    invalidate_every_copy_ = REXCVAR_GET(bo2zm_native_invalidate_textures_every_copy);
    REXLOG_INFO("[native] C2: texture caches dropped on every copy (bo2zm_native_invalidate_textures_every_copy) = {}",
                invalidate_every_copy_ ? "SI" : "no");
    REXLOG_INFO("[native] C2: output without waiting for the previous one (bo2zm_native_output_without_wait) = {}",
                output_no_wait_ ? "SI" : "no");
    marks_precise_ = REXCVAR_GET(bo2zm_native_precise_marks);
    alternate_marks_s_ = REXCVAR_GET(bo2zm_native_precise_marks_alternate_s);
    start_marks_ = std::chrono::steady_clock::now();
    REXLOG_INFO("[native] C2: precise GPU marks (bo2zm_native_precise_marks) = {}; alternate every {} s",
                marks_precise_ ? "SI" : "no", alternate_marks_s_);
    // GPU time per work unit: two timestamps per slot. Without timestamp bits on the queue, it is not measured.
    {
      const auto& ifn = vulkan_device_->vulkan_instance()->functions();
      uint32_t families = 0;
      ifn.vkGetPhysicalDeviceQueueFamilyProperties(vulkan_device_->physical_device(), &families,
                                                   nullptr);
      std::vector<VkQueueFamilyProperties> queues(families);
      ifn.vkGetPhysicalDeviceQueueFamilyProperties(vulkan_device_->physical_device(), &families,
                                                   queues.data());
      VkPhysicalDeviceProperties physical{};
      ifn.vkGetPhysicalDeviceProperties(vulkan_device_->physical_device(), &physical);
      write_mark_ = reinterpret_cast<FnWriteMark>(
          deferred::Wrap("vkCmdWriteTimestamp", ifn.vkGetDeviceProcAddr(device_, "vkCmdWriteTimestamp")));
      read_queries_ = reinterpret_cast<FnReadQueries>(
          ifn.vkGetDeviceProcAddr(device_, "vkGetQueryPoolResults"));
      if (family_ < families && queues[family_].timestampValidBits && write_mark_ &&
          read_queries_ && physical.limits.timestampPeriod > 0.0f) {
        VkQueryPoolCreateInfo info_queries{};
        info_queries.sType = VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO;
        info_queries.queryType = VK_QUERY_TYPE_TIMESTAMP;
        info_queries.queryCount = uint32_t(slots_.size() * kMarksPerSlot);
        if (dfn_.vkCreateQueryPool(device_, &info_queries, nullptr, &queries_) != VK_SUCCESS) {
          queries_ = VK_NULL_HANDLE;
        }
        period_mark_ns_ = physical.limits.timestampPeriod;
      }
      bo2zm::perf_log::SetMeta("gpu", physical.deviceName);
      REXLOG_INFO("[native] C2: GPU time per Swap {}",
                  queries_ != VK_NULL_HANDLE ? "measured with timestamps"
                                               : "not available (the queue has no timestamps)");
      // The game's occlusion queries (the sun flare), counted on the GPU.
      VkQueryPoolCreateInfo info_occlusions{};
      info_occlusions.sType = VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO;
      info_occlusions.queryType = VK_QUERY_TYPE_OCCLUSION;
      info_occlusions.queryCount = uint32_t(slots_.size() * kOcclusionsPerSlot);
      if (!read_queries_ ||
          dfn_.vkCreateQueryPool(device_, &info_occlusions, nullptr, &occlusions_) != VK_SUCCESS) {
        occlusions_ = VK_NULL_HANDLE;
      }
      occlusion_precise_ = vulkan_device_->properties().occlusionQueryPrecise;
      REXLOG_INFO("[native] C2: host occlusion queries {} ({})",
                  occlusions_ != VK_NULL_HANDLE ? "available" : "not available: fake count",
                  occlusion_precise_ ? "precise" : "not precise: they count whether there was any sample");
      // Reflection visibility, in a separate pool so as not to touch the count of the game's queries.
      if (bo2zm::reflection_demand::MeasureVisibility() && read_queries_) {
        VkQueryPoolCreateInfo info_visibility{};
        info_visibility.sType = VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO;
        info_visibility.queryType = VK_QUERY_TYPE_OCCLUSION;
        info_visibility.queryCount = uint32_t(slots_.size() * kVisibilityPerSlot);
        if (dfn_.vkCreateQueryPool(device_, &info_visibility, nullptr, &visibility_) != VK_SUCCESS) {
          visibility_ = VK_NULL_HANDLE;
        }
        REXLOG_INFO("[native] C2: reflection visibility: {}",
                    visibility_ != VK_NULL_HANDLE ? "queries available"
                                                   : "NO queries: the reflection is decided by readbacks (like 191)");
      }
      // Pipeline statistics per pass. The order of the counters is the order of the bits,
      // not the order of this list: vertices, clipped primitives and fragments.
      if (vulkan_device_->properties().pipelineStatisticsQuery && read_queries_) {
        VkQueryPoolCreateInfo info_stats{};
        info_stats.sType = VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO;
        info_stats.queryType = VK_QUERY_TYPE_PIPELINE_STATISTICS;
        info_stats.queryCount = uint32_t(slots_.size() * kStatsPerSlot);
        info_stats.pipelineStatistics =
            VK_QUERY_PIPELINE_STATISTIC_VERTEX_SHADER_INVOCATIONS_BIT |
            VK_QUERY_PIPELINE_STATISTIC_CLIPPING_PRIMITIVES_BIT |
            VK_QUERY_PIPELINE_STATISTIC_FRAGMENT_SHADER_INVOCATIONS_BIT;
        if (dfn_.vkCreateQueryPool(device_, &info_stats, nullptr, &stats_) != VK_SUCCESS) {
          stats_ = VK_NULL_HANDLE;
        }
      }
      if (stats_ != VK_NULL_HANDLE) {
        VkQueryPoolCreateInfo info_draw{};
        info_draw.sType = VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO;
        info_draw.queryType = VK_QUERY_TYPE_PIPELINE_STATISTICS;
        info_draw.queryCount = uint32_t(slots_.size() * kStatsDrawPerSlot);
        info_draw.pipelineStatistics =
            VK_QUERY_PIPELINE_STATISTIC_VERTEX_SHADER_INVOCATIONS_BIT |
            VK_QUERY_PIPELINE_STATISTIC_CLIPPING_PRIMITIVES_BIT |
            VK_QUERY_PIPELINE_STATISTIC_FRAGMENT_SHADER_INVOCATIONS_BIT;
        if (dfn_.vkCreateQueryPool(device_, &info_draw, nullptr, &stats_draw_) != VK_SUCCESS) {
          stats_draw_ = VK_NULL_HANDLE;
        }
        fragments_per_shader_.assign(size_t(kLabelsShader) * kGpuCategories, 0);
        draws_per_shader_.assign(size_t(kLabelsShader) * kGpuCategories, 0);
      }
      REXLOG_INFO("[native] C2: pipeline statistics per pass {}",
                  stats_ != VK_NULL_HANDLE ? "available (bo2zm_native_pipeline_stats)"
                                                  : "not available");
    }

    VkSamplerCreateInfo info_sampler{};
    info_sampler.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
    info_sampler.magFilter = VK_FILTER_LINEAR;
    info_sampler.minFilter = VK_FILTER_LINEAR;
    info_sampler.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
    info_sampler.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    info_sampler.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    info_sampler.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    if (dfn_.vkCreateSampler(device_, &info_sampler, nullptr, &sampler_) != VK_SUCCESS) {
      return false;
    }

    // Same layout as the presenter: image at 0 and sampler at 1, and the gamma ramp at 2 (the pipeline without
    // the ramp does not use it).
    VkDescriptorSetLayoutBinding bindings[3]{};
    bindings[0].binding = 0;
    bindings[0].descriptorType = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
    bindings[0].descriptorCount = 1;
    bindings[0].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
    bindings[1].binding = 1;
    bindings[1].descriptorType = VK_DESCRIPTOR_TYPE_SAMPLER;
    bindings[1].descriptorCount = 1;
    bindings[1].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
    bindings[1].pImmutableSamplers = &sampler_;
    bindings[2].binding = 2;
    bindings[2].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
    bindings[2].descriptorCount = 1;
    bindings[2].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
    VkDescriptorSetLayoutCreateInfo info_layout{};
    info_layout.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    info_layout.bindingCount = 3;
    info_layout.pBindings = bindings;
    if (dfn_.vkCreateDescriptorSetLayout(device_, &info_layout, nullptr, &layout_descriptors_) !=
        VK_SUCCESS) {
      return false;
    }
    VkPushConstantRange ranges[2]{};
    ranges[0].stageFlags = VK_SHADER_STAGE_VERTEX_BIT;
    ranges[0].offset = 0;
    ranges[0].size = 16;  // GuestOutputPaintRectangleConstants
    ranges[1].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
    ranges[1].offset = 16;
    ranges[1].size = 96;  // Presenter::BilinearConstants, plus the FPS counter (bo2zm_show_fps), the
                          // preset note and the top banner (bo2zm_preset); 16 + 96 bytes, within the 128 every
                          // Vulkan device guarantees
    VkPipelineLayoutCreateInfo info_pipeline_layout{};
    info_pipeline_layout.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    info_pipeline_layout.setLayoutCount = 1;
    info_pipeline_layout.pSetLayouts = &layout_descriptors_;
    info_pipeline_layout.pushConstantRangeCount = 2;
    info_pipeline_layout.pPushConstantRanges = ranges;
    if (dfn_.vkCreatePipelineLayout(device_, &info_pipeline_layout, nullptr, &layout_pipeline_) !=
        VK_SUCCESS) {
      return false;
    }
    VkDescriptorPoolSize sizes[3] = {{VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, kSlotsOutput},
                                       {VK_DESCRIPTOR_TYPE_SAMPLER, kSlotsOutput},
                                       {VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, kSlotsOutput}};
    VkDescriptorPoolCreateInfo info_pool_desc{};
    info_pool_desc.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    info_pool_desc.maxSets = kSlotsOutput;
    info_pool_desc.poolSizeCount = 3;
    info_pool_desc.pPoolSizes = sizes;
    if (dfn_.vkCreateDescriptorPool(device_, &info_pool_desc, nullptr, &pool_descriptors_) !=
        VK_SUCCESS) {
      return false;
    }
    VkDescriptorSetAllocateInfo reserve_desc{};
    reserve_desc.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
    reserve_desc.descriptorPool = pool_descriptors_;
    reserve_desc.descriptorSetCount = 1;
    reserve_desc.pSetLayouts = &layout_descriptors_;
    for (uint32_t i = 0; i < kSlotsOutput; ++i) {
      if (dfn_.vkAllocateDescriptorSets(device_, &reserve_desc, &descriptors_output_[i]) != VK_SUCCESS) {
        return false;
      }
    }
    // One ramp buffer per output slot, so it is only changed in a slot the GPU is no longer using. Without
    // them, the output has no ramp (as before).
    ramp_gamma_ = REXCVAR_GET(bo2zm_native_gamma_ramp);
    for (uint32_t i = 0; i < kSlotsOutput && ramp_gamma_; ++i) {
      OutputRamp& ramp = ramps_output_[i];
      uint32_t type = 0;
      void* mapped = nullptr;
      if (!rex::ui::vulkan::util::CreateDedicatedAllocationBuffer(
              vulkan_device_, sizeof(ramp_values_) + sizeof(bo2zm::native::output::Crosshairs), VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT,
              rex::ui::vulkan::util::MemoryPurpose::kUpload, ramp.buffer, ramp.guest_memory, &type) ||
          dfn_.vkMapMemory(device_, ramp.guest_memory, 0, VK_WHOLE_SIZE, 0, &mapped) != VK_SUCCESS) {
        REXLOG_WARN("[native] C2: could not create the gamma ramp buffer: the output goes without it");
        ramp_gamma_ = false;
        break;
      }
      ramp.data = static_cast<uint8_t*>(mapped);
      ramp.type = type;
      VkDescriptorBufferInfo info_buffer{ramp.buffer, 0, sizeof(ramp_values_) + sizeof(bo2zm::native::output::Crosshairs)};
      VkWriteDescriptorSet write{};
      write.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
      write.dstSet = descriptors_output_[i];
      write.dstBinding = 2;
      write.descriptorCount = 1;
      write.descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
      write.pBufferInfo = &info_buffer;
      dfn_.vkUpdateDescriptorSets(device_, 1, &write, 0, nullptr);
    }
    if (!ramp_gamma_) {
      // Without a ramp, binding 2 stays unwritten: the pipeline without the ramp does not read it.
      for (OutputRamp& ramp : ramps_output_) {
        if (ramp.buffer != VK_NULL_HANDLE) dfn_.vkDestroyBuffer(device_, ramp.buffer, nullptr);
        if (ramp.guest_memory != VK_NULL_HANDLE) dfn_.vkFreeMemory(device_, ramp.guest_memory, nullptr);
        ramp = OutputRamp{};
      }
    }

    VkShaderModuleCreateInfo module_info{};
    module_info.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
    module_info.codeSize = sizeof(shaders::guest_output_triangle_strip_rect_vs);
    module_info.pCode = shaders::guest_output_triangle_strip_rect_vs;
    if (dfn_.vkCreateShaderModule(device_, &module_info, nullptr, &vs_) != VK_SUCCESS) {
      return false;
    }
    module_info.codeSize = sizeof(shaders::guest_output_bilinear_ps);
    module_info.pCode = shaders::guest_output_bilinear_ps;
    if (dfn_.vkCreateShaderModule(device_, &module_info, nullptr, &fs_) != VK_SUCCESS) {
      return false;
    }
    if (ramp_gamma_) {
      module_info.codeSize = sizeof(shaders::bo2zm_output_gamma_ramp_ps);
      module_info.pCode = shaders::bo2zm_output_gamma_ramp_ps;
      if (dfn_.vkCreateShaderModule(device_, &module_info, nullptr, &fs_ramp_) != VK_SUCCESS) {
        return false;
      }
    }

    VkAttachmentDescription attachment{};
    attachment.format = VulkanPresenter::kGuestOutputFormat;
    attachment.samples = VK_SAMPLE_COUNT_1_BIT;
    attachment.loadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    attachment.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    attachment.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    attachment.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    attachment.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    attachment.finalLayout = VulkanPresenter::kGuestOutputInternalLayout;
    VkAttachmentReference reference{0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
    VkSubpassDescription subpass{};
    subpass.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
    subpass.colorAttachmentCount = 1;
    subpass.pColorAttachments = &reference;
    VkRenderPassCreateInfo info_rp{};
    info_rp.sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO;
    info_rp.attachmentCount = 1;
    info_rp.pAttachments = &attachment;
    info_rp.subpassCount = 1;
    info_rp.pSubpasses = &subpass;
    if (dfn_.vkCreateRenderPass(device_, &info_rp, nullptr, &render_pass_output_) != VK_SUCCESS) {
      return false;
    }

    pipeline_ = CreatePipelineOutput(fs_, nullptr);
    if (pipeline_ == VK_NULL_HANDLE) {
      return false;
    }
    // The ramp variants without extras (bilinear and exact texel) are created now; the post-processing ones
    // with per-pixel work and FXAA, the first time they are requested (PipelineRamp).
    if (ramp_gamma_ && (PipelineRamp(0) == VK_NULL_HANDLE || PipelineRamp(1) == VK_NULL_HANDLE)) {
      return false;
    }
    REXLOG_INFO("[native] C2: the game's gamma ramp at the output (bo2zm_native_gamma_ramp) = {}",
                ramp_gamma_ ? "SI" : "no");
    // The draws (bo2zm_native_draws.cpp): without the required capabilities only copies and presentation remain.
    draws_ = VulkanDraws::Create(vulkan_device_, memory_, this);
    return true;
  }

  // Pipeline of the output pass with the fragment shader fs (and its specialization constants, if any).
  VkPipeline CreatePipelineOutput(VkShaderModule fs, const VkSpecializationInfo* special,
                                 VkRenderPass pass = VK_NULL_HANDLE,  // the render pass to draw in
                                 bool alpha_blend = false) {  // blended over the image (the credits)
    VkPipelineShaderStageCreateInfo stages[2]{};
    stages[0].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
    stages[0].module = vs_;
    stages[0].pName = "main";
    stages[1].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
    stages[1].module = fs;
    stages[1].pName = "main";
    stages[1].pSpecializationInfo = special;
    VkPipelineVertexInputStateCreateInfo input{};
    input.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;
    VkPipelineInputAssemblyStateCreateInfo assembly{};
    assembly.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
    assembly.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP;
    // Not indexed, so restart changes nothing; Metal cannot turn it off and MoltenVK warns when it is off.
    assembly.primitiveRestartEnable = VK_TRUE;
    VkPipelineViewportStateCreateInfo view{};
    view.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO;
    view.viewportCount = 1;
    view.scissorCount = 1;
    VkPipelineRasterizationStateCreateInfo rasterization{};
    rasterization.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
    rasterization.polygonMode = VK_POLYGON_MODE_FILL;
    rasterization.cullMode = VK_CULL_MODE_NONE;
    rasterization.frontFace = VK_FRONT_FACE_CLOCKWISE;
    rasterization.lineWidth = 1.0f;
    VkPipelineMultisampleStateCreateInfo sampling{};
    sampling.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
    sampling.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
    VkPipelineColorBlendAttachmentState blend_attachment{};
    blend_attachment.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
                                    VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
    if (alpha_blend) {
      blend_attachment.blendEnable = VK_TRUE;
      blend_attachment.srcColorBlendFactor = VK_BLEND_FACTOR_SRC_ALPHA;
      blend_attachment.dstColorBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
      blend_attachment.colorBlendOp = VK_BLEND_OP_ADD;
      blend_attachment.srcAlphaBlendFactor = VK_BLEND_FACTOR_ZERO;  // the image's alpha stays
      blend_attachment.dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
      blend_attachment.alphaBlendOp = VK_BLEND_OP_ADD;
    }
    VkPipelineColorBlendStateCreateInfo blend{};
    blend.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
    blend.attachmentCount = 1;
    blend.pAttachments = &blend_attachment;
    const VkDynamicState dynamic_states[] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
    VkPipelineDynamicStateCreateInfo dynamic{};
    dynamic.sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO;
    dynamic.dynamicStateCount = 2;
    dynamic.pDynamicStates = dynamic_states;
    VkGraphicsPipelineCreateInfo info_pipeline{};
    info_pipeline.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
    info_pipeline.stageCount = 2;
    info_pipeline.pStages = stages;
    info_pipeline.pVertexInputState = &input;
    info_pipeline.pInputAssemblyState = &assembly;
    info_pipeline.pViewportState = &view;
    info_pipeline.pRasterizationState = &rasterization;
    info_pipeline.pMultisampleState = &sampling;
    info_pipeline.pColorBlendState = &blend;
    info_pipeline.pDynamicState = &dynamic;
    info_pipeline.layout = layout_pipeline_;
    info_pipeline.renderPass = pass != VK_NULL_HANDLE ? pass : render_pass_output_;
    info_pipeline.basePipelineIndex = -1;
    VkPipeline pipeline = VK_NULL_HANDLE;
    if (dfn_.vkCreateGraphicsPipelines(device_, VK_NULL_HANDLE, 1, &info_pipeline, nullptr, &pipeline) !=
        VK_SUCCESS) {
      return VK_NULL_HANDLE;
    }
    return pipeline;
  }

  // Variant of the output pass with the ramp, by index: 4 * fxaa + 2 * grading + exact (specialization
  // constants 2, 1 and 0 of the shader). Created the first time it is requested; VK_NULL_HANDLE if it fails
  // (and it is logged).
  VkPipeline PipelineRamp(uint32_t index) {
    VkPipeline& pipeline = pipelines_ramp_[index];
    if (pipeline == VK_NULL_HANDLE && !pipelines_ramp_failed_[index]) {
      const VkSpecializationMapEntry inputs[3] = {{0, 0, sizeof(VkBool32)},
                                                    {1, sizeof(VkBool32), sizeof(VkBool32)},
                                                    {2, 2 * sizeof(VkBool32), sizeof(VkBool32)}};
      const VkBool32 values[3] = {(index & 1) ? VK_TRUE : VK_FALSE, (index & 2) ? VK_TRUE : VK_FALSE,
                                   (index & 4) ? VK_TRUE : VK_FALSE};
      const VkSpecializationInfo special{3, inputs, sizeof(values), values};
      const auto before = std::chrono::steady_clock::now();
      pipeline = CreatePipelineOutput(fs_ramp_, &special);
      if (pipeline == VK_NULL_HANDLE) {
        pipelines_ramp_failed_[index] = true;
        REXLOG_ERROR("[native] C2: could not create variant {} of the output pass", index);
      } else {
        REXLOG_INFO("[native] C2: variant {} of the output pass (exact {}, grading {}, FXAA {}) created in {:.1f} ms",
                    index, (index & 1) ? "si" : "no", (index & 2) ? "si" : "no", (index & 4) ? "si" : "no",
                    std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - before).count());
      }
    }
    return pipeline;
  }

  // The home screen's credits line (bo2zm_credits.cpp): its picture as an image, a descriptor set of the
  // output pass's layout (the image at 0, the pass's sampler at 1; 2 unused) and a pipeline that blends it over the
  // output, made the first time it shows. false: not available (logged once, never tried again).
  bool CreditsReady() {
    Credits& c = credits_;
    if (c.pipeline != VK_NULL_HANDLE || c.failed) {
      return !c.failed;
    }
    c.failed = true;  // until everything below is made
    const bo2zm::credits::Picture* picture = bo2zm::credits::Images();
    if (!picture) {
      return false;
    }
    VkImageCreateInfo info{};
    info.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    info.imageType = VK_IMAGE_TYPE_2D;
    info.format = VK_FORMAT_R8G8B8A8_UNORM;
    info.extent = {picture->width, picture->height, 1};
    info.mipLevels = 1;
    info.arrayLayers = 1;
    info.samples = VK_SAMPLE_COUNT_1_BIT;
    info.tiling = VK_IMAGE_TILING_OPTIMAL;
    info.usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    info.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    if (!rex::ui::vulkan::util::CreateDedicatedAllocationImage(
            vulkan_device_, info, rex::ui::vulkan::util::MemoryPurpose::kDeviceLocal, c.image, c.memory)) {
      REXLOG_ERROR("[native] credits: could not create the image: no credits line");
      return false;
    }
    VkImageViewCreateInfo info_view{};
    info_view.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    info_view.image = c.image;
    info_view.viewType = VK_IMAGE_VIEW_TYPE_2D;
    info_view.format = VK_FORMAT_R8G8B8A8_UNORM;
    info_view.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    if (dfn_.vkCreateImageView(device_, &info_view, nullptr, &c.view) != VK_SUCCESS) {
      REXLOG_ERROR("[native] credits: could not create the image view: no credits line");
      return false;
    }
    // The picture's bytes, copied into the image by the first output that draws it (RecordCreditsUpload).
    uint32_t type = 0;
    void* mapped = nullptr;
    if (!rex::ui::vulkan::util::CreateDedicatedAllocationBuffer(
            vulkan_device_, picture->rgba.size(), VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
            rex::ui::vulkan::util::MemoryPurpose::kUpload, c.staging, c.staging_memory, &type) ||
        dfn_.vkMapMemory(device_, c.staging_memory, 0, VK_WHOLE_SIZE, 0, &mapped) != VK_SUCCESS) {
      REXLOG_ERROR("[native] credits: could not create the upload buffer: no credits line");
      return false;
    }
    std::memcpy(mapped, picture->rgba.data(), picture->rgba.size());
    rex::ui::vulkan::util::FlushMappedMemoryRange(vulkan_device_, c.staging_memory, type);
    dfn_.vkUnmapMemory(device_, c.staging_memory);
    const VkDescriptorPoolSize sizes[3] = {{VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 1}, {VK_DESCRIPTOR_TYPE_SAMPLER, 1},
                                           {VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1}};
    VkDescriptorPoolCreateInfo info_pool{};
    info_pool.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    info_pool.maxSets = 1;
    info_pool.poolSizeCount = 3;
    info_pool.pPoolSizes = sizes;
    VkDescriptorSetAllocateInfo reserve{};
    reserve.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
    reserve.descriptorSetCount = 1;
    reserve.pSetLayouts = &layout_descriptors_;
    if (dfn_.vkCreateDescriptorPool(device_, &info_pool, nullptr, &c.pool) != VK_SUCCESS ||
        (reserve.descriptorPool = c.pool, dfn_.vkAllocateDescriptorSets(device_, &reserve, &c.set)) != VK_SUCCESS) {
      REXLOG_ERROR("[native] credits: could not get a descriptor set: no credits line");
      return false;
    }
    VkDescriptorImageInfo info_image{};
    info_image.imageView = c.view;
    info_image.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    VkWriteDescriptorSet write{};
    write.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    write.dstSet = c.set;
    write.dstBinding = 0;
    write.descriptorCount = 1;
    write.descriptorType = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
    write.pImageInfo = &info_image;
    dfn_.vkUpdateDescriptorSets(device_, 1, &write, 0, nullptr);
    VkShaderModuleCreateInfo module_info{};
    module_info.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
    module_info.codeSize = sizeof(shaders::bo2zm_credits_ps);
    module_info.pCode = shaders::bo2zm_credits_ps;
    if (dfn_.vkCreateShaderModule(device_, &module_info, nullptr, &c.fs) != VK_SUCCESS) {
      REXLOG_ERROR("[native] credits: could not create the shader: no credits line");
      return false;
    }
    c.pipeline = CreatePipelineOutput(c.fs, nullptr, VK_NULL_HANDLE, true);
    if (c.pipeline == VK_NULL_HANDLE) {
      REXLOG_ERROR("[native] credits: could not create the pipeline: no credits line");
      return false;
    }
    c.failed = false;
    REXLOG_INFO("[native] credits: ready ({} x {} picture)", picture->width, picture->height);
    return true;
  }

  // The picture into the credits image, in the output's command buffer before its pass (once).
  void RecordCreditsUpload(VkCommandBuffer commands) {
    const bo2zm::credits::Picture* picture = bo2zm::credits::Images();
    VkImageMemoryBarrier barrier{};
    barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    barrier.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    barrier.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    barrier.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.image = credits_.image;
    barrier.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    dfn_.vkCmdPipelineBarrier(commands, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0,
                              nullptr, 0, nullptr, 1, &barrier);
    VkBufferImageCopy region{};
    region.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    region.imageExtent = {picture->width, picture->height, 1};
    dfn_.vkCmdCopyBufferToImage(commands, credits_.staging, credits_.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1,
                                &region);
    barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
    barrier.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    barrier.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    dfn_.vkCmdPipelineBarrier(commands, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 0, 0,
                              nullptr, 0, nullptr, 1, &barrier);
    credits_.uploaded = true;
  }

  // The credits line over the output, inside its pass, after the image.
  void DrawCredits(VkCommandBuffer commands, const bo2zm::credits::Placement& place, uint32_t width,
                   uint32_t height) {
    const uint32_t* rect = bo2zm::credits::Images()->rects[place.image];
    dfn_.vkCmdBindPipeline(commands, VK_PIPELINE_BIND_POINT_GRAPHICS, credits_.pipeline);
    dfn_.vkCmdBindDescriptorSets(commands, VK_PIPELINE_BIND_POINT_GRAPHICS, layout_pipeline_, 0, 1, &credits_.set, 0,
                                 nullptr);
    const float rectangle[4] = {-1.0f + 2.0f * float(place.x) / float(width),
                                -1.0f + 2.0f * float(place.y) / float(height), 2.0f * float(rect[2]) / float(width),
                                2.0f * float(rect[3]) / float(height)};
    dfn_.vkCmdPushConstants(commands, layout_pipeline_, VK_SHADER_STAGE_VERTEX_BIT, 0, sizeof(rectangle), rectangle);
    const struct {
      int32_t origin[2];
      int32_t source[2];
      float alpha;
    } constants = {{place.x, place.y}, {int32_t(rect[0]), int32_t(rect[1])}, place.alpha};
    dfn_.vkCmdPushConstants(commands, layout_pipeline_, VK_SHADER_STAGE_FRAGMENT_BIT, 16, sizeof(constants),
                            &constants);
    dfn_.vkCmdDraw(commands, 4, 1, 0, 0);
  }

  bool FrontTraceLine() {
    if (!front_trace_active_) return false;
    if (front_trace_lines_ < 512) {
      ++front_trace_lines_;
      return true;
    }
    ++front_trace_suppressed_;
    return false;
  }

  void TraceFrontFrame(bool active) override {
    if (!active) {
      if (front_trace_active_) {
        REXLOG_INFO("[front-trace] end frame {}: records {}, suppressed {}, owners {}",
                    presented_, front_trace_lines_, front_trace_suppressed_, front_compositions_.size());
      }
      front_trace_active_ = false;
      return;
    }
    front_trace_active_ = REXCVAR_GET(bo2zm_native_diag_front);
    if (!front_trace_active_) return;
    front_trace_lines_ = front_trace_suppressed_ = 0;
    REXLOG_INFO("[front-trace] begin frame {}: clear enabled {}, failed {}, owners {}",
                presented_, REXCVAR_GET(bo2zm_native_front_partial_clear), front_partial_clear_failed_,
                front_compositions_.size());
    for (const auto& [image, front] : front_compositions_) {
      if (FrontTraceLine()) {
        REXLOG_INFO("[front-trace] snapshot target {:p} front {:08X} {}x{} write_frame {} frame {} "
                    "swap_only {} sampled {} image {}x{} format {}",
                    static_cast<const void*>(image), front.address, front.width, front.height,
                    front.write_frame, presented_, IsSwapFrontOnly(front.address),
                    front_composition_sampled_.count(front.address) != 0,
                    image->width, image->height, uint32_t(image->format));
      }
    }
  }

  bool Copy(const CopyRegisters& reg) override {
    // Draws since the previous copy: those of the render target being resolved now (reflection
    // diagnostic).
    const uint64_t drawn = draws_ ? draws_->Drawn() : 0;
    const uint64_t draws_before = drawn - drawn_last_copy_;
    drawn_last_copy_ = drawn;
    if (draws_) {
      draws_->FinishPass();  // copying and clearing are not allowed inside a pass
      // No longer on every copy; see GetResolved and Prepare.
      if (invalidate_every_copy_) {
        draws_->InvalidateTextures();
      }
    }
    if (Record()) {
      MarkGpu(kGpuCopies);  // GPU time of copies and clears (C2 report)
    }
    const uint32_t control = reg.rb_copy_control;
    const uint32_t source = control & 0x7;
    const bool clear_color = (control >> 8) & 0x1;
    const uint32_t command = (control >> 20) & 0x3;
    const bool do_copy = command == uint32_t(xenos::CopyCommand::kRaw) ||
                        command == uint32_t(xenos::CopyCommand::kConvert);
    const uint32_t pitch = reg.rb_surface_info & 0x3FFF;
    const uint32_t msaa = (reg.rb_surface_info >> 16) & 0x3;
    if (msaa != uint32_t(xenos::MsaaSamples::k1X) && warned_.insert(1).second) {
      REXLOG_INFO("[native] C2: target with MSAA: used with 1 sample");
    }
    if (source >= xenos::kMaxColorRenderTargets) {
      // From depth: the copy goes to a resolved texture with the host depth
      // format, which the draws sample as k_24_8. With a depth source no color
      // is cleared (IsClearingColor, graphics/util/draw.h:534-538).
      const bool would_clear = ((control >> 9) & 0x1) && clear_depth_;
      if (do_copy) {
        CopyDepth(reg, pitch, would_clear);
      }
      if (would_clear) {
        ClearDepth(reg, pitch);
      }
      return true;
    }
    const uint32_t info_color = reg.rb_color_info[source];
    const uint32_t format_color = (info_color >> 16) & 0xF;
    if (!bo2zm_formats::ColorApprox(format_color)) {
      return Reject(100 + format_color, "render target format not supported yet");
    }
    int32_t x0, y0, x1, y1;
    if (!Rectangle(reg, pitch, x0, y0, x1, y1)) {
      return false;
    }

    // A resolve through an alias (CopyRegisters::surface_real, set by the ring). The source is the
    // image the game drew at that EDRAM base; x0..y1 stay in the alias's pixels (they place the destination) and
    // the source rectangle is scaled to the same samples: a 4X pixel (2x2 samples) is 2x2 pixels of a 1X surface.
    uint32_t pitch_source = pitch;
    int32_t ox0 = x0, oy0 = y0, ox1 = x1, oy1 = y1;
    const bool alias = reg.surface_real != 0;
    if (alias) {
      const auto in_x = [](uint32_t sup) { return ((sup >> 16) & 0x3) >= 2 ? 2 : 1; };
      const auto in_y = [](uint32_t sup) { return ((sup >> 16) & 0x3) >= 1 ? 2 : 1; };
      const uint32_t real = reg.surface_real;
      ox0 = x0 * in_x(reg.rb_surface_info) / in_x(real);
      ox1 = x1 * in_x(reg.rb_surface_info) / in_x(real);
      oy0 = y0 * in_y(reg.rb_surface_info) / in_y(real);
      oy1 = y1 * in_y(reg.rb_surface_info) / in_y(real);
      pitch_source = real & 0x3FFF;
      if (alias_seen_.size() < 16 && alias_seen_.insert((uint64_t(reg.rb_surface_info) << 32) | real).second) {
        REXLOG_INFO("[native] C2: copy through an alias: surface {:08X} [{},{})x[{},{}) read from the drawn one {:08X} "
                    "[{},{})x[{},{})",
                    reg.rb_surface_info, x0, x1, y0, y1, real, ox0, ox1, oy0, oy1);
      }
    }
    // First the objects (creating or recreating can submit work), then recording.
    OwnedImage* target_render = GetTarget(info_color & 0xFFF, format_color, pitch_source);
    if (!target_render) {
      return false;
    }
    // The game has just said which area of this render target matters to it. It is the data RestoreContent
    // uses to stop copying the bottom rows that nobody draws or reads (the scene render target is created
    // 880x880 to draw 880x720).
    AnnotateAreaUseful(*target_render, oy1);
    AnnotateUseClear(*target_render, uint32_t(std::max(ox1, 0)), uint32_t(std::max(oy1, 0)));
    uint32_t base_resolved = 0;
    Resolved* resolved = nullptr;
    uint32_t dx = 0, dy = 0;
    if (do_copy) {
      const uint32_t info_target = reg.rb_copy_dest_info;
      const uint32_t format_target = (info_target >> 7) & 0x3F;
      if ((info_target >> 3) & 0x1) {
        Reject(3, "copy to a 3D or array texture: not yet");
      } else if (!bo2zm_formats::CopyApprox(format_target)) {
        Reject(200 + format_target, "copy format not supported yet");
      } else {
        const uint32_t pitch_target = reg.rb_copy_dest_pitch & 0x3FFF;
        const uint32_t height_target = (reg.rb_copy_dest_pitch >> 16) & 0x3FFF;
        // Base in multiples of 32 texels (GetResolveInfo). 8 bytes per texel for the 64-bit
        // formats BO2 also resolves to, 4 for the rest.
        const uint32_t base_x = uint32_t(x0) & ~uint32_t(31);
        const uint32_t base_y = uint32_t(y0) & ~uint32_t(31);
        const uint32_t base =
            reg.rb_copy_dest_base +
            uint32_t(OffsetTiled2D(int32_t(base_x), int32_t(base_y), pitch_target,
                                             bo2zm_formats::Log2BytesCopy(format_target)));
        dx = uint32_t(x0) - base_x;
        dy = uint32_t(y0) - base_y;
        resolved = GetResolved(base & 0x1FFFFFFF, pitch_target, height_target, format_target,
                                   (info_target >> 24) & 0x1, FormatHostResolved(info_target));
        base_resolved = base & 0x1FFFFFFF;
        if (resolved) {
          AnnotateCopy(base & 0x1FFFFFFF, pitch_target, height_target, draws_before);
          if (composite_has_ || composite_expired_ != 0) {  // bo2zm_native_lazy_composite
            CompositeBeforeWriteTexture(base & 0x1FFFFFFF, x0 == 0 && y0 == 0 &&
                                                                   uint32_t(x1 - x0) >= resolved->image.width &&
                                                                   uint32_t(y1 - y0) >= resolved->image.height);
          }
          if (diag_window_ || !diag_watched_.empty()) {  // bo2zm_native_diag_readers_s
            DiagWrite(base & 0x1FFFFFFF, info_color & 0xFFF, pitch, x0, y0, uint32_t(x1 - x0), uint32_t(y1 - y0),
                          resolved->image.width, resolved->image.height, draws_before, false);
          }
        }
        if (resolved && REXCVAR_GET(bo2zm_native_diag_resolved) &&
            resolved_frame_.size() < 64 &&
            std::find(resolved_frame_.begin(), resolved_frame_.end(),
                      base & 0x1FFFFFFF) == resolved_frame_.end()) {
          resolved_frame_.push_back(base & 0x1FFFFFFF);
        }
      }
    }

    if (const auto front = front_compositions_.find(target_render); front != front_compositions_.end() &&
        (!resolved || !IsSwapFrontOnly(base_resolved))) {
      if (FrontTraceLine()) {
        REXLOG_INFO("[front-trace] invalidate target {:p} reason intermediate front {:08X} "
                    "write_frame {} frame {} new_dest {:08X} resolved {}",
                    static_cast<const void*>(target_render), front->second.address, front->second.write_frame,
                    presented_, base_resolved, resolved != nullptr);
      }
      front_compositions_.erase(front);  // this source is now serving a retained/intermediate destination
    }
    if (FrontTraceLine()) {
      REXLOG_INFO("[front-trace] copy target {:p} edram {:03X} surface {:08X} source {} dest {:08X} "
                  "resolved {} alias {} source_rect [{},{})x[{},{}) dest_offset {},{} "
                  "target {}x{} format {} dest {}x{} format {} swap_only {} sampled {}",
                  static_cast<const void*>(target_render), info_color & 0xFFF, reg.rb_surface_info,
                  source, base_resolved, resolved != nullptr, alias, x0, x1, y0, y1, dx, dy,
                  target_render->width, target_render->height, uint32_t(target_render->format),
                  resolved ? resolved->image.width : 0, resolved ? resolved->image.height : 0,
                  resolved ? uint32_t(resolved->image.format) : 0, IsSwapFrontOnly(base_resolved),
                  front_composition_sampled_.count(base_resolved) != 0);
    }
    if (!Record()) {
      return false;
    }
    Prepare(*target_render);
    if (resolved && alias) {
      if (FrontTraceLine()) REXLOG_INFO("[front-trace] route alias target {:p} dest {:08X}",
                                       static_cast<const void*>(target_render), base_resolved);
      // Through an alias, a blit from the drawn image's rectangle to the alias's size: linear when it
      // shrinks (2:1 is the average of the 2x2 samples, as the GPU's resolve), nearest when it grows.
      Prepare(resolved->image);
      const uint32_t width = std::min(uint32_t(std::max(x1 - x0, 0)),
                                      resolved->image.width > dx ? resolved->image.width - dx : 0u);
      const uint32_t height = std::min(uint32_t(std::max(y1 - y0, 0)),
                                     resolved->image.height > dy ? resolved->image.height - dy : 0u);
      const int32_t sx1 = std::min(ox1, int32_t(target_render->width));
      const int32_t sy1 = std::min(oy1, int32_t(target_render->height));
      if (width && height && blit_ && target_render->format == kFormatColor &&
          resolved->image.format == kFormatColor && ox0 >= 0 && oy0 >= 0 && ox0 < sx1 && oy0 < sy1) {
        ResolveFrontEarlier(base_resolved, dx == 0 && dy == 0 && width == resolved->image.width &&
                                                   height == resolved->image.height);
        VkImageBlit scale{};
        scale.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
        scale.srcOffsets[0] = {ox0, oy0, 0};
        scale.srcOffsets[1] = {sx1, sy1, 1};
        scale.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
        scale.dstOffsets[0] = {int32_t(dx), int32_t(dy), 0};
        scale.dstOffsets[1] = {int32_t(dx + width), int32_t(dy + height), 1};
        const bool reduce = uint32_t(sx1 - ox0) > width || uint32_t(sy1 - oy0) > height;
        blit_(commands_job_, target_render->image, VK_IMAGE_LAYOUT_GENERAL, resolved->image.image,
              VK_IMAGE_LAYOUT_GENERAL, 1, &scale, reduce ? VK_FILTER_LINEAR : VK_FILTER_NEAREST);
        ++copies_;
        AnnotateCopy(width, height);
        ResolvedWritten(base_resolved, uint64_t(sx1 - ox0) * uint64_t(sy1 - oy0));
        ReadResolved(reg, *resolved, x0, y0, dx, dy, width, height);
      } else if (alias_failed_++ < 8) {
        REXLOG_WARN("[native] C2: copy through an alias not done: {}x{} from [{},{})x[{},{}) of {}x{} format {} to "
                    "{}x{} format {}",
                    width, height, ox0, sx1, oy0, sy1, target_render->width, target_render->height,
                    uint32_t(target_render->format), resolved->image.width, resolved->image.height,
                    uint32_t(resolved->image.format));
      }
    } else if (resolved) {
      Prepare(resolved->image);
      // What the game asks for and what fits in the render target.
      const uint32_t requested_width =
          std::min(uint32_t(x1 - x0), target_render->width - uint32_t(x0));
      const uint32_t requested_height = std::min(uint32_t(y1 - y0), target_render->height - uint32_t(y0));
      const uint32_t fit_width = resolved->image.width > dx ? resolved->image.width - dx : 0;
      const uint32_t fit_height = resolved->image.height > dy ? resolved->image.height - dy : 0;
      const uint32_t width = std::min(requested_width, fit_width);
      const uint32_t height = std::min(requested_height, fit_height);
      // With the scene at a higher resolution than the render target (bo2zm_internal_resolution = 1920x1080
      // and a 1280x720 front buffer), copying 1 to 1 takes only a piece: the image comes out cropped, with the
      // player view and the HUD out of frame. When it does not fit, it is shrunk with a linear blit, which is exactly
      // the scaling wanted. If it fits, it is copied as always, bit for bit.
      // It only shrinks when the scene really has to be reduced (by a factor of 1.25 or more) and both images
      // are color: vkCmdBlitImage with a linear filter on a depth target is not valid and brings the process
      // down. Mismatches of a few pixels are still cropped as always.
      const bool shrink = blit_ != nullptr && fit_width && fit_height &&
                           target_render->format == kFormatColor &&
                           resolved->image.format == kFormatColor &&
                           (requested_width * 4 >= fit_width * 5 || requested_height * 4 >= fit_height * 5);
      if (shrink && uint32_t(x0) < target_render->width && uint32_t(y0) < target_render->height) {
        if (FrontTraceLine()) REXLOG_INFO("[front-trace] route shrink target {:p} dest {:08X}",
                                         static_cast<const void*>(target_render), base_resolved);
        // bo2zm_native_lazy_front. If this texture had a deferred copy, it is dropped if this resolve
        // covers it entirely, and recorded first otherwise.
        ResolveFrontEarlier(base_resolved, dx == 0 && dy == 0 && fit_width == resolved->image.width &&
                                                   fit_height == resolved->image.height);
        VkImageBlit reduction{};
        reduction.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
        reduction.srcOffsets[0] = {x0, y0, 0};
        reduction.srcOffsets[1] = {x0 + int32_t(requested_width), y0 + int32_t(requested_height), 1};
        reduction.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
        reduction.dstOffsets[0] = {int32_t(dx), int32_t(dy), 0};
        reduction.dstOffsets[1] = {int32_t(dx + fit_width), int32_t(dy + fit_height), 1};
        blit_(commands_job_, target_render->image, VK_IMAGE_LAYOUT_GENERAL,
              resolved->image.image, VK_IMAGE_LAYOUT_GENERAL, 1, &reduction, VK_FILTER_LINEAR);
        ++copies_;
        ++reductions_;
        if (reductions_ <= 4) {
          REXLOG_INFO("[resolution] the {}x{} scene is shrunk to {}x{} when resolving it (supersampling)",
                      requested_width, requested_height, fit_width, fit_height);
        }
        AnnotateCopy(fit_width, fit_height);
        // A linear-filter blit reads the large rectangle and writes the small one, so it costs more than a copy
        // of the output size. The pixels read are recorded, since they are what dominates.
        ResolvedWritten(base_resolved, uint64_t(requested_width) * requested_height);
        ReadResolved(reg, *resolved, x0, y0, dx, dy, fit_width, fit_height);
      } else if (width && height && uint32_t(x0) < target_render->width &&
                 uint32_t(y0) < target_render->height && dx < resolved->image.width &&
                 dy < resolved->image.height) {
        /*
         * The same as is done with depth. If the whole render target is resolved to a texture of the same size,
         * the two images are swapped and not a single pixel is copied.
         */
        const bool whole_color = x0 == 0 && y0 == 0 && dx == 0 && dy == 0 &&
                                  width == target_render->width && height == target_render->height;
        // A resolve that changes format, number format or scale is not a bit copy.
        const uint32_t number_target = (reg.rb_copy_dest_info >> 13) & 0x7;
        const int32_t bias_target = int32_t(reg.rb_copy_dest_info << 10) >> 26;
        if (target_render->format != resolved->image.format || bias_target != 0 || number_target >= 2) {
          if (FrontTraceLine()) REXLOG_INFO("[front-trace] route converted target {:p} dest {:08X} bias {} number {}",
                                           static_cast<const void*>(target_render), base_resolved, bias_target, number_target);
          if (CopyConverted(*target_render, resolved->image, x0, y0, dx, dy, width, height, reg.rb_copy_dest_info)) {
            ++copies_;
            ++copies_converted_;
            AnnotateCopy(width, height);
            ResolvedWritten(base_resolved, uint64_t(width) * height);
            ReadResolved(reg, *resolved, x0, y0, dx, dy, width, height);
          }
          // (the clear below still applies)
        } else {
        /*
         * For color the clear is a requirement, unlike for depth. Tested without it: about one restore per
         * color swap; the game draws on top again without clearing and the copy is
         * paid anyway, only later and with two operations instead of one.
         */
        if (whole_color && clear_color && ResolveNoCopy() &&
            REXCVAR_GET(bo2zm_native_swap_color)) {
          // With x0 = y0 = 0 the tiling offset is 0, so the address is the base as is.
          if (SwapWithResolved(*target_render, *resolved, reg.rb_copy_dest_base & 0x1FFFFFFF)) {
            if (FrontTraceLine()) REXLOG_INFO("[front-trace] route image-swap target {:p} dest {:08X}",
                                             static_cast<const void*>(target_render), base_resolved);
            ++swaps_color_;
            ReadResolved(reg, *resolved, x0, y0, dx, dy, width, height);
            return true;
          }
        }
        VkImageCopy copy{};
        copy.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
        copy.srcOffset = {x0, y0, 0};
        copy.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
        copy.dstOffset = {int32_t(dx), int32_t(dy), 0};
        copy.extent = {width, height, 1};
        if (FrontTraceLine()) {
          const auto old = front_compositions_.find(target_render);
          REXLOG_INFO("[front-trace] bit-copy target {:p} front {:08X} extent {}x{} primary {} "
                      "origin {} full_dest {} rgba8 {} old_write_frame {} frame {}",
                      static_cast<const void*>(target_render), base_resolved, width, height, source == 0,
                      x0 == 0 && y0 == 0 && dx == 0 && dy == 0,
                      width == resolved->image.width && height == resolved->image.height,
                      target_render->format == kFormatColor && resolved->image.format == kFormatColor,
                      old == front_compositions_.end() ? UINT64_MAX : old->second.write_frame, presented_);
        }
        if (source == 0 && IsSwapFrontOnly(base_resolved) && !front_composition_sampled_.count(base_resolved) &&
            x0 == 0 && y0 == 0 && dx == 0 && dy == 0 &&
            width == resolved->image.width && height == resolved->image.height &&
            target_render->format == kFormatColor && resolved->image.format == kFormatColor) {
          // Learn presentation ownership from actual full resolves and Swaps, never from a game mode or size.
          front_compositions_[target_render] = {base_resolved, width, height, presented_};
          if (FrontTraceLine()) {
            REXLOG_INFO("[front-trace] learn target {:p} front {:08X} write_frame {}",
                        static_cast<const void*>(target_render), base_resolved, presented_);
          }
        }
        // bo2zm_native_lazy_front. The previous copy to this texture, if still deferred, is dropped (this
        // one covers it entirely) or recorded first; and if this texture is a front buffer that only the Swap
        // reads, it is deferred.
        ResolveFrontEarlier(base_resolved,
                                dx == 0 && dy == 0 && width == resolved->image.width && height == resolved->image.height);
        if (!PostponeCopyFront(base_resolved, *target_render, *resolved, copy) &&
            !PostponeComposite(base_resolved, *target_render, *resolved, copy,
                              (info_color & 0xFFF) | (pitch_source << 12))) {
          copy_image_(commands_job_, target_render->image, VK_IMAGE_LAYOUT_GENERAL,
                         resolved->image.image, VK_IMAGE_LAYOUT_GENERAL, 1, &copy);
          ++copies_;
          AnnotateCopy(width, height);
          ResolvedWritten(base_resolved, uint64_t(width) * height);
          ReadResolved(reg, *resolved, x0, y0, dx, dy, width, height);
        }
        }  // Bit copy
      }
    }
    if (clear_color) {
      const uint64_t value_raw =
          uint64_t(reg.rb_color_clear) | (uint64_t(reg.rb_color_clear_lo) << 32);
      /*
       * Two things a color clear does besides clearing.
       *  1. The clear leaves the whole render target with a known color, so if its content had gone away
       *     in a swap it no longer has to be brought back. Depth already did this (ClearDepth);
       *     color did not, and that is why, when bo2zm_native_swap_color was turned on, there were
       *     about as many restores as swaps: almost all of them restored only to clear right after.
       *  2. If the render target is already cleared to that same color and nothing has been drawn since
       *     then, the vkCmdClearColorImage does not change a single bit and is skipped.
       */
      if (composite_has_ && composite_.source == ((info_color & 0xFFF) | (pitch_source << 12))) {
        CompositeBeforeWriteSource();  // bo2zm_native_lazy_composite (its source is cleared)
      }
      if (!diag_watched_.empty()) {  // bo2zm_native_diag_readers_s (a source is cleared)
        DiagSourceWritten((info_color & 0xFFF) | (pitch_source << 12));
      }
      target_render->content_invalid = false;
      const uint64_t value_state =
          REXCVAR_GET(bo2zm_native_diag_clear) ? ~uint64_t(0) : value_raw;
      if (ClearRedundant(*target_render, value_state)) {
        ++clears_skipped_;
      } else {
        // bo2zm_native_lazy_front. If this render target is the source of a deferred front buffer, the
        // clear goes to a spare image and the one with the content is kept for the front buffer (no copy).
        RotateFrontBeforeClear(*target_render);
        MarkGpu(kGpuClears);  // clears, separate from copies (C2 report)
        VkClearColorValue color{};
        if (REXCVAR_GET(bo2zm_native_diag_clear)) {
          color = ColorDiagnostic(info_color & 0xFFF, format_color, pitch_source);
        } else {
          for (uint32_t j = 0; j < 4; ++j) {
            color.float32[j] = float((value_raw >> (j * 8)) & 0xFF) * (1.0f / 255.0f);
          }
        }
        // bo2zm_native_clear_useful_area. Only the rows in use; the bottom band, if needed.
        if (!ClearColorAreaUseful(*target_render, color)) {
          dfn_.vkCmdClearColorImage(commands_job_, target_render->image,
                                    VK_IMAGE_LAYOUT_GENERAL, &color, 1, &kRangeColor);
          RemoveBand(*target_render);
        }
        ++clears_;
        AnnotateClearDiag(*target_render, info_color & 0xFFF, format_color, pitch_source, false, false);
      }
    }
    if (((control >> 9) & 0x1) && clear_depth_) {
      ClearDepth(reg, pitch);
    }
    return true;
  }

  void ClearDepth(const CopyRegisters& reg, uint32_t pitch) {
    const uint32_t info = reg.rb_depth_info;
    DepthTarget* depth = GetDepth(info & 0xFFF, (info >> 16) & 0x1, pitch);
    if (!depth || !Record()) {
      return;
    }
    Prepare(*depth);
    BeforeWriteDepth(*depth);  // bo2zm_native_lazy_depth
    ShadowMinBeforeClear(*depth);   // bo2zm_native_shadow_min
    depth->content_invalid = false;  // The clear gives it contents
    const VkClearDepthStencilValue value{float(reg.rb_depth_clear >> 8) / 16777215.0f,
                                         reg.rb_depth_clear & 0xFF};
    // If it is already cleared with this same value and nobody has drawn anything since then, the clear does
    // not change a single bit. Mind the path below: the per-pass clear also resets the ZCULL plane, so that
    // one is never skipped (only the vkCmdClearDepthStencilImage one).
    if (depth->accepts_copy_target &&
        ClearRedundant(*depth, uint64_t(reg.rb_depth_clear))) {
      ++clears_skipped_depth_;
      return;
    }
    MarkGpu(kGpuClears);  // clears, separate from copies (C2 report)
    ++clears_depth_;
    // bo2zm_native_diag_clears. Without TRANSFER_DST the clear is done by opening a pass (ZCULL).
    AnnotateClearDiag(*depth, info & 0xFFF, (info >> 16) & 0x1, pitch, true,
                      !depth->accepts_copy_target);
    // ZCULL: see Prepare. Without TRANSFER_DST the clear has to be done by opening a pass.
    if (!depth->accepts_copy_target && draws_) {
      // This bool cannot be dropped. If the clear pass cannot be opened, the depth keeps the previous frame's
      // content and the ZCULL hi-Z is not reset either (only a loadOp = CLEAR resets it), so the whole frame
      // culls against stale data. Without TRANSFER_DST there is no alternative path, so at least it is
      // counted and reported.
      if (!draws_->ClearDepthInPass(commands_job_, *depth, value.depth,
                                             value.stencil)) {
        if (++clears_in_pass_failed_ <= 8) {
          REXLOG_WARN("[native] C2: could NOT clear the {}x{} depth by opening a pass; it keeps the previous content "
                      "(error {})",
                      depth->width, depth->height, clears_in_pass_failed_);
        }
      }
      return;
    }
    clear_depth_(commands_job_, depth->image, VK_IMAGE_LAYOUT_GENERAL, &value, 1,
                        &kRangeDepth);
  }

  // A Direct3D Clear() that BO2 issued as a draw (DrawClear; decoded by the ring in ClearD3D).
  // The ring has already moved the rectangle from D3D's alias surface to the one the target is drawn with, so
  // this only clears images that already exist with that key: an alias must not create an image of its own.
  // Each target goes through TargetColor / TargetDepth first, like a draw pass (prepared, deferred
  // copies recorded, swapped content brought back), because a clear can cover only part of it.
  bool ClearAsDraw(const DrawClear& b) override {
    const uint32_t pitch = b.rb_surface_info & 0x3FFF;
    if (!draws_ || !pitch || b.x1 <= b.x0 || b.y1 <= b.y0) {
      return false;
    }
    draws_->FinishPass();
    if (!Record()) {
      return false;
    }
    // The rectangle in the image's pixels (the shadow map can be drawn smaller than the guest thinks).
    const auto area_in = [&b](const OwnedImage& image, VkRect2D& area) {
      int64_t x0 = b.x0, y0 = b.y0, x1 = b.x1, y1 = b.y1;
      if (image.width_guest && image.height_guest &&
          (image.width_guest != image.width || image.height_guest != image.height)) {
        x0 = x0 * image.width / image.width_guest;
        x1 = (x1 * image.width + image.width_guest - 1) / image.width_guest;
        y0 = y0 * image.height / image.height_guest;
        y1 = (y1 * image.height + image.height_guest - 1) / image.height_guest;
      }
      x0 = std::clamp<int64_t>(x0, 0, image.width);
      x1 = std::clamp<int64_t>(x1, 0, image.width);
      y0 = std::clamp<int64_t>(y0, 0, image.height);
      y1 = std::clamp<int64_t>(y1, 0, image.height);
      if (x0 >= x1 || y0 >= y1) {
        return false;
      }
      area.offset = {int32_t(x0), int32_t(y0)};
      area.extent = {uint32_t(x1 - x0), uint32_t(y1 - y0)};
      return true;
    };
    // Earlier draws and copies of the image finish before the clear, and the clear before what follows.
    const auto barrier = [this]() {
      VkMemoryBarrier m{};
      m.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER;
      m.srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT |
                        VK_ACCESS_TRANSFER_WRITE_BIT;
      m.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT |
                        VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT |
                        VK_ACCESS_TRANSFER_READ_BIT | VK_ACCESS_TRANSFER_WRITE_BIT | VK_ACCESS_SHADER_READ_BIT;
      constexpr VkPipelineStageFlags kStages =
          VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT |
          VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT | VK_PIPELINE_STAGE_TRANSFER_BIT;
      dfn_.vkCmdPipelineBarrier(commands_job_, kStages, kStages | VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 0, 1,
                                &m, 0, nullptr, 0, nullptr);
    };
    bool done = false;
    if (b.depth || b.stencil) {
      const uint32_t base = b.rb_depth_info & 0xFFF;
      const uint32_t format = (b.rb_depth_info >> 16) & 0x1;
      const uint64_t key = (uint64_t(base) << 20) | (uint64_t(format) << 16) | pitch;
      if (depths_.count(key)) {
        OwnedImage* image = static_cast<OwnedImage*>(TargetDepth(base, format, pitch));
        VkRect2D area{};
        if (image && area_in(*image, area)) {
          VkClearValue value{};
          value.depthStencil = {std::clamp(b.z, 0.0f, 1.0f), b.value_stencil & 0xFF};
          const VkImageAspectFlags aspects = (b.depth ? VK_IMAGE_ASPECT_DEPTH_BIT : 0u) |
                                              (b.stencil ? VK_IMAGE_ASPECT_STENCIL_BIT : 0u);
          MarkGpu(kGpuClears);
          barrier();
          if (draws_->ClearAttachmentInPass(commands_job_, *image, aspects, value, area)) {
            barrier();
            ForgetClear(*image);
            ++clears_depth_;
            done = true;
          }
        }
      }
    }
    if (b.color) {
      const uint32_t base = b.rb_color_info & 0xFFF;
      const uint32_t format = (b.rb_color_info >> 16) & 0xF;
      const uint64_t key =
          (uint64_t(base) << 20) | (uint64_t(bo2zm_formats::Storage(format)) << 16) | pitch;
      if (bo2zm_formats::ColorApprox(format) && targets_.count(key)) {
        OwnedImage* image = static_cast<OwnedImage*>(TargetColor(base, format, pitch));
        VkRect2D area{};
        if (image && area_in(*image, area)) {
          VkClearValue value{};
          for (uint32_t i = 0; i < 4; ++i) {
            value.color.float32[i] = b.rgba[i];
          }
          MarkGpu(kGpuClears);
          barrier();
          if (draws_->ClearAttachmentInPass(commands_job_, *image, VK_IMAGE_ASPECT_COLOR_BIT, value, area)) {
            barrier();
            ForgetClear(*image);
            if (auto front = front_compositions_.find(image); front != front_compositions_.end()) {
              if (FrontTraceLine()) {
                REXLOG_INFO("[front-trace] guest-clear target {:p} front {:08X} write_frame {} -> {} "
                            "area {},{} {}x{}",
                            static_cast<const void*>(image), front->second.address, front->second.write_frame,
                            presented_, area.offset.x, area.offset.y, area.extent.width, area.extent.height);
              }
              front->second.write_frame = presented_;  // an explicit guest clear already began this composition
            }
            ++clears_;
            done = true;
          }
        }
      }
    }
    return done;
  }

  // Depth copy: the rectangle of the depth render target to a resolved texture
  // of the same host format, at the base GetResolveInfo computes
  // (graphics/util/draw.cpp:945-1010; 4 bytes per texel, like k_24_8).
  // would_clear: this same game command clears the render target right after resolving it. Only then is it worth
  // swapping the images: if the game kept drawing on top, the content would have to be brought back and the
  // copy would be paid anyway (measured: one restore per frame on the shadow map).
  void CopyDepth(const CopyRegisters& reg, uint32_t pitch, bool would_clear) {
    const uint32_t info_target = reg.rb_copy_dest_info;
    if ((info_target >> 3) & 0x1) {
      Reject(3, "copy to a 3D or array texture: not yet");
      return;
    }
    int32_t x0, y0, x1, y1;
    if (!copy_image_ || !Rectangle(reg, pitch, x0, y0, x1, y1)) {
      return;
    }
    const uint32_t info = reg.rb_depth_info;
    ++copies_depth_;  // the game asks to resolve the depth to a texture
    DepthTarget* depth = GetDepth(info & 0xFFF, (info >> 16) & 0x1, pitch);
    if (!depth) {
      return;
    }
    const uint32_t pitch_target = reg.rb_copy_dest_pitch & 0x3FFF;
    const uint32_t height_target = (reg.rb_copy_dest_pitch >> 16) & 0x3FFF;
    const uint32_t base_x = uint32_t(x0) & ~uint32_t(31);
    const uint32_t base_y = uint32_t(y0) & ~uint32_t(31);
    const uint32_t base =
        reg.rb_copy_dest_base +
        uint32_t(OffsetTiled2D(int32_t(base_x), int32_t(base_y), pitch_target, 2));
    const uint32_t dx = uint32_t(x0) - base_x;
    const uint32_t dy = uint32_t(y0) - base_y;
    const uint32_t format_texture = ((info >> 16) & 0x1) ? 23 : 22;  // k_24_8_FLOAT : k_24_8
    // With the scaled shadow map, the resolved texture stays at that same size.
    // The scene picks it by address, without looking at the size, and samples it with normalized coordinates
    // and point sampling, so the smaller map read with the UVs of the full one gives the same texel as the
    // smaller map upscaled with NEAREST: the image does not change. What is saved is the upscaling blit and
    // most of the copy.
    uint32_t width_resolved = pitch_target, height_resolved = height_target;
    if (depth->width_guest && depth->width_guest != depth->width) {
      width_resolved = uint32_t(uint64_t(pitch_target) * depth->width / depth->width_guest);
      height_resolved = uint32_t(uint64_t(height_target) * depth->height / depth->height_guest);
    }
    // If the previous copy to this address is still deferred, it is recorded first (exact; it does not happen
    // in gameplay: the scene clear resolves it earlier). See bo2zm_native_lazy_depth.
    if (!pending_.empty() && pending_.count(base & 0x1FFFFFFF)) {
      RecordCopyPending(base & 0x1FFFFFFF);
      ++lazy_copied_write_;
    }
    Resolved* resolved = GetResolved(base & 0x1FFFFFFF, width_resolved, height_resolved,
                                         format_texture, false, format_depth_);
    if (!resolved || !Record()) {
      return;
    }
    // Depth is also included in the per-render-target inventory. Previously only the square cubemap faces were
    // recorded, so the shadow map (the most expensive copy in the frame) did not show up.
    AnnotateCopy(base & 0x1FFFFFFF, width_resolved, height_resolved, 0);
    if (diag_window_ || !diag_watched_.empty()) {  // bo2zm_native_diag_readers_s
      DiagWrite(base & 0x1FFFFFFF, info & 0xFFF, pitch, x0, y0, uint32_t(x1 - x0), uint32_t(y1 - y0),
                    resolved->image.width, resolved->image.height, 0, true);
    }
    Prepare(*depth);
    Prepare(resolved->image);
    AnnotateAreaUseful(*depth, y1);  // the area the game really resolves
    AnnotateUseClear(*depth, uint32_t(std::max(x1, 0)), uint32_t(std::max(y1, 0)));
    // With the scaled shadow map, the rectangle the guest sends is in the full map's pixels even though the image
    // is smaller. Everything below reasons in guest pixels.
    const uint32_t width_guest = depth->width_guest ? depth->width_guest : depth->width;
    const uint32_t height_guest = depth->height_guest ? depth->height_guest : depth->height;
    const bool scaled = width_guest != depth->width || height_guest != depth->height;
    // A tall resolve can cross EDRAM surfaces that were rendered separately. Host images have
    // independent storage, so copying only the first one leaves part of the shadow atlas unwritten.
    if (!scaled && ((reg.rb_surface_info >> 16) & 3) == 0 && x0 >= 0 && y0 >= 0 &&
        uint32_t(x1) <= width_guest && dx < resolved->image.width && dy < resolved->image.height &&
        uint32_t(y1) > height_guest && depth->draw_frame == presented_) {
      std::array<DepthResolveBank, 64> banks{};
      std::array<OwnedImage*, 64> sources{};
      size_t count_banks = 0;
      for (auto& [key, image] : depths_) {
        if ((key & 0xFFFFF) != ((uint64_t((info >> 16) & 1) << 16) | pitch) ||
            !image.prepared || image.content_invalid || image.resolved_base || image.draw_frame != presented_ ||
            image.draw_order < depth->draw_order || image.width_guest || image.height_guest || image.width != pitch ||
            image.format != resolved->image.format) {
          continue;
        }
        if (count_banks == banks.size()) {
          count_banks = 0;
          break;
        }
        banks[count_banks] = {uint32_t(key >> 20), image.width, image.height};
        sources[count_banks++] = &image;
      }
      const uint32_t width_copy = std::min(uint32_t(x1 - x0), resolved->image.width - dx);
      const uint32_t height_copy = std::min(uint32_t(y1 - y0), resolved->image.height - dy);
      const auto plan = PlanDepthResolve(info & 0xFFF, pitch, uint32_t(y0), height_copy,
                                        std::span(banks.data(), count_banks));
      if (width_copy && plan.count > 1) {
        for (size_t i = 0; i < plan.count; ++i) {
          const auto& region = plan.regions[i];
          auto& source = *sources[region.bank];
          VkImageCopy copy{};
          copy.srcSubresource = {VK_IMAGE_ASPECT_DEPTH_BIT, 0, 0, 1};
          copy.srcOffset = {x0, int32_t(region.source_y), 0};
          copy.dstSubresource = {VK_IMAGE_ASPECT_DEPTH_BIT, 0, 0, 1};
          copy.dstOffset = {int32_t(dx), int32_t(dy + region.destination_y), 0};
          copy.extent = {width_copy, region.height, 1};
          copy_image_(commands_job_, source.image, VK_IMAGE_LAYOUT_GENERAL, resolved->image.image,
                      VK_IMAGE_LAYOUT_GENERAL, 1, &copy);
          ++copies_;
          AnnotateCopy(width_copy, region.height);
          if (warnings_depth_banks_ < 4) {
            REXLOG_INFO("[native] depth resolve bank {:03X}: source rows {}-{} to destination rows {}-{}",
                        banks[region.bank].base, region.source_y, region.source_y + region.height,
                        dy + region.destination_y, dy + region.destination_y + region.height);
          }
        }
        ResolvedWritten(base & 0x1FFFFFFF, uint64_t(width_copy) * height_copy);
        if (warnings_depth_banks_++ < 4) {
          REXLOG_INFO("[native] depth resolve across {} EDRAM surfaces: {}x{} at {:08X}", plan.count,
                      width_copy, height_copy, base & 0x1FFFFFFF);
        }
        return;
      }
    }
    const uint32_t width = std::min({uint32_t(x1 - x0), width_guest - uint32_t(x0),
                                     resolved->image.width - dx});
    const uint32_t height = std::min({uint32_t(y1 - y0), height_guest - uint32_t(y0),
                                    resolved->image.height - dy});
    if (!width || !height || uint32_t(x0) >= width_guest ||
        uint32_t(y0) >= height_guest || dx >= resolved->image.width ||
        dy >= resolved->image.height) {
      return;
    }
    // bo2zm_native_resolve_valid_content. If the content of this render target went away in an earlier
    // swap and nobody brought it back, it is in another texture: it is brought back before reading it (the
    // menu flicker).
    if (depth->content_invalid) {
      if (REXCVAR_GET(bo2zm_native_resolve_valid_content)) {
        RestoreContent(*depth, true);
      } else {
        ++resolve_content_old_;  // previous behavior: the old image is resolved
      }
    }
    if (scaled) {
      // Source and destination have the same reduced size, so it is a normal 1 to 1 copy
      // with the rectangle converted to image pixels. No blit, no scaling.
      const auto aX = [&](int32_t v) {
        return int32_t(int64_t(v) * depth->width / width_guest);
      };
      const auto aY = [&](int32_t v) {
        return int32_t(int64_t(v) * depth->height / height_guest);
      };
      const uint32_t width_img = std::min(
          {uint32_t(std::max(1, aX(x0 + int32_t(width)) - aX(x0))),
           depth->width - uint32_t(aX(x0)), resolved->image.width - uint32_t(aX(int32_t(dx)))});
      const uint32_t height_img = std::min(
          {uint32_t(std::max(1, aY(y0 + int32_t(height)) - aY(y0))),
           depth->height - uint32_t(aY(y0)), resolved->image.height - uint32_t(aY(int32_t(dy)))});
      if (!width_img || !height_img) {
        return;
      }
      VkImageCopy copy{};
      copy.srcSubresource = {VK_IMAGE_ASPECT_DEPTH_BIT, 0, 0, 1};
      copy.srcOffset = {aX(x0), aY(y0), 0};
      copy.dstSubresource = {VK_IMAGE_ASPECT_DEPTH_BIT, 0, 0, 1};
      copy.dstOffset = {aX(int32_t(dx)), aY(int32_t(dy)), 0};
      copy.extent = {width_img, height_img, 1};
      copy_image_(commands_job_, depth->image, VK_IMAGE_LAYOUT_GENERAL,
                     resolved->image.image, VK_IMAGE_LAYOUT_GENERAL, 1, &copy);
      ++copies_;
      ++shadows_uploads_;
      AnnotateCopy(width_img, height_img);
      ResolvedWritten(base & 0x1FFFFFFF, uint64_t(width_img) * height_img);
      return;
    }
    // If the whole render target is resolved to a texture of the same size, the images are swapped and
    // nothing is copied.
    if (ResolveNoCopy()) {  // and if it is not swapped, why not
      const bool whole = x0 == 0 && y0 == 0 && dx == 0 && dy == 0 &&
                          width == depth->width && height == depth->height;
      // The clear is no longer a requirement (see bo2zm_native_swap_without_clear).
      const bool can = whole && (would_clear || REXCVAR_GET(bo2zm_native_swap_without_clear));
      if (can) {
        if (SwapWithResolved(*depth, *resolved, base & 0x1FFFFFFF)) {
          if (!would_clear) {
            ++swaps_no_clear_;
          }
          ShadowMinAfterSwap(*depth, base & 0x1FFFFFFF, would_clear);
          return;
        }
        ++no_swap_[2];  // the swap itself could not be done
      } else {
        /*
         * The log's reason follows the real cause. With bo2zm_native_swap_without_clear on, `can` is just
         * "whole", so everything that lands here was not resolved whole; counting it as "the command does not
         * clear the render target" would be wrong. If the resolve is whole, the cause is the clear; otherwise it
         * is the size.
         */
        ++no_swap_[whole ? 0 : 1];  // 0: the command does not clear the render target; 1: not the whole render target
        if (warnings_no_swap_ < 8) {
          ++warnings_no_swap_;
          REXLOG_INFO("[native] C2 without a swap: {}x{} of {}x{} at ({},{})->({},{}), clears {} (base {:03X})",
                      width, height, depth->width, depth->height, x0, y0, dx, dy, would_clear,
                      base & 0xFFF);
        }
      }
    }
    VkImageCopy copy{};
    copy.srcSubresource = {VK_IMAGE_ASPECT_DEPTH_BIT, 0, 0, 1};
    copy.srcOffset = {x0, y0, 0};
    copy.dstSubresource = {VK_IMAGE_ASPECT_DEPTH_BIT, 0, 0, 1};
    copy.dstOffset = {int32_t(dx), int32_t(dy), 0};
    copy.extent = {width, height, 1};
    // If this address is only requested by the composition without blur, the copy is deferred
    // (bo2zm_native_lazy_depth): it is done as soon as a draw really samples it; if nobody does,
    // it is not.
    ShadowMinCopyFrom(*depth);  // bo2zm_native_shadow_min
    if (PostponeCopyDepth(base & 0x1FFFFFFF, *depth, *resolved, copy)) {
      return;
    }
    copy_image_(commands_job_, depth->image, VK_IMAGE_LAYOUT_GENERAL,
                   resolved->image.image, VK_IMAGE_LAYOUT_GENERAL, 1, &copy);
    ++copies_;
    AnnotateCopy(width, height);
    ResolvedWritten(base & 0x1FFFFFFF, uint64_t(width) * height);
  }

  void RampGamma(const std::array<std::array<uint16_t, 3>, 256>& ramp) override {
    ramp_game_ = ramp;
    RecomputeRamp();
  }

  // The output table is the game's ramp followed by the post-processing, with the formula and order of the
  // public-domain color-grading shader the presets come from: temperature, brightness, contrast, tint, saturation,
  // vibrance and gamma. Everything that is single-channel is computed here for the 256 entries, so without saturation,
  // vibrance, vignette or scanlines there is no per-pixel work (and when off, the table is the ramp as
  // is). With them (grading_), the table stops before the gamma and without clamping, and the shader
  // does the rest.
  void RecomputeRamp() {
    const bo2zm::settings::Postprocess& p = postprocess_;
    grading_ = p.active && (p.saturation != 1.0f || p.vibrance != 0.0f || p.vignette > 0.0f || p.lines > 0.0f);
    const float tint[3] = {p.tint_r, p.tint_g, p.tint_b};
    const float inverse_gamma = p.gamma > 0.0f ? 1.0f / p.gamma : 1.0f;
    for (uint32_t i = 0; i < 256; ++i) {
      for (uint32_t c = 0; c < 3; ++c) {
        float x = float(ramp_game_[i][c] & 0x3FF) / 1023.0f;
        if (p.active) {
          x += c == 0 ? p.temperature * 0.10f : c == 2 ? -p.temperature * 0.10f : 0.0f;
          x += p.brightness;
          x = (x - 0.5f) * p.contrast + 0.5f;
          x = x + (x * tint[c] - x) * p.tint;
          if (!grading_) {
            x = std::clamp(std::pow(std::max(x, 0.0001f), inverse_gamma), 0.0f, 1.0f);
          }
        }
        ramp_values_[i * 4 + c] = x;
      }
      ramp_values_[i * 4 + 3] = 1.0f;
    }
    const size_t extra = 256 * 4;
    ramp_values_[extra] = p.saturation;
    ramp_values_[extra + 1] = p.vibrance;
    ramp_values_[extra + 2] = inverse_gamma;
    ramp_values_[extra + 3] = 0.0f;
    ramp_values_[extra + 4] = p.vignette;
    ramp_values_[extra + 5] = p.lines;
    ramp_values_[extra + 6] = 0.0f;
    ramp_values_[extra + 7] = 0.0f;
    ++version_ramp_;
  }

  bool Present(rex::ui::Presenter* ui_presenter, const SwapTexture& swap, uint32_t width,
                 uint32_t height) override {
    bo2zm::reflection_demand::AnnotateSwap();  // reflection on demand
    // The per-draw diagnostic window opens here, on the PM4 ring thread, which is the one that records the
    // draws. Inside the paint call it would be another thread and the window would catch an arbitrary piece
    // of the frame (a fraction of the shadow draws).
    OpenWindowDiagnostic();
#if REX_PLATFORM_SWITCH
    RexSwitchPerfCount(0);  // the profiler's "game N fps", which read 0.0 with the native renderer
#endif
    // Histogram of intervals between Swaps (C2 report): whether they come in vsync steps or spread out.
    {
      const auto now = std::chrono::steady_clock::now();
      if (last_swap_ != std::chrono::steady_clock::time_point{}) {
        const double ms = std::chrono::duration<double, std::milli>(now - last_swap_).count();
#if REX_PLATFORM_SWITCH
        // Frames over 45 ms go to stack sampling. The window starts one normal frame (33 ms) earlier, because
        // the game thread prepares the frame ahead of the ring. Ticks at 19.2 MHz.
        if (ms > 45.0) {
          uint64_t end;
          asm volatile("mrs %0, cntpct_el0" : "=r"(end));  // libnx's armGetSystemTick
          const uint64_t run_length = uint64_t((ms + 33.3) * 19200.0);
          RexSwitchPerfHitch(end > run_length ? end - run_length : 0, end);
        }
#endif
        const uint32_t bucket = ms < 15.0    ? 0
                                : ms < 18.0  ? 1
                                : ms < 25.0  ? 2
                                : ms < 30.0  ? 3
                                : ms < 36.0  ? 4
                                : ms < 50.0  ? 5
                                : ms < 60.0  ? 6
                                : ms < 75.0  ? 7
                                : ms < 100.0 ? 8
                                : ms < 150.0 ? 9
                                             : 10;
        ++buckets_swap_[bucket];
        // The worst frame of the interval. The average is not felt; the peak is.
        if (ms > worst_swap_ms_) {
          worst_swap_ms_ = ms;
        }
        bo2zm::guard30::Heartbeat(ms);  // the 30 FPS guard decides with this
        /*
         * What happens in a frame that runs long.
         *
         * Frames can run long while pipelines are stable and the texture cache no longer evicts: a stutter
         * with a perfectly normal render load (the usual copies, clears, swaps and restores) that is not
         * quantized to the vblank means the time went somewhere other than drawing.
         *
         * So when a frame runs long, what that frame did is dumped, not the running totals:
         *   - its GPU work, from the timestamps already measured: if it rises toward 60 ms it is scene load; if
         *     it stays at 33 the time went to the CPU or to waiting;
         *   - how long the thread took to record and how long to present;
         *   - new textures and pipelines, the usual suspects of an isolated peak.
         * It only writes when there is a stutter, so it costs nothing in the normal case.
         */
        /*
         * Up to 1,000 of these lines per session. A cap of 200 ran out a little over halfway through a long
         * session, and the most-played stretch afterwards had nearly a hundred frames over 50 ms without a single
         * line. 1,000 covers a whole session, and since a line is only written when there is a stutter it costs
         * nothing in the normal case.
         */
        if (ms > 60.0 && warnings_hitch_ < 1000) {
          ++warnings_hitch_;
          /* gpu_ns_ are raw GPU timestamps: multiply by 1.627 to get real milliseconds (see docs/measuring.md). */
          const double gpu_ms = double(gpu_ns_ - hitch_gpu_ns_) / 1e6 * 1.627;
          // The three [hitch] lines go to the report thread (BO2ZM_REPORT_RING).
          BO2ZM_REPORT_RING(
              "[hitch] frame of {:.1f} ms (GPU {:.1f} real, record {:.1f}): {} copies, {} clears, {} swaps, {} "
              "restorations, {} waits for the GPU",
              ms, gpu_ms, double(ns_record_ - hitch_ns_record_) / 1e6, copies_ - hitch_copies_,
              clears_ - hitch_clears_, swaps_ - hitch_resolves_,
              restores_ - hitch_restores_, waits_gpu_ - hitch_waits_);
          /*
           * Who waits for whom in this frame, in ms (see bo2zm_hitch_waits.h).
           * The game's and the ring's waits overlap: they do not add up to the frame, they say where it went.
           */
          namespace e = bo2zm::waits;
          const auto delta = [&](e::Kind t) {
            return double(e::g_ns[t].load(std::memory_order_relaxed) - hitch_waits_ns_[t]) / 1e6;
          };
          const auto times = [&](e::Kind t) {
            return e::g_times[t].load(std::memory_order_relaxed) - hitch_waits_times_[t];
          };
          BO2ZM_REPORT_RING(
              "[hitch] waits (ms): game: executor handoff {:.1f} ({}), preparer handoff {:.1f} ({}), room in the ring "
              "{:.1f} ({}), inside sub_826E8EE8 {:.1f} ({}; vtable {:08X}, caller {:08X}) | ring: no work {:.1f} ({}), "
              "WAIT_REG_MEM {:.1f} ({}), GPU fence {:.1f}, output {:.1f} | textures checked {:.1f} MB, postponed {}",
              delta(e::kHandoffExecutor), times(e::kHandoffExecutor), delta(e::kHandoffPreparer),
              times(e::kHandoffPreparer), delta(e::kRoomRing), times(e::kRoomRing),
              delta(e::kGameMid), times(e::kGameMid), e::g_vtable_mid.load(std::memory_order_relaxed),
              e::g_caller_mid.load(std::memory_order_relaxed), delta(e::kRingNoJob),
              times(e::kRingNoJob), delta(e::kRingRegMem), times(e::kRingRegMem),
              double(ns_waits_gpu_ - hitch_ns_waits_gpu_) / 1e6,
              double(ns_wait_output_ - hitch_ns_wait_output_) / 1e6,
              double(e::g_bytes_hash.load(std::memory_order_relaxed) - hitch_bytes_hash_) / 1048576.0,
              e::g_hashes_postponed.load(std::memory_order_relaxed) - hitch_hashes_postponed_);
          // What the ring spent this frame on.
          const auto diff_of = [](const std::atomic<uint64_t>& a, uint64_t before) {
            return a.load(std::memory_order_relaxed) - before;
          };
          // And how long the ring waited for the vertex copy thread (WaitUploads). The wait before each
          // submit is inside "working" and the one before returning the read pointer is outside: it is read
          // separately, not added.
          // And what the ring spent creating textures (image, memory and view; "textures" starts afterwards),
          // how many the binding thread bound and how long the ring waited for it
          // (bo2zm_native_texture_bind_thread).
          BO2ZM_REPORT_RING("[hitch] ring: {} draws, working {:.1f} ms, textures {:.1f} ms ({} uploads, {:.1f} MB; {} "
                      "created; hashes {:.1f} + {:.1f} ms); waiting for the vertex copy thread {:.1f} ms ({} times), "
                      "helping it {:.1f} ms ({} copies); creating textures {:.1f} ms on the ring, {} bound on the "
                      "thread, waiting for it {:.1f} ms; hashes on the thread: {} textures in {:.1f} ms of the thread, "
                      "copies on the ring {:.1f} ms, {} done by the ring in {:.1f} ms, waiting for it {:.1f} ms",
                      diff_of(e::g_draws, hitch_draws_), double(diff_of(e::g_ns_ring_working, hitch_ns_ring_)) / 1e6,
                      double(diff_of(e::g_ns_textures, hitch_ns_textures_)) / 1e6,
                      diff_of(e::g_textures_uploads, hitch_textures_uploads_),
                      double(diff_of(e::g_bytes_uploaded, hitch_bytes_uploaded_)) / 1048576.0,
                      diff_of(e::g_textures_created, hitch_textures_created_),
                      double(diff_of(e::g_ns_hash_raw, hitch_ns_hash_raw_)) / 1e6,
                      double(diff_of(e::g_ns_hash_data, hitch_ns_hash_data_)) / 1e6,
                      double(diff_of(e::g_ns_waiting_copies, hitch_ns_waiting_copies_)) / 1e6,
                      diff_of(e::g_waits_copies, hitch_waits_copies_),
                      double(diff_of(e::g_ns_helping_copies, hitch_ns_helping_copies_)) / 1e6,
                      diff_of(e::g_copies_helped, hitch_copies_helped_),
                      double(diff_of(e::g_ns_create_textures, hitch_ns_create_textures_)) / 1e6,
                      diff_of(e::g_textures_bound_thread, hitch_textures_bound_thread_),
                      double(diff_of(e::g_ns_waiting_bindings, hitch_ns_waiting_bindings_)) / 1e6,
                      // the texture fingerprint thread (bo2zm_native_texture_hash_thread)
                      diff_of(e::g_textures_hash_thread, hitch_textures_hash_thread_),
                      double(diff_of(e::g_ns_hash_thread, hitch_ns_hash_thread_)) / 1e6,
                      double(diff_of(e::g_ns_hash_snapshot, hitch_ns_hash_snapshot_)) / 1e6,
                      diff_of(e::g_textures_hash_ring, hitch_textures_hash_ring_),
                      double(diff_of(e::g_ns_hash_ring, hitch_ns_hash_ring_)) / 1e6,
                      double(diff_of(e::g_ns_waiting_hashes, hitch_ns_waiting_hashes_)) / 1e6);
          /*
           * The game's side. Four of the seven gameplay stutters in one run come from the preparer (the executor
           * waiting for the handoff, or waiting for commands inside the list, with the ring idle), and the wait
           * without commands did not show up in any line. "outside the list" is the preparer's simulation plus
           * its handoff wait (the one in the line above): the difference is what the game itself takes.
           */
          BO2ZM_REPORT_RING("[hitch] game: the executor without commands {:.1f} ms ({}) | the preparer: filling the "
                               "list {:.1f} ms ({}), of which TreeCull {:.1f} ms ({}); outside the list {:.1f} ms ({})",
                               delta(e::kExecutorNoOrders), times(e::kExecutorNoOrders),
                               delta(e::kPreparerList), times(e::kPreparerList),
                               delta(e::kPreparerScenery), times(e::kPreparerScenery),
                               delta(e::kPreparerOut), times(e::kPreparerOut));
        }
        for (uint32_t t = 0; t < bo2zm::waits::kNumTypes; ++t) {
          hitch_waits_ns_[t] = bo2zm::waits::g_ns[t].load(std::memory_order_relaxed);
          hitch_waits_times_[t] = bo2zm::waits::g_times[t].load(std::memory_order_relaxed);
        }
        hitch_ns_waits_gpu_ = ns_waits_gpu_;
        hitch_draws_ = bo2zm::waits::g_draws.load(std::memory_order_relaxed);
        hitch_ns_ring_ = bo2zm::waits::g_ns_ring_working.load(std::memory_order_relaxed);
        hitch_ns_textures_ = bo2zm::waits::g_ns_textures.load(std::memory_order_relaxed);
        hitch_textures_uploads_ = bo2zm::waits::g_textures_uploads.load(std::memory_order_relaxed);
        hitch_bytes_uploaded_ = bo2zm::waits::g_bytes_uploaded.load(std::memory_order_relaxed);
        hitch_textures_created_ = bo2zm::waits::g_textures_created.load(std::memory_order_relaxed);
        hitch_ns_hash_raw_ = bo2zm::waits::g_ns_hash_raw.load(std::memory_order_relaxed);
        hitch_ns_hash_data_ = bo2zm::waits::g_ns_hash_data.load(std::memory_order_relaxed);
        hitch_bytes_hash_ = bo2zm::waits::g_bytes_hash.load(std::memory_order_relaxed);
        hitch_hashes_postponed_ = bo2zm::waits::g_hashes_postponed.load(std::memory_order_relaxed);
        hitch_ns_waiting_copies_ = bo2zm::waits::g_ns_waiting_copies.load(std::memory_order_relaxed);  // 170
        hitch_waits_copies_ = bo2zm::waits::g_waits_copies.load(std::memory_order_relaxed);
        hitch_ns_helping_copies_ = bo2zm::waits::g_ns_helping_copies.load(std::memory_order_relaxed);  // 185
        hitch_copies_helped_ = bo2zm::waits::g_copies_helped.load(std::memory_order_relaxed);
        // Creating textures and the binding thread (bo2zm_native_texture_bind_thread).
        hitch_ns_create_textures_ = bo2zm::waits::g_ns_create_textures.load(std::memory_order_relaxed);
        hitch_textures_bound_thread_ = bo2zm::waits::g_textures_bound_thread.load(std::memory_order_relaxed);
        hitch_ns_waiting_bindings_ = bo2zm::waits::g_ns_waiting_bindings.load(std::memory_order_relaxed);
        // The texture fingerprint thread (bo2zm_native_texture_hash_thread).
        hitch_textures_hash_thread_ = bo2zm::waits::g_textures_hash_thread.load(std::memory_order_relaxed);
        hitch_ns_hash_thread_ = bo2zm::waits::g_ns_hash_thread.load(std::memory_order_relaxed);
        hitch_ns_hash_snapshot_ = bo2zm::waits::g_ns_hash_snapshot.load(std::memory_order_relaxed);
        hitch_textures_hash_ring_ = bo2zm::waits::g_textures_hash_ring.load(std::memory_order_relaxed);
        hitch_ns_hash_ring_ = bo2zm::waits::g_ns_hash_ring.load(std::memory_order_relaxed);
        hitch_ns_waiting_hashes_ = bo2zm::waits::g_ns_waiting_hashes.load(std::memory_order_relaxed);
        hitch_ns_wait_output_ = ns_wait_output_;
        hitch_gpu_ns_ = gpu_ns_;
        hitch_ns_record_ = ns_record_;
        hitch_copies_ = copies_;
        hitch_restores_ = restores_;
        hitch_waits_ = waits_gpu_;
        hitch_clears_ = clears_;
        hitch_resolves_ = swaps_;
      }
      last_swap_ = now;
    }
    const uint32_t base = (swap.dword[1] & 0xFFFFF000) & 0x1FFFFFFF;
    DiagReadersOnPresent(base);  // bo2zm_native_diag_readers_s
    // bo2zm_native_lazy_front. Before submitting the work: either it is drawn from the image that
    // holds the content (no copy) or the deferred copy is recorded now, ahead of the output.
    OwnedImage* const source_front = FrontOnPresent(base, width, height);
    const bool tiled =
        ui_presenter && REXCVAR_GET(bo2zm_native_diag_resolved) && ComposeTiled(base);
    // Without waiting for the GPU: the output goes after the work on the same queue.
    if (!ui_presenter || !SubmitJob(false)) {
      return false;
    }
    if (tiled) {
      bool painted_tiled = false;
      ui_presenter->RefreshGuestOutput(
          1280, 720, 1280, 720, [&](rex::ui::Presenter::GuestOutputRefreshContext& base_context) {
            auto& context =
                static_cast<VulkanPresenter::VulkanGuestOutputRefreshContext&>(base_context);
            painted_tiled = PaintOutput(context, tiled_, 1280, 720);
            return painted_tiled;
          });
      if (painted_tiled) {
        ++presented_;
      }
      return painted_tiled;
    }
    auto it = resolved_.find(base);
    if (it == resolved_.end() || !it->second.image.prepared) {
      Reject(4, "Swap without a resolved texture: the test color is painted");
      return false;
    }
    Resolved& resolved = it->second;
    const uint32_t w = std::min(width ? width : resolved.image.width, resolved.image.width);
    const uint32_t h = std::min(height ? height : resolved.image.height, resolved.image.height);
    bool painted = false;
    // How much of RefreshGuestOutput belongs to the SDK (before and after the callback) and how much to
    // PaintOutput.
    const auto before_refresh = std::chrono::steady_clock::now();
    auto input_call = before_refresh;
    auto output_call = before_refresh;
    bool call = false;
    ui_presenter->RefreshGuestOutput(
        w, h, 1280, 720, [&](rex::ui::Presenter::GuestOutputRefreshContext& base_context) {
          input_call = std::chrono::steady_clock::now();
          call = true;
          auto& context =
              static_cast<VulkanPresenter::VulkanGuestOutputRefreshContext&>(base_context);
          painted = PaintOutput(context, source_front ? *source_front : resolved.image, w, h,
                                 source_front != nullptr);  // bo2zm_native_lazy_front
          output_call = std::chrono::steady_clock::now();
          return painted;
        });
    if (call) {
      ns_before_call_ += Ns(before_refresh, input_call);
      ns_in_call_ += Ns(input_call, output_call);
      ns_after_call_ += Ns(output_call, std::chrono::steady_clock::now());
      ++refreshes_;
    }
    if (painted) {
      ++presented_;
    }
    if (!probe_done_ && presented_ >= 3000 && REXCVAR_GET(bo2zm_native_probe_host_import)) {
      probe_done_ = true;
      ProbeHostImport(kProbeBytes);
      ProbeHostImport(kProbeBytes * 16);
    }
    return painted;
  }

  /*
   * bo2zm_native_probe_host_import (test). Whether the GPU can read guest memory where it is, which the copies of
   * vertex data (and later indices) would then not need (checking every byte on the CPU costs about as much as the
   * copy itself). Once, after 3,000 frames: the 1 MB (then 16 MB) of guest physical memory around a recent vertex
   * range is imported with VK_EXT_external_memory_host (on the Switch, NVK's nvMapCreate of that address), the GPU
   * copies the first 1 MB into a readback buffer, and that is compared with what the CPU sees. The log says what
   * worked, what each step cost and whether the bytes match.
   */
  static constexpr VkDeviceSize kProbeBytes = VkDeviceSize(1) << 20;
  bool probe_done_ = false;

  void ProbeHostImport(VkDeviceSize bytes) {
    const uint32_t vertex = g_probe_vertex_address.load(std::memory_order_relaxed);
    const uint32_t address = vertex & ~uint32_t(bytes - 1);
    if (vertex == 0 || uint64_t(address) + bytes > 0x20000000ull) {
      REXLOG_WARN("[native] host import probe ({} MB): no vertex address to try", bytes >> 20);
      return;
    }
    uint8_t* const host = memory_->TranslatePhysical(address);
    const auto t0 = std::chrono::steady_clock::now();
    uint32_t sum = 0;
    for (VkDeviceSize i = 0; i < bytes; i += 4096) {
      sum += host[i];  // a first touch commits the page on the Switch
    }
    const auto& ifn = vulkan_device_->vulkan_instance()->functions();
    const auto properties_of = reinterpret_cast<PFN_vkGetMemoryHostPointerPropertiesEXT>(
        ifn.vkGetDeviceProcAddr(device_, "vkGetMemoryHostPointerPropertiesEXT"));
    if (!vulkan_device_->extensions().ext_EXT_external_memory_host || !properties_of) {
      REXLOG_WARN("[native] host import probe: VK_EXT_external_memory_host is not available on this device");
      return;
    }
    VkMemoryHostPointerPropertiesEXT properties{VK_STRUCTURE_TYPE_MEMORY_HOST_POINTER_PROPERTIES_EXT};
    const VkResult r_properties = properties_of(device_, VK_EXTERNAL_MEMORY_HANDLE_TYPE_HOST_ALLOCATION_BIT_EXT,
                                                host, &properties);
    uint32_t type = 0;
    if (r_properties != VK_SUCCESS || !rex::bit_scan_forward(properties.memoryTypeBits, &type)) {
      REXLOG_WARN("[native] host import probe ({} MB at guest {:08X}, host {}): no memory type (VkResult {}, types "
                  "{:#x})",
                  bytes >> 20, address, static_cast<void*>(host), int(r_properties), properties.memoryTypeBits);
      return;
    }
    VkImportMemoryHostPointerInfoEXT import{VK_STRUCTURE_TYPE_IMPORT_MEMORY_HOST_POINTER_INFO_EXT};
    import.handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_HOST_ALLOCATION_BIT_EXT;
    import.pHostPointer = host;
    VkMemoryAllocateInfo reserve{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    reserve.pNext = &import;
    reserve.allocationSize = bytes;
    reserve.memoryTypeIndex = type;
    VkDeviceMemory imported = VK_NULL_HANDLE;
    const auto t1 = std::chrono::steady_clock::now();
    const VkResult r_import = dfn_.vkAllocateMemory(device_, &reserve, nullptr, &imported);
    const auto t2 = std::chrono::steady_clock::now();
    const auto& types = vulkan_device_->memory_types();
    if (r_import != VK_SUCCESS) {
      REXLOG_WARN("[native] host import probe ({} MB at guest {:08X}): vkAllocateMemory with the host pointer FAILED "
                  "(VkResult {}) in {:.2f} ms; memory types {:#x}",
                  bytes >> 20, address, int(r_import), Ns(t1, t2) / 1e6, properties.memoryTypeBits);
      return;
    }
    VkExternalMemoryBufferCreateInfo external{VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_BUFFER_CREATE_INFO};
    external.handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_HOST_ALLOCATION_BIT_EXT;
    VkBufferCreateInfo info{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
    info.pNext = &external;
    info.size = bytes;
    info.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_VERTEX_BUFFER_BIT;
    info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    VkBuffer source = VK_NULL_HANDLE, target = VK_NULL_HANDLE;
    VkDeviceMemory target_memory = VK_NULL_HANDLE;
    VkCommandPool pool = VK_NULL_HANDLE;
    VkCommandBuffer commands = VK_NULL_HANDLE;
    VkFence fence = VK_NULL_HANDLE;
    void* mapped = nullptr;
    uint32_t target_type = 0;
    const char* failed = nullptr;
    VkCommandPoolCreateInfo info_pool{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
    info_pool.flags = VK_COMMAND_POOL_CREATE_TRANSIENT_BIT;
    info_pool.queueFamilyIndex = family_;
    VkCommandBufferAllocateInfo info_commands{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
    info_commands.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    info_commands.commandBufferCount = 1;
    VkFenceCreateInfo info_fence{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
    if (dfn_.vkCreateBuffer(device_, &info, nullptr, &source) != VK_SUCCESS ||
        dfn_.vkBindBufferMemory(device_, source, imported, 0) != VK_SUCCESS) {
      failed = "binding the imported memory to a buffer";
    } else if (!rex::ui::vulkan::util::CreateDedicatedAllocationBuffer(
                   vulkan_device_, kProbeBytes, VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                   rex::ui::vulkan::util::MemoryPurpose::kReadback, target, target_memory, &target_type) ||
               dfn_.vkMapMemory(device_, target_memory, 0, VK_WHOLE_SIZE, 0, &mapped) != VK_SUCCESS) {
      failed = "the readback buffer";
    } else if (dfn_.vkCreateCommandPool(device_, &info_pool, nullptr, &pool) != VK_SUCCESS ||
               (info_commands.commandPool = pool,
                dfn_.vkAllocateCommandBuffers(device_, &info_commands, &commands) != VK_SUCCESS) ||
               dfn_.vkCreateFence(device_, &info_fence, nullptr, &fence) != VK_SUCCESS) {
      failed = "the command buffer";
    }
    double ms_gpu = 0.0;
    bool match = false;
    size_t first_difference = 0;
    if (!failed) {
#if defined(__SWITCH__)
      armDCacheFlush(host, size_t(bytes));  // the CPU's writes, out to memory for the GPU
#endif
      VkCommandBufferBeginInfo begin{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
      begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
      dfn_.vkBeginCommandBuffer(commands, &begin);
      VkBufferCopy region{0, 0, kProbeBytes};
      dfn_.vkCmdCopyBuffer(commands, source, target, 1, &region);
      dfn_.vkEndCommandBuffer(commands);
      VkSubmitInfo submit{VK_STRUCTURE_TYPE_SUBMIT_INFO};
      submit.commandBufferCount = 1;
      submit.pCommandBuffers = &commands;
      const auto t3 = std::chrono::steady_clock::now();
      VkResult r_submit;
      {
        const auto queue = vulkan_device_->AcquireQueue(family_, 0);
        r_submit = dfn_.vkQueueSubmit(queue.queue(), 1, &submit, fence);
      }
      const VkResult r_wait =
          r_submit == VK_SUCCESS ? dfn_.vkWaitForFences(device_, 1, &fence, VK_TRUE, 2'000'000'000ull) : r_submit;
      ms_gpu = Ns(t3, std::chrono::steady_clock::now()) / 1e6;
      if (r_wait != VK_SUCCESS) {
        failed = "the GPU copy (submit or wait)";
      } else {
        if (!((types.host_coherent >> target_type) & 1)) {
          VkMappedMemoryRange range{VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE};
          range.memory = target_memory;
          range.size = VK_WHOLE_SIZE;
          dfn_.vkInvalidateMappedMemoryRanges(device_, 1, &range);
        }
        const uint8_t* read = static_cast<const uint8_t*>(mapped);
        match = std::memcmp(read, host, size_t(kProbeBytes)) == 0;
        while (!match && first_difference < kProbeBytes && read[first_difference] == host[first_difference]) {
          ++first_difference;
        }
      }
    }
    REXLOG_INFO("[native] host import probe ({} MB at guest {:08X}): imported in {:.2f} ms (memory type {}{}{}; pages "
                "touched in {:.2f} ms){}{}; GPU copy of 1 MB {}{:.2f} ms: {}",
                bytes >> 20, address, Ns(t1, t2) / 1e6, type,
                ((types.host_coherent >> type) & 1) ? ", coherent" : "",
                ((types.host_cached >> type) & 1) ? ", CPU-cached" : "", Ns(t0, t1) / 1e6,
                failed ? "; FAILED at " : "", failed ? failed : "", failed ? "not made, " : "in ", ms_gpu,
                failed ? "no comparison"
                       : match ? "the GPU read the same bytes as the CPU"
                               : fmt::format("DIFFERENT from byte {}", first_difference));
    if (fence != VK_NULL_HANDLE) dfn_.vkDestroyFence(device_, fence, nullptr);
    if (pool != VK_NULL_HANDLE) dfn_.vkDestroyCommandPool(device_, pool, nullptr);
    if (mapped) dfn_.vkUnmapMemory(device_, target_memory);
    if (target != VK_NULL_HANDLE) dfn_.vkDestroyBuffer(device_, target, nullptr);
    if (target_memory != VK_NULL_HANDLE) dfn_.vkFreeMemory(device_, target_memory, nullptr);
    if (source != VK_NULL_HANDLE) dfn_.vkDestroyBuffer(device_, source, nullptr);
    dfn_.vkFreeMemory(device_, imported, nullptr);
    (void)sum;
  }

  // bo2zm_native_lazy_composite (see the cvar's comment). PM4 ring thread only.
  // It applies to the (address, source) pair that the diagnostic's watching has seen for kCompositeALook
  // writes without any read after the source was written again. Until then, it copies as always (WATCHING).
  bool CompositeApplicable(uint32_t address, uint32_t source) {
    if (composite_off_ || !REXCVAR_GET(bo2zm_native_lazy_composite)) {
      return false;
    }
    for (const DiagWatched& v : diag_watched_) {
      if (v.address != address || v.source != source) {
        continue;
      }
      if (v.late != 0 || v.writes < kCompositeALook) {
        return false;
      }
      if (composite_applying_ != address) {
        composite_applying_ = address;
        BO2ZM_REPORT_RING("[native] C2 lazy composite: {:08X} from {:03X}/{}: {} writes watched, {} read before their "
                             "source was written again and 0 after: APPLYING (the copy is postponed; it is recorded "
                             "before the first draw that reads it and dropped when its source is written)",
                             address, source & 0xFFF, source >> 12, v.writes, v.before);
      }
      return true;
    }
    return false;
  }

  // From Copy's 1 to 1 path: true if the copy is deferred (not recorded now).
  bool PostponeComposite(uint32_t address, OwnedImage& target, const Resolved& resolved, const VkImageCopy& copy,
                        uint32_t source) {
    if (composite_has_ || REXCVAR_GET(bo2zm_native_diag_resolved) || REXCVAR_GET(bo2zm_native_swap_color)) {
      return false;
    }
    if (copy.srcOffset.x != 0 || copy.srcOffset.y != 0 || copy.dstOffset.x != 0 || copy.dstOffset.y != 0 ||
        copy.extent.width != resolved.image.width || copy.extent.height != resolved.image.height ||
        target.format != kFormatColor || resolved.image.format != kFormatColor) {
      return false;
    }
    // The small ones are read back for the guest (ReadResolved): never those.
    const int32_t texels_read = REXCVAR_GET(bo2zm_native_read_resolved_texels);
    if (uint64_t(copy.extent.width) * copy.extent.height <= uint64_t(std::max<int32_t>(texels_read, 0))) {
      return false;
    }
    if (!CompositeApplicable(address, source)) {
      return false;
    }
    composite_.address = address;
    composite_.source = source;
    composite_.target = &target;
    composite_.source_vk = target.image;
    composite_.texture_vk = resolved.image.image;
    composite_.copy = copy;
    composite_has_ = true;
    ResolvedWritten(address);  // new contents (deferred)
    ++composite_postponed_;
    return true;
  }

  // Records the deferred copy now (outside a pass). Exact: its source has not been touched since it was
  // deferred.
  void RecordComposite() {
    if (!composite_has_) {
      return;
    }
    composite_has_ = false;
    const PendingComposite p = composite_;
    const auto r = resolved_.find(p.address);
    if (r == resolved_.end() || r->second.image.image != p.texture_vk || !p.target ||
        p.target->image != p.source_vk || !copy_image_ || !Record()) {
      composite_expired_ = p.address;
      CompositeMismatch(p.address, "the postponed copy can no longer be recorded (the source or texture image "
                                       "changed)");
      return;
    }
    if (draws_) {
      draws_->FinishPass();  // it can arrive from a draw: the copy goes outside the pass
    }
    MarkGpu(kGpuCopies);
    copy_image_(commands_job_, p.source_vk, VK_IMAGE_LAYOUT_GENERAL, p.texture_vk, VK_IMAGE_LAYOUT_GENERAL, 1,
                   &p.copy);
    ++copies_;
    AnnotateCopy(p.copy.extent.width, p.copy.extent.height);
    ResolvedWritten(p.address, uint64_t(p.copy.extent.width) * p.copy.extent.height);
  }

  // Its source is about to be written and nobody has read it: it is dropped (or recorded, if the guard
  // turned it off). The texture is left without it until the next whole write; if someone samples it
  // before that, the guard fires.
  void CompositeBeforeWriteSource() {
    if (!composite_has_) {
      return;
    }
    if (composite_off_) {
      RecordComposite();
      ++composite_recorded_others_;
      return;
    }
    composite_has_ = false;
    composite_expired_ = composite_.address;
    composite_pixels_saved_ += uint64_t(composite_.copy.extent.width) * composite_.copy.extent.height;
    ++composite_dropped_source_;
  }

  // Before each draw with a deferred copy or a stale texture: if the draw samples it, the deferred copy is
  // recorded first (exact) and the stale one trips the guard; if it draws into its source, the deferred
  // copy is dropped.
  void CompositeBeforeDraw(const DrawRequest& p) {
    if (!p.ps || !p.registers) {
      return;  // without a PS, no color is sampled or written
    }
    const uint32_t* r = p.registers;
    for (const SamplerShader& s : p.ps->samplers) {
      if (s.reg_index >= 16) {
        continue;
      }
      const uint32_t* f = r + kDiagRegFetch + uint32_t(s.reg_index) * 6;
      if ((f[0] & 0x3) != uint32_t(xenos::FetchConstantType::kTexture) ||
          ((f[5] >> 9) & 0x3) != uint32_t(xenos::DataDimension::k2DOrStacked)) {
        continue;
      }
      const uint32_t base = ((f[1] >> 12) << 12) & 0x1FFFFFFF;
      if (composite_has_ && base == composite_.address) {
        RecordComposite();
        ++composite_recorded_read_;
      } else if (composite_expired_ != 0 && base == composite_expired_) {
        ++composite_reads_late_;
        CompositeMismatch(base, "a draw samples it after its copy was dropped (it sees the scene from before the "
                                  "composite)");
        composite_expired_ = 0;
      }
    }
    if (composite_has_ && DrawWritesIn(r, composite_.source)) {
      CompositeBeforeWriteSource();
    }
  }

  // Before each resolve to a texture (from Copy, before copying anything): another write to the deferred
  // address. If it covers it entirely, the deferred copy is unnecessary (nobody has read it: exact);
  // otherwise it is recorded first.
  void CompositeBeforeWriteTexture(uint32_t address, bool whole) {
    if (composite_has_ && composite_.address == address) {
      if (whole) {
        composite_has_ = false;
        composite_pixels_saved_ += uint64_t(composite_.copy.extent.width) * composite_.copy.extent.height;
        ++composite_replaced_;
      } else {
        RecordComposite();
        ++composite_recorded_others_;
      }
    }
    if (whole && composite_expired_ == address) {
      composite_expired_ = 0;  // whole new content: it no longer misses the dropped copy
    }
  }

  // An image that is destroyed (Destroy) cannot stay in the deferred copy.
  void CompositeOnDestroy(const OwnedImage& image) {
    if (!composite_has_ || image.image == VK_NULL_HANDLE) {
      return;
    }
    if (image.image == composite_.source_vk) {
      composite_has_ = false;  // without its source there is no copy to make: the texture becomes stale
      composite_expired_ = composite_.address;
    } else if (image.image == composite_.texture_vk) {
      composite_has_ = false;  // the texture is rebuilt: its content is lost just as before
    }
  }

  void CompositeMismatch(uint32_t address, const char* reason) {
    ++composite_mismatches_;
    if (composite_off_) {
      return;
    }
    composite_off_ = true;
    REXLOG_ERROR("[native] C2 lazy composite: MISMATCH at {:08X}: {}. Off for the rest of the session: always copied, "
                 "as before",
                 address, reason);
  }

  // With the diagnostic's summary (every bo2zm_native_diag_readers_s seconds).
  void CompositeReport() {
    if (!composite_postponed_ && !composite_mismatches_) {
      return;
    }
    const uint64_t frames = presented_ - composite_presented_previous_;
    const double mp = double(composite_pixels_saved_ - composite_pixels_previous_) / 1e6;
    const double per_frame = frames ? mp / double(frames) : 0.0;
    BO2ZM_REPORT_RING("[native] C2 lazy composite: {} copies postponed since boot; {} recorded before a "
                         "draw that reads it (exact) and {} before another write; {} dropped when their source was "
                         "written and {} when another whole one covered it: {:.2f} Mpixels per frame not copied "
                         "(~{:.2f} ms real); late reads {}{}",
                         composite_postponed_, composite_recorded_read_, composite_recorded_others_,
                         composite_dropped_source_, composite_replaced_, per_frame, per_frame * 0.60,
                         composite_reads_late_,
                         composite_off_ ? " *** OFF BY THE GUARD ***" : " (0 = the image is the same)");
    composite_pixels_previous_ = composite_pixels_saved_;
    composite_presented_previous_ = presented_;
  }

  // bo2zm_native_diag_readers_s (see the cvar's comment). PM4 ring thread only.
  // On each Swap, with the front buffer it draws: opens the window (two whole frames, Swap to Swap) or closes
  // it.
  void DiagReadersOnPresent(uint32_t front) {
    if (diag_window_) {
      if (resolved_.count(front)) {
        DiagRead(front, kReaderSwap, 0, 0);
      }
      if (++diag_frame_ >= 2) {
        DiagReadersReport();
        diag_window_ = false;
        diag_writes_.clear();
      }
      return;
    }
    const int32_t every = REXCVAR_GET(bo2zm_native_diag_readers_s);
    if (every <= 0) {
      diag_watched_.clear();  // turned off at run time: nothing is watched either
      return;
    }
    const auto now = std::chrono::steady_clock::now();
    if (now - diag_readers_last_ < std::chrono::seconds(every)) {
      return;
    }
    diag_readers_last_ = now;
    diag_window_ = true;
    diag_frame_ = 0;
    diag_writes_.clear();
  }

  // A logical write to the resolved texture at `address`: the resolve the game requests (color or depth).
  void DiagWrite(uint32_t address, uint32_t source, uint32_t pitch, int32_t x0, int32_t y0,
                     uint32_t requested_width, uint32_t requested_height, uint32_t width, uint32_t height, uint64_t draws,
                     bool depth) {
    const uint32_t of = (source & 0xFFF) | (pitch << 12);
    for (DiagWatched& v : diag_watched_) {
      if (v.address == address) {
        v.pending = of == v.source;  // from here on, whoever reads it needs the watched copy
        v.source_written = false;
        v.writes += v.pending ? 1 : 0;
      }
    }
    if (!diag_window_) {
      return;
    }
    std::vector<DiagWriteEntry>& list = diag_writes_[address];
    if (list.size() >= kDiagMaxWrites) {
      ++diag_writes_lost_;
      return;
    }
    DiagWriteEntry e;
    e.frame = int32_t(diag_frame_);
    e.order = 1;
    for (const DiagWriteEntry& other : list) {
      e.order += other.frame == e.frame ? 1 : 0;
    }
    e.source = of;
    e.requested_width = requested_width;
    e.requested_height = requested_height;
    e.width = width;
    e.height = height;
    e.draws = draws;
    e.depth = depth;
    e.whole = x0 == 0 && y0 == 0 && requested_width >= width && requested_height >= height;
    list.push_back(std::move(e));
  }

  // A read of `address` in the window: it goes to its last write (or to the one before the window).
  void DiagRead(uint32_t address, uint32_t ps, uint32_t reg_index, uint32_t target) {
    std::vector<DiagWriteEntry>& list = diag_writes_[address];
    if (list.empty()) {
      list.emplace_back();  // what it already held before the window (frame -1)
    }
    DiagWriteEntry& e = list.back();
    for (DiagReader& l : e.readers) {
      if (l.ps == ps && l.reg_index == reg_index && l.target == target) {
        ++l.times;
        return;
      }
    }
    if (e.readers.size() >= kDiagMaxReaders) {
      ++e.others;
      return;
    }
    e.readers.push_back(DiagReader{ps, reg_index, target, 1});
  }

  // The resolved textures the PS of this draw samples, read from its fetch constants the way VulkanDraws
  // reads them (sampler registers 0-15; 2D by their address and cubemaps by their 6 faces), bypassing the
  // caches.
  void DiagReadsDraw(const DrawRequest& p) {
    if (!p.ps || !p.registers) {
      return;
    }
    const uint32_t* r = p.registers;
    const uint32_t target = (r[kDiagRegColorInfo] & 0xFFF) | ((r[kDiagRegSurfaceInfo] & 0x3FFF) << 12);
    for (const SamplerShader& s : p.ps->samplers) {
      if (s.reg_index >= 16) {
        continue;
      }
      const uint32_t* f = r + kDiagRegFetch + uint32_t(s.reg_index) * 6;
      if ((f[0] & 0x3) != uint32_t(xenos::FetchConstantType::kTexture)) {
        continue;
      }
      const uint32_t base = ((f[1] >> 12) << 12) & 0x1FFFFFFF;
      const uint32_t dimension = (f[5] >> 9) & 0x3;
      if (dimension == uint32_t(xenos::DataDimension::k2DOrStacked)) {
        if (resolved_.count(base)) {
          DiagRead(base, p.ps->number, s.reg_index, target);
          WatchRead(base, p.ps->number);
        }
      } else if (dimension == uint32_t(xenos::DataDimension::kCube)) {
        // The faces of a resolved cubemap are consecutive, as VulkanDraws looks for them (8888: 4 bytes per
        // texel, rows and columns aligned to 32 and each face to 4 KB).
        const uint32_t height = ((f[2] >> 13) & 0x1FFF) + 1;
        const uint32_t pitch = std::max<uint32_t>(((f[0] >> 22) & 0x1FF) << 5, 1);
        const uint64_t stride =
            (uint64_t((pitch + 31) & ~uint32_t(31)) * 4 * ((height + 31) & ~uint32_t(31)) + 4095) & ~uint64_t(4095);
        for (uint32_t c = 0; c < 6; ++c) {
          const uint32_t face = uint32_t((uint64_t(base) + c * stride) & 0x1FFFFFFF);
          if (resolved_.count(face)) {
            DiagRead(face, p.ps->number, s.reg_index, target);
          }
        }
      }
    }
    DiagSourceWrittenPerDraw(r);  // after its reads: the draw reads its textures before writing
  }

  // Outside the window, with something watched: only the 2D address of each sampler is compared with the
  // watched ones.
  void DiagWatch(const DrawRequest& p) {
    if (!p.ps || !p.registers) {
      return;
    }
    const uint32_t* r = p.registers;
    for (const SamplerShader& s : p.ps->samplers) {
      if (s.reg_index >= 16) {
        continue;
      }
      const uint32_t* f = r + kDiagRegFetch + uint32_t(s.reg_index) * 6;
      if ((f[0] & 0x3) == uint32_t(xenos::FetchConstantType::kTexture) &&
          ((f[5] >> 9) & 0x3) == uint32_t(xenos::DataDimension::k2DOrStacked)) {
        WatchRead(((f[1] >> 12) << 12) & 0x1FFFFFFF, p.ps->number);
      }
    }
    DiagSourceWrittenPerDraw(r);
  }

  // A read of a watched address after its watched write. Before the source is written again, a deferred
  // copy would cover it (it would be recorded right before that draw); afterwards, dropping the copy when
  // the source is written would not be exact.
  void WatchRead(uint32_t address, uint32_t ps) {
    for (DiagWatched& v : diag_watched_) {
      if (v.address != address || !v.pending) {
        continue;
      }
      if (!v.source_written) {
        if (v.before++ == 0) {
          v.ps_before = ps;
          BO2ZM_REPORT_RING("[native] C2 readers: {:08X}, written from {:03X}/{}, is read by PS n{} BEFORE "
                               "its source is written again (after {} watched writes): a postponed copy covers it",
                               address, v.source & 0xFFF, v.source >> 12, ps, v.writes);
        }
      } else if (v.late++ == 0) {
        v.ps_late = ps;
        BO2ZM_REPORT_RING("[native] C2 readers: {:08X}, written from {:03X}/{}, is read by PS n{} AFTER its "
                             "source is written (after {} watched writes): dropping that copy when the source is "
                             "written would NOT be exact",
                             address, v.source & 0xFFF, v.source >> 12, ps, v.writes);
      }
    }
  }

  // A draw (or a clear) on render target `target` (EDRAM base | pitch << 12): if it is the source of a
  // pending watched write, its content is no longer in the source.
  void DiagSourceWritten(uint32_t target) {
    for (DiagWatched& v : diag_watched_) {
      if (v.pending && !v.source_written && v.source == target) {
        v.source_written = true;
      }
    }
  }

  // Whether a draw writes to `target`: its four color render targets with their write mask. Over-reporting
  // is harmless (it is treated as written earlier); under-reporting is not.
  static bool DrawWritesIn(const uint32_t* r, uint32_t target) {
    const uint32_t mask = r[kDiagRegColorMask];
    const uint32_t pitch = (r[kDiagRegSurfaceInfo] & 0x3FFF) << 12;
    for (uint32_t i = 0; i < 4; ++i) {
      if (((mask >> (4 * i)) & 0xF) != 0 && ((r[kDiagRegsColorInfo[i]] & 0xFFF) | pitch) == target) {
        return true;
      }
    }
    return false;
  }

  void DiagSourceWrittenPerDraw(const uint32_t* r) {
    for (DiagWatched& v : diag_watched_) {
      if (v.pending && !v.source_written && DrawWritesIn(r, v.source)) {
        v.source_written = true;
      }
    }
  }

  // When the window closes: one line per address (if its pattern has changed) and the summary line.
  void DiagReadersReport() {
    ++diag_windows_;
    std::vector<uint32_t> addresses;
    addresses.reserve(diag_writes_.size());
    for (const auto& [address, list] : diag_writes_) {
      addresses.push_back(address);
    }
    std::sort(addresses.begin(), addresses.end());
    const uint64_t texels_cpu = uint64_t(std::max<int32_t>(REXCVAR_GET(bo2zm_native_read_resolved_texels), 0));
    std::string excess;
    uint32_t writes = 0, new_ones = 0, equal = 0;
    for (const uint32_t address : addresses) {
      const std::vector<DiagWriteEntry>& list = diag_writes_[address];
      const auto r = resolved_.find(address);
      const uint32_t width = r != resolved_.end() ? r->second.image.width : 0;
      const uint32_t height = r != resolved_.end() ? r->second.image.height : 0;
      // The small ones are also copied to guest memory and read by the CPU (the exposure): they are never
      // unnecessary.
      const bool cpu = width && uint64_t(width) * height <= texels_cpu;
      std::string line;
      uint64_t signature = address;
      for (size_t i = 0; i < list.size(); ++i) {
        const DiagWriteEntry& e = list[i];
        if (e.frame < 0) {
          line += " [before]";
        } else {
          ++writes;
          const bool blit = e.requested_width * 4 >= e.width * 5 || e.requested_height * 4 >= e.height * 5;
          line += fmt::format(" [{}.{}] {} {}x{} from {:03X}/{}{} ({} draws before)", e.frame, e.order,
                               e.depth ? "depth" : (blit ? "blit" : "1:1"), e.requested_width,
                               e.requested_height, e.source & 0xFFF, e.source >> 12, e.whole ? "" : ", partial",
                               e.draws);
        }
        signature = (signature ^ (uint64_t(uint32_t(e.frame + 1)) << 48 | uint64_t(e.order) << 40 | e.source)) *
                0x100000001B3ull;
        if (e.readers.empty()) {
          const bool next = i + 1 < list.size();
          if (cpu) {
            line += " -> no draw (the CPU reads it)";
          } else if (e.frame >= 0 && next && list[i + 1].whole) {
            line += " -> NOBODY before the next one, which covers it whole: SPARE";
            excess += fmt::format(" {:08X} [{}.{}] from {:03X}/{}", address, e.frame, e.order,
                                  e.source & 0xFFF, e.source >> 12);
            signature ^= 0x5A5Au;
            AddWatched(address, e.source);
          } else {
            line += next ? " -> nobody before the next one (which does not cover it whole)"
                               : " -> nobody until the end of the window";
          }
        } else {
          line += " ->";
          for (const DiagReader& l : e.readers) {
            if (l.ps == kReaderSwap) {
              line += fmt::format(" Swap x{}", l.times);
            } else {
              line += fmt::format(" PS n{} s{} x{} at {:03X}/{}", l.ps, l.reg_index, l.times, l.target & 0xFFF,
                                   l.target >> 12);
            }
            signature = (signature ^ (uint64_t(l.ps) << 32 | uint64_t(l.reg_index) << 26 | (l.target & 0x3FFFFFF))) *
                    0x100000001B3ull;
          }
          if (e.others) {
            line += fmt::format(" (and {} more readers)", e.others);
          }
        }
        line += ";";
      }
      uint64_t& previous = diag_signatures_[address];
      if (previous == signature && diag_windows_ > 3) {
        ++equal;  // the same pattern as the last time it was written: not repeated
        continue;
      }
      previous = signature;
      ++new_ones;
      BO2ZM_REPORT_RING("[native] C2 readers (window {}): {:08X} {}x{}:{}", diag_windows_, address,
                           width, height, line);
    }
    std::string watched;
    for (const DiagWatched& v : diag_watched_) {
      watched += fmt::format(" {:08X} from {:03X}/{}: {} writes; {} reads before the source is written again{} and {} "
                               "after{};",
                               v.address, v.source & 0xFFF, v.source >> 12, v.writes, v.before,
                               v.before ? fmt::format(" (the first, PS n{})", v.ps_before) : std::string(),
                               v.late,
                               v.late ? fmt::format(" (the first, PS n{}): dropping it would NOT be exact", v.ps_late)
                                         : std::string(v.writes ? " (dropping it when the source is written would be "
                                                                  "exact)"
                                                                    : ""));
    }
    BO2ZM_REPORT_RING("[native] C2 readers of the resolved textures (window {}, 2 frames every {} s): {} "
                         "addresses and {} writes; {} lines written and {} equal to the last one written (not "
                         "repeated){}; writes nobody reads before another that covers them whole:{} | watched in every "
                         "frame:{}",
                         diag_windows_, REXCVAR_GET(bo2zm_native_diag_readers_s), addresses.size(), writes,
                         new_ones, equal,
                         diag_writes_lost_
                             ? fmt::format(" ({} writes not recorded because of the cap of {} per address)",
                                           diag_writes_lost_, kDiagMaxWrites)
                             : std::string(),
                         excess.empty() ? std::string(" none") : excess,
                         watched.empty() ? std::string(" none") : watched);
    CompositeReport();  // bo2zm_native_lazy_composite
  }

  // A write that is unnecessary in a window becomes watched in every frame (at most kDiagMaxWatched).
  void AddWatched(uint32_t address, uint32_t source) {
    for (const DiagWatched& v : diag_watched_) {
      if (v.address == address && v.source == source) {
        return;
      }
    }
    if (diag_watched_.size() < kDiagMaxWatched) {
      DiagWatched v;
      v.address = address;
      v.source = source;
      diag_watched_.push_back(v);
    }
  }

  // Per-draw diagnostic window, one whole frame every N seconds. It is decided
  // in the Swap because the upload buffer rotates several times per frame.
  void OpenWindowDiagnostic() {
    const int32_t every = REXCVAR_GET(bo2zm_native_per_draw_stats_s);
    if (every <= 0) {
      window_diagnostic_ = false;
      return;
    }
    const auto now = std::chrono::steady_clock::now();
    if (window_diagnostic_) {
      window_diagnostic_ = false;  // that frame was already measured
      return;
    }
    if (std::chrono::duration_cast<std::chrono::seconds>(now - last_window_).count() >= every) {
      last_window_ = now;
      window_diagnostic_ = true;
      window_read_ = false;
    }
  }

  void Stats(uint64_t& copies, uint64_t& clears, uint64_t& presented,
                    uint64_t& rejections) const override {
    copies = copies_;
    clears = clears_;
    presented = presented_;
    rejections = rejections_;
  }

  // Depth copies (the only ones that force the depth tile to be stored).
  uint64_t CopiesDepth() const override { return copies_depth_; }
  uint64_t ClearsDepth() const override { return clears_depth_; }

  void TimeGpu(uint64_t& nanoseconds, uint64_t& jobs) const override {
    nanoseconds = gpu_ns_;
    jobs = gpu_jobs_;
  }

  uint64_t JobsSubmitted() const override { return submits_; }

  uint64_t MarkGpuFinalNs() const override { return uint64_t(double(last_mark_gpu_) * period_mark_ns_); }
  uint64_t JobsMarksPrecise() const override { return gpu_jobs_precise_; }

  void WaitsGpu(uint64_t& times, uint64_t& nanoseconds) const override {
    times = waits_gpu_;
    nanoseconds = ns_waits_gpu_;
  }

  void DurationJobsGpu(uint64_t& times, uint64_t& nanoseconds) const override {
    times = jobs_gpu_;
    nanoseconds = ns_job_gpu_;
  }

  void CostRecord(uint64_t cost[6]) const override {
    cost[0] = recordings_;
    cost[1] = ns_record_;
    cost[2] = ns_reset_pools_;
    cost[3] = reads_written_;
    cost[4] = texels_written_;
    cost[5] = ns_write_reads_;
  }

  // C2 report: waits of the ring for the GPU in CompleteOne.
  uint64_t waits_gpu_ = 0;
  uint64_t ns_waits_gpu_ = 0;
  uint64_t jobs_gpu_ = 0;     // work units timed with the wall clock
  uint64_t ns_job_gpu_ = 0;   // and how long they took from submit to fence
  // C2 report: cost of Record and of the readbacks.
  uint64_t recordings_ = 0;
  uint64_t ns_record_ = 0;
  uint64_t ns_reset_pools_ = 0;
  uint64_t reads_written_ = 0;
  uint64_t texels_written_ = 0;
  uint64_t ns_write_reads_ = 0;

  void TimeGpuPerCategory(
      std::array<uint64_t, kGpuCategories>& nanoseconds) const override {
    nanoseconds = gpu_categories_ns_;
  }

  void StatsPipeline(std::array<uint64_t, kGpuCategories>& fragments,
                            std::array<uint64_t, kGpuCategories>& vertices,
                            std::array<uint64_t, kGpuCategories>& primitives) const override {
    fragments = fragments_category_;
    vertices = vertices_category_;
    primitives = primitives_category_;
  }

  void CopiesPerSize(std::array<uint64_t, 4>& copies, std::array<uint64_t, 4>& pixels) const override {
    copies = copies_bucket_;
    pixels = pixels_bucket_;
  }

  void StatsPerShader(std::vector<uint64_t>& fragments, std::vector<uint64_t>& draws,
                             uint64_t& frames) const override {
    fragments = fragments_per_shader_;
    draws = draws_per_shader_;
    frames = frames_diagnostic_;
  }

  void CostPresent(uint64_t cost[12]) const override {
    const uint64_t values[12] = {waits_output_,   ns_wait_output_,  submits_job_,  ns_latch_job_,
                                  ns_submit_job_, submits_output_,     ns_latch_output_, ns_submit_output_,
                                  refreshes_,        ns_before_call_, ns_in_call_,     ns_after_call_};
    std::copy(std::begin(values), std::end(values), cost);
  }

  void IntervalsBetweenSwaps(std::array<uint64_t, kBucketsSwap>& buckets, uint64_t& overlaps,
                            double& worst_ms) const override {
    buckets = buckets_swap_;
    worst_ms = worst_swap_ms_;
    worst_swap_ms_ = 0.0;  // reset on every report: what matters is the worst of this interval
    overlaps = overlaps_gpu_;
  }

  bool Draw(const DrawRequest& request) override {
    if (composite_has_ || composite_expired_ != 0) {
      CompositeBeforeDraw(request);  // bo2zm_native_lazy_composite
    }
    if (diag_window_) {
      DiagReadsDraw(request);  // bo2zm_native_diag_readers_s
    } else if (!diag_watched_.empty()) {
      DiagWatch(request);
    }
    return draws_ && draws_->Draw(request);
  }

  DrawStats DrawsStats() const override {
    return draws_ ? draws_->Stats() : DrawStats{};
  }

  void WaitUploads() override {
    if (draws_) {
      draws_->WaitUploads();
    }
  }

  size_t CopiesPending() const override {  // fence measurement only
    return draws_ ? draws_->CopiesPending() : 0;
  }

  void Precompile() override {
    if (draws_) {
      draws_->Precompile();
    }
  }

  // --- TargetContext (what the draws use) ---------------------------------------

  VkCommandBuffer CommandsJob() override {
    return Record() ? commands_job_ : VK_NULL_HANDLE;
  }

  // The upload buffer (texture uploads, clears, image copies, the GPU skinning dispatch) is submitted
  // just before the job buffer, and nothing ordered it against either side: each texture upload's own barrier waits
  // for nothing (TOP_OF_PIPE), and no barrier made the uploads visible to the draws. The compute path of the
  // driver's copies ran in order with the draws anyway; NVK's copy engine (bo2zm_native_nvk_copy_engine bit 0) runs
  // beside the 3D engine, so with the GPU slow (handheld on battery, ~600 MHz) an upload could change a texture that
  // the previous frame's draws were still sampling, or the draws sample one the copy had not finished: shapes
  // dropping out for a frame (on a Switch Lite, Buried's light shafts jumped 10-20 in brightness for one frame with
  // the copy engine on for uploads, and never with it off or used for image copies only). One full
  // barrier when the buffer begins (after everything submitted before it) and one when it ends (before the job).
  void UploadBarrier() {
    VkMemoryBarrier barrier{};
    barrier.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER;
    barrier.srcAccessMask = VK_ACCESS_MEMORY_WRITE_BIT;
    barrier.dstAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT;
    dfn_.vkCmdPipelineBarrier(commands_upload_, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
                              VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 1, &barrier, 0, nullptr, 0, nullptr);
  }

  VkCommandBuffer CommandsUpload() override {
    if (!Record()) {
      return VK_NULL_HANDLE;
    }
    if (!recording_upload_) {
      VkCommandBufferBeginInfo start{};
      start.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
      start.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
      if (dfn_.vkBeginCommandBuffer(commands_upload_, &start) != VK_SUCCESS) {
        return VK_NULL_HANDLE;
      }
      recording_upload_ = true;
      UploadBarrier();  // what this buffer writes waits for the earlier submissions' work (see UploadBarrier)
    }
    return commands_upload_;
  }

  uint64_t GenerationCommands() const override { return generation_commands_; }

  NativeImage* TargetColor(uint32_t base, uint32_t format, uint32_t pitch) override {
    OwnedImage* image = GetTarget(base, format, pitch);
    if (!image || !Record()) {
      return nullptr;
    }
    Prepare(*image);
    BeforeWriteColor(*image);  // bo2zm_native_lazy_front
    // Mandatory since color is also swapped. Here a pass starts that draws on top; if the content went away
    // in a swap and nobody has cleared it, it has to be brought back. Without this, the first frame that
    // draws without clearing shows the previous frame.
    RestoreContent(*image);
    return image;
  }

  void BeginColorPass(NativeImage& image, const uint32_t* r, const ShaderInput* ps) override {
    const auto found = front_compositions_.find(&image);
    if (found == front_compositions_.end() || !r) {
      if (FrontTraceLine()) {
        REXLOG_INFO("[front-trace] pass target {:p} {}x{} format {} frame {} owner {} registers {} PS n{}",
                    static_cast<const void*>(&image), image.width, image.height, uint32_t(image.format),
                    presented_, found != front_compositions_.end(), r != nullptr, ps ? int(ps->number) : -1);
      }
      return;
    }
    FrontComposition& front = found->second;
    const bool already_written = front.write_frame == presented_;
    if (already_written && !front_trace_active_) return;
    namespace g = rex::graphics;
    FrontWriteRect rect{0, 0, float(front.width), float(front.height)};
    const uint32_t vte = r[g::XE_GPU_REG_PA_CL_VTE_CNTL];
    const bool clipped = !(r[g::XE_GPU_REG_PA_CL_CLIP_CNTL] & (1u << 16));
    // The viewport is a bounds sanity check, not the submitted geometry's coverage.
    // Unknown or pretransformed modes use the learned presentation extent.
    if ((vte & 0xF) == 0xF && clipped) {
      const auto value = [](uint32_t bits) {
        float result;
        std::memcpy(&result, &bits, sizeof(result));
        return result;
      };
      const float sx = std::abs(value(r[g::XE_GPU_REG_PA_CL_VPORT_XSCALE]));
      const float sy = std::abs(value(r[g::XE_GPU_REG_PA_CL_VPORT_YSCALE]));
      float cx = value(r[g::XE_GPU_REG_PA_CL_VPORT_XOFFSET]);
      float cy = value(r[g::XE_GPU_REG_PA_CL_VPORT_YOFFSET]);
      const int32_t wx = ExtenderSign15(r[g::XE_GPU_REG_PA_SC_WINDOW_OFFSET] & 0x7FFF);
      const int32_t wy = ExtenderSign15((r[g::XE_GPU_REG_PA_SC_WINDOW_OFFSET] >> 16) & 0x7FFF);
      if (r[g::XE_GPU_REG_PA_SU_SC_MODE_CNTL] & (1u << 16)) {
        cx += float(wx);
        cy += float(wy);
      }
      // Scissor clipping does not establish a composition boundary.
      if (std::isfinite(cx) && std::isfinite(cy) && std::isfinite(sx) && std::isfinite(sy) && sx > 0 && sy > 0) {
        rect.left = std::max(0.0f, cx - sx);
        rect.top = std::max(0.0f, cy - sy);
        rect.right = std::min(float(front.width), cx + sx);
        rect.bottom = std::min(float(front.height), cy + sy);
      }
    }
    bool resolved_input = false;
    if (ps) {
      for (const auto& sampler : ps->samplers) {
        if (sampler.reg_index >= 16) continue;
        const uint32_t* fetch = r + g::XE_GPU_REG_SHADER_CONSTANT_FETCH_00_0 + sampler.reg_index * 6;
        if ((fetch[0] & 3) != uint32_t(xenos::FetchConstantType::kTexture)) continue;
        const auto input = resolved_.find(fetch[1] & 0x1FFFF000u);
        if (FrontTraceLine()) {
          REXLOG_INFO("[front-trace] input target {:p} PS n{} t{} address {:08X} resolved {} prepared {} "
                      "depth {} presented {}",
                      static_cast<const void*>(&image), ps->number, sampler.reg_index, fetch[1] & 0x1FFFF000u,
                      input != resolved_.end(), input != resolved_.end() && input->second.image.prepared,
                      input != resolved_.end() && IsDepthFormat(input->second.image.format),
                      input != resolved_.end() && fronts_presented_.count(input->first) != 0);
        }
        if (input != resolved_.end() && input->second.image.prepared &&
            !IsDepthFormat(input->second.image.format) && !fronts_presented_.count(input->first)) {
          resolved_input = true;
          break;
        }
      }
    }
    const uint32_t mask = r[g::XE_GPU_REG_RB_COLOR_MASK];
    const uint32_t blend = r[g::XE_GPU_REG_RB_BLENDCONTROL0];
    const uint32_t color_control = r[g::XE_GPU_REG_RB_COLORCONTROL];
    const bool opaque_rgba = FrontOpaqueRGBAWrite(mask, blend, color_control,
                                                 ps && (ps->outputs & 1), ps && ps->discards);
    const uint64_t drawn = draws_ ? draws_->Drawn() : 0;
    const auto clear = state_target_.find(&image);
    const bool clear_clean = clear != state_target_.end() && clear->second.clear_clean;
    const uint64_t clear_draw = clear != state_target_.end() ? clear->second.draws_on_clear : 0;
    const bool known_clear = draws_ && FrontKnownClearValid(clear_clean, clear_draw, drawn);
    const bool eligible = !front_partial_clear_failed_ && REXCVAR_GET(bo2zm_native_front_partial_clear) &&
                          IsSwapFrontOnly(front.address) && !front_composition_sampled_.count(front.address) &&
                          image.format == kFormatColor &&
                          front.width <= image.width && front.height <= image.height;
    if (FrontTraceLine()) {
      const bool provenance = IsSwapFrontOnly(front.address) && !front_composition_sampled_.count(front.address) &&
                              image.format == kFormatColor && front.width <= image.width && front.height <= image.height;
      REXLOG_INFO("[front-trace] pass target {:p} front {:08X} write_frame {} frame {} "
                  "decision {} enabled {} failed {} swap_only {} sampled {} rgba8 {} fits {} input {} "
                  "opaque_rgba {} known_clear {} "
                  "viewport [{},{})x[{},{}) vte {:08X} clip {:08X} scissor {:08X}/{:08X} PS n{}",
                  static_cast<const void*>(&image), front.address, front.write_frame, presented_,
                  FrontDecisionName(front.Inspect(presented_, provenance, rect, resolved_input, opaque_rgba, known_clear)),
                  REXCVAR_GET(bo2zm_native_front_partial_clear), front_partial_clear_failed_,
                  IsSwapFrontOnly(front.address), front_composition_sampled_.count(front.address) != 0,
                  image.format == kFormatColor, front.width <= image.width && front.height <= image.height,
                  resolved_input, opaque_rgba, known_clear, rect.left, rect.right, rect.top, rect.bottom, vte,
                  r[g::XE_GPU_REG_PA_CL_CLIP_CNTL], r[g::XE_GPU_REG_PA_SC_WINDOW_SCISSOR_TL],
                  r[g::XE_GPU_REG_PA_SC_WINDOW_SCISSOR_BR], ps ? int(ps->number) : -1);
    }
    if (FrontTraceLine()) {
      REXLOG_INFO("[front-trace] write-state target {:p} mask {:08X} blend {:08X} colorctl {:08X} "
                  "outputs {:X} discard {} clear_clean {} clear_draw {} draw {} clear_value {:016X}",
                  static_cast<const void*>(&image), mask, blend, color_control, ps ? ps->outputs : 0,
                  ps && ps->discards, clear_clean, clear_draw, drawn,
                  clear != state_target_.end() ? clear->second.value_clear : uint64_t(0));
    }
    if (already_written) return;
    if (!front.Begin(presented_, eligible, rect, resolved_input, opaque_rgba, known_clear)) return;
    // TargetColor has already materialized any older deferred resolve and restored lent contents.
    // Clear only the visible front region. Once per Swap, so later views and HUD passes survive.
    VkMemoryBarrier barrier{};
    barrier.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER;
    barrier.srcAccessMask = VK_ACCESS_MEMORY_WRITE_BIT;
    barrier.dstAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT;
    const auto order = [&]() {
      dfn_.vkCmdPipelineBarrier(commands_job_, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
                                VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 1, &barrier, 0, nullptr, 0, nullptr);
    };
    order();
    MarkGpu(kGpuClears);
    const VkClearValue black{};
    const VkRect2D area{{0, 0}, {front.width, front.height}};
    if (draws_ && draws_->ClearAttachmentInPass(commands_job_, image, VK_IMAGE_ASPECT_COLOR_BIT, black, area)) {
      order();
      ForgetClear(image);
      ++clears_;
      if (++front_partial_clears_ == 1) {
        REXLOG_INFO("[native] C2: partial front composition initialized black ({}x{}, front {:08X})",
                    front.width, front.height, front.address);
      }
    } else {
      // The first-write decision stays consumed: retrying later could erase a view already drawn.
      front_partial_clear_failed_ = true;
      REXLOG_ERROR("[native] C2: could not initialize partial front composition ({}x{}, front {:08X}); "
                   "automatic front initialization disabled, retaining contents",
                   front.width, front.height, front.address);
    }
  }

  NativeImage* TargetDepth(uint32_t base, uint32_t format, uint32_t pitch) override {
    DepthTarget* image = GetDepth(base, format, pitch);
    if (!image || !Record()) {
      return nullptr;
    }
    Prepare(*image);
    BeforeWriteDepth(*image);  // bo2zm_native_lazy_depth
    // A pass that draws on top starts here, so if the content went away in a swap and nobody has cleared it
    // since then, it has to be brought back.
    RestoreContent(*image);
    image->draw_frame = presented_;
    image->draw_order = ++depth_draw_order_;
    return image;
  }

  const NativeImage* TextureResolved(uint32_t address) override {
    const auto it = resolved_.find(address);
    // Watchdog of bo2zm_native_restore_by_swap. This is the only door through which draws reach a
    // resolved texture, so if someone requests one that is lent out (its content was given back to the render
    // target and it has not been resolved again) the lending is not safe and the previous frame's image would
    // be sampled. It must stay at 0 in the log.
    if (!lent_.empty() && lent_.count(address)) {
      ++lent_read_;
    }
    if (it == resolved_.end()) {
      return nullptr;
    }
    // And how many times each address is requested, for the per-render-target copy report. A resolved
    // texture that is copied every frame and never requested is a copy that is not needed. It is a ++ on the
    // entry that has already been looked up: it costs nothing even though this is called thousands of times
    // per frame.
    ++it->second.reads;
    if (address == bo2zm::reflection_demand::kAddress) {
      bo2zm::reflection_demand::AnnotateRead();  // reflection on demand
    }
    // bo2zm_native_lazy_depth. Who requests each depth texture; if its copy is deferred and this is
    // a real sampling, it is recorded now, before this draw.
    if (it->second.image.format == format_depth_) {
      AnnotateReadDepth(address);
    } else if (!fronts_presented_.empty() && fronts_presented_.count(address)) {
      AnnotateReadFront(address);  // bo2zm_native_lazy_front
    }
    return it->second.image.prepared ? &it->second.image : nullptr;
  }

  // A resolve that is not a bit copy. A full-screen rectangle with bo2zm_resolve_copy.frag
  // reads the render target texel, scales it by 2^exp_bias and packs it per the destination's number format into
  // the resolved image (its host format comes from FormatHostResolved). Render pass, pipeline and framebuffer
  // are made on first use and kept; the descriptor sets go round a ring long enough for the frames in flight.
  bool CopyConverted(OwnedImage& source, OwnedImage& target, int32_t x0, int32_t y0, uint32_t dx, uint32_t dy,
                       uint32_t width, uint32_t height, uint32_t info_target) {
    if (!width || !height || source.view == VK_NULL_HANDLE || target.view == VK_NULL_HANDLE ||
        IsDepthFormat(source.format) || IsDepthFormat(target.format)) {
      return false;
    }
    if (conv_fs_ == VK_NULL_HANDLE && !conv_failed_) {
      VkShaderModuleCreateInfo module_info{};
      module_info.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
      module_info.codeSize = sizeof(shaders::kSpirvBo2zmCopyResolve);
      module_info.pCode = shaders::kSpirvBo2zmCopyResolve;
      VkDescriptorPoolSize sizes[3] = {{VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, kConvSets},
                                         {VK_DESCRIPTOR_TYPE_SAMPLER, kConvSets},
                                         {VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, kConvSets}};
      VkDescriptorPoolCreateInfo info_pool{};
      info_pool.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
      info_pool.maxSets = kConvSets;
      info_pool.poolSizeCount = 3;
      info_pool.pPoolSizes = sizes;
      bool ok = dfn_.vkCreateShaderModule(device_, &module_info, nullptr, &conv_fs_) == VK_SUCCESS &&
                dfn_.vkCreateDescriptorPool(device_, &info_pool, nullptr, &conv_pool_) == VK_SUCCESS;
      for (uint32_t i = 0; ok && i < kConvSets; ++i) {
        VkDescriptorSetAllocateInfo reserve{};
        reserve.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
        reserve.descriptorPool = conv_pool_;
        reserve.descriptorSetCount = 1;
        reserve.pSetLayouts = &layout_descriptors_;
        ok = dfn_.vkAllocateDescriptorSets(device_, &reserve, &conv_sets_[i]) == VK_SUCCESS;
      }
      if (!ok) {
        conv_failed_ = true;
        REXLOG_ERROR("[native] BO2: could not prepare the converted copy of the resolves");
      }
    }
    if (conv_failed_) {
      return false;
    }
    // Render pass and pipeline for this destination format.
    VkRenderPass& pass = conv_passes_[target.format];
    if (pass == VK_NULL_HANDLE) {
      VkAttachmentDescription attachment{};
      attachment.format = target.format;
      attachment.samples = VK_SAMPLE_COUNT_1_BIT;
      attachment.loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
      attachment.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
      attachment.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
      attachment.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
      attachment.initialLayout = VK_IMAGE_LAYOUT_GENERAL;
      attachment.finalLayout = VK_IMAGE_LAYOUT_GENERAL;
      VkAttachmentReference reference{0, VK_IMAGE_LAYOUT_GENERAL};
      VkSubpassDescription subpass{};
      subpass.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
      subpass.colorAttachmentCount = 1;
      subpass.pColorAttachments = &reference;
      VkRenderPassCreateInfo info_rp{};
      info_rp.sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO;
      info_rp.attachmentCount = 1;
      info_rp.pAttachments = &attachment;
      info_rp.subpassCount = 1;
      info_rp.pSubpasses = &subpass;
      if (dfn_.vkCreateRenderPass(device_, &info_rp, nullptr, &pass) != VK_SUCCESS) {
        pass = VK_NULL_HANDLE;
        return false;
      }
    }
    VkPipeline& pipeline = conv_pipelines_[target.format];
    if (pipeline == VK_NULL_HANDLE) {
      pipeline = CreatePipelineOutput(conv_fs_, nullptr, pass);
      if (pipeline == VK_NULL_HANDLE) {
        return false;
      }
      REXLOG_INFO("[native] BO2: converted copy of resolves to format {}", uint32_t(target.format));
    }
    VkFramebuffer& framebuffer = conv_framebuffers_[target.view];
    if (framebuffer == VK_NULL_HANDLE) {
      VkFramebufferCreateInfo info_fb{};
      info_fb.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
      info_fb.renderPass = pass;
      info_fb.attachmentCount = 1;
      info_fb.pAttachments = &target.view;
      info_fb.width = target.width;
      info_fb.height = target.height;
      info_fb.layers = 1;
      if (dfn_.vkCreateFramebuffer(device_, &info_fb, nullptr, &framebuffer) != VK_SUCCESS) {
        framebuffer = VK_NULL_HANDLE;
        return false;
      }
    }
    const VkDescriptorSet set = conv_sets_[conv_next_++ % kConvSets];
    VkDescriptorImageInfo info_image{VK_NULL_HANDLE, source.view, VK_IMAGE_LAYOUT_GENERAL};
    VkWriteDescriptorSet write{};
    write.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    write.dstSet = set;
    write.dstBinding = 0;
    write.descriptorCount = 1;
    write.descriptorType = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
    write.pImageInfo = &info_image;
    dfn_.vkUpdateDescriptorSets(device_, 1, &write, 0, nullptr);

    // The render target's writes before it is read, and the result before it is sampled, copied or read back.
    VkMemoryBarrier barrier{};
    barrier.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER;
    barrier.srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_TRANSFER_WRITE_BIT;
    barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_READ_BIT |
                            VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
    dfn_.vkCmdPipelineBarrier(commands_job_,
                              VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT | VK_PIPELINE_STAGE_TRANSFER_BIT,
                              VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, 0,
                              1, &barrier, 0, nullptr, 0, nullptr);
    VkRenderPassBeginInfo start{};
    start.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
    start.renderPass = pass;
    start.framebuffer = framebuffer;
    start.renderArea = {{int32_t(dx), int32_t(dy)}, {width, height}};
    MarkGpu(kGpuCopies);
    dfn_.vkCmdBeginRenderPass(commands_job_, &start, VK_SUBPASS_CONTENTS_INLINE);
    dfn_.vkCmdBindPipeline(commands_job_, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline);
    dfn_.vkCmdBindDescriptorSets(commands_job_, VK_PIPELINE_BIND_POINT_GRAPHICS, layout_pipeline_, 0, 1,
                                 &set, 0, nullptr);
    const VkViewport view{float(dx), float(dy), float(width), float(height), 0.0f, 1.0f};
    const VkRect2D scissor{{int32_t(dx), int32_t(dy)}, {width, height}};
    dfn_.vkCmdSetViewport(commands_job_, 0, 1, &view);
    dfn_.vkCmdSetScissor(commands_job_, 0, 1, &scissor);
    const float rectangle[4] = {-1.0f, -1.0f, 2.0f, 2.0f};
    dfn_.vkCmdPushConstants(commands_job_, layout_pipeline_, VK_SHADER_STAGE_VERTEX_BIT, 0, sizeof(rectangle),
                            rectangle);
    const uint32_t number = (info_target >> 13) & 0x7;
    const int32_t bias = int32_t(info_target << 10) >> 26;
    {  // one line per kind of conversion
      const uint64_t type = (uint64_t(source.format) << 40) ^ (uint64_t(target.format) << 20) ^ info_target;
      if (conv_views_.size() < 32 && conv_views_.insert(type).second) {
        REXLOG_INFO("[native] BO2 converted copy: source format {} -> target format {} (info {:08X}: guest format {}, "
                    "number {}, bias {}), {}x{}",
                    uint32_t(source.format), uint32_t(target.format), info_target, (info_target >> 7) & 0x3F,
                    number, bias, width, height);
      }
    }
    struct {
      int32_t displacement[2];
      float scale;
      uint32_t mode;
    } constants{{x0 - int32_t(dx), y0 - int32_t(dy)},
                 std::ldexp(1.0f, bias),
                 target.format == VK_FORMAT_R16G16B16A16_SFLOAT ? 4u : number <= 3 ? number : 0u};
    dfn_.vkCmdPushConstants(commands_job_, layout_pipeline_, VK_SHADER_STAGE_FRAGMENT_BIT, 16,
                            sizeof(constants), &constants);
    dfn_.vkCmdDraw(commands_job_, 4, 1, 0, 0);
    dfn_.vkCmdEndRenderPass(commands_job_);
    // This pass bound its own pipeline, set 0, push constants, viewport and scissor in the same command
    // buffer the draws use, and VulkanDraws only rebinds what it believes changed (sets_bound_, push_recorded_,
    // state_recorded_, the bound pipeline), resetting that belief when the command generation changes. Without this
    // every draw after BO2's luminance chain kept this pass's set 0 instead of the bindless texture heap: on the PC's
    // NVIDIA driver the indices still landed on valid descriptors, on the Switch's NVK they read nothing and the whole
    // in-game image after the scene copy came out black, HUD included. A new generation
    // makes the next draw rebind everything, as after a new command buffer (its pass was already closed: copies
    // happen outside passes).
    ++generation_commands_;
    barrier.srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
    barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_TRANSFER_READ_BIT | VK_ACCESS_TRANSFER_WRITE_BIT;
    dfn_.vkCmdPipelineBarrier(commands_job_, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
                              VK_PIPELINE_STAGE_VERTEX_SHADER_BIT | VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT |
                                  VK_PIPELINE_STAGE_TRANSFER_BIT,
                              0, 1, &barrier, 0, nullptr, 0, nullptr);
    return true;
  }

  // A 3D texture built from 2D resolves. On the Xbox 360 the resolve writes the 2D tiled layout
  // and the 3D fetch reads the same bytes with the 3D tiling, so each 3D texel (x, y, z) is the 2D texel whose
  // tiled offset equals its own. For BO2's 32x32x32 table, 8x8 blocks keep their shape: 512 copy regions.
  const NativeImage* TextureResolved3D(uint32_t address, uint32_t format, uint32_t width, uint32_t height,
                                        uint32_t depth3d, uint32_t pitch, bool tiled) override {
    const NativeImage* flat = TextureResolved(address);
    if (!flat || !copy_image_ || flat->format != kFormatColor || flat->width_guest ||
        format != uint32_t(xenos::TextureFormat::k_8_8_8_8)) {
      return nullptr;
    }
    const auto e = writes_resolved_.find(address);
    const uint64_t write = e != writes_resolved_.end() ? e->second : 0;
    auto it = volumes_.find(address);
    if (it != volumes_.end()) {
      ResolvedVolume& v = it->second;
      if (v.image.width != width || v.image.height != height || v.depth3d != depth3d || v.pitch != pitch ||
          v.pitch_flat != flat->width) {
        return nullptr;  // the same address with another shape: not handled
      }
      if (v.write == write && v.source == flat->image) {
        return &v.image;
      }
    } else {
      if (!tiled || width > 256 || height > 256 || depth3d > 256) {
        if (warned_.insert(0x3D0000).second) {
          REXLOG_WARN("[native] BO2: 3D texture {}x{}x{} over a resolved one at {:08X} {}: not handled", width, height,
                      depth3d, address, tiled ? "too large" : "linear");
        }
        return nullptr;
      }
      ResolvedVolume v;
      if (!RegionsVolume(*flat, width, height, depth3d, pitch, v.regions, v.block)) {
        if (warned_.insert(0x3D0001).second) {
          REXLOG_WARN("[native] BO2: 3D texture {}x{}x{} at {:08X}: the resolved {}x{} does not cover its "
                      "memory", width,
                      height, depth3d, address, flat->width, flat->height);
        }
        return nullptr;
      }
      VkImageCreateInfo info{};
      info.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
      info.imageType = VK_IMAGE_TYPE_3D;
      info.format = kFormatColor;
      info.extent = {width, height, depth3d};
      info.mipLevels = 1;
      info.arrayLayers = 1;
      info.samples = VK_SAMPLE_COUNT_1_BIT;
      info.tiling = VK_IMAGE_TILING_OPTIMAL;
      info.usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
      info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
      info.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
      if (!rex::ui::vulkan::util::CreateDedicatedAllocationImage(vulkan_device_, info,
                                                                 rex::ui::vulkan::util::MemoryPurpose::kDeviceLocal,
                                                                 v.image.image, v.image.guest_memory)) {
        return nullptr;
      }
      v.image.width = width;
      v.image.height = height;
      v.image.format = kFormatColor;
      v.depth3d = depth3d;
      v.pitch = pitch;
      v.pitch_flat = flat->width;
      REXLOG_INFO("[native] BO2: 3D texture {}x{}x{} at {:08X} made from the resolved {}x{} ({} regions of {}x{})",
                  width, height, depth3d, address, flat->width, flat->height, v.regions.size(), v.block & 0xFFFF,
                  v.block >> 16);
      it = volumes_.emplace(address, std::move(v)).first;
    }
    ResolvedVolume& v = it->second;
    if (draws_) {
      draws_->FinishPass();  // copies are not allowed inside a pass
    }
    if (!Record()) {
      return nullptr;
    }
    // The resolve's writes (and the previous frame's reads of this volume) before the copy; the copy before the
    // draws that sample it.
    VkMemoryBarrier before{};
    before.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER;
    before.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
    before.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT | VK_ACCESS_TRANSFER_WRITE_BIT;
    VkImageMemoryBarrier first{};
    first.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    first.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    first.newLayout = VK_IMAGE_LAYOUT_GENERAL;
    first.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    first.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    first.image = v.image.image;
    first.subresourceRange = kRangeColor;
    first.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    dfn_.vkCmdPipelineBarrier(commands_job_,
                              VK_PIPELINE_STAGE_TRANSFER_BIT | VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT |
                                  VK_PIPELINE_STAGE_VERTEX_SHADER_BIT | VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
                              VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 1, &before, 0, nullptr, v.image.prepared ? 0 : 1,
                              &first);
    v.image.prepared = true;
    MarkGpu(kGpuCopies);
    copy_image_(commands_job_, flat->image, VK_IMAGE_LAYOUT_GENERAL, v.image.image, VK_IMAGE_LAYOUT_GENERAL,
                   uint32_t(v.regions.size()), v.regions.data());
    VkMemoryBarrier later{};
    later.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER;
    later.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    later.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
    dfn_.vkCmdPipelineBarrier(commands_job_, VK_PIPELINE_STAGE_TRANSFER_BIT,
                              VK_PIPELINE_STAGE_VERTEX_SHADER_BIT | VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 0, 1,
                              &later, 0, nullptr, 0, nullptr);
    v.image.swap_rb = flat->swap_rb;
    v.write = write;
    v.source = flat->image;
    ++volumes_done_;
    return &v.image;
  }

  // The copy regions that move a 2D resolve (tiled, 4-byte texels, pitch = its width) into a tiled 3D texture of
  // the same memory: the largest block size whose texels all keep their relative positions, down to 1x1.
  // `block` returns its width | height << 16. false if a 3D texel falls outside the resolve.
  static bool RegionsVolume(const OwnedImage& flat, uint32_t width, uint32_t height, uint32_t depth3d, uint32_t pitch,
                              std::vector<VkImageCopy>& regions, uint32_t& block) {
    std::unordered_map<int32_t, uint32_t> inverse;
    inverse.reserve(size_t(flat.width) * flat.height);
    for (uint32_t v = 0; v < flat.height; ++v) {
      for (uint32_t u = 0; u < flat.width; ++u) {
        inverse.emplace(OffsetTiled2D(int32_t(u), int32_t(v), flat.width, 2), u | (v << 16));
      }
    }
    const auto where = [&](uint32_t x, uint32_t y, uint32_t z, uint32_t& uv) {
      const auto it = inverse.find(OffsetTiled3D(int32_t(x), int32_t(y), int32_t(z), pitch, height, 2));
      if (it == inverse.end()) {
        return false;
      }
      uv = it->second;
      return true;
    };
    static constexpr uint32_t kBlocks[][2] = {{8, 8}, {8, 4}, {8, 2}, {4, 1}, {1, 1}};
    for (const auto& b : kBlocks) {
      const uint32_t bw = b[0], bh = b[1];
      if (width % bw || height % bh) {
        continue;
      }
      regions.clear();
      bool whole = true;
      for (uint32_t z = 0; z < depth3d && whole; ++z) {
        for (uint32_t by = 0; by < height && whole; by += bh) {
          for (uint32_t bx = 0; bx < width && whole; bx += bw) {
            uint32_t uv0 = 0;
            if (!where(bx, by, z, uv0)) {
              return false;
            }
            for (uint32_t y = by; y < by + bh && whole; ++y) {
              for (uint32_t x = bx; x < bx + bw; ++x) {
                uint32_t uv = 0;
                if (!where(x, y, z, uv)) {
                  return false;
                }
                if (uv != uv0 + (x - bx) + ((y - by) << 16)) {
                  whole = false;
                  break;
                }
              }
            }
            VkImageCopy r{};
            r.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
            r.srcOffset = {int32_t(uv0 & 0xFFFF), int32_t(uv0 >> 16), 0};
            r.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
            r.dstOffset = {int32_t(bx), int32_t(by), int32_t(z)};
            r.extent = {bw, bh, 1};
            regions.push_back(r);
          }
        }
      }
      if (whole) {
        block = bw | (bh << 16);
        return true;
      }
    }
    return false;
  }

  // bo2zm_native_lazy_depth. VulkanDraws sets it around the textures of the final composition
  // when its depth sampling is dead (no blur).
  void DeadDepthReads(bool dead) override { reads_depth_dead_ = dead; }

  // bo2zm_native_shadow_min, what the draws ask (see the cvar's comment).
  // When a pass is opened on this depth: if it is the overlay pass of the current cycle, each draw is validated.
  bool OverlayShadowPass(const NativeImage* depth, bool only_depth) override {
    if (!depth || depth != sm_target_cycle_ || sm_state_ != kSmOverlay) {
      return false;
    }
    if (!only_depth) {
      ShadowMinMissCycle("the overlay pass has a color target", 0, true);
    }
    return true;
  }

  // A draw of the overlay pass. `exact`: it leaves in the depth the minimum of what was there and of its
  // fragments.
  void OverlayShadowDraw(bool exact, uint32_t control_z) override {
    ++sm_draws_overlay_;
    if (!exact) {
      ++sm_draws_no_exact_;
      ShadowMinMissCycle("an overlay draw does not leave the minimum (the datum is its Z control)", control_z, true);
    }
  }

  // The texture with the overlay that the draws must look at: whenever it holds only the overlay, and otherwise while
  // watching or applying. 0 = none.
  uint32_t AddressShadowOverlay() const override {
    return (sm_virtual_ || (sm_on_ && sm_phase_ != kSmOff)) ? sm_overlay_ : 0;
  }

  // A draw samples the texture with the overlay. `capable`: its pixel shader has tfetch2DShadowMin in that register
  // (`ps` is its number, for the log). Returns the partner to take the minimum with, or nullptr to sample
  // it as is.
  const NativeImage* PartnerShadowOverlay(bool capable, uint32_t ps) override {
    if (sm_virtual_) {
      // It holds the overlay only: without the minimum, the scene would miss the base shadows.
      const auto it = resolved_.find(sm_base_);
      const bool base_ready = it != resolved_.end() && it->second.image.prepared;
      if (!sm_virtual_valid_ || !capable || !base_ready) {
        ++sm_reads_unable_;
        ShadowMinMismatch(!sm_virtual_valid_ ? "the overlay texture alone is sampled and the base one is already "
                                                     "from another cycle (the datum is the pixel shader)"
                               : !capable ? "a pixel shader without tfetch2DShadowMin samples the overlay "
                                          "alone (the datum is its number)"
                                        : "the world texture is not there (the datum is the pixel shader)",
                               ps);
        return nullptr;
      }
      ++sm_reads_min_;
      return &it->second.image;
    }
    // It is the usual one (base and overlay): who reads it and when is watched, so the minimum can be applied
    // without surprises.
    if (sm_state_ != kSmFree) {
      ++sm_reads_a_offbeat_;
      ShadowMinNoFit("the overlay texture is sampled between the two resolves (the datum is the pixel shader)",
                         ps);
    }
    if (!capable) {
      ++sm_reads_unable_;
      ShadowMinNoFit("a pixel shader without tfetch2DShadowMin samples the overlay texture: library without minimum? "
                         "(the datum is its number)",
                         ps);
      return nullptr;
    }
    const auto it = resolved_.find(sm_overlay_);
    if (!sm_on_ || sm_phase_ == kSmOff || it == resolved_.end() || !it->second.image.prepared) {
      ++sm_reads_normal_;
      return nullptr;
    }
    // Minimum with itself (the same texel): the pipeline with the bit already exists when applying starts.
    ++sm_reads_yes_same_;
    return &it->second.image;
  }

  // bo2zm_native_diag_clears. What is known about the clears of a render target.
  struct ClearUse {
    uint32_t base = 0;             // EDRAM base, guest format and pitch (for the report)
    uint32_t format = 0;
    uint32_t pitch = 0;
    bool depth = false;
    bool in_pass = false;          // cleared by opening a pass (depth without TRANSFER_DST: ZCULL)
    bool open = false;          // there is a clear whose use is being measured
    uint32_t used_width = 0;      // since the last clear: the largest x1 and y1 used
    uint32_t used_height = 0;
    uint32_t max_width = 0;        // the same since startup (only grows)
    uint32_t max_height = 0;
    uint64_t cycles = 0;           // clears with their use already measured, since startup
    uint64_t clears = 0;         // in the report interval
    uint64_t pixels_clears = 0;
    uint64_t pixels_used = 0;   // of the cycles closed in the interval
    // bo2zm_native_clear_useful_area. The bottom band the last clear left uncleared.
    bool band = false;
    uint32_t band_since = 0;
    VkImage band_image = VK_NULL_HANDLE;
    VkClearColorValue band_color{};
    uint32_t bands_completed = 0;
    bool area_useful_off = false;  // its useful area is not stable: cleared entirely
  };

  // What is drawn in a pass on that render target (its renderArea). Called by VulkanDraws in BeginPass.
  void AnnotatePassArea(const NativeImage* image, uint32_t width, uint32_t height) override {
    if (image) {
      AnnotateUseClear(*image, width, height);
    }
  }

  // A whole clear of that image. Closes the previous cycle (what has been used since the previous clear) and
  // opens another.
  void AnnotateClearDiag(const OwnedImage& image, uint32_t base, uint32_t format, uint32_t pitch, bool depth,
                         bool in_pass) {
    if (!diag_clears_) {
      return;
    }
    ClearUse& u = use_clears_[&image];
    if (u.open) {
      u.pixels_used += uint64_t(u.used_width) * u.used_height;
      ++u.cycles;
    }
    u.base = base;
    u.format = format;
    u.pitch = pitch;
    u.depth = depth;
    u.in_pass = in_pass;
    ++u.clears;
    u.pixels_clears += uint64_t(image.width) * image.height;
    u.used_width = 0;
    u.used_height = 0;
    u.open = true;
  }

  // The rectangle from 0 to width and from 0 to height of that image is used (a pass, a resolve, a restore
  // or a swap). It only counts on render targets that have been cleared at least once.
  void AnnotateUseClear(const OwnedImage& image, uint32_t width, uint32_t height) {
    // bo2zm_native_clear_useful_area. While any band remains uncleared it is checked even if the diagnostic
    // was turned off at run time: a pending band is always completed before it is used.
    if (use_clears_.empty() || (!diag_clears_ && !bands_pending_)) {
      return;
    }
    const auto it = use_clears_.find(&image);
    if (it == use_clears_.end()) {
      return;
    }
    ClearUse& u = it->second;
    width = std::min(width, image.width);
    height = std::min(height, image.height);
    // bo2zm_native_clear_useful_area. If this use reaches the band the last clear left uncleared (or its
    // first row, which a linear blit can read), it is cleared now, before the use: the result is the same
    // as if it had been cleared entirely.
    if (u.band && (u.band_image != image.image || height >= u.band_since)) {
      CompleteBand(image, u);
    }
    if (!diag_clears_) {
      return;  // it was only checked because of the band
    }
    u.used_width = std::max(u.used_width, width);
    u.used_height = std::max(u.used_height, height);
    u.max_width = std::max(u.max_width, width);
    u.max_height = std::max(u.max_height, height);
  }

  // bo2zm_native_clear_useful_area. Clears only the rows that render target uses, if they are already known.
  // Returns false if it has to be cleared entirely, as always.
  bool ClearColorAreaUseful(OwnedImage& target, const VkClearColorValue& color) {
    if (!clear_area_useful_ || !diag_clears_ || !draws_ || target.format != kFormatColor) {
      return false;
    }
    const auto it = use_clears_.find(&target);
    if (it == use_clears_.end()) {
      return false;
    }
    ClearUse& u = it->second;
    if (u.area_useful_off || u.cycles < kAreaUsefulCycles || u.max_height == 0) {
      return false;
    }
    // Up to the multiple of 64 that leaves at least one row of margin below what is used: a linear blit (the
    // resolve that shrinks the scene) can read the row below its rectangle, and that row must be cleared.
    const uint32_t height = std::min(target.height, (u.max_height + 64u) & ~63u);
    if (height >= target.height) {
      return false;
    }
    if (!draws_->ClearColorInPass(commands_job_, target, color, VkRect2D{{0, 0}, {target.width, height}})) {
      return false;
    }
    if (!u.band) {
      ++bands_pending_;
    }
    u.band = true;
    u.band_since = height;
    u.band_image = target.image;
    u.band_color = color;
    ++area_useful_clears_;
    area_useful_pixels_ += uint64_t(target.width) * (target.height - height);
    return true;
  }

  // The render target has been cleared entirely: no band is left.
  void RemoveBand(const OwnedImage& target) {
    if (use_clears_.empty()) {
      return;
    }
    const auto it = use_clears_.find(&target);
    if (it != use_clears_.end() && it->second.band) {
      it->second.band = false;
      if (bands_pending_) {
        --bands_pending_;
      }
    }
  }

  // Clears the band that was left uncleared, before a use that reaches it (outside a pass: called by Copy,
  // RestoreContent, SwapWithResolved and BeginPass before opening its own).
  void CompleteBand(const OwnedImage& image, ClearUse& u) {
    u.band = false;
    if (bands_pending_) {
      --bands_pending_;
    }
    ++u.bands_completed;
    ++area_useful_completed_;
    if (u.bands_completed >= 3) {
      u.area_useful_off = true;  // its useful area is not stable: from now on it is cleared entirely
    }
    const bool same_image = u.band_image == image.image;
    const uint32_t height_band = image.height > u.band_since ? image.height - u.band_since : 0;
    if (same_image && (!height_band || (draws_ && Record() &&
                                         draws_->ClearColorInPass(
                                             commands_job_, image, u.band_color,
                                             VkRect2D{{0, int32_t(u.band_since)}, {image.width, height_band}})))) {
      return;
    }
    // The image changed without going through a clear (should not happen) or the band could not be cleared.
    ++area_useful_misses_;
    if (!area_useful_global_off_) {
      area_useful_global_off_ = true;
      clear_area_useful_ = false;
      REXLOG_ERROR("[native] C2 clear useful area: MISMATCH, could not complete the {}x{} band from row {} ({}). Off "
                   "for the rest of the session: the whole image is cleared",
                   image.width, image.height, u.band_since, same_image ? "the pass failed" : "the image changed");
    }
  }

  // Every 20 s, per render target, what was cleared against what was used (C2 clears per target).
  // Clears cost ~0.075 ms real per Mpixel (measured, see "C2 clears skipped").
  void ReportClears() {
    diag_clears_ = REXCVAR_GET(bo2zm_native_diag_clears);
    clear_area_useful_ = REXCVAR_GET(bo2zm_native_clear_useful_area) && !area_useful_global_off_;
    const auto now = std::chrono::steady_clock::now();
    if (report_clears_ == std::chrono::steady_clock::time_point{}) {
      report_clears_ = now;
      presented_report_clears_ = presented_;
      return;
    }
    if (now - report_clears_ < std::chrono::seconds(20)) {
      return;
    }
    report_clears_ = now;
    const uint64_t frames = presented_ - presented_report_clears_;
    presented_report_clears_ = presented_;
    if (diag_clears_ && frames > 0 && !use_clears_.empty()) {
      struct ReportRow {
        const ClearUse* use;
        uint32_t width;
        uint32_t height;
        double clear;  // Mpixels per frame
        double used;
      };
      std::vector<ReportRow> rows;
      double total_clear = 0.0, total_used = 0.0;
      for (const auto& [image, u] : use_clears_) {
        if (!u.clears) {
          continue;
        }
        const double clear = double(u.pixels_clears) / 1e6 / double(frames);
        // Each clear closes the previous one's cycle: in steady state, as many closed cycles as clears.
        const double used = std::min(clear, double(u.pixels_used) / 1e6 / double(frames));
        rows.push_back({&u, image->width, image->height, clear, used});
        total_clear += clear;
        total_used += used;
      }
      std::sort(rows.begin(), rows.end(),
                [](const ReportRow& a, const ReportRow& b) { return a.clear - a.used > b.clear - b.used; });
      std::string list;
      for (const ReportRow& row : rows) {
        const ClearUse& u = *row.use;
        list += fmt::format(" | {:03X}/{} {} {}x{}{}: {:.2f} per frame, {:.2f} Mpixels cleared, {:.2f} used (up to "
                             "{}x{})",
                             u.base, u.pitch, u.depth ? "depth" : "color", row.width, row.height,
                             u.in_pass ? " (per pass, ZCULL)" : "", double(u.clears) / double(frames),
                             row.clear, row.used, u.max_width, u.max_height);
      }
      BO2ZM_REPORT_RING("[native] C2 clears per target ({} frames): {:.2f} Mpixels cleared and {:.2f} used "
                           "per frame (spare {:.2f}: ~{:.2f} ms real){}",
                           frames, total_clear, total_used, total_clear - total_used,
                           (total_clear - total_used) * 0.075, list);
    }
    if (area_useful_clears_ || area_useful_misses_) {  // bo2zm_native_clear_useful_area
      const double mp =
          double(area_useful_pixels_ - area_useful_pixels_previous_) / 1e6 / double(std::max<uint64_t>(frames, 1));
      BO2ZM_REPORT_RING("[native] C2 clear useful area: {} color clears trimmed since boot; {:.2f} Mpixels "
                           "per frame not cleared (~{:.2f} ms real); {} bands completed before a use; {} failures{}",
                           area_useful_clears_, mp, mp * 0.075, area_useful_completed_, area_useful_misses_,
                           area_useful_global_off_ ? " *** OFF BY THE GUARD ***" : " (0 = the image is the same)");
      area_useful_pixels_previous_ = area_useful_pixels_;
    }
    for (auto& [image, u] : use_clears_) {
      u.clears = 0;
      u.pixels_clears = 0;
      u.pixels_used = 0;
    }
  }

  // A request for a resolved depth texture (see bo2zm_native_lazy_depth).
  void AnnotateReadDepth(uint32_t address) {
    if (reads_depth_dead_) {
      last_read_dead_[address] = presented_;
      if (!pending_.empty()) {
        const auto p = pending_.find(address);
        if (p != pending_.end()) {
          p->second.read_dead = true;
        }
      }
      return;
    }
    last_read_live_[address] = presented_;
    if (!pending_.empty() && pending_.count(address)) {
      RecordCopyPending(address);
      ++lazy_copied_read_;
    }
    if (!expired_.empty() && expired_.erase(address)) {
      ++lazy_reads_late_;
      if (!lazy_off_) {
        lazy_off_ = true;
        REXLOG_ERROR("[native] C2 lazy depth: MISMATCH, a draw samples {:08X} after its copy was dropped (late read: it "
                     "reads an old depth). Off for the rest of the session: always copied",
                     address);
      }
    }
  }

  // Records the deferred copy of that address, outside a pass, and removes it from the list.
  void RecordCopyPending(uint32_t address) {
    const auto p = pending_.find(address);
    if (p == pending_.end()) {
      return;
    }
    const PendingCopy pending = p->second;
    pending_.erase(p);
    const auto r = resolved_.find(address);
    if (r == resolved_.end() || r->second.image.image != pending.target_vk || !pending.source ||
        pending.source->image != pending.source_vk || !copy_image_ || !Record()) {
      expired_.insert(address);  // should not happen: every image change goes through the hooks first
      return;
    }
    if (draws_) {
      draws_->FinishPass();  // it can arrive from a draw (TextureResolved): the copy goes outside the pass
    }
    MarkGpu(kGpuCopies);
    copy_image_(commands_job_, pending.source_vk, VK_IMAGE_LAYOUT_GENERAL, pending.target_vk,
                   VK_IMAGE_LAYOUT_GENERAL, 1, &pending.copy);
    ++copies_;
    AnnotateCopy(pending.copy.extent.width, pending.copy.extent.height);
    ResolvedWritten(address, uint64_t(pending.copy.extent.width) * pending.copy.extent.height);
  }

  // Before any write to a depth render target. The deferred copies that come from it are recorded (same
  // frame, or the composition has not requested them yet) or dropped (nobody has sampled them).
  void BeforeWriteDepth(const OwnedImage& image) {
    if (pending_.empty() || image.image == VK_NULL_HANDLE) {
      return;
    }
    for (auto it = pending_.begin(); it != pending_.end();) {
      if (it->second.source_vk != image.image) {
        ++it;
        continue;
      }
      const uint32_t address = it->first;
      // Dropped even within the same frame if the composite without blur already requested it (read_dead).
      // Keeping same-frame copies saved nothing: the game always rewrites the source within the same frame,
      // and the reader watch saw 0 reads after the rewrite. The guard catches a late read.
      if (!it->second.read_dead) {
        ++it;  // RecordCopyPending erases that entry: the iterator is already on the next one
        RecordCopyPending(address);
        ++lazy_copied_write_;
      } else {
        lazy_pixels_saved_ += uint64_t(it->second.copy.extent.width) * it->second.copy.extent.height;
        it = pending_.erase(it);
        expired_.insert(address);
        ++lazy_dropped_;
      }
    }
  }

  // Defers the copy if, lately, the only reader of that address is the composite without blur.
  bool PostponeCopyDepth(uint32_t address, const OwnedImage& source, const Resolved& resolved,
                               const VkImageCopy& copy) {
    if (lazy_off_ || !REXCVAR_GET(bo2zm_native_lazy_depth)) {
      return false;
    }
    const auto dead = last_read_dead_.find(address);
    if (dead == last_read_dead_.end() || presented_ - dead->second > kLazyFrames) {
      return false;  // the composite without blur has not requested it lately
    }
    const auto live = last_read_live_.find(address);
    if (live != last_read_live_.end() && presented_ - live->second <= kLazyFrames) {
      return false;  // something really samples it: copy as usual
    }
    PendingCopy& pending = pending_[address];
    pending.source = &source;
    pending.source_vk = source.image;
    pending.target_vk = resolved.image.image;
    pending.copy = copy;
    pending.frame = presented_;
    pending.read_dead = false;
    ResolvedWritten(address);  // new contents (deferred): no longer lent or stale
    ++lazy_postponed_;
    return true;
  }

  // bo2zm_native_lazy_front. Deferred copy to a front buffer, and the render target's spare images.
  struct PendingFront {
    OwnedImage* target = nullptr;           // source render target (targets_ never erases: the pointer stays valid)
    VkImage source_vk = VK_NULL_HANDLE;  // image holding the content: the target's own or a retained one
    int32_t retained = -1;               // index in fronts_images_ if it is no longer in the target
    VkImage texture_vk = VK_NULL_HANDLE;
    VkImageCopy copy{};
  };
  struct FrontImage {
    OwnedImage image;              // same size, format and usage as the target: swapped with it
    uint32_t retained_per = 0;  // address of the front buffer that retains it; 0 = free
  };

  // Is this address a front buffer read only by the Swap? A Swap presented it recently and no draw has
  // sampled it in that time.
  void ForgetFrontComposition(uint32_t address, const char* reason) {
    for (auto it = front_compositions_.begin(); it != front_compositions_.end();) {
      if (it->second.address == address) {
        if (FrontTraceLine()) {
          REXLOG_INFO("[front-trace] invalidate target {:p} front {:08X} reason {} write_frame {} frame {}",
                      static_cast<const void*>(it->first), address, reason, it->second.write_frame, presented_);
        }
        it = front_compositions_.erase(it);
      } else ++it;
    }
  }

  bool IsSwapFrontOnly(uint32_t address) const {
    const auto presented = fronts_presented_.find(address);
    if (presented == fronts_presented_.end() || presented_ - presented->second > kFrontFrames) {
      return false;
    }
    const auto read = fronts_read_.find(address);
    return read == fronts_read_.end() || presented_ - read->second > kFrontFrames;
  }

  // The retained image of a deferred copy becomes free again.
  void FreeRetained(const PendingFront& pending) {
    if (pending.retained >= 0 && size_t(pending.retained) < fronts_images_.size()) {
      fronts_images_[size_t(pending.retained)].retained_per = 0;
    }
  }

  // The image holding the deferred copy's content, if it is still the same one; otherwise nullptr.
  OwnedImage* SourceFront(const PendingFront& pending) {
    OwnedImage* source = nullptr;
    if (pending.retained >= 0) {
      if (size_t(pending.retained) < fronts_images_.size()) {
        source = &fronts_images_[size_t(pending.retained)].image;
      }
    } else {
      source = pending.target;
    }
    return source && source->image == pending.source_vk && source->prepared ? source : nullptr;
  }

  // Records the deferred copy to that front buffer (outside a pass) and removes it from the list.
  void RecordCopyFront(uint32_t address) {
    const auto p = fronts_pending_.find(address);
    if (p == fronts_pending_.end()) {
      return;
    }
    const PendingFront pending = p->second;
    fronts_pending_.erase(p);
    const auto r = resolved_.find(address);
    OwnedImage* const source = SourceFront(pending);
    if (r == resolved_.end() || r->second.image.image != pending.texture_vk || !source || !copy_image_ ||
        !Record()) {
      FreeRetained(pending);
      fronts_expired_.insert(address);  // should not happen: every image change goes through the hooks first
      return;
    }
    if (draws_) {
      draws_->FinishPass();  // may come from a draw (TextureResolved): the copy goes outside the pass
    }
    MarkGpu(kGpuCopies);
    copy_image_(commands_job_, pending.source_vk, VK_IMAGE_LAYOUT_GENERAL, pending.texture_vk,
                   VK_IMAGE_LAYOUT_GENERAL, 1, &pending.copy);
    ++copies_;
    AnnotateCopy(pending.copy.extent.width, pending.copy.extent.height);
    ResolvedWritten(address, uint64_t(pending.copy.extent.width) * pending.copy.extent.height);
    fronts_expired_.erase(address);
    FreeRetained(pending);
  }

  // Another resolve (or an image swap) reaches that texture. If it covers the whole texture, the deferred
  // copy is unnecessary and is dropped: nobody will see its content. Otherwise it is recorded first.
  void ResolveFrontEarlier(uint32_t address, bool whole) {
    if (whole && !fronts_expired_.empty()) {
      fronts_expired_.erase(address);  // fully covered: it is no longer missing any copy
    }
    if (fronts_pending_.empty()) {
      return;
    }
    const auto p = fronts_pending_.find(address);
    if (p == fronts_pending_.end()) {
      return;
    }
    if (!whole) {
      RecordCopyFront(address);
      ++front_copied_write_;
      return;
    }
    front_pixels_saved_ += uint64_t(p->second.copy.extent.width) * p->second.copy.extent.height;
    FreeRetained(p->second);
    fronts_pending_.erase(p);
    ++front_replaced_;
  }

  // Defers the copy if it is 1:1, covers the whole texture from the target's corner, and that address is
  // read only by the Swap.
  bool PostponeCopyFront(uint32_t address, OwnedImage& target, const Resolved& resolved, const VkImageCopy& copy) {
    if (front_off_ || !REXCVAR_GET(bo2zm_native_lazy_front) || REXCVAR_GET(bo2zm_native_diag_resolved)) {
      return false;
    }
    if (copy.srcOffset.x != 0 || copy.srcOffset.y != 0 || copy.dstOffset.x != 0 || copy.dstOffset.y != 0 ||
        copy.extent.width != resolved.image.width || copy.extent.height != resolved.image.height ||
        target.format != kFormatColor || resolved.image.format != kFormatColor) {
      return false;
    }
    // Small ones are read back for the guest (ReadResolved): those are never deferred.
    const int32_t texels_read = REXCVAR_GET(bo2zm_native_read_resolved_texels);
    if (uint64_t(copy.extent.width) * copy.extent.height <= uint64_t(std::max<int32_t>(texels_read, 0))) {
      return false;
    }
    if (!IsSwapFrontOnly(address)) {
      return false;
    }
    PendingFront& pending = fronts_pending_[address];
    pending.target = &target;
    pending.source_vk = target.image;
    pending.retained = -1;
    pending.texture_vk = resolved.image.image;
    pending.copy = copy;
    ResolvedWritten(address);  // new contents (deferred)
    fronts_expired_.erase(address);
    ++front_postponed_;
    return true;
  }

  // Before writing to a color target in any way other than a full clear (a pass, a restore, an image
  // swap): deferred copies whose source is its image are recorded now (exact).
  void BeforeWriteColor(const OwnedImage& target) {
    if (fronts_pending_.empty() || target.image == VK_NULL_HANDLE) {
      return;
    }
    for (auto it = fronts_pending_.begin(); it != fronts_pending_.end();) {
      if (it->second.retained >= 0 || it->second.source_vk != target.image) {
        ++it;
        continue;
      }
      const uint32_t address = it->first;
      ++it;  // RecordCopyFront erases that entry: the iterator is already on the next one
      RecordCopyFront(address);
      ++front_copied_write_;
    }
  }

  // Before a full clear of a color target. If its image is the source of a deferred front buffer, the
  // target takes a free spare image of the same size and the one holding the content is retained for the
  // front buffer: the clear does not need the old content, so nothing is copied. Without a spare (or with
  // two front buffers from the same image), the copy is recorded first, as usual.
  void RotateFrontBeforeClear(OwnedImage& target) {
    if (fronts_pending_.empty() || target.image == VK_NULL_HANDLE) {
      return;
    }
    uint32_t base = 0;
    uint32_t how_many = 0;
    for (const auto& [address, pending] : fronts_pending_) {
      if (pending.retained < 0 && pending.source_vk == target.image) {
        base = address;
        ++how_many;
      }
    }
    if (how_many == 0) {
      return;
    }
    int32_t free = -1;
    if (how_many == 1) {
      for (size_t i = 0; i < fronts_images_.size(); ++i) {
        const OwnedImage& candidate = fronts_images_[i].image;
        if (fronts_images_[i].retained_per == 0 && candidate.image != VK_NULL_HANDLE &&
            candidate.width == target.width && candidate.height == target.height &&
            candidate.format == target.format) {
          free = int32_t(i);
          break;
        }
      }
      if (free < 0 && fronts_images_.size() < kFrontImagesMax) {
        FrontImage fresh;
        // Same usage flags as GetTarget: the image becomes the render target.
        if (Create(fresh.image, target.width, target.height,
                  VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT |
                      VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
                  target.format)) {
          Prepare(fresh.image);
          fronts_images_.push_back(fresh);
          free = int32_t(fronts_images_.size()) - 1;
          REXLOG_INFO("[native] C2 lazy front buffer: spare image {} of {}x{} for the front buffer's target",
                      fronts_images_.size(), target.width, target.height);
        }
      }
    }
    if (free < 0) {
      ++front_no_spare_;
      BeforeWriteColor(target);  // the copy is recorded before the clear, as usual
      return;
    }
    OwnedImage& spare = fronts_images_[size_t(free)].image;
    std::swap(target.image, spare.image);
    std::swap(target.guest_memory, spare.guest_memory);
    std::swap(target.view, spare.view);
    std::swap(target.prepared, spare.prepared);
    fronts_images_[size_t(free)].retained_per = base;
    fronts_pending_[base].retained = free;  // source_vk is still the content image, now retained
    if (draws_) {
      draws_->InvalidateImages(target.image, spare.image);
    }
    ++front_rotations_;
  }

  // A draw samples a front buffer. If its copy is deferred, it is recorded now (before the draw).
  void AnnotateReadFront(uint32_t address) {
    fronts_read_[address] = presented_;
    front_composition_sampled_.insert(address);
    ForgetFrontComposition(address, "sampled");  // sampled front contents are retained resources, not a fresh composition
    if (!fronts_pending_.empty() && fronts_pending_.count(address)) {
      RecordCopyFront(address);
      ++front_copied_read_;
    }
    if (!fronts_expired_.empty() && fronts_expired_.erase(address)) {
      ++front_reads_late_;
      if (!front_off_) {
        front_off_ = true;
        REXLOG_ERROR("[native] C2 lazy front buffer: MISMATCH, a draw samples the front buffer {:08X} without its last "
                     "copy. Off for the rest of the session: always copied",
                     address);
      }
    }
  }

  // The Swap of that front buffer, before the submission. Returns the image that can be presented without
  // a copy (the target's or the retained one, with the front buffer in its corner) or nullptr: in that
  // case any deferred copy is recorded here (in the same submission, ahead of the output) and the output
  // reads the texture as usual.
  OwnedImage* FrontOnPresent(uint32_t address, uint32_t width, uint32_t height) {
    if (FrontTraceLine()) {
      REXLOG_INFO("[front-trace] swap front {:08X} {}x{} frame {} owners {}",
                  address, width, height, presented_, front_compositions_.size());
    }
    fronts_presented_[address] = presented_;
    if (!fronts_expired_.empty() && fronts_expired_.erase(address)) {
      ++front_reads_late_;
      if (!front_off_) {
        front_off_ = true;
        REXLOG_ERROR("[native] C2 lazy front buffer: MISMATCH, the Swap paints the front buffer {:08X} without its last "
                     "copy. Off for the rest of the session: always copied",
                     address);
      }
    }
    if (fronts_pending_.empty()) {
      return nullptr;
    }
    if (REXCVAR_GET(bo2zm_native_diag_resolved)) {
      // The diagnostic grid shows every resolved texture: if it is enabled at runtime, no copy stays deferred.
      while (!fronts_pending_.empty()) {
        RecordCopyFront(fronts_pending_.begin()->first);  // removes it from the list, whether it succeeds or not
        ++front_copied_swap_;
      }
      return nullptr;
    }
    const auto p = fronts_pending_.find(address);
    if (p == fronts_pending_.end()) {
      return nullptr;
    }
    const auto r = resolved_.find(address);
    OwnedImage* const source = SourceFront(p->second);
    bool can = source && r != resolved_.end() && r->second.image.image == p->second.texture_vk &&
                 r->second.image.prepared && ramp_gamma_ && !bo2zm::settings::AntialiasingFxaa() &&
                 !front_off_;
    if (can) {
      // Same computation as Present: the output has the texture's size (exact variant) and the copy
      // covers it.
      const OwnedImage& texture = r->second.image;
      const uint32_t w = std::min(width ? width : texture.width, texture.width);
      const uint32_t h = std::min(height ? height : texture.height, texture.height);
      can = w == texture.width && h == texture.height && p->second.copy.extent.width == w &&
              p->second.copy.extent.height == h;
    }
    if (!can) {
      RecordCopyFront(address);
      ++front_copied_swap_;
      return nullptr;
    }
    if (p->second.retained >= 0) {
      ++front_painted_retained_;
    } else {
      ++front_painted_target_;
    }
    return source;
  }

  // Upload buffer full: submit what has been recorded and continue in the other slot, with its empty
  // upload buffer (Record only waits if that slot's last submission is still on the GPU).
  bool SubmitYWait() override { return SubmitJob(false) && Record(); }

  // The function above submits and continues; this one waits. Same order as every other place that must
  // destroy something the GPU might still be reading (see the resolved texture that changes size).
  bool WaitGpuIdle() override {
    WaitGpu();
    return Record();
  }

  void MarkGpu(uint32_t category) override {
    if (queries_ == VK_NULL_HANDLE || !recording_) {
      return;
    }
    WorkSlot& slot = slots_[slot_];
    if (slot.categories.size() + 1 >= kMarksPerSlot ||
        (!slot.categories.empty() && slot.categories.back() == category)) {
      return;  // no room (the last one is for the end mark) or same category continues
    }
    write_mark_(commands_job_,
                    slot.marks_precise ? VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT : VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                    queries_, slot_ * kMarksPerSlot + uint32_t(slot.categories.size()));
    slot.categories.push_back(uint8_t(category));
  }

  // The game's occlusion queries (see bo2zm_native_targets.h).
  void BeginOcclusion(uint32_t base) override {
    if (occlusion_open_ != 0) {
      uint64_t discarded = 0;
      FinishOcclusion(0, discarded);
    }
    if (!draws_ || occlusions_ == VK_NULL_HANDLE) {
      return;
    }
    if (occlusions_game_.size() >= 256) {
      occlusions_game_.clear();  // spans that were never read (should not happen): leftovers are ignored
    }
    occlusion_open_ = next_occlusion_++;
    occlusions_game_[occlusion_open_].base = base;
    draws_->OcclusionOpen(true);
  }

  bool FinishOcclusion(uint32_t base, uint64_t& samples) override {
    if (occlusion_open_ != 0) {
      if (draws_) {
        draws_->OcclusionOpen(false);
      }
      const auto it = occlusions_game_.find(occlusion_open_);
      occlusion_open_ = 0;
      if (it != occlusions_game_.end()) {
        it->second.finished = true;
        if (it->second.spans_pending == 0) {
          PublishOcclusion(it);  // no draws counted: 0 samples
        }
      }
    }
    const auto measure = occlusion_per_base_.find(base);
    if (measure == occlusion_per_base_.end()) {
      return false;
    }
    samples = measure->second;
    return true;
  }

  void StatsOcclusion(uint64_t values[5]) const override {
    std::copy(std::begin(stats_occlusion_), std::end(stats_occlusion_), values);
  }

  uint32_t BeginQueryOcclusion() override {
    if (occlusions_ == VK_NULL_HANDLE || occlusion_open_ == 0 || !recording_) {
      return UINT32_MAX;
    }
    const auto it = occlusions_game_.find(occlusion_open_);
    if (it == occlusions_game_.end()) {
      return UINT32_MAX;
    }
    WorkSlot& slot = slots_[slot_];
    if (slot.occlusions.size() >= kOcclusionsPerSlot) {
      ++stats_occlusion_[1];
      it->second.failed = true;
      return UINT32_MAX;
    }
    const uint32_t index = slot_ * kOcclusionsPerSlot + uint32_t(slot.occlusions.size());
    dfn_.vkCmdBeginQuery(commands_job_, occlusions_, index,
                         occlusion_precise_ ? VK_QUERY_CONTROL_PRECISE_BIT : 0);
    slot.occlusions.emplace_back(index, occlusion_open_);
    ++it->second.spans_pending;
    ++stats_occlusion_[0];
    return index;
  }

  void FinishQueryOcclusion(uint32_t index) override {
    if (occlusions_ != VK_NULL_HANDLE && index != UINT32_MAX && recording_) {
      dfn_.vkCmdEndQuery(commands_job_, occlusions_, index);
    }
  }

  // Reflection visibility. Our own query around a single draw (the one that samples the reflection,
  // or the witness), inside the open pass. UINT32_MAX if there is no pool, nothing is being recorded or
  // there is no room left.
  uint32_t BeginQueryVisibility(bool witness) override {
    if (visibility_ == VK_NULL_HANDLE || !recording_) {
      return UINT32_MAX;
    }
    WorkSlot& slot = slots_[slot_];
    if (slot.visibility.size() >= kVisibilityPerSlot) {
      ++visibility_no_room_;
      return UINT32_MAX;
    }
    const uint32_t index = slot_ * kVisibilityPerSlot + uint32_t(slot.visibility.size());
    dfn_.vkCmdBeginQuery(commands_job_, visibility_, index, 0);
    slot.visibility.emplace_back(index, witness ? kVisibilityWitness : kVisibilityWater);
    return index;
  }

  void FinishQueryVisibility(uint32_t index) override {
    if (visibility_ != VK_NULL_HANDLE && index != UINT32_MAX && recording_) {
      dfn_.vkCmdEndQuery(commands_job_, visibility_, index);
    }
  }

  // One query per pass, from before vkCmdBeginRenderPass to after EndRenderPass.
  uint32_t BeginStats(uint32_t category) override {
    // With the per-draw diagnostic enabled this one is not opened: two queries of the same type cannot be
    // active at once, and the per-pass query would enclose the per-draw ones.
    if (stats_ == VK_NULL_HANDLE || !recording_ ||
        !REXCVAR_GET(bo2zm_native_pipeline_stats) ||
        REXCVAR_GET(bo2zm_native_per_draw_stats_s) > 0) {
      return UINT32_MAX;
    }
    WorkSlot& slot = slots_[slot_];
    if (slot.stats.size() >= kStatsPerSlot) {
      ++stats_no_room_;
      if (stats_no_room_ % 200 == 1) {
        REXLOG_WARN("[native] C2: no room for pass statistics ({} times): the job has "
                    "more than {} passes", stats_no_room_, kStatsPerSlot);
      }
      return UINT32_MAX;
    }
    const uint32_t index =
        slot_ * kStatsPerSlot + uint32_t(slot.stats.size());
    dfn_.vkCmdBeginQuery(commands_job_, stats_, index, 0);
    slot.stats.emplace_back(index, uint8_t(category));
    return index;
  }

  void FinishStats(uint32_t index) override {
    if (stats_ != VK_NULL_HANDLE && index != UINT32_MAX && recording_) {
      dfn_.vkCmdEndQuery(commands_job_, stats_, index);
    }
  }

  // One query per draw, tagged with its pixel shader.
  uint32_t BeginStatsDraw(uint32_t label, uint32_t category) override {
    if (stats_draw_ == VK_NULL_HANDLE || !recording_ || !window_diagnostic_) {
      return UINT32_MAX;
    }
    WorkSlot& slot = slots_[slot_];
    if (slot.stats_draw.size() >= kStatsDrawPerSlot) {
      ++stats_draw_no_room_;
      if (stats_draw_no_room_ % 2000 == 1) {
        REXLOG_WARN("[native] C2: no room for per-draw statistics ({} times)",
                    stats_draw_no_room_);
      }
      return UINT32_MAX;
    }
    const uint32_t index = slot_ * kStatsDrawPerSlot +
                            uint32_t(slot.stats_draw.size());
    dfn_.vkCmdBeginQuery(commands_job_, stats_draw_, index, 0);
    slot.stats_draw.emplace_back(
        index, uint16_t((category % kGpuCategories) * kLabelsShader + label % kLabelsShader));
    return index;
  }

  void FinishStatsDraw(uint32_t index) override {
    if (stats_draw_ != VK_NULL_HANDLE && index != UINT32_MAX && recording_) {
      dfn_.vkCmdEndQuery(commands_job_, stats_draw_, index);
    }
  }

 private:
  bool Reject(uint32_t cause, const char* text) {
    ++rejections_;
    if (warned_.insert(cause).second) {
      REXLOG_WARN("[native] C2: {} (cause {})", text, cause);
    }
    return false;
  }

  // Rectangle covered by the copy, as in GetResolveInfo (draw.cpp:787-889).
  bool Rectangle(const CopyRegisters& reg, uint32_t pitch, int32_t& x0, int32_t& y0,
                  int32_t& x1, int32_t& y1) {
    const uint32_t type = reg.fetch_vertices[0] & 0x3;
    const uint32_t address = reg.fetch_vertices[0] >> 2;
    const auto order = static_cast<xenos::Endian>(reg.fetch_vertices[1] & 0x3);
    const uint32_t size = (reg.fetch_vertices[1] >> 2) & 0xFFFFFF;
    if (type != uint32_t(xenos::FetchConstantType::kVertex) || size != 3 * 2) {
      return Reject(5, "copy vertices in an unsupported format");
    }
    const uint8_t* vertices = memory_->TranslatePhysical(address * 4);
    const float mid_pixel =
        (reg.pa_su_vtx_cntl & 0x1) == uint32_t(xenos::PixelCenter::kD3DZero) ? 0.5f : 0.0f;
    int32_t fixed[6];
    for (int i = 0; i < 6; ++i) {
      float value;
      std::memcpy(&value, vertices + i * 4, sizeof(value));
      fixed[i] = Fixed16p8(xenos::GpuSwap(value, order) + mid_pixel);
    }
    x0 = (std::min({fixed[0], fixed[2], fixed[4]}) + 127) >> 8;
    y0 = (std::min({fixed[1], fixed[3], fixed[5]}) + 127) >> 8;
    x1 = (std::max({fixed[0], fixed[2], fixed[4]}) + 127) >> 8;
    y1 = (std::max({fixed[1], fixed[3], fixed[5]}) + 127) >> 8;

    const int32_t offset_x = ExtenderSign15(reg.pa_sc_window_offset & 0x7FFF);
    const int32_t offset_y = ExtenderSign15((reg.pa_sc_window_offset >> 16) & 0x7FFF);
    if ((reg.pa_su_sc_mode_cntl >> 16) & 0x1) {  // vtx_window_offset_enable
      x0 += offset_x;
      y0 += offset_y;
      x1 += offset_x;
      y1 += offset_y;
    }
    // Window scissor (GetScissor without clamping to the pitch).
    int32_t left = int32_t(reg.pa_sc_window_scissor_tl & 0x3FFF);
    int32_t up = int32_t((reg.pa_sc_window_scissor_tl >> 16) & 0x3FFF);
    int32_t right = int32_t(reg.pa_sc_window_scissor_br & 0x3FFF);
    int32_t down = int32_t((reg.pa_sc_window_scissor_br >> 16) & 0x3FFF);
    if (!((reg.pa_sc_window_scissor_tl >> 31) & 0x1)) {  // window_offset_disable
      left += offset_x;
      up += offset_y;
      right += offset_x;
      down += offset_y;
    }
    left = std::max(left, 0);
    up = std::max(up, 0);
    right = std::max(right, left);
    down = std::max(down, up);
    x0 = std::clamp(x0, left, right);
    y0 = std::clamp(y0, up, down);
    x1 = std::clamp(x1, left, right);
    y1 = std::clamp(y1, up, down);
    // D3D9 aligns to 8 (kResolveAlignmentPixels).
    x0 &= ~int32_t(7);
    y0 &= ~int32_t(7);
    x1 = (x1 + 7) & ~int32_t(7);
    y1 = (y1 + 7) & ~int32_t(7);
    const int32_t pitch_aligned = int32_t(pitch & ~uint32_t(7));
    x0 = std::min(x0, pitch_aligned);
    x1 = std::min(x1, pitch_aligned);
    if (x0 >= x1 || y0 >= y1) {
      return Reject(6, "empty copy rectangle");
    }
    return true;
  }

  OwnedImage* GetTarget(uint32_t base, uint32_t format, uint32_t pitch) {
    // Formats that only differ in blending precision share their EDRAM storage (BO2 draws the
    // scene as 2_10_10_10_AS_10_10_10_10 and resolves it as 2_10_10_10): one image for both.
    format = bo2zm_formats::Storage(format);
    if (!pitch) {
      Reject(7, "render target with pitch 0");
      return nullptr;
    }
    const uint64_t key = (uint64_t(base) << 20) | (uint64_t(format) << 16) | pitch;
    auto it = targets_.find(key);
    if (it != targets_.end()) {
      return &it->second;
    }
    /*
     * The height comes from the pitch, so the scene's color target measures 880x880 to draw 880x720
     * and can never be swapped with its resolved texture. 720 rows would be enough for the scene and the
     * sizes would match, but narrow targets draw below row 720 (see HeightTarget).
     */
    /*
     * Creating the color target 720 rows high (instead of deriving the height from the pitch), so that it
     * matches the resolved texture and the images can be swapped instead of copied, was tried: it breaks
     * the scene (the screen fills with a yellow smear), so the height stays derived from the pitch.
     */
    const uint32_t height = HeightTarget(pitch);
    OwnedImage image;
    if (!Create(image, pitch, height,
               VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT |
                   VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
               FormatHostTarget(format))) {
      Reject(8, "could not create a render target");
      return nullptr;
    }
    const VkClearColorValue diag = ColorDiagnostic(base, format, pitch);
    REXLOG_INFO("[native] C2: render target base {:03X}, format {}, {}x{} (diagnostic color {:02X}{:02X}{:02X})",
                base, format, pitch, height, uint32_t(std::lround(diag.float32[0] * 255.0f)),
                uint32_t(std::lround(diag.float32[1] * 255.0f)),
                uint32_t(std::lround(diag.float32[2] * 255.0f)));
    return &targets_.emplace(key, image).first->second;
  }

  DepthTarget* GetDepth(uint32_t base, uint32_t format, uint32_t pitch) {
    if (!pitch || format_depth_ == VK_FORMAT_UNDEFINED) {
      Reject(11, "no depth target (pitch 0 or format not available)");
      return nullptr;
    }
    const uint64_t key = (uint64_t(base) << 20) | (uint64_t(format) << 16) | pitch;
    auto it = depths_.find(key);
    if (it != depths_.end()) {
      return &it->second;
    }
    const uint32_t height = HeightTarget(pitch);
    // Smaller shadow map.
    //
    // A square depth target of kSideShadows pixels is drawn at bo2zm_native_shadow_scale percent and
    // upscaled when resolved, so the texture the scene samples is the usual one and only the detail goes
    // down. It is recognized by that size alone. BO2 draws its shadows into 512x512 viewports of a 560x720
    // depth target, so this never applies in BO2. The scale is read only once (when the first map is
    // created) so that changing it at runtime does not leave old images unused.
    uint32_t width_image = pitch, height_image = height;
    uint32_t width_guest = 0, height_guest = 0;
    if (pitch == kSideShadows && height == kSideShadows) {
      if (scale_shadows_ == 0) {
        scale_shadows_ = uint32_t(std::clamp(REXCVAR_GET(bo2zm_native_shadow_scale), 50, 100));
        if (!blit_ || !depth_scalable_) {
          scale_shadows_ = 100;
        }
      }
      if (scale_shadows_ < 100) {
        width_image = std::max<uint32_t>(64, ((pitch * scale_shadows_ / 100) + 15) & ~15u);
        height_image = std::max<uint32_t>(64, ((height * scale_shadows_ / 100) + 15) & ~15u);
        width_guest = pitch;
        height_guest = height;
        REXLOG_INFO("[native] C2: shadow map at {} %: drawn and resolved at {}x{} (the game asks for {}x{})",
                    scale_shadows_, width_image, height_image, pitch, height);
      }
    }
    // The usage flag that decides whether there is hierarchical depth culling.
    //
    // The driver only assigns a ZCULL plane if the usage flags fit in
    //   DEPTH_STENCIL_ATTACHMENT | TRANSFER_SRC | SAMPLED | INPUT_ATTACHMENT
    // (nvk_image.c). TRANSFER_DST disqualifies the image, and we only request it for
    // vkCmdClearDepthStencilImage, which the specification requires with that usage.
    //
    // TRANSFER_DST is dropped from the scene depth and kept for the shadow map. Reason: the scene resolves
    // 880x720 out of an 880x880 image, so it is never "whole" and never swaps with its resolved texture
    // (console logs: thousands of "without a swap ... because the command does not clear the target",
    // 0 restores). A shadow map resolved whole does swap, and that swap pays off. So the scene gains ZCULL
    // without losing a single swap.
    //
    // In exchange, that image can no longer be cleared with vkCmdClearDepthStencilImage: it is cleared
    // by opening a pass with loadOp = CLEAR (VulkanDraws::ClearDepthInPass).
    // With bo2zm_native_zcull off, every image gets TRANSFER_DST: none receives a ZCULL plane and clears
    // go back to vkCmdClearDepthStencilImage, the path used before ZCULL.
    //
    // =========================================================================================
    // Do not remove TRANSFER_DST from the shadow map. Examined in depth and rejected.
    //
    // 1. It still needs it. The reasoning was that the map is no longer copied but swapped, so the flag
    //    would be unnecessary. Wrong: each swap is paid for with a restore, and a restore is exactly a
    //    copy into this image (RestoreContent -> copy_image_ with target.image as dst, which
    //    requires TRANSFER_DST). Swaps without a clear and restores rise by the same amounts: exactly
    //    one restore per swap. The full-size copy does not go away: it is paid later, in TargetDepth.
    //    Without TRANSFER_DST that is an illegal write.
    //    Also, SwapWithResolved requires accepts_copy_target, so removing the flag would also
    //    disable the 2 swaps per frame that do pay off.
    //
    // 2. Even if it were possible, it would not help. A fit of the shadow pass's time against its
    //    triangle count, with a constant open area, leaves an intercept near zero: with the maps open and
    //    zero triangles the pass costs nothing. The whole pass is geometry, and ZCULL discards fragments,
    //    so its ceiling here is that near-zero intercept, far below what the lost swap is worth.
    //
    // 3. ZCULL for shadows only, without touching the scene depth: in NVK (nvk_cmd_draw.c,
    //    `use_zcull`) a pass opened with loadOp = LOAD_OP_CLEAR enables ZCULL even when the image has
    //    no plane. It is ephemeral, without LOAD_ZCULL/STORE_ZCULL between passes, which a shadow map
    //    drawn whole every time does not need. The shadow pass opens with LOAD, or with DONT_CARE under
    //    bo2zm_native_shadow_pass_without_load, so it gets no ephemeral ZCULL either. Given point 2, that
    //    does not pay off while the pass is pure geometry.
    // =========================================================================================
    const bool zcull = REXCVAR_GET(bo2zm_native_zcull);
    const bool is_shadows_map = (pitch == kSideShadows && height == kSideShadows);
    const bool with_transfer_dst = is_shadows_map || !zcull;
    VkImageUsageFlags use_depth = VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT |
                                        VK_IMAGE_USAGE_TRANSFER_SRC_BIT |
                                        VK_IMAGE_USAGE_SAMPLED_BIT;
    if (with_transfer_dst) {
      use_depth |= VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    }
    DepthTarget image;
    // Source of the depth copies (CopyDepth): TRANSFER_SRC is required.
    // SAMPLED: with bo2zm_native_resolve_without_copy this image can end up being the resolved texture.
    if (!Create(image, width_image, height_image, use_depth, format_depth_)) {
      Reject(12, "could not create a depth target");
      return nullptr;
    }
    image.width_guest = width_guest;
    image.height_guest = height_guest;
    image.accepts_copy_target = with_transfer_dst;
    REXLOG_INFO("[native] C2: depth target base {:03X}, format {}, {}x{}", base,
                format, pitch, height);
    return &depths_.emplace(key, image).first->second;
  }

  Resolved* GetResolved(uint32_t base, uint32_t width, uint32_t height, uint32_t format,
                            bool swap_rb, VkFormat format_host = kFormatColor) {
    if (!width || !height) {
      Reject(9, "copy to a texture of size 0");
      return nullptr;
    }
    auto it = resolved_.find(base);
    if (it != resolved_.end()) {
      if (it->second.image.width == width && it->second.image.height == height &&
          it->second.image.format == format_host) {
        if (draws_ && (it->second.format_guest != format || it->second.swap_rb != swap_rb)) {
          draws_->InvalidateTexturesIn(base);  // changes the swizzle it is sampled with
        }
        it->second.format_guest = format;
        it->second.swap_rb = swap_rb;
        it->second.image.swap_rb = swap_rb;
        return &it->second;
      }
      ForgetFrontComposition(base, "reshape");  // a different shape/format must establish presentation ownership again
      front_composition_sampled_.erase(base);
      // BO2 resolves two different shapes to the same address every frame (1C324000: the 896x720
      // scene and a 512x512 16-bit target; 1C724000: the 32x1024 grading strip and a 512x1024 target). Each switch
      // used to destroy the image after a full GPU wait, four times per frame. The image is parked instead (by
      // address, size and format) and taken back on the next resolve of that shape: nothing is destroyed, so
      // nothing waits. Not while the address's content is lent to a render target (bo2zm_native_resolve_without_copy).
      if (park_resolved_ && !lent_.count(base) && parked_.size() < 32) {
        const auto key = [base](uint32_t w, uint32_t h, VkFormat f) {
          return (uint64_t(base) << 32) ^ (uint64_t(w) << 20) ^ (uint64_t(h) << 6) ^ uint64_t(f);
        };
        Resolved old = it->second;
        const uint64_t key_old = key(old.image.width, old.image.height, old.image.format);
        const auto parked = parked_.find(key(width, height, format_host));
        if (parked != parked_.end()) {
          it->second = parked->second;
          parked_.erase(parked);
          parked_.emplace(key_old, old);
          it->second.format_guest = format;
          it->second.swap_rb = swap_rb;
          it->second.image.swap_rb = swap_rb;
          if (++resolved_unparked_ == 1 || resolved_unparked_ % 10000 == 0) {
            REXLOG_INFO("[native] C2: resolved {:08X} {}x{} taken from the parked ones ({} times, {} parked)", base,
                        width, height, resolved_unparked_, parked_.size());
          }
          if (draws_) {
            draws_->InvalidateTexturesIn(base);  // the address is now sampled from another image
          }
          ShadowMinResolvedWritten(base);
          return &it->second;
        }
        parked_.emplace(key_old, old);
        resolved_.erase(it);
        ShadowMinResolvedWritten(base);
      } else {
      // Another size or format at the same address: the old image may still be in use.
      SubmitJob(true);
      WaitGpu();
      if (draws_) {
        draws_->ForgetImage(it->second.image.image);
      }
      Destroy(it->second.image);
      resolved_.erase(it);
      // The image at that address no longer exists, so no render target owes it anything.
      if (!lent_.empty()) {
        lent_.erase(base);
      }
      ShadowMinResolvedWritten(base);  // the new image does not have its content
      }
    }
    Resolved resolved;
    // The attachment usage (color or depth) is needed for bo2zm_native_resolve_without_copy: this image can
    // end up being the render target it is swapped with.
    const VkImageUsageFlags use_target = IsDepthFormat(format_host)
                                              ? VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT
                                              : VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
    if (!Create(resolved.image, width, height,
               VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT |
                   VK_IMAGE_USAGE_TRANSFER_DST_BIT | use_target,
               format_host)) {
      Reject(10, "could not create a resolved texture");
      return nullptr;
    }
    resolved.format_guest = format;
    resolved.swap_rb = swap_rb;
    resolved.image.swap_rb = swap_rb;
    REXLOG_INFO("[native] C2: resolved texture at {:08X}, {}x{}, format {}", base, width, height,
                format);
    if (draws_) {
      draws_->InvalidateTexturesIn(base);  // that address is now sampled from the resolved texture
    }
    return &resolved_.emplace(base, resolved).first->second;
  }

  // One more copy in its size bucket, to tell whether the copy milliseconds come from pixels or from a
  // fixed cost per copy.
  void AnnotateCopy(uint32_t width, uint32_t height) {
    const uint64_t pixels = uint64_t(width) * height;
    for (uint32_t c = 0; c < kBucketsCopy; ++c) {
      if (pixels <= kPixelsBucket[c]) {
        ++copies_bucket_[c];
        pixels_bucket_[c] += pixels;
        return;
      }
    }
  }

  // The setting, or its alternating test.
  bool ResolveNoCopy() {
    const int32_t alternate = REXCVAR_GET(bo2zm_native_resolve_without_copy_alternate_s);
    if (alternate <= 0) {
      return REXCVAR_GET(bo2zm_native_resolve_without_copy);
    }
    const auto seconds = std::chrono::duration_cast<std::chrono::seconds>(
                              std::chrono::steady_clock::now() - start_alternation_)
                              .count();
    const bool no_copy = (seconds / alternate) % 2 == 1;
    if (no_copy != alternation_annotated_) {
      alternation_annotated_ = no_copy;
      REXLOG_INFO("[native] C2 resolve: {}", no_copy ? "no copy (swap)" : "copying");
    }
    return no_copy;
  }

  static bool IsDepthFormat(VkFormat format) {
    return format == VK_FORMAT_D24_UNORM_S8_UINT || format == VK_FORMAT_D32_SFLOAT_S8_UINT ||
           format == VK_FORMAT_D16_UNORM || format == VK_FORMAT_D32_SFLOAT ||
           format == VK_FORMAT_X8_D24_UNORM_PACK32;
  }

  // Resolve without copying. The whole render target becomes the resolved texture and that texture's
  // old image stays as the render target. What the game sees is the same; what is saved is moving the
  // pixels. false if it is not possible.
  bool SwapWithResolved(OwnedImage& target, Resolved& resolved, uint32_t base) {
    if (target.width != resolved.image.width || target.height != resolved.image.height ||
        target.format != resolved.image.format) {
      return false;
    }
    // ZCULL safety net. If the render target was created without TRANSFER_DST (so it can have a ZCULL
    // plane), swapping it would put it in the resolved texture's place, which does receive copies: an
    // illegal write. And the other way round, the resolved texture (with TRANSFER_DST and no plane) would
    // become the depth target and ZCULL would switch off on alternate frames. Fall back to the usual copy
    // path instead.
    if (!target.accepts_copy_target) {
      return false;
    }
    // bo2zm_native_diag_clears. The whole image becomes a sampled texture, so all of it counts as used
    // (noted before the images change places).
    AnnotateUseClear(target, target.width, target.height);
    // Careful: ForgetImage must not be called here. It frees the descriptor slot and reuses it in the
    // same frame, so draws already recorded with it end up reading another image (flicker). Bumping the
    // generation is enough: later draws resolve their view again, and existing views stay valid because
    // they go with their image.
    // bo2zm_native_lazy_depth. The render target changes image: first, whatever it has deferred
    // as a source is resolved; and the texture receives the whole target, so its deferred copy is no
    // longer needed.
    if (!pending_.empty()) {
      BeforeWriteDepth(target);
      if (pending_.erase(base)) {
        ++lazy_replaced_;
      }
    }
    // bo2zm_native_lazy_front. Same for front buffers: deferred copies whose source is this target
    // are recorded first, and the texture receives the whole target (its deferred copy is unnecessary).
    if (!fronts_pending_.empty()) {
      BeforeWriteColor(target);
      ResolveFrontEarlier(base, true);
    }
    std::swap(target.image, resolved.image.image);
    std::swap(target.guest_memory, resolved.image.guest_memory);
    std::swap(target.view, resolved.image.view);
    std::swap(target.prepared, resolved.image.prepared);
    resolved.image.swap_rb = resolved.swap_rb;  // belongs to the content, not the image
    resolved.image.content_invalid = false;
    target.content_invalid = true;  // its content is now in the resolved texture
    target.resolved_base = base;
    ++swaps_;
    ResolvedWritten(base);   // if it was lent, it has valid content again
    ForgetClear(target);  // and the target keeps another image: its clear no longer holds
    if (draws_) {
      // Only what points to these two images: invalidating the whole cache here costs more than the copy
      // it saves (measured on PC).
      draws_->InvalidateImages(target.image, resolved.image.image);
    }
    return true;
  }

  // If the render target lost its content in an image swap and the game is about to draw on top, the
  // content is brought back from the resolved texture.
  //
  // In gameplay this is not rare: it happens once for every swap without a clear (measured: the same number
  // of swaps and restores in consecutive intervals), a cost per frame that no bucket of the copy inventory
  // accounted for. Here they are counted, trimmed to the useful area and, if requested, done without copying
  // a single pixel.
  //
  // for_resolve = requested by a resolve (bo2zm_native_resolve_valid_content), not by a pass that
  // will draw on top. Then it is only copied: no lending (it would leave the source texture without
  // content just when the overlay's materials are about to read it) and no bo2zm_native_shadow_min clear (that
  // is for the overlay pass).
  void RestoreContent(OwnedImage& target, bool for_resolve = false) {
    if (!target.content_invalid) {
      return;
    }
    target.content_invalid = false;
    // bo2zm_native_lazy_depth. The resolved texture is about to be read (if its copy was
    // deferred, it is recorded first) and the render target written (first, whatever it has deferred as
    // a source).
    if (!pending_.empty()) {
      if (pending_.count(target.resolved_base)) {
        RecordCopyPending(target.resolved_base);
        ++lazy_copied_read_;
      }
      BeforeWriteDepth(target);
    }
    // bo2zm_native_lazy_front. Same for front buffers: if the texture about to be read has a
    // deferred copy, it is recorded first; and so are deferred copies whose source is this render target.
    if (!fronts_pending_.empty()) {
      if (fronts_pending_.count(target.resolved_base)) {
        RecordCopyFront(target.resolved_base);
        ++front_copied_read_;
      }
      BeforeWriteColor(target);
    }
    auto it = resolved_.find(target.resolved_base);
    if (it == resolved_.end() || it->second.image.width != target.width ||
        it->second.image.height != target.height || it->second.image.format != target.format ||
        !copy_image_ || !Record()) {
      if (for_resolve) {
        ++resolve_no_source_;  // unknown where to bring it from; resolve whatever is there
      }
      return;
    }
    if (for_resolve) {
      // bo2zm_native_resolve_valid_content. Only the copy, with its barriers.
      CopyBackForResolve(target, it->second.image);
      return;
    }
    // The content does not need duplicating, only to be in the render target: swapping the two images
    // back puts it where it belongs without moving a byte. The resolved texture stays lent until the next
    // resolve to that address; 'loaned reads' checks that nobody looks at it in the meantime.
    // Both must already be in GENERAL: the swap moves the image, not its layout, and preparing after the
    // swap would put the barrier on the wrong image.
    /*
     * Why the restore does not swap.
     *
     * Restores copy a full map per frame, and log report after log report shows a saving of
     * 0.00. This path exists precisely to avoid the copy, and the counter says "0 by swap": it
     * never activates. It has three conditions, so each one is counted instead of guessing which one
     * fails. Three uint64 increments on a path that already copies megabytes: zero cost.
     */
    if (REXCVAR_GET(bo2zm_native_restore_by_swap)) {
      if (!target.accepts_copy_target) {
        ++no_swap_no_transfer_dst_;
      } else if (!target.prepared) {
        ++no_swap_target_no_prepare_;
      } else if (!it->second.image.prepared) {
        ++no_swap_resolved_no_prepare_;
      } else if (target.width != it->second.image.width ||
                 target.height != it->second.image.height) {
        // Not a condition of the if below, but it does matter: the swap moves the whole image, and if the
        // sizes do not match it would leave the render target with the wrong dimensions. If this count is
        // high, the lever cannot work as written.
        ++no_swap_sizes_different_;
      }
    }
    if (REXCVAR_GET(bo2zm_native_restore_by_swap) && target.accepts_copy_target &&
        target.prepared && it->second.image.prepared) {
      const uint32_t base = target.resolved_base;
      std::swap(target.image, it->second.image.image);
      std::swap(target.guest_memory, it->second.image.guest_memory);
      std::swap(target.view, it->second.image.view);
      std::swap(target.prepared, it->second.image.prepared);
      it->second.image.content_invalid = false;
      if (draws_) {
        draws_->InvalidateImages(target.image, it->second.image.image);
      }
      lent_.insert(base);
      ++loans_;
      ++restores_;
      pixels_restore_saved_ += uint64_t(target.width) * target.height;
      ForgetClear(target);
      return;
    }
    // bo2zm_native_shadow_min. The overlay pass of the shadow map: while it is being applied, the render
    // target is cleared to 1.0 instead of copying the world back (the world draws take the minimum).
    if (ShadowMinInsteadOfRestore(target)) {
      return;
    }
    Prepare(it->second.image);
    Prepare(target);
    const VkImageAspectFlags aspect =
        IsDepthFormat(target.format) ? VK_IMAGE_ASPECT_DEPTH_BIT : VK_IMAGE_ASPECT_COLOR_BIT;
    // Render targets are created with height = max(720, pitch): the scene measures 880x880 to draw
    // 880x720, and narrow targets are taller still. What lies below the useful area is never
    // drawn or resolved, so copying it wastes bandwidth. The useful area is the largest y1 the game has
    // asked to resolve from this target; until there is data, the whole target is copied.
    uint32_t height = target.height;
    if (REXCVAR_GET(bo2zm_native_restore_useful_area)) {
      const auto e = state_target_.find(&target);
      if (e != state_target_.end() && e->second.height_used &&
          e->second.height_used < target.height) {
        height = e->second.height_used;
        ++restores_clipped_;
        pixels_restore_saved_ += uint64_t(target.width) * (target.height - height);
      }
    }
    VkImageCopy copy{};
    copy.srcSubresource = {aspect, 0, 0, 1};
    copy.dstSubresource = {aspect, 0, 0, 1};
    copy.extent = {target.width, height, 1};
    AnnotateUseClear(target, target.width, height);  // bo2zm_native_diag_clears
    MarkGpu(kGpuCopies);
    copy_image_(commands_job_, it->second.image.image, VK_IMAGE_LAYOUT_GENERAL,
                   target.image, VK_IMAGE_LAYOUT_GENERAL, 1, &copy);
    ++restores_;
    pixels_restored_ += uint64_t(target.width) * height;
    AnnotateCopy(target.width, height);  // so the copy inventory adds up to the measured ms
    ForgetClear(target);
  }

  /*
   * bo2zm_native_resolve_valid_content. The render target gets its content back from the resolved
   * texture where the image swap left it, just before a resolve reads it.
   *
   * The copy goes between two barriers, and here they are needed: the source is the image that was just
   * drawn as the depth target (the previous resolve moved it into the texture by swapping), and what is
   * copied will be sampled as soon as the current resolve swaps it. NVK only waits for the GPU and
   * flushes the texture cache at a barrier (in gameplay the restore still goes without them, as always:
   * this does not touch it). Each one costs a pipeline drain, twice per frame in the menu. The whole
   * target is copied: that is what the EDRAM held and what the resolve reads.
   */
  void CopyBackForResolve(OwnedImage& target, OwnedImage& source) {
    Prepare(source);
    Prepare(target);
    const VkImageAspectFlags aspect =
        IsDepthFormat(target.format) ? VK_IMAGE_ASPECT_DEPTH_BIT : VK_IMAGE_ASPECT_COLOR_BIT;
    VkMemoryBarrier barrier{};
    barrier.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER;
    barrier.srcAccessMask = VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT |
                            VK_ACCESS_TRANSFER_WRITE_BIT;
    barrier.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT | VK_ACCESS_TRANSFER_WRITE_BIT;
    constexpr VkPipelineStageFlags kWrites =
        VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT |
        VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT | VK_PIPELINE_STAGE_TRANSFER_BIT;
    dfn_.vkCmdPipelineBarrier(commands_job_, kWrites, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 1, &barrier, 0,
                              nullptr, 0, nullptr);
    VkImageCopy copy{};
    copy.srcSubresource = {aspect, 0, 0, 1};
    copy.dstSubresource = {aspect, 0, 0, 1};
    copy.extent = {target.width, target.height, 1};
    AnnotateUseClear(target, target.width, target.height);  // bo2zm_native_diag_clears, like the restore
    MarkGpu(kGpuCopies);
    copy_image_(commands_job_, source.image, VK_IMAGE_LAYOUT_GENERAL, target.image, VK_IMAGE_LAYOUT_GENERAL,
                   1, &copy);
    // And the copied data made visible to what follows: the current resolve (copy or swap), the draws that
    // sample the texture that receives it, and the pass that draws to the render target again.
    barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    barrier.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT | VK_ACCESS_TRANSFER_WRITE_BIT | VK_ACCESS_SHADER_READ_BIT |
                            VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT |
                            VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
    dfn_.vkCmdPipelineBarrier(commands_job_, VK_PIPELINE_STAGE_TRANSFER_BIT,
                              VK_PIPELINE_STAGE_TRANSFER_BIT | VK_PIPELINE_STAGE_VERTEX_SHADER_BIT |
                                  VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT |
                                  VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT,
                              0, 1, &barrier, 0, nullptr, 0, nullptr);
    ++restores_for_resolve_;
    pixels_restored_ += uint64_t(target.width) * target.height;
    AnnotateCopy(target.width, target.height);
    ForgetClear(target);
  }

  // --- bo2zm_native_shadow_min (see the cvar comment) --------------------------------------------------

  // A failure while applying may change the image of this very frame: MISMATCH in the log and it
  // switches off for the session. Textures that already hold only the overlay keep being served with the
  // minimum until they are written again.
  void ShadowMinMismatch(const char* reason, uint32_t datum) {
    ++sm_mismatches_;
    if (sm_phase_ != kSmOff) {
      sm_phase_ = kSmOff;
      REXLOG_ERROR("[native] C2 shadow by minimum: MISMATCH ({}; datum {:08X}). Turned off for the session: the shadow "
                   "map is copied again as before",
                   reason, datum);
    }
  }

  // Something the minimum would not reproduce, seen before any image changed: not applied this session.
  void ShadowMinNoFit(const char* reason, uint32_t datum) {
    if (sm_phase_ != kSmOff) {
      sm_phase_ = kSmOff;
      REXLOG_WARN("[native] C2 shadow by minimum: not applied in this session ({}; datum {:08X}); the shadow map is "
                  "still copied as always",
                  reason, datum);
    }
  }

  // A failure within the current cycle. If the render target was already cleared (applying) it is a
  // MISMATCH; otherwise either the minimum does not work for this game (permanent) or the cycle simply
  // does not count (transient: menus, loading).
  void ShadowMinMissCycle(const char* reason, uint32_t datum, bool permanent) {
    sm_cycle_clean_ = false;
    if (sm_cycle_clear_) {
      ShadowMinMismatch(reason, datum);
    } else if (permanent) {
      ShadowMinNoFit(reason, datum);
    }
  }

  // The setting for the cycle that starts: the cvar, its alternating test and the cheap PCF.
  bool ShadowMinOn() {
    if (!REXCVAR_GET(bo2zm_native_shadow_min) || !REXCVAR_GET(bo2zm_native_cheap_pcf)) {
      return false;
    }
    const int32_t alternate = REXCVAR_GET(bo2zm_native_shadow_min_alternate_s);
    if (alternate <= 0 || sm_phase_ != kSmApplying) {
      return true;
    }
    const auto seconds = std::chrono::duration_cast<std::chrono::seconds>(std::chrono::steady_clock::now() -
                                                                           sm_start_alternation_)
                              .count();
    const bool with_min = (seconds / alternate) % 2 == 0;
    if (with_min != sm_span_annotated_) {
      sm_span_annotated_ = with_min;
      REXLOG_INFO("[native] C2 shadow by minimum (alternation): {}", with_min ? "with the minimum" : "copying");
    }
    return with_min;
  }

  // After resolving a whole depth target by swapping images (CopyDepth). Without a clear it
  // starts a cycle (the texture receives the base map); with a clear, after the overlay pass, it ends it (the
  // texture receives the overlay).
  void ShadowMinAfterSwap(const OwnedImage& target, uint32_t base, bool would_clear) {
    if (target.width != kSideShadows || target.height != kSideShadows) {
      return;
    }
    if (!would_clear) {
      if (sm_state_ == kSmOverlay && sm_cycle_clear_) {
        ShadowMinMismatch("the target with the overlay alone is resolved without clearing", base);
      }
      if (sm_state_ != kSmFree) {
        sm_clean_consecutive_ = 0;  // the previous cycle did not close: count from scratch
      }
      sm_state_ = kSmAfterBase;
      sm_target_cycle_ = &target;
      sm_base_cycle_ = base;
      sm_cycle_clean_ = true;
      sm_cycle_clear_ = false;
      sm_base_rewritten_ = false;
      sm_on_ = ShadowMinOn();
      return;
    }
    if (sm_state_ != kSmOverlay || &target != sm_target_cycle_) {
      if (sm_state_ != kSmFree) {
        sm_clean_consecutive_ = 0;
      }
      sm_state_ = kSmFree;
      return;
    }
    sm_state_ = kSmFree;
    ++sm_cycles_;
    if (sm_cycle_clear_) {
      // The texture has just received only the overlay: from here on it is sampled paired with the base map.
      if (base != sm_overlay_) {
        ShadowMinMismatch("the overlay resolve goes to another texture", base);
        sm_overlay_ = base;  // it holds only the overlay: serve that one
      }
      sm_virtual_ = true;
      sm_virtual_valid_ = !sm_base_rewritten_;
      if (sm_base_rewritten_) {
        ShadowMinMismatch("the world texture was written again inside the cycle", sm_base_);
      }
      ++sm_cycles_applied_;
      return;
    }
    // Observing (or applying with the setting off in this cycle): the texture is the usual one, with the
    // base map and the overlay.
    if (base != sm_overlay_ || sm_base_cycle_ != sm_base_ || sm_target_cycle_ != sm_target_) {
      if (sm_phase_ == kSmApplying) {
        // A cycle different from the learned one (split screen, another mode): it cannot be served.
        // Stop applying.
        ShadowMinNoFit("a shadow map cycle different from the learned one appears (the datum is its texture)", base);
        return;
      }
      sm_overlay_ = base;  // learned (or changed): count again from zero
      sm_base_ = sm_base_cycle_;
      sm_target_ = sm_target_cycle_;
      sm_clean_consecutive_ = 0;
    }
    sm_clean_consecutive_ = (sm_cycle_clean_ && !sm_base_rewritten_) ? sm_clean_consecutive_ + 1 : 0;
    if (sm_phase_ == kSmObserving && sm_on_ && sm_clean_consecutive_ >= kShadowMinCycles) {
      sm_phase_ = kSmApplying;
      REXLOG_INFO("[native] C2 shadow by minimum: APPLYING after {} clean cycles in a row (target {}x{}, base at "
                  "{:08X}, overlay at {:08X}): the overlay pass is drawn onto the cleared target and not copied",
                  sm_clean_consecutive_, target.width, target.height, sm_base_, sm_overlay_);
    }
  }

  // From RestoreContent: the render target lost its content in a swap and is about to be drawn on.
  // If this is the overlay pass of the current cycle it is noted, and while applying, the target is cleared
  // to 1.0 instead of bringing the world back (true: no copy needed).
  bool ShadowMinInsteadOfRestore(OwnedImage& target) {
    if (&target != sm_target_cycle_ || sm_state_ != kSmAfterBase) {
      if (&target == sm_target_cycle_ && sm_state_ == kSmOverlay) {
        ShadowMinMissCycle("the overlay target is restored again", target.resolved_base, false);
      }
      return false;
    }
    if (target.resolved_base != sm_base_cycle_) {
      ShadowMinMissCycle("the target is restored from another texture", target.resolved_base, false);
      return false;
    }
    sm_state_ = kSmOverlay;
    // Only with the learned cycle: the same render target and the same world texture as in the observing
    // phase.
    if (sm_phase_ != kSmApplying || !sm_on_ || &target != sm_target_ || sm_base_cycle_ != sm_base_ ||
        !sm_overlay_ || !clear_depth_ || !target.accepts_copy_target) {
      return false;
    }
    Prepare(target);
    MarkGpu(kGpuClears);
    const VkClearDepthStencilValue value{1.0f, 0};
    clear_depth_(commands_job_, target.image, VK_IMAGE_LAYOUT_GENERAL, &value, 1, &kRangeDepth);
    AnnotateUseClear(target, target.width, target.height);  // bo2zm_native_diag_clears: like the restore
    ForgetClear(target);  // not a game clear: the game's next clear cannot be skipped
    sm_cycle_clear_ = true;
    sm_pixels_saved_ += uint64_t(target.width) * target.height;
    return true;
  }

  // A copy (not a swap) from a depth target: if it is the current cycle's target, something reads its
  // content mid-cycle. With the target already cleared, what gets copied is only the overlay.
  void ShadowMinCopyFrom(const OwnedImage& source) {
    if (&source == sm_target_cycle_ && sm_state_ != kSmFree) {
      ShadowMinMissCycle("the shadow map target is copied from in the middle of the cycle", 0, false);
    }
  }

  // The game clears the current cycle's target without the second resolve: its content is lost the same
  // way on both paths, but in this cycle the texture with the overlay is not renewed while the base one
  // is. The cycle does not count.
  void ShadowMinBeforeClear(const OwnedImage& target) {
    if (&target == sm_target_cycle_ && sm_state_ != kSmFree) {
      sm_clean_consecutive_ = 0;
      sm_state_ = kSmFree;
    }
  }

  // A resolved texture receives new content (ResolvedWritten) or is recreated (GetResolved).
  void ShadowMinResolvedWritten(uint32_t base) {
    if (sm_virtual_) {
      if (base == sm_overlay_) {
        sm_virtual_ = false;  // new content: no longer only the overlay
      } else if (base == sm_base_) {
        sm_virtual_valid_ = false;  // its pair changes: reading it now would not give its cycle's minimum
      }
    }
    if (sm_state_ != kSmFree && base == sm_base_cycle_) {
      sm_base_rewritten_ = true;
    }
  }

  // C2 report line (every 10 s, with the other copy lines). The ms use 0.60 per Mpixel copied and 0.075
  // per Mpixel cleared.
  void ReportShadowMin() {
    if (!sm_cycles_ && !sm_mismatches_ && !sm_reads_unable_ && !sm_reads_a_offbeat_) {
      return;
    }
    const double frames = double(presented_ - presented_report_copies_);
    const double perFrm = frames > 0.0 ? 1.0 / frames : 0.0;
    const double mp = double(sm_pixels_saved_ - sm_pixels_saved_previous_) / 1e6 * perFrm;
    static constexpr const char* kPhases[3] = {"observing", "APPLYING", "off"};
    BO2ZM_REPORT_RING(
        "[native] C2 shadow by minimum: phase {}; {} cycles since boot ({} clean in a row, {} applied); "
        "{:.2f} Mpixels per frame cleared instead of copied (~{:.2f} ms real of copy less and ~{:.2f} of clear more); "
        "overlay draws {} ({} not exact); reads of {:08X} since boot: {} with the paired base, {} with itself, "
        "{} without the minimum, {} between the two resolves, {} without tfetch2DShadowMin; MISMATCHES {}{}",
        kPhases[sm_phase_ < 3 ? sm_phase_ : 2], sm_cycles_, sm_clean_consecutive_, sm_cycles_applied_, mp, mp * 0.60,
        mp * 0.075, sm_draws_overlay_, sm_draws_no_exact_, sm_overlay_, sm_reads_min_, sm_reads_yes_same_,
        sm_reads_normal_, sm_reads_a_offbeat_, sm_reads_unable_, sm_mismatches_,
        sm_mismatches_ ? " *** THE IMAGE MAY HAVE CHANGED: see the ERROR in the log ***" : " (0 = the image is the same)");
    sm_pixels_saved_previous_ = sm_pixels_saved_;
  }

  // The resolved texture has just received new content, so if it was lent it no longer is. An empty set
  // (the normal case) touches nothing. `pixels` = the pixels actually copied (0 if the resolve swapped
  // images), so that the per-target inventory shows where the traffic goes.
  void ResolvedWritten(uint32_t base, uint64_t pixels = 0) {
    // Whatever samples this address next goes back through TextureResolved (which counts the reads,
    // records deferred copies and rebuilds the 3D table built from it), as with a cache invalidation on every
    // copy, but the rest of the sampler cache stays valid (VulkanDraws::InvalidateTexturesIn).
    if (draws_) {
      draws_->InvalidateTexturesIn(base);
    }
    ShadowMinResolvedWritten(base);  // bo2zm_native_shadow_min
    if (!volumes_.empty()) {  // A 3D texture built from it has to be rebuilt
      writes_resolved_[base] = ++counter_writes_;
    }
    if (!lent_.empty()) {
      lent_.erase(base);
    }
    if (!expired_.empty()) {
      expired_.erase(base);  // New contents: no dropped copy is missing any more
    }
    if (pixels) {
      const auto c = copies_per_target_.find(base);
      if (c != copies_per_target_.end()) {
        c->second.pixels += pixels;
      }
    }
  }

  // The render target's content is no longer that of its last clear (something was copied over it or
  // it changed image). The game's next clear cannot be skipped.
  //
  // It also answers the other question about clears: if the target's content changes without anything
  // drawn since it was cleared, that clear was wiped out by a swap or a restore and served no purpose.
  // It is the equivalent of a loadOp = CLEAR that gets thrown away: these are DONT_CARE candidates.
  void ForgetClear(const OwnedImage& target) {
    const auto e = state_target_.find(&target);
    if (e == state_target_.end()) {
      return;
    }
    if (FrontKnownClearValid(e->second.clear_clean, e->second.draws_on_clear, draws_ ? draws_->Drawn() : 0)) {
      ++clears_useless_;
      pixels_clears_useless_ += uint64_t(target.width) * target.height;
    }
    e->second.clear_clean = false;
  }

  // The largest rectangle the game resolves from this render target: the area it actually uses.
  void AnnotateAreaUseful(const OwnedImage& target, int32_t y1) {
    if (y1 > 0) {
      auto& e = state_target_[&target];
      e.height_used = std::max(e.height_used, std::min(uint32_t(y1), target.height));
    }
  }

  // A clear that does not change a single bit. It is skipped if the render target is already cleared
  // to that same value and nothing has been drawn, anywhere, since then. Returns true if it must be
  // skipped.
  bool ClearRedundant(const OwnedImage& target, uint64_t value) {
    auto& e = state_target_[&target];
    const uint64_t drawn = draws_ ? draws_->Drawn() : 0;
    if (REXCVAR_GET(bo2zm_native_skip_repeated_clears) && e.value_clear == value &&
        FrontKnownClearValid(e.clear_clean, e.draws_on_clear, drawn)) {
      pixels_clears_skipped_ += uint64_t(target.width) * target.height;
      return true;
    }
    e.clear_clean = true;
    e.value_clear = value;
    e.draws_on_clear = drawn;
    return false;
  }

  bool Create(OwnedImage& image, uint32_t width, uint32_t height, VkImageUsageFlags use,
             VkFormat format = kFormatColor) {
    VkImageCreateInfo info{};
    info.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    info.imageType = VK_IMAGE_TYPE_2D;
    info.format = format;
    info.extent = {width, height, 1};
    info.mipLevels = 1;
    info.arrayLayers = 1;
    info.samples = VK_SAMPLE_COUNT_1_BIT;
    info.tiling = VK_IMAGE_TILING_OPTIMAL;
    info.usage = use;
    info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    info.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    if (!rex::ui::vulkan::util::CreateDedicatedAllocationImage(
            vulkan_device_, info, rex::ui::vulkan::util::MemoryPurpose::kDeviceLocal, image.image,
            image.guest_memory)) {
      // The GPU can run out of memory here too. Render targets are few and large, so one that does not fit
      // shows up at once: the texture cache is asked to release memory and the allocation is retried once.
      if (!draws_ || !draws_->ReleaseTexturesOnOutOfMemory() ||
          !rex::ui::vulkan::util::CreateDedicatedAllocationImage(
              vulkan_device_, info, rex::ui::vulkan::util::MemoryPurpose::kDeviceLocal, image.image,
              image.guest_memory)) {
        REXLOG_ERROR("[native] C2: out of GPU memory for a {}x{} target (format {}) and releasing the cache was not "
                     "enough",
                     width, height, uint32_t(format));
        return false;
      }
    }
    VkImageViewCreateInfo info_view{};
    info_view.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    info_view.image = image.image;
    info_view.viewType = VK_IMAGE_VIEW_TYPE_2D;
    info_view.format = format;
    info_view.subresourceRange = IsDepthFormat(format) ? kRangeDepth : kRangeColor;
    if (dfn_.vkCreateImageView(device_, &info_view, nullptr, &image.view) != VK_SUCCESS) {
      Destroy(image);
      return false;
    }
    image.width = width;
    image.height = height;
    image.format = format;
    image.prepared = false;
    return true;
  }

  void Destroy(OwnedImage& image) {
    if (front_compositions_.count(&image) && FrontTraceLine()) {
      REXLOG_INFO("[front-trace] invalidate target {:p} reason destroy frame {}",
                  static_cast<const void*>(&image), presented_);
    }
    front_compositions_.erase(&image);
    CompositeOnDestroy(image);  // bo2zm_native_lazy_composite
    // bo2zm_native_lazy_front. A deferred copy cannot keep a destroyed image. If the texture is
    // what gets destroyed (GetResolved recreates it with another size or format), its content is lost
    // just as before: the copy is simply dropped. If it is the source (only at shutdown: render targets
    // are not destroyed, and whatever leaves a target through a swap is recorded first), the texture lacks
    // that copy: it becomes stale and, if something requests it before another full resolve, the guard
    // trips.
    if (!fronts_pending_.empty() && image.image != VK_NULL_HANDLE) {
      for (auto it = fronts_pending_.begin(); it != fronts_pending_.end();) {
        if (it->second.texture_vk == image.image) {
          FreeRetained(it->second);
          it = fronts_pending_.erase(it);
        } else if (it->second.source_vk == image.image) {
          fronts_expired_.insert(it->first);
          FreeRetained(it->second);
          it = fronts_pending_.erase(it);
        } else {
          ++it;
        }
      }
    }
    // bo2zm_native_lazy_depth. Without the image there is no copy to do: its texture becomes
    // stale (if something samples it before the next resolve, the guard trips).
    if (!pending_.empty() && image.image != VK_NULL_HANDLE) {
      for (auto it = pending_.begin(); it != pending_.end();) {
        if (it->second.source_vk == image.image || it->second.target_vk == image.image) {
          expired_.insert(it->first);
          it = pending_.erase(it);
        } else {
          ++it;
        }
      }
    }
    // The VulkanDraws framebuffers that use this view are destroyed before it: Vulkan may give the same
    // handle to a new view and FramebufferOf would return a stale one (see
    // bo2zm_native_framebuffers_forget_views). At shutdown, draws_ is already gone and so are its
    // framebuffers.
    if (draws_ && image.view != VK_NULL_HANDLE) {
      draws_->ForgetView(image.view);
    }
    if (image.view != VK_NULL_HANDLE) {  // Its CopyConverted framebuffer goes too
      const auto fb = conv_framebuffers_.find(image.view);
      if (fb != conv_framebuffers_.end()) {
        if (fb->second != VK_NULL_HANDLE) dfn_.vkDestroyFramebuffer(device_, fb->second, nullptr);
        conv_framebuffers_.erase(fb);
      }
    }
    if (image.view != VK_NULL_HANDLE) dfn_.vkDestroyImageView(device_, image.view, nullptr);
    if (image.image != VK_NULL_HANDLE) dfn_.vkDestroyImage(device_, image.image, nullptr);
    if (image.guest_memory != VK_NULL_HANDLE) dfn_.vkFreeMemory(device_, image.guest_memory, nullptr);
    image = OwnedImage{};
  }

  // First time: to GENERAL (valid for copy, clear and sampling) and cleared to zero.
  void Prepare(OwnedImage& image) {
    if (image.prepared) {
      return;
    }
    // In the upload command buffer, which runs before the work one (the reflection cubemap copies
    // its resolved faces there and, on its first frame, read them unprepared).
    const VkCommandBuffer commands = CommandsUpload();
    if (commands == VK_NULL_HANDLE) {
      return;
    }
    VkImageMemoryBarrier barrier{};
    barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    barrier.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    barrier.newLayout = VK_IMAGE_LAYOUT_GENERAL;
    barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    const bool depth = image.format == VK_FORMAT_D24_UNORM_S8_UINT ||
                             image.format == VK_FORMAT_D32_SFLOAT_S8_UINT;
    barrier.image = image.image;
    barrier.subresourceRange = depth ? kRangeDepth : kRangeColor;
    barrier.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT | VK_ACCESS_TRANSFER_WRITE_BIT;
    dfn_.vkCmdPipelineBarrier(commands, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                              VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr, 1,
                              &barrier);
    if (depth) {
      // ZCULL: images created without TRANSFER_DST (the ones eligible for a ZCULL plane) cannot be
      // cleared with vkCmdClearDepthStencilImage. They are cleared by opening a pass with
      // loadOp = CLEAR. The barrier above (UNDEFINED -> GENERAL) stays as it is: it is what makes the
      // driver zero the ZCULL plane, and without it the hardware kills the context.
      if (!image.accepts_copy_target && draws_) {
        if (!draws_->ClearDepthInPass(commands, image, 1.0f, 0) &&
            ++clears_in_pass_failed_ <= 8) {
          REXLOG_WARN("[native] C2: could NOT prepare the {}x{} depth by opening a clear pass (error {})",
                      image.width, image.height, clears_in_pass_failed_);
        }
      } else if (clear_depth_) {
        const VkClearDepthStencilValue far_depth{1.0f, 0};
        clear_depth_(commands, image.image, VK_IMAGE_LAYOUT_GENERAL, &far_depth, 1,
                            &kRangeDepth);
      }
    } else {
      const VkClearColorValue cero{};
      dfn_.vkCmdClearColorImage(commands, image.image, VK_IMAGE_LAYOUT_GENERAL, &cero,
                                1, &kRangeColor);
    }
    image.prepared = true;
    if (draws_) {
      draws_->InvalidateTextures();  // TextureResolved only returns prepared ones
    }
  }

  bool Record() {
    if (recording_) {
      return true;
    }
    const auto before_record = std::chrono::steady_clock::now();
    const bool recording = BeginRecording();
    ns_record_ += uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(
        std::chrono::steady_clock::now() - before_record).count());
    ++recordings_;
    return recording;
  }

  bool BeginRecording() {
    // Slots rotate: while one frame is recorded, the previous ones can still be on the GPU. It only waits
    // if this slot's last submission has not finished yet, so the more slots there are, the further ahead
    // the CPU can get before it has to stop.
    slot_ = (slot_ + 1) % slots_used_;
    WorkSlot& slot = slots_[slot_];
    Complete(slot);
    deferred::Finish();  // nothing of an earlier recording may still be queued when its pool is reset
    const auto before_pools = std::chrono::steady_clock::now();
    dfn_.vkResetCommandPool(device_, slot.pool_job, 0);
    dfn_.vkResetCommandPool(device_, slot.pool_upload, 0);
    ns_reset_pools_ += uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(
        std::chrono::steady_clock::now() - before_pools).count());
    commands_job_ = slot.job;
    commands_upload_ = slot.upload;
    recording_upload_ = false;
    VkCommandBufferBeginInfo start{};
    start.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    start.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    if (dfn_.vkBeginCommandBuffer(commands_job_, &start) != VK_SUCCESS) {
      return false;
    }
    deferred::Begin(commands_job_);  // from here to SubmitJob its vkCmd* calls are queued
    slot.categories.clear();
    if (queries_ != VK_NULL_HANDLE) {
      dfn_.vkCmdResetQueryPool(commands_job_, queries_, slot_ * kMarksPerSlot,
                               kMarksPerSlot);
    }
    if (occlusions_ != VK_NULL_HANDLE) {
      DiscardOcclusions(slot);  // Complete already read the last submission's; these were never submitted
      dfn_.vkCmdResetQueryPool(commands_job_, occlusions_, slot_ * kOcclusionsPerSlot,
                               kOcclusionsPerSlot);
    }
    // Reflection visibility. Any left over belong to work that was never submitted: without a
    // measurement, they count as visible. The reset always happens when there is a pool, even if the
    // guard trips mid-frame.
    if (visibility_ != VK_NULL_HANDLE) {
      if (!slot.visibility.empty()) {
        bo2zm::reflection_demand::AnnotateVisible(false);
        slot.visibility.clear();
      }
      dfn_.vkCmdResetQueryPool(commands_job_, visibility_, slot_ * kVisibilityPerSlot,
                               kVisibilityPerSlot);
    }
    /*
     * Do not reset pools that this frame will not use.
     *
     * Resetting every pool on every frame meant 64 statistics queries plus 2,048 per-draw statistics queries: 2,144
     * resets per frame. On NVK for Tegra the bulk reset path is excluded for the layout of these pools
     * (nvk_query_pool.c: everything that is not a timestamp falls into ALIGNED_INTERLEAVED), so each reset is a 5-dword
     * SET_REPORT_SEMAPHORE with RELEASE_AFTER_ALL_PRECEEDING_WRITES_COMPLETE and PIPELINE_LOCATION_ALL: 2,144 chained
     * writes that wait on each other, 42.9 KB of command stream, at the start of every frame.
     *
     * And the pools are almost never used: the per-pass statistics are effectively disabled (see the `> 0` check in
     * BeginStats: with per_draw_stats_s set, the per-pass query is never opened), and the per-draw ones are only used
     * one frame every 20 seconds.
     *
     * The reset comes before the first MarkGpu, so its cost shows up in no category of the breakdown; it hides in the
     * "gap between jobs", bounded by the smallest gap seen over a whole session. With both statistics off, 32 resets
     * remain instead of 2,144.
     * The CPU-side vectors are cleared anyway, which costs nothing.
     */
    slot.stats.clear();  // those never submitted are not read
    slot.stats_draw.clear();
    if (stats_ != VK_NULL_HANDLE && REXCVAR_GET(bo2zm_native_pipeline_stats) &&
        REXCVAR_GET(bo2zm_native_per_draw_stats_s) <= 0) {
      dfn_.vkCmdResetQueryPool(commands_job_, stats_,
                               slot_ * kStatsPerSlot, kStatsPerSlot);
    }
    if (stats_draw_ != VK_NULL_HANDLE && window_diagnostic_) {
      dfn_.vkCmdResetQueryPool(commands_job_, stats_draw_,
                               slot_ * kStatsDrawPerSlot,
                               kStatsDrawPerSlot);
    }
    // The timestamp mode is decided per whole submission.
    if (alternate_marks_s_ > 0) {
      const int64_t seconds = std::chrono::duration_cast<std::chrono::seconds>(
                                   std::chrono::steady_clock::now() - start_marks_)
                                   .count();
      const bool precise = (seconds / alternate_marks_s_) % 2 == 1;
      if (precise != marks_precise_) {
        marks_precise_ = precise;
        REXLOG_INFO("[native] marks test: {} (job {})", precise ? "precise" : "normal",
                    generation_commands_);
      }
    }
    slot.marks_precise = marks_precise_;
    recording_ = true;
    MarkGpu(kGpuOthers);
    ++generation_commands_;
    if (draws_) {
      draws_->UseSlot(slot_);
    }
    return true;
  }

  // Waits for that slot's submission. Earlier submissions that have already finished are collected
  // first, so read-backs are written in frame order.
  /*
   * This was what prevented CPU and GPU from overlapping.
   *
   * It used to wait as well for every earlier slot still pending, so each frame drained the whole GPU
   * before recording continued. That is why the report always said "overlapping jobs on the GPU 0",
   * why adding a third slot changed nothing (it waited for all of them, however many there were) and
   * why the wait moved elsewhere instead of disappearing.
   *
   * Now it only blocks on the slot about to be reused, the only one whose resources are needed. Earlier
   * ones are collected only if they have already finished (polling without waiting), so their
   * timestamps are read in order when possible; one still running is collected when its turn comes.
   */
  void Complete(WorkSlot& slot) {
    for (WorkSlot& other : slots_) {
      if (&other != &slot && other.pending && other.order < slot.order &&
          dfn_.vkGetFenceStatus(device_, other.fence) == VK_SUCCESS) {
        CompleteOne(other);
      }
    }
    CompleteOne(slot);
  }

  void CompleteOne(WorkSlot& slot) {
    if (!slot.pending) {
      return;
    }
    const auto before_wait = std::chrono::steady_clock::now();
    dfn_.vkWaitForFences(device_, 1, &slot.fence, VK_TRUE, UINT64_MAX);
    ns_waits_gpu_ += uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(
                                    std::chrono::steady_clock::now() - before_wait)
                                    .count());
    ++waits_gpu_;
    if (slot.submitted.time_since_epoch().count()) {
      ns_job_gpu_ += uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(
                                      std::chrono::steady_clock::now() - slot.submitted)
                                      .count());
      ++jobs_gpu_;
    }
    dfn_.vkResetFences(device_, 1, &slot.fence);
    slot.pending = false;
    const uint32_t marked = uint32_t(slot.categories.size());
    if (queries_ != VK_NULL_HANDLE && marked >= 2) {
      std::array<uint64_t, kMarksPerSlot> marks{};
      const uint32_t index = uint32_t(&slot - slots_.data());
      if (read_queries_(device_, queries_, index * kMarksPerSlot, marked,
                          sizeof(uint64_t) * marked, marks.data(), sizeof(uint64_t),
                          VK_QUERY_RESULT_64_BIT) == VK_SUCCESS &&
          marks[marked - 1] >= marks[0]) {
        for (uint32_t i = 0; i + 1 < marked; ++i) {
          if (marks[i + 1] >= marks[i] && slot.categories[i] < kGpuCategories) {
            gpu_categories_ns_[slot.categories[i]] +=
                uint64_t(double(marks[i + 1] - marks[i]) * period_mark_ns_);
          }
        }
        // GPU gap since the end of the previous submission (C2 report). Submissions are read in the order
        // they were sent; if one could not be read, mark_end_previous_ is 0 and that gap is not counted.
        if (mark_end_previous_ != 0) {
          if (marks[0] >= mark_end_previous_) {
            gpu_categories_ns_[kGpuGapBetweenJobs] +=
                uint64_t(double(marks[0] - mark_end_previous_) * period_mark_ns_);
          } else {
            ++overlaps_gpu_;
          }
        }
        mark_end_previous_ = marks[marked - 1];
        last_mark_gpu_ = marks[marked - 1];
        const uint64_t ns_job = uint64_t(double(marks[marked - 1] - marks[0]) * period_mark_ns_);
        gpu_ns_ += ns_job;
        ++gpu_jobs_;
        bo2zm::perf_log::NoteGpuJob(slot.order, ns_job, true);
        if (slot.marks_precise) {
          ++gpu_jobs_precise_;
        }
      } else {
        mark_end_previous_ = 0;  // without this submission's timestamps, the next gap is not counted
        bo2zm::perf_log::NoteGpuJob(slot.order, 0, false);
      }
    } else {
      mark_end_previous_ = 0;
      bo2zm::perf_log::NoteGpuJob(slot.order, 0, false);
    }
    slot.categories.clear();
    ReadOcclusions(slot);
    ReadVisibility(slot);
    ReadStats(slot);
    ReadStatsDraw(slot);
    WriteReads(slot.reads);
  }

  // Per-draw statistics of a finished submission, added to its pixel shader.
  void ReadStatsDraw(WorkSlot& slot) {
    if (slot.stats_draw.empty()) {
      return;
    }
    const uint32_t n = uint32_t(slot.stats_draw.size());
    std::vector<uint64_t> values(size_t(n) * kCountersStat, 0);
    if (stats_draw_ != VK_NULL_HANDLE && read_queries_ &&
        read_queries_(device_, stats_draw_, slot.stats_draw.front().first, n,
                        sizeof(uint64_t) * values.size(), values.data(),
                        sizeof(uint64_t) * kCountersStat,
                        VK_QUERY_RESULT_64_BIT) == VK_SUCCESS) {
      for (uint32_t i = 0; i < n; ++i) {
        const uint16_t label = slot.stats_draw[i].second;
        fragments_per_shader_[label] += values[i * kCountersStat + 2];
        ++draws_per_shader_[label];
      }
      // A diagnostic frame may span several submissions: the first one of each window counts.
      if (!window_read_) {
        window_read_ = true;
        ++frames_diagnostic_;
      }
    }
    slot.stats_draw.clear();
  }

  // Pass statistics of a finished submission, added to their category.
  void ReadStats(WorkSlot& slot) {
    if (slot.stats.empty()) {
      return;
    }
    const uint32_t n = uint32_t(slot.stats.size());
    std::array<uint64_t, kStatsPerSlot * kCountersStat> values{};
    if (stats_ != VK_NULL_HANDLE && read_queries_ &&
        read_queries_(device_, stats_, slot.stats.front().first, n,
                        sizeof(uint64_t) * kCountersStat * n, values.data(),
                        sizeof(uint64_t) * kCountersStat,
                        VK_QUERY_RESULT_64_BIT) == VK_SUCCESS) {
      for (uint32_t i = 0; i < n; ++i) {
        const uint8_t c = slot.stats[i].second;
        if (c < kGpuCategories) {
          vertices_category_[c] += values[i * kCountersStat + 0];
          primitives_category_[c] += values[i * kCountersStat + 1];
          fragments_category_[c] += values[i * kCountersStat + 2];
        }
      }
    }
    slot.stats.clear();
  }

  // Occlusion queries of a finished submission. They add to their game query, which is published once
  // it has its Issue(END) and all its spans have been read.
  void ReadOcclusions(WorkSlot& slot) {
    if (slot.occlusions.empty()) {
      return;
    }
    const uint32_t n = uint32_t(slot.occlusions.size());
    std::array<uint64_t, kOcclusionsPerSlot> counts{};
    const bool read = occlusions_ != VK_NULL_HANDLE && read_queries_ &&
                        read_queries_(device_, occlusions_, slot.occlusions.front().first, n,
                                        sizeof(uint64_t) * n, counts.data(), sizeof(uint64_t),
                                        VK_QUERY_RESULT_64_BIT) == VK_SUCCESS;
    for (uint32_t i = 0; i < n; ++i) {
      auto it = occlusions_game_.find(slot.occlusions[i].second);
      if (it == occlusions_game_.end()) {
        continue;
      }
      if (read) {
        it->second.samples += counts[i];
      } else {
        it->second.failed = true;
      }
      if (it->second.spans_pending) {
        --it->second.spans_pending;
      }
      if (it->second.finished && it->second.spans_pending == 0) {
        PublishOcclusion(it);
      }
    }
    slot.occlusions.clear();
  }

  // Reflection visibility. Our own queries of a finished submission: for each water draw, whether
  // it left any sample; the witness must always leave some. If they cannot be read, the water counts as
  // visible and the witness counts neither for nor against.
  void ReadVisibility(WorkSlot& slot) {
    if (slot.visibility.empty()) {
      return;
    }
    const uint32_t n = uint32_t(slot.visibility.size());
    std::array<uint64_t, kVisibilityPerSlot> counts{};
    const bool read = visibility_ != VK_NULL_HANDLE && read_queries_ &&
                        read_queries_(device_, visibility_, slot.visibility.front().first, n,
                                        sizeof(uint64_t) * n, counts.data(), sizeof(uint64_t),
                                        VK_QUERY_RESULT_64_BIT) == VK_SUCCESS;
    for (uint32_t i = 0; i < n; ++i) {
      if (slot.visibility[i].second == kVisibilityWitness) {
        if (read) {
          bo2zm::reflection_demand::AnnotateWitness(counts[i] != 0);
        }
      } else if (!read) {
        bo2zm::reflection_demand::AnnotateVisible(false);
      } else if (counts[i] != 0) {
        bo2zm::reflection_demand::AnnotateVisible(true);
      } else {
        bo2zm::reflection_demand::AnnotateHidden();
      }
    }
    slot.visibility.clear();
  }

  // Spans recorded in a submission that was never sent: their game query is not published.
  void DiscardOcclusions(WorkSlot& slot) {
    for (const auto& [index, id] : slot.occlusions) {
      auto it = occlusions_game_.find(id);
      if (it != occlusions_game_.end()) {
        it->second.failed = true;
        if (it->second.spans_pending) {
          --it->second.spans_pending;
        }
        if (it->second.finished && it->second.spans_pending == 0) {
          PublishOcclusion(it);
        }
      }
    }
    slot.occlusions.clear();
  }

  void PublishOcclusion(std::unordered_map<uint64_t, GameOcclusionQuery>::iterator it) {
    if (!it->second.failed) {
      if (occlusion_per_base_.size() >= 64 && !occlusion_per_base_.count(it->second.base)) {
        occlusion_per_base_.clear();
      }
      occlusion_per_base_[it->second.base] = it->second.samples;
      ++stats_occlusion_[2];
      stats_occlusion_[3] += it->second.samples;
      stats_occlusion_[4] = std::max(stats_occlusion_[4], it->second.samples);
    }
    occlusions_game_.erase(it);
  }

  bool SubmitJob(bool wait) {
    if (recording_) {
      if (draws_) {
        draws_->BeforeSubmit();  // closes the pass and publishes the upload buffer
      }
      recording_ = false;
      std::array<VkCommandBuffer, 2> buffers{};
      uint32_t n = 0;
      if (recording_upload_) {
        recording_upload_ = false;
        UploadBarrier();  // and the job buffer after it waits for what it wrote
        if (dfn_.vkEndCommandBuffer(commands_upload_) != VK_SUCCESS) {
          return false;
        }
        buffers[n++] = commands_upload_;
      }
      if (queries_ != VK_NULL_HANDLE) {
        // Final mark: closes the last span (MarkGpu leaves room for it).
        WorkSlot& marked = slots_[slot_];
        write_mark_(commands_job_, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, queries_,
                        slot_ * kMarksPerSlot + uint32_t(marked.categories.size()));
        marked.categories.push_back(kGpuEnd);
      }
      deferred::Finish();  // the recorder makes the last queued calls; then the buffer is the ring thread's again
      deferred::Report();
      if (dfn_.vkEndCommandBuffer(commands_job_) != VK_SUCCESS) {
        return false;
      }
      buffers[n++] = commands_job_;
      VkSubmitInfo submit{};
      submit.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
      submit.commandBufferCount = n;
      submit.pCommandBuffers = buffers.data();
      {
        const auto before_latch = std::chrono::steady_clock::now();  // "C2: present" report
        const auto queue = vulkan_device_->AcquireQueue(family_, 0);
        const auto before_submit = std::chrono::steady_clock::now();
        if (dfn_.vkQueueSubmit(queue.queue(), 1, &submit, slots_[slot_].fence) != VK_SUCCESS) {
          return false;
        }
        ns_latch_job_ += Ns(before_latch, before_submit);
        ns_submit_job_ += Ns(before_submit, std::chrono::steady_clock::now());
        ++submits_job_;
      }
      WorkSlot& slot = slots_[slot_];
      slot.pending = true;
      slot.order = ++submits_;
      slot.submitted = std::chrono::steady_clock::now();
      // Read-backs recorded in this submission are written when it finishes.
      slot.reads.insert(slot.reads.end(), reads_pending_.begin(),
                             reads_pending_.end());
      reads_pending_.clear();
    }
    if (wait) {
      Complete(slots_[slot_]);
    }
    return true;
  }

  void WaitGpu() {
    if (recording_) {
      SubmitJob(false);
    }
    for (WorkSlot& slot : slots_) {
      Complete(slot);
    }
    WaitOutputs();
  }

  // All pending outputs, before destroying or reusing what they use.
  void WaitOutputs() {
    for (uint32_t i = 0; i < kSlotsOutput; ++i) {
      if (outputs_pending_[i]) {
        dfn_.vkWaitForFences(device_, 1, &fences_output_[i], VK_TRUE, UINT64_MAX);
        dfn_.vkResetFences(device_, 1, &fences_output_[i]);
        outputs_pending_[i] = false;
      }
    }
  }

  // Diagnostic (bo2zm_native_diag_resolved): the frame's resolved textures in a 4x4 grid over
  // 1280x720, in the order of their first copy. Cells without a texture stay dark purple.
  bool ComposeTiled(uint32_t base_swap) {
    std::vector<uint32_t> bases;
    bases.swap(resolved_frame_);
    if (bases.empty() || !blit_) {
      return false;
    }
    if (tiled_.image == VK_NULL_HANDLE &&
        !Create(tiled_, 1280, 720, VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT)) {
      blit_ = nullptr;
      return false;
    }
    if (draws_) {
      draws_->FinishPass();
    }
    if (!Record()) {
      return false;
    }
    Prepare(tiled_);
    const VkClearColorValue depth3d{{0.12f, 0.0f, 0.12f, 1.0f}};
    dfn_.vkCmdClearColorImage(commands_job_, tiled_.image, VK_IMAGE_LAYOUT_GENERAL, &depth3d,
                              1, &kRangeColor);
    std::string list;
    for (size_t i = 0; i < bases.size() && i < 16; ++i) {
      const auto it = resolved_.find(bases[i]);
      if (it == resolved_.end() || !it->second.image.prepared ||
          it->second.image.format != kFormatColor) {
        continue;  // depth ones do not support blits to color
      }
      const OwnedImage& image = it->second.image;
      const int32_t x = int32_t(i % 4) * 320;
      const int32_t y = int32_t(i / 4) * 180;
      VkImageBlit copy{};
      copy.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
      copy.srcOffsets[1] = {int32_t(image.width), int32_t(image.height), 1};
      copy.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
      copy.dstOffsets[0] = {x + 2, y + 2, 0};
      copy.dstOffsets[1] = {x + 318, y + 178, 1};
      blit_(commands_job_, image.image, VK_IMAGE_LAYOUT_GENERAL, tiled_.image,
            VK_IMAGE_LAYOUT_GENERAL, 1, &copy, VK_FILTER_LINEAR);
      list += fmt::format(" {}{}:{:08X} {}x{}", i, bases[i] == base_swap ? "*" : "", bases[i],
                           image.width, image.height);
    }
    const auto now = std::chrono::steady_clock::now();
    if (now - last_warning_tiled_ >= std::chrono::seconds(10)) {
      last_warning_tiled_ = now;
      REXLOG_INFO("[native] diag resolved: {} in the frame (* = the Swap's):{}", bases.size(),
                  list);
    }
    return true;
  }

  // Reflection diagnostic: for each resolved texture, how many copies there are and how many had
  // draws since the previous copy (for example cubemap faces). A face resolved without draws
  // copies again what the render target already had.
  // It is no longer limited to square textures up to 512. The copy inventory has to be complete
  // (address, size, Mpixels and reads) because it is the only thing that tells whether a copy is
  // needed. There are ~20 addresses in gameplay, which fit easily in one line.
  void AnnotateCopy(uint32_t base, uint32_t width, uint32_t height, uint64_t draws) {
    if (base == bo2zm::reflection_demand::kAddress) {
      bo2zm::reflection_demand::AnnotateCopy();  // reflection on demand
    }
    TargetCopy& d = copies_per_target_[base];
    d.width = width;
    d.height = height;
    ++d.copies;
    d.draws += draws;
    d.with_draws += draws != 0 ? 1 : 0;
    const auto now = std::chrono::steady_clock::now();
    if (now - report_copies_ < std::chrono::seconds(10)) {
      return;
    }
    report_copies_ = now;
    std::vector<std::pair<uint32_t, TargetCopy>> order(copies_per_target_.begin(),
                                                         copies_per_target_.end());
    std::sort(order.begin(), order.end(),
              [](const auto& a, const auto& b) { return a.first < b.first; });
    std::string list;
    uint32_t written = 0;
    for (const auto& [address, c] : order) {
      if (++written > 48) {  // defensive cap: ~20 in gameplay, but a load can touch many more
        list += fmt::format(" (and {} more addresses)", order.size() - 48);
        break;
      }
      const auto r = resolved_.find(address);
      const uint64_t reads = r != resolved_.end() ? r->second.reads : 0;
      if (r != resolved_.end()) {
        r->second.reads = 0;
      }
      list += fmt::format(" {:08X} {}x{}: {} copies, {} with draws ({:.1f} draws per copy), {:.2f} Mpixels, {} "
                           "reads{};",
                           address, c.width, c.height, c.copies, c.with_draws,
                           c.copies ? double(c.draws) / double(c.copies) : 0.0,
                           double(c.pixels) / 1e6, reads,
                           c.copies && !reads ? " *** NOBODY READS IT ***" : "");
    }
    copies_per_target_.clear();
    BO2ZM_REPORT_RING("[native] C2 faces resolved since the previous report:{}", list);
    if (swaps_ || restores_) {
      // Restores are the verdict. If they go up, the swap without a clear saves nothing: the same copy is
      // paid, just later.
      BO2ZM_REPORT_RING("[native] C2 resolves without a copy: {} swaps ({} without a clear, {} of COLOR) and {} "
                  "restorations since boot -> {}",
                  swaps_, swaps_no_clear_, swaps_color_, restores_,
                  restores_ == 0 ? "not a single restoration: the saving is clean"
                                       : "*** there are restorations: the saving is NOT clean ***");
      // bo2zm_native_resolve_valid_content. In the menu, one per frame with shadows; in gameplay, 0. The
      // "without a source" count is resolves of a render target whose content is no longer anywhere (must be
      // 0); the other count is the resolves that, with the setting off, would read the stale image.
      if (restores_for_resolve_ || resolve_no_source_ || resolve_content_old_) {
        BO2ZM_REPORT_RING("[native] C2 resolve with valid content: {} times the content was brought back "
                             "before resolving (before, the resolve read the previous frame's image: the menu flicker); "
                             "{} without a source; {} read the stale image (setting off)",
                             restores_for_resolve_, resolve_no_source_, resolve_content_old_);
      }
      // And what they cost per frame, which is the only thing that decides: 0.78 real ms per Mpixel
      // copied and 0.075 per Mpixel cleared, the console's copy and clear speeds.
      const double frames = double(presented_ - presented_report_copies_);
      const double perFrm = frames > 0.0 ? 1.0 / frames : 0.0;
      const double mp_rest = double(pixels_restored_ - pixels_restored_previous_) / 1e6;
      const double mp_saving =
          double(pixels_restore_saved_ - pixels_restore_saved_previous_) / 1e6;
      BO2ZM_REPORT_RING("[native] C2 restorations per frame: {:.2f} Mpixels copied ({:.2f} ms real) and {:.2f} Mpixels "
                  "saved ({:.2f} ms); since boot: {} trimmed to the useful area and {} by swap (loaned reads {}{})",
                  mp_rest * perFrm, mp_rest * perFrm * 0.78, mp_saving * perFrm,
                  mp_saving * perFrm * 0.78, restores_clipped_, loans_,
                  lent_read_,
                  lent_read_ ? " *** THE LOAN IS NOT SAFE ***" : ", clean");
      // And why the restore does not swap, which is what has to be known to fix it.
      if (no_swap_no_transfer_dst_ || no_swap_target_no_prepare_ ||
          no_swap_resolved_no_prepare_ || no_swap_sizes_different_) {
        BO2ZM_REPORT_RING(
            "[native] C2 restore by swap, times it could NOT: {} without TRANSFER_DST, {} with the target not prepared, "
            "{} with the resolved one not prepared; and of those that qualified, {} had different sizes (those would "
            "break the image)",
            no_swap_no_transfer_dst_, no_swap_target_no_prepare_,
            no_swap_resolved_no_prepare_, no_swap_sizes_different_);
      }
      pixels_restored_previous_ = pixels_restored_;
      pixels_restore_saved_previous_ = pixels_restore_saved_;
    }
    if (clears_skipped_ || clears_skipped_depth_) {
      // Clears that did not change a single bit (bo2zm_native_skip_repeated_clears). A clear costs
      // 0.075 real ms per Mpixel: ten times less than a copy, but it adds up.
      const double frames = double(presented_ - presented_report_copies_);
      const double perFrm = frames > 0.0 ? 1.0 / frames : 0.0;
      const double mp = double(pixels_clears_skipped_ - pixels_clears_skipped_previous_) / 1e6;
      BO2ZM_REPORT_RING("[native] C2 clears skipped: {} color and {} depth since boot; {:.2f} Mpixels per frame not "
                  "cleared ({:.2f} ms real)",
                  clears_skipped_, clears_skipped_depth_, mp * perFrm,
                  mp * perFrm * 0.075);
      pixels_clears_skipped_previous_ = pixels_clears_skipped_;
    }
    if (clears_useless_) {
      // Clears wiped out by a swap or a restore before anything was drawn. These are the real candidates
      // for removal (the equivalent of a loadOp = DONT_CARE).
      const double frames = double(presented_ - presented_report_copies_);
      const double perFrm = frames > 0.0 ? 1.0 / frames : 0.0;
      const double mp = double(pixels_clears_useless_ - pixels_clears_useless_previous_) / 1e6;
      BO2ZM_REPORT_RING("[native] C2 clears that were of no use (the target changed content without anything being "
                  "drawn): {} since boot; {:.2f} Mpixels per frame ({:.2f} ms real)",
                  clears_useless_, mp * perFrm, mp * perFrm * 0.075);
      pixels_clears_useless_previous_ = pixels_clears_useless_;
    }
    if (lazy_postponed_ || lazy_reads_late_) {  // bo2zm_native_lazy_depth
      // 0.60 real ms per Mpixel copied.
      const double frames = double(presented_ - presented_report_copies_);
      const double perFrm = frames > 0.0 ? 1.0 / frames : 0.0;
      const double mp = double(lazy_pixels_saved_ - lazy_pixels_saved_previous_) / 1e6;
      BO2ZM_REPORT_RING("[native] C2 lazy depth: {} depth resolves postponed since boot; {} recorded when "
                           "sampled, {} before their source was written again or another resolve, {} replaced by a swap "
                           "and {} dropped without copying (nobody sampled them): {:.2f} Mpixels per frame not copied "
                           "(~{:.2f} ms real); late reads {}{}",
                           lazy_postponed_, lazy_copied_read_, lazy_copied_write_,
                           lazy_replaced_, lazy_dropped_, mp * perFrm, mp * perFrm * 0.60,
                           lazy_reads_late_,
                           lazy_off_ ? " *** OFF BY THE GUARD ***" : " (0 = the image is the same)");
      lazy_pixels_saved_previous_ = lazy_pixels_saved_;
    }
    if (front_postponed_ || front_reads_late_) {  // bo2zm_native_lazy_front
      const double frames = double(presented_ - presented_report_copies_);
      const double perFrm = frames > 0.0 ? 1.0 / frames : 0.0;
      const double mp = double(front_pixels_saved_ - front_pixels_saved_previous_) / 1e6;
      BO2ZM_REPORT_RING("[native] C2 lazy front buffer: {} front buffer copies postponed since boot; Swaps "
                           "painted from the target {} and from a retained image {}; {} clears onto a spare image ({} "
                           "without a spare); recorded: {} when sampled, {} before the target was written again or "
                           "another resolve, {} at the Swap (FXAA, no ramp or another output); {} dropped without "
                           "copying (another resolve covers them whole): {:.2f} Mpixels per frame not copied (~{:.2f} "
                           "ms real); {} spare images; late reads {}{}",
                           front_postponed_, front_painted_target_, front_painted_retained_,
                           front_rotations_, front_no_spare_, front_copied_read_,
                           front_copied_write_, front_copied_swap_, front_replaced_, mp * perFrm,
                           mp * perFrm * 0.60, fronts_images_.size(), front_reads_late_,
                           front_off_ ? " *** OFF BY THE GUARD ***" : " (0 = the image is the same)");
      front_pixels_saved_previous_ = front_pixels_saved_;
    }
    ReportShadowMin();  // bo2zm_native_shadow_min
    ReportClears();  // bo2zm_native_diag_clears, every 20 s
    presented_report_copies_ = presented_;
    if (no_swap_[0] || no_swap_[1] || no_swap_[2]) {
      BO2ZM_REPORT_RING("[native] C2 depth copies without a swap since the previous report: {} because the command does "
                  "not clear the target, {} because it is not resolved whole, {} because it could not",
                  no_swap_[0], no_swap_[1], no_swap_[2]);
      no_swap_ = {};
    }
  }

  // Read-back of a small resolved texture: the copied rectangle goes to a host-visible buffer in the
  // same submission; WriteReads moves it into guest memory.
  void ReadResolved(const CopyRegisters& reg, const Resolved& resolved, int32_t x0, int32_t y0,
                    uint32_t dx, uint32_t dy, uint32_t width, uint32_t height) {
    const int32_t max = REXCVAR_GET(bo2zm_native_read_resolved_texels);
    if (max <= 0 || uint64_t(width) * height > uint64_t(max) ||
        (resolved.image.format != kFormatColor && !Is16BitsPerChannel(resolved.image.format))) {
      return;
    }
    const uint64_t key_target =
        (uint64_t(reg.rb_copy_dest_base) << 28) ^ (uint64_t(width) << 14) ^ uint64_t(height);
    ReadTarget& target = reads_per_target_[key_target];
    target.base = reg.rb_copy_dest_base;
    target.width = width;
    target.height = height;
    const uint64_t every = uint64_t(std::max(REXCVAR_GET(bo2zm_native_reads_every), 1));
    if (target.copies++ % every != 0) {
      ++target.skipped;  // exposure changes slowly: the guest keeps the previous one
      return;
    }
    const VkDeviceSize bytes = VkDeviceSize(width) * height * (Is16BitsPerChannel(resolved.image.format) ? 8 : 4);
    const uint64_t key = (uint64_t(reg.rb_copy_dest_base & 0x1FFFFFFF) << 32) ^ (uint64_t(uint32_t(x0)) << 16) ^
                           uint64_t(uint32_t(y0));
    // One buffer per slot: earlier submissions may still be copying into the other slots'. The slot goes
    // in bits 61-63, which the physical base leaves free. It used to be slot_ << 63, which only kept the slot's lowest
    // bit: with three slots, 0 and 2 shared a buffer, and finishing slot 0 could read slot 2's copy, newer or half
    // written, so the exposure the game computes from it could go back and forth [inference, not measured].
    Readback* pointer = &reads_[key | (uint64_t(slot_) << 61)];
    if (pointer->bytes < bytes && pointer->buffer != VK_NULL_HANDLE) {
      // Larger: the old buffer may have recorded or submitted copies.
      SubmitJob(true);
      WaitGpu();
      DestroyRead(*pointer);
      if (!Record()) {
        return;
      }
      pointer = &reads_[key | (uint64_t(slot_) << 61)];
      if (pointer->bytes < bytes && pointer->buffer != VK_NULL_HANDLE) {
        DestroyRead(*pointer);  // the GPU has nothing pending any more
      }
    }
    Readback& read = *pointer;
    if (read.bytes < bytes) {
      uint32_t type = 0;
      if (!rex::ui::vulkan::util::CreateDedicatedAllocationBuffer(
              vulkan_device_, bytes, VK_BUFFER_USAGE_TRANSFER_DST_BIT,
              rex::ui::vulkan::util::MemoryPurpose::kReadback, read.buffer, read.guest_memory, &type)) {
        read = Readback{};
        Reject(16, "could not create a readback buffer for a resolved texture");
        return;
      }
      void* mapped = nullptr;
      if (dfn_.vkMapMemory(device_, read.guest_memory, 0, VK_WHOLE_SIZE, 0, &mapped) != VK_SUCCESS) {
        DestroyRead(read);
        Reject(16, "could not create a readback buffer for a resolved texture");
        return;
      }
      read.data = static_cast<uint8_t*>(mapped);
      read.bytes = bytes;
      read.coherent = (vulkan_device_->memory_types().host_coherent >> type) & 0x1;
    }
    VkBufferImageCopy copy{};
    copy.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    copy.imageOffset = {int32_t(dx), int32_t(dy), 0};
    copy.imageExtent = {width, height, 1};
    dfn_.vkCmdCopyImageToBuffer(commands_job_, resolved.image.image, VK_IMAGE_LAYOUT_GENERAL,
                                read.buffer, 1, &copy);
    reads_pending_.push_back({&read, reg.rb_copy_dest_base, x0, y0, width, height,
                                    reg.rb_copy_dest_pitch & 0x3FFF,
                                    (reg.rb_copy_dest_pitch >> 16) & 0x3FFF, reg.rb_copy_dest_info,
                                    resolved.image.format});
    if (reads_done_++ == 0) {
      REXLOG_INFO("[native] C2: readback of resolved textures of up to {} texels (the first: {:08X}, {}x{})",
                  max, reg.rb_copy_dest_base, width, height);
    }
  }

  // The submission finished: the submitted read-backs go to guest memory. Loaded with its fetch
  // constant (GpuSwap per word and R8G8B8A8), each texel must give the same channels as the resolved
  // texture on the GPU: (B, G, R, A) with copy_dest_swap and (R, G, B, A) without it.
  // A 64-bit resolve destination read back for the CPU. BO2 resolves its luminance chain to
  // 16_16_16_16 with number format UINT and exponent bias 11 (u5.11 fixed point: value x 2048) and computes the
  // auto exposure from the 96x45 one on the CPU. These copies used to be written as 4-byte RGBA8 texels in the
  // 4-byte tiling, so the CPU read garbage exposure: a much too dark image, and black spells. Here each channel
  // of the RGBA8 render target texel is scaled by 2^bias and packed as the destination's number format says,
  // in the 8-byte tiling, with the copy's byte order. (The 8-bit source still limits precision: stage 2.)
  void WriteRead64(const PendingRead& p, const Readback& read) {
    const uint32_t format = (p.info >> 7) & 0x3F;
    const uint32_t number = (p.info >> 13) & 0x7;
    const int32_t bias = int32_t(p.info << 10) >> 26;  // bits 16-21, signed
    const bool swap = (p.info >> 24) & 0x1;
    const uint32_t order_copy = p.info & 0x7;
    const auto order = order_copy <= 3 ? static_cast<xenos::Endian>(order_copy) : xenos::Endian::k8in32;
    const float scale = std::ldexp(1.0f, bias);
    const bool float_value = format == uint32_t(xenos::ColorFormat::k_16_16_16_16_FLOAT);
    const auto pack = [&](float x) -> uint16_t {
      x *= scale;
      if (float_value) {
        return rex::float_to_xenos_half(x);
      }
      switch (number) {
        case 1:  // signed repeating fraction
          return uint16_t(int16_t(std::lround(std::clamp(x, -1.0f, 1.0f) * 32767.0f)));
        case 2:  // unsigned integer
          return uint16_t(std::lround(std::clamp(x, 0.0f, 65535.0f)));
        case 3:  // signed integer
          return uint16_t(int16_t(std::lround(std::clamp(x, -32768.0f, 32767.0f))));
        default:  // unsigned repeating fraction
          return uint16_t(std::lround(std::clamp(x, 0.0f, 1.0f) * 65535.0f));
      }
    };
    uint8_t* const physical = memory_->TranslatePhysical(0);
    const uint64_t base = uint64_t(p.base & 0x1FFFFFFF);
    // One line every 10 s per destination: the rectangle's average texel before and after packing.
    {
      auto& last = reads64_report_[p.base];
      const auto now = std::chrono::steady_clock::now();
      if (now - last >= std::chrono::seconds(10)) {
        last = now;
        double sum[4] = {};
        const size_t n = size_t(p.width) * p.height;
        const bool sixteen = Is16BitsPerChannel(p.format_host);
        for (size_t t = 0; t < n; ++t) {
          for (uint32_t k = 0; k < 4; ++k) {
            if (sixteen) {
              uint16_t v;
              std::memcpy(&v, read.data + t * 8 + k * 2, 2);
              sum[k] += v / 257.0;  // shown on the RGBA8 scale
            } else {
              sum[k] += read.data[t * 4 + k];
            }
          }
        }
        REXLOG_INFO("[native] BO2 64-bit readback {:08X} {}x{} format {} number {} bias {}: RGBA8 average "
                    "({:.1f},{:.1f},{:.1f},{:.1f}) -> {} {} {} {}",
                    p.base, p.width, p.height, format, number, bias, sum[0] / n, sum[1] / n, sum[2] / n,
                    sum[3] / n, pack(float(sum[0] / n / 255.0)), pack(float(sum[1] / n / 255.0)),
                    pack(float(sum[2] / n / 255.0)), pack(float(sum[3] / n / 255.0)));
      }
    }
    for (uint32_t j = 0; j < p.height; ++j) {
      const uint32_t ty = uint32_t(p.y0) + j;
      if (p.height_target && ty >= p.height_target) {
        break;
      }
      for (uint32_t i = 0; i < p.width; ++i) {
        const uint32_t tx = uint32_t(p.x0) + i;
        if (tx >= p.pitch) {
          break;
        }
        const uint64_t address =
            base + uint64_t(std::max(OffsetTiled2D(int32_t(tx), int32_t(ty), p.pitch, 3), 0));
        if (address + 8 > 0x20000000) {
          continue;
        }
        if (Is16BitsPerChannel(p.format_host)) {
          // Stage 2: the conversion pass already packed them (UINT as UNORM16 bits, etc.).
          uint16_t v[4];
          std::memcpy(v, read.data + (size_t(j) * p.width + i) * 8, sizeof(v));
          if (swap) {
            std::swap(v[0], v[2]);
          }
          const uint32_t raw[2] = {xenos::GpuSwap(uint32_t(v[0]) | (uint32_t(v[1]) << 16), order),
                                      xenos::GpuSwap(uint32_t(v[2]) | (uint32_t(v[3]) << 16), order)};
          std::memcpy(physical + address, raw, sizeof(raw));
          continue;
        }
        const uint8_t* s = read.data + (size_t(j) * p.width + i) * 4;
        float c[4] = {s[0] / 255.0f, s[1] / 255.0f, s[2] / 255.0f, s[3] / 255.0f};
        if (swap) {
          std::swap(c[0], c[2]);
        }
        const uint32_t words[2] = {
            xenos::GpuSwap(uint32_t(pack(c[0])) | (uint32_t(pack(c[1])) << 16), order),
            xenos::GpuSwap(uint32_t(pack(c[2])) | (uint32_t(pack(c[3])) << 16), order)};
        std::memcpy(physical + address, words, sizeof(words));
      }
    }
  }

  std::unordered_map<uint32_t, std::chrono::steady_clock::time_point> reads64_report_;

  void WriteReads(std::vector<PendingRead>& reads) {
    const auto before_reads = std::chrono::steady_clock::now();
    if (before_reads - report_reads_ >= std::chrono::seconds(10)) {
      std::string list;
      for (auto it = reads_per_target_.begin(); it != reads_per_target_.end();) {
        ReadTarget& d = it->second;
        if (!d.copies) {
          it = reads_per_target_.erase(it);  // has not shown up again
          continue;
        }
        list += fmt::format(" {:08X} {}x{} {}/{};", d.base, d.width, d.height, d.copies - d.skipped,
                             d.copies);
        d.copies = 0;
        d.skipped = 0;
        ++it;
      }
      if (!list.empty()) {
        BO2ZM_REPORT_RING("[native] C2 readbacks per target (done/copies):{}", list);
      }
      report_reads_ = before_reads;
    }
    for (const PendingRead& p : reads) {
      const Readback& read = *p.read;
      if (!read.data) {
        continue;
      }
      ++reads_written_;
      texels_written_ += uint64_t(p.width) * p.height;
      if (!read.coherent) {
        VkMappedMemoryRange range{};
        range.sType = VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE;
        range.memory = read.guest_memory;
        range.size = VK_WHOLE_SIZE;
        dfn_.vkInvalidateMappedMemoryRanges(device_, 1, &range);
      }
      // 64-bit destinations (16_16_16_16 and its FLOAT) have their own packing.
      if (bo2zm_formats::Log2BytesCopy((p.info >> 7) & 0x3F) == 3) {
        WriteRead64(p, read);
        continue;
      }
      const bool swap = (p.info >> 24) & 0x1;
      const uint32_t order_copy = p.info & 0x7;  // Endian128: 0-3 as xenos::Endian
      const auto order =
          order_copy <= 3 ? static_cast<xenos::Endian>(order_copy) : xenos::Endian::k8in32;
      // Each texel is a permutation of its 4 bytes (R/B swap plus GpuSwap): it is computed once with
      // marked bytes. Addresses come from the tile and the tiling table, and the guest's physical memory
      // is contiguous.
      const uint32_t permutation =
          xenos::GpuSwap(swap ? uint32_t(0x03000102) : uint32_t(0x03020100), order);
      const uint8_t b0 = uint8_t(permutation), b1 = uint8_t(permutation >> 8),
                    b2 = uint8_t(permutation >> 16), b3 = uint8_t(permutation >> 24);
      uint8_t* const physical = memory_->TranslatePhysical(0);
      const uint64_t base = uint64_t(p.base & 0x1FFFFFFF);
      const auto& table = TableTiled2DTexel4();
      const uint64_t grid_tiles_per_row = ((p.pitch + 31) & ~uint32_t(31)) >> 5;
      for (uint32_t j = 0; j < p.height; ++j) {
        const uint32_t ty = uint32_t(p.y0) + j;
        if (p.height_target && ty >= p.height_target) {
          break;
        }
        const uint64_t row = base + ((uint64_t(ty >> 5) * grid_tiles_per_row) << 12);
        const uint16_t* const local = table.data() + size_t(ty & 31) * 32;
        const uint8_t* s = read.data + size_t(j) * p.width * 4;
        for (uint32_t i = 0; i < p.width; ++i, s += 4) {
          const uint32_t tx = uint32_t(p.x0) + i;
          if (tx >= p.pitch) {
            break;
          }
          const uint64_t address = row + (uint64_t(tx >> 5) << 12) + local[tx & 31];
          if (address + 4 > 0x20000000) {
            continue;
          }
          const uint8_t t0 = s[b0], t1 = s[b1], t2 = s[b2], t3 = s[b3];
          uint8_t* const d = physical + address;
          d[0] = t0;
          d[1] = t1;
          d[2] = t2;
          d[3] = t3;
        }
      }
    }
    reads.clear();
    ns_write_reads_ += uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(
        std::chrono::steady_clock::now() - before_reads).count());
  }

  void DestroyRead(Readback& read) {
    if (read.guest_memory != VK_NULL_HANDLE) {
      if (read.data) {
        dfn_.vkUnmapMemory(device_, read.guest_memory);
      }
      dfn_.vkFreeMemory(device_, read.guest_memory, nullptr);
    }
    if (read.buffer != VK_NULL_HANDLE) {
      dfn_.vkDestroyBuffer(device_, read.buffer, nullptr);
    }
    read = Readback{};
  }

  // since_target = the source is a render target's image (or a retained one) with the front buffer in its
  // corner and larger than the output (bo2zm_native_lazy_front). Only requested with the exact variant
  // and without FXAA.
  // bo2zm_show_fps: frames presented per second, times 10, over windows of half a second.
  uint32_t TenthsFps() {
    const auto now = std::chrono::steady_clock::now();
    const double seconds = std::chrono::duration<double>(now - fps_since_).count();
    if (seconds >= 0.5) {
      fps_tenths_ = uint32_t(double(presented_ - fps_presented_) * 10.0 / seconds + 0.5);
      fps_since_ = now;
      fps_presented_ = presented_;
    }
    return fps_tenths_;
  }

  bool PaintOutput(VulkanPresenter::VulkanGuestOutputRefreshContext& context, OwnedImage& source,
                    uint32_t width, uint32_t height, bool since_target = false) {
    // With bo2zm_native_output_without_wait, the next of 3 slots, and it only waits if that one is still pending.
    // Without it, always slot 0: it waits for the previous output.
    const uint32_t s = output_no_wait_ ? (output_current_ + 1) % kSlotsOutput : 0;
    if (outputs_pending_[s]) {
      const auto before_wait = std::chrono::steady_clock::now();  // "C2: present" report
      dfn_.vkWaitForFences(device_, 1, &fences_output_[s], VK_TRUE, UINT64_MAX);
      ns_wait_output_ += Ns(before_wait, std::chrono::steady_clock::now());
      ++waits_output_;
      dfn_.vkResetFences(device_, 1, &fences_output_[s]);
      outputs_pending_[s] = false;
    }
    output_current_ = s;
    // The menu post-processing settings, if they changed (an atomic counter; the cvars are only read then).
    if (ramp_gamma_) {
      const uint64_t version_postprocess = bo2zm::settings::VersionPostprocess();
      if (version_postprocess != version_postprocess_) {
        version_postprocess_ = version_postprocess;
        postprocess_ = bo2zm::settings::ReadPostprocess();
        RecomputeRamp();
        const auto& p = postprocess_;
        REXLOG_INFO("[native] C2: postprocess {}: brightness {:.2f}, contrast {:.2f}, saturation {:.2f}, vibrance "
                    "{:.2f}, temperature {:.2f}, gamma {:.2f}, tint {:.2f}/{:.2f}/{:.2f} at {:.2f}, vignette {:.2f}, "
                    "lines {:.2f}; {}",
                    p.active ? "on" : "off", p.brightness, p.contrast, p.saturation, p.vibrance,
                    p.temperature, p.gamma, p.tint_r, p.tint_g, p.tint_b, p.tint, p.vignette, p.lines,
                    grading_ ? "with per-pixel work (saturation, vibrance, vignette or lines)"
                                : "everything in the table, no per-pixel work");
      }
    }
    bo2zm::native::output::Crosshairs crosshairs;
    if (ramp_gamma_) {
      bo2zm::cheats::CrosshairOverlay records[4]{};
      const int count = bo2zm::cheats::GetCrosshairOverlays(records, 4);
      crosshairs = bo2zm::native::output::PackCrosshairs(records, count);
    }
    // This slot's fence has completed, so its table and overlay data are idle.
    if (ramp_gamma_) {
      OutputRamp& ramp = ramps_output_[s];
      const bool table_changed = ramp.version != version_ramp_;
      const bool overlays_changed = std::memcmp(&ramp.crosshairs, &crosshairs, sizeof(crosshairs)) != 0;
      if (table_changed) std::memcpy(ramp.data, ramp_values_.data(), sizeof(ramp_values_));
      if (table_changed || overlays_changed) {
        std::memcpy(ramp.data + sizeof(ramp_values_), &crosshairs, sizeof(crosshairs));
        rex::ui::vulkan::util::FlushMappedMemoryRange(vulkan_device_, ramp.guest_memory, ramp.type);
        ramp.version = version_ramp_;
        ramp.crosshairs = crosshairs;
      }
    }
    VkFramebuffer framebuffer = VK_NULL_HANDLE;
    for (const auto& f : framebuffers_) {
      if (f.framebuffer != VK_NULL_HANDLE && f.version == context.image_version()) {
        framebuffer = f.framebuffer;
      }
    }
    if (framebuffer == VK_NULL_HANDLE) {
      auto& f = framebuffers_[next_framebuffer_];
      next_framebuffer_ = (next_framebuffer_ + 1) % framebuffers_.size();
      if (f.framebuffer != VK_NULL_HANDLE) {
        WaitOutputs();  // a pending output could still use this framebuffer
        dfn_.vkDestroyFramebuffer(device_, f.framebuffer, nullptr);
        f.framebuffer = VK_NULL_HANDLE;
      }
      VkImageView view = context.image_view();
      VkFramebufferCreateInfo info{};
      info.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
      info.renderPass = render_pass_output_;
      info.attachmentCount = 1;
      info.pAttachments = &view;
      info.width = width;
      info.height = height;
      info.layers = 1;
      if (dfn_.vkCreateFramebuffer(device_, &info, nullptr, &f.framebuffer) != VK_SUCCESS) {
        f.framebuffer = VK_NULL_HANDLE;
        return false;
      }
      f.version = context.image_version();
      framebuffer = f.framebuffer;
    }

    VkDescriptorImageInfo info_image{};
    info_image.imageView = source.view;
    info_image.imageLayout = VK_IMAGE_LAYOUT_GENERAL;
    VkWriteDescriptorSet write{};
    write.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    write.dstSet = descriptors_output_[s];
    write.dstBinding = 0;
    write.descriptorCount = 1;
    write.descriptorType = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
    write.pImageInfo = &info_image;
    dfn_.vkUpdateDescriptorSets(device_, 1, &write, 0, nullptr);

    // The home screen's credits line (bo2zm_credits.cpp), while the menu shows it.
    bo2zm::credits::Placement credits{};
    const bool draw_credits = bo2zm::credits::Place(width, height, credits) && CreditsReady();

    dfn_.vkResetCommandPool(device_, pools_output_[s], 0);
    VkCommandBufferBeginInfo start{};
    start.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    start.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    if (dfn_.vkBeginCommandBuffer(commands_output_[s], &start) != VK_SUCCESS) {
      return false;
    }
    if (draw_credits && !credits_.uploaded) {
      RecordCreditsUpload(commands_output_[s]);
    }
    VkRenderPassBeginInfo pass{};
    pass.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
    pass.renderPass = render_pass_output_;
    pass.framebuffer = framebuffer;
    pass.renderArea.extent = {width, height};
    if (since_target) {
      // That image was just written as a render target (or copy destination) in the frame's work, which goes in
      // another submission: make the output read it fully written.
      VkMemoryBarrier barrier{};
      barrier.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER;
      barrier.srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_TRANSFER_WRITE_BIT;
      barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
      dfn_.vkCmdPipelineBarrier(commands_output_[s],
                                VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT | VK_PIPELINE_STAGE_TRANSFER_BIT,
                                VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 0, 1, &barrier, 0, nullptr, 0, nullptr);
    }
    dfn_.vkCmdBeginRenderPass(commands_output_[s], &pass, VK_SUBPASS_CONTENTS_INLINE);
    const VkViewport viewport{0.0f, 0.0f, float(width), float(height), 0.0f, 1.0f};
    dfn_.vkCmdSetViewport(commands_output_[s], 0, 1, &viewport);
    const VkRect2D scissor{{0, 0}, {width, height}};
    dfn_.vkCmdSetScissor(commands_output_[s], 0, 1, &scissor);
    VkPipeline pipeline = pipeline_;
    if (ramp_gamma_) {
      // With an output the size of the source (the normal case), the exact texel, unfiltered.
      // From a render target, the output has the front buffer's size, not the image's, and has no FXAA
      // (FrontOnPresent only requests it that way): texelFetch of the pixel, the same texel the texture
      // would hold.
      const bool exact = since_target || (width == source.width && height == source.height);
      const bool fxaa = !since_target && bo2zm::settings::AntialiasingFxaa();
      if (fxaa != fxaa_annotated_) {
        fxaa_annotated_ = fxaa;
        REXLOG_INFO("[native] C2: antialiasing at the output: {}", fxaa ? "FXAA" : "none");
      }
      pipeline = PipelineRamp((fxaa ? 4 : 0) + (grading_ ? 2 : 0) + (exact ? 1 : 0));
      if (pipeline == VK_NULL_HANDLE) {
        pipeline = pipelines_ramp_[exact ? 1 : 0];  // requested variant missing: the usual one
      }
      if (!exact && !warned_ramp_bilinear_) {
        warned_ramp_bilinear_ = true;
        REXLOG_INFO("[native] C2: {}x{} output from a {}x{} source: ramp with bilinear sampling", width,
                    height, source.width, source.height);
      }
    }
    dfn_.vkCmdBindPipeline(commands_output_[s], VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline);
    dfn_.vkCmdBindDescriptorSets(commands_output_[s], VK_PIPELINE_BIND_POINT_GRAPHICS,
                                 layout_pipeline_, 0, 1, &descriptors_output_[s], 0, nullptr);
    const float rectangle[4] = {-1.0f, -1.0f, 2.0f, 2.0f};  // x, y, width, height in NDC
    dfn_.vkCmdPushConstants(commands_output_[s], layout_pipeline_, VK_SHADER_STAGE_VERTEX_BIT, 0,
                            sizeof(rectangle), rectangle);
    struct {
      int32_t displacement[2];
      float inverse[2];
      uint32_t counter[4];  // bo2zm_show_fps and the preset note (see bo2zm_output_gamma_ramp.frag)
      uint32_t note[4];      // The preset note's text (bo2zm_preset.cpp)
      uint32_t banner[8];    // The top banner's text (bo2zm_preset.cpp OnScreenBanner)
      uint32_t banner_info[4];  // x = screen pixels per font dot, 0 = no banner
    } bilinear = {{0, 0}, {1.0f / float(width), 1.0f / float(height)}, {}, {}, {}, {}};
    // counter[2]: bit 0 the shared FPS counter, bit 1 per-view crosshairs in the UBO.
    const uint32_t overlays = (ramp_gamma_ && REXCVAR_GET(bo2zm_show_fps) ? 1u : 0u) |
                              (ramp_gamma_ && crosshairs.Any() ? 2u : 0u);
    if (overlays) {
      bilinear.counter[0] = (overlays & 1u) ? TenthsFps() : 0;
      bilinear.counter[1] = std::max<uint32_t>(2, height / 240);  // 3 pixels per dot at 720p, 4 at 1080p
      bilinear.counter[2] = overlays;
    }
    if (ramp_gamma_ && bo2zm::preset::OnScreenNote(bilinear.note)) {
      bilinear.counter[3] = std::max<uint32_t>(2, height / 240);  // the same dots as the counter, below it
    }
    if (ramp_gamma_ && bo2zm::preset::OnScreenBanner(bilinear.banner)) {
      bilinear.banner_info[0] = std::max<uint32_t>(3, height / 180);  // a size larger than the note's
    }
    dfn_.vkCmdPushConstants(commands_output_[s], layout_pipeline_, VK_SHADER_STAGE_FRAGMENT_BIT, 16,
                            sizeof(bilinear), &bilinear);
    dfn_.vkCmdDraw(commands_output_[s], 4, 1, 0, 0);
    if (draw_credits) {
      DrawCredits(commands_output_[s], credits, width, height);
    }
    dfn_.vkCmdEndRenderPass(commands_output_[s]);
    if (since_target) {
      // And nothing submitted later (the next frame draws and clears that image again) writes it before the
      // output has read it.
      dfn_.vkCmdPipelineBarrier(commands_output_[s], VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
                                VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT | VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0,
                                nullptr, 0, nullptr, 0, nullptr);
    }
    if (dfn_.vkEndCommandBuffer(commands_output_[s]) != VK_SUCCESS) {
      return false;
    }
    VkSubmitInfo submit{};
    submit.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    submit.commandBufferCount = 1;
    submit.pCommandBuffers = &commands_output_[s];
    {
      const auto before_latch = std::chrono::steady_clock::now();  // "C2: present" report
      const auto queue = vulkan_device_->AcquireQueue(family_, 0);
      const auto before_submit = std::chrono::steady_clock::now();
      if (dfn_.vkQueueSubmit(queue.queue(), 1, &submit, fences_output_[s]) != VK_SUCCESS) {
        return false;
      }
      ns_latch_output_ += Ns(before_latch, before_submit);
      ns_submit_output_ += Ns(before_submit, std::chrono::steady_clock::now());
      ++submits_output_;
    }
    outputs_pending_[s] = true;
    context.SetIs8bpc(true);
    return true;
  }

  struct Framebuffer {
    uint64_t version = UINT64_MAX;
    VkFramebuffer framebuffer = VK_NULL_HANDLE;
  };

  const VulkanDevice* vulkan_device_;
  const VulkanDevice::Functions& dfn_;
  VkDevice device_;
  rex::memory::Memory* memory_;
  uint32_t family_;

  std::array<VkCommandPool, kSlotsOutput> pools_output_{};  // output slots
  FnCopyImage copy_image_ = nullptr;
  uint32_t reductions_ = 0;  // resolves that had to shrink
  // Those of the slot being recorded (Record switches them).
  VkCommandBuffer commands_job_ = VK_NULL_HANDLE;
  VkCommandBuffer commands_upload_ = VK_NULL_HANDLE;
  // Room for four work slots; bo2zm_native_work_slots sets how many are used (three by default, see its
  // explanation). With two, the CPU is only one frame ahead and waits for the GPU inside Record() about half the time
  // on the console. A fourth slot would help where the GPU is the bottleneck (the fence wait inside Record() is
  // several ms in open, GPU-bound areas and about 0 in busy, CPU-bound ones) and fits in memory easily, but it
  // needs more output slots first.
  std::array<WorkSlot, 4> slots_{};
  uint32_t slots_used_ = 3;  // set by the cvar (bo2zm_native_work_slots)
  uint32_t slot_ = 1;  // Record starts with slot 0
  uint64_t submits_ = 0;
  std::array<VkCommandBuffer, kSlotsOutput> commands_output_{};
  std::array<VkFence, kSlotsOutput> fences_output_{};
  bool recording_ = false;
  std::array<bool, kSlotsOutput> outputs_pending_{};
  uint32_t output_current_ = 0;
  bool output_no_wait_ = false;
  bool marks_precise_ = false;
  int32_t alternate_marks_s_ = 0;
  std::chrono::steady_clock::time_point start_marks_{};
  bool invalidate_every_copy_ = false;
  bool recording_upload_ = false;
  uint64_t generation_commands_ = 0;
  FnClearDepth clear_depth_ = nullptr;
  // GPU time of the submissions (timestamps, if the queue supports them).
  using FnWriteMark = void(VKAPI_PTR*)(VkCommandBuffer, VkPipelineStageFlags, VkQueryPool,
                                          uint32_t);
  using FnReadQueries = VkResult(VKAPI_PTR*)(VkDevice, VkQueryPool, uint32_t, uint32_t, size_t,
                                              void*, VkDeviceSize, VkQueryResultFlags);
  FnWriteMark write_mark_ = nullptr;
  FnReadQueries read_queries_ = nullptr;
  VkQueryPool queries_ = VK_NULL_HANDLE;
  // The game's occlusion queries. The open one (0 = none), those waiting for their spans by number, the last
  // complete count of each D3D structure and, accumulated, spans, spans without room, published queries,
  // published samples and the maximum of a single query.
  VkQueryPool occlusions_ = VK_NULL_HANDLE;
  // Pipeline statistics per pass category.
  VkQueryPool stats_ = VK_NULL_HANDLE;
  std::array<uint64_t, kGpuCategories> fragments_category_{};
  std::array<uint64_t, kGpuCategories> vertices_category_{};
  std::array<uint64_t, kGpuCategories> primitives_category_{};
  uint64_t stats_no_room_ = 0;
  VkQueryPool stats_draw_ = VK_NULL_HANDLE;
  std::vector<uint64_t> fragments_per_shader_;
  std::vector<uint64_t> draws_per_shader_;
  uint64_t frames_diagnostic_ = 0;
  uint64_t stats_draw_no_room_ = 0;
  std::array<uint64_t, 3> no_swap_{};          // no clear, not whole, not possible
  uint32_t warnings_no_swap_ = 0;
  std::array<uint64_t, kBucketsCopy> copies_bucket_{};
  std::array<uint64_t, kBucketsCopy> pixels_bucket_{};
  bool window_diagnostic_ = false;
  bool window_read_ = false;
  std::chrono::steady_clock::time_point last_window_ = std::chrono::steady_clock::now();
  bool occlusion_precise_ = false;
  VkQueryPool visibility_ = VK_NULL_HANDLE;  // reflection visibility
  uint64_t visibility_no_room_ = 0;
  uint64_t occlusion_open_ = 0;
  uint64_t next_occlusion_ = 1;
  std::unordered_map<uint64_t, GameOcclusionQuery> occlusions_game_;
  std::unordered_map<uint32_t, uint64_t> occlusion_per_base_;
  uint64_t stats_occlusion_[5] = {};
  double period_mark_ns_ = 0.0;
  uint64_t gpu_ns_ = 0;
  uint64_t gpu_jobs_ = 0;
  uint64_t gpu_jobs_precise_ = 0;  // measured with precise intermediate marks
  std::array<uint64_t, kGpuCategories> gpu_categories_ns_{};
  uint64_t mark_end_previous_ = 0;  // end timestamp of the last submission read (gap between submissions)
  uint64_t last_mark_gpu_ = 0;  // same, but kept when a read fails (timestamp scale)
  // Breakdown of Present's time (CostPresent).
  uint64_t waits_output_ = 0, ns_wait_output_ = 0;
  uint64_t submits_job_ = 0, ns_latch_job_ = 0, ns_submit_job_ = 0;
  uint64_t submits_output_ = 0, ns_latch_output_ = 0, ns_submit_output_ = 0;
  uint64_t refreshes_ = 0, ns_before_call_ = 0, ns_in_call_ = 0, ns_after_call_ = 0;
  static uint64_t Ns(std::chrono::steady_clock::time_point since, std::chrono::steady_clock::time_point until) {
    return until > since ? uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(until - since).count()) : 0;
  }
  uint64_t overlaps_gpu_ = 0;
  std::array<uint64_t, kBucketsSwap> buckets_swap_{};
  mutable double worst_swap_ms_ = 0.0;  // worst frame since the previous report
  std::chrono::steady_clock::time_point last_swap_{};
  // Resolved texture grid diagnostic (bo2zm_native_diag_resolved).
  FnBlit blit_ = nullptr;
  // The side of the square depth target bo2zm_native_shadow_scale acts on (BO2's shadow target is 560x720, so
  // it never matches), and the scale it is being drawn at (0 = not decided yet).
  static constexpr uint32_t kSideShadows = 1600;
  bool depth_scalable_ = false;
  uint32_t scale_shadows_ = 0;
  uint64_t shadows_uploads_ = 0;
  std::vector<uint32_t> resolved_frame_;
  OwnedImage tiled_;
  std::chrono::steady_clock::time_point last_warning_tiled_{};
  // Read-back of small resolved textures (bo2zm_native_read_resolved_texels).
  std::unordered_map<uint64_t, Readback> reads_;
  std::unordered_map<uint64_t, ReadTarget> reads_per_target_;  // C2 report and cadence
  std::chrono::steady_clock::time_point report_reads_{};
  std::vector<PendingRead> reads_pending_;  // recorded in the current submission
  uint64_t reads_done_ = 0;
  // Reflection diagnostic (AnnotateCopy).
  uint64_t drawn_last_copy_ = 0;
  std::unordered_map<uint32_t, TargetCopy> copies_per_target_;
  std::chrono::steady_clock::time_point report_copies_{};
  VkFormat format_depth_ = VK_FORMAT_UNDEFINED;
  // The draws with the library shaders (nullptr without the required capabilities).
  std::unique_ptr<VulkanDraws> draws_;

  std::unordered_map<uint64_t, OwnedImage> targets_;
  std::unordered_map<uint32_t, Resolved> resolved_;
  // Resolved images set aside when their address takes another shape (GetResolved), by address,
  // size and format.
  std::unordered_map<uint64_t, Resolved> parked_;
  bool park_resolved_ = REXCVAR_GET(bo2zm_native_park_resolves);
  uint64_t resolved_unparked_ = 0;
  // bo2zm_native_diag_readers_s (DiagReadersOnPresent). Ring thread only.
  struct DiagReader {
    uint32_t ps = 0;        // PS number in the library (kReaderSwap: the Swap presenting the front buffer)
    uint32_t reg_index = 0;  // sampler (fetch constant)
    uint32_t target = 0;   // EDRAM base of color 0 | pitch << 12: where it draws
    uint32_t times = 0;     // draws
  };
  struct DiagWriteEntry {
    int32_t frame = -1;  // 0 or 1 within the window; -1: the last one before the window
    uint32_t order = 0;      // n-th write to that address in its frame
    uint32_t source = 0;     // EDRAM base | pitch << 12 of the target being resolved
    uint32_t requested_width = 0;
    uint32_t requested_height = 0;
    uint32_t width = 0;  // the texture's
    uint32_t height = 0;
    uint64_t draws = 0;    // draws since the previous copy (any)
    bool depth = false;
    bool whole = false;  // covers it fully: from (0,0) and at least its size
    uint32_t others = 0;   // readers that do not fit in the list
    std::vector<DiagReader> readers;
  };
  // A write classified as UNREAD in a window, watched in every frame since then.
  struct DiagWatched {
    uint32_t address = 0;
    uint32_t source = 0;          // EDRAM base | pitch << 12 of the watched writes
    bool pending = false;       // the texture's last write comes from that source
    bool source_written = false;  // and the source was drawn to or cleared since then
    uint64_t writes = 0;      // from that source since watching began
    uint64_t before = 0;           // reads before the source is rewritten (a deferred copy covers them)
    uint64_t late = 0;         // reads after the source is written (dropping the copy there would fail)
    uint32_t ps_before = 0;        // PS of the first one of each kind
    uint32_t ps_late = 0;
  };
  static constexpr uint32_t kReaderSwap = UINT32_MAX;
  static constexpr size_t kDiagMaxWrites = 32;
  static constexpr size_t kDiagMaxReaders = 12;
  static constexpr size_t kDiagMaxWatched = 4;
  static constexpr uint32_t kDiagRegSurfaceInfo = 0x2000;  // RB_SURFACE_INFO
  static constexpr uint32_t kDiagRegColorInfo = 0x2001;    // RB_COLOR_INFO
  static constexpr uint32_t kDiagRegFetch = 0x4800;        // SHADER_CONSTANT_FETCH_00_0
  static constexpr uint32_t kDiagRegColorMask = 0x2104;    // RB_COLOR_MASK: 4 bits per color target
  static constexpr uint32_t kDiagRegsColorInfo[4] = {0x2001, 0x2003, 0x2004, 0x2005};  // RB_COLOR_INFO, RB_COLORn_INFO
  bool diag_window_ = false;
  uint32_t diag_frame_ = 0;
  uint64_t diag_windows_ = 0;
  uint64_t diag_writes_lost_ = 0;
  std::chrono::steady_clock::time_point diag_readers_last_ = std::chrono::steady_clock::now();
  std::unordered_map<uint32_t, std::vector<DiagWriteEntry>> diag_writes_;
  std::unordered_map<uint32_t, uint64_t> diag_signatures_;  // pattern of the last line written, per address
  std::vector<DiagWatched> diag_watched_;
  // bo2zm_native_lazy_composite (PostponeComposite). A single deferred copy at a time.
  struct PendingComposite {
    uint32_t address = 0;
    uint32_t source = 0;                 // EDRAM base | pitch << 12 of the source target
    OwnedImage* target = nullptr;           // that target (targets_ never erases: the pointer stays valid)
    VkImage source_vk = VK_NULL_HANDLE;  // its image when deferred: if it changes, it can no longer be recorded
    VkImage texture_vk = VK_NULL_HANDLE;
    VkImageCopy copy{};
  };
  static constexpr uint64_t kCompositeALook = 900;  // watched writes without late reads (~30 s of play)
  PendingComposite composite_;
  bool composite_has_ = false;
  bool composite_off_ = false;
  uint32_t composite_expired_ = 0;   // address whose last copy was dropped; 0 = none
  uint32_t composite_applying_ = 0;  // address of the last APPLYING notice
  uint64_t composite_postponed_ = 0;
  uint64_t composite_recorded_read_ = 0;
  uint64_t composite_recorded_others_ = 0;
  uint64_t composite_dropped_source_ = 0;
  uint64_t composite_replaced_ = 0;
  uint64_t composite_reads_late_ = 0;
  uint64_t composite_mismatches_ = 0;
  uint64_t composite_pixels_saved_ = 0;
  uint64_t composite_pixels_previous_ = 0;
  uint64_t composite_presented_previous_ = 0;
  std::unordered_map<uint64_t, DepthTarget> depths_;
  uint64_t depth_draw_order_ = 0;
  uint64_t warnings_depth_banks_ = 0;

  VkSampler sampler_ = VK_NULL_HANDLE;
  VkDescriptorSetLayout layout_descriptors_ = VK_NULL_HANDLE;
  VkPipelineLayout layout_pipeline_ = VK_NULL_HANDLE;
  VkDescriptorPool pool_descriptors_ = VK_NULL_HANDLE;
  std::array<VkDescriptorSet, kSlotsOutput> descriptors_output_{};
  VkShaderModule vs_ = VK_NULL_HANDLE;
  VkShaderModule fs_ = VK_NULL_HANDLE;
  VkRenderPass render_pass_output_ = VK_NULL_HANDLE;
  VkPipeline pipeline_ = VK_NULL_HANDLE;
  // Output with the game's gamma ramp (bo2zm_native_gamma_ramp).
  struct OutputRamp {
    VkBuffer buffer = VK_NULL_HANDLE;
    VkDeviceMemory guest_memory = VK_NULL_HANDLE;
    uint8_t* data = nullptr;
    uint32_t type = 0;
    uint64_t version = UINT64_MAX;  // the ramp_values_ version the buffer holds
    bo2zm::native::output::Crosshairs crosshairs;
  };
  bool ramp_gamma_ = false;
  std::array<OutputRamp, kSlotsOutput> ramps_output_{};
  VkShaderModule fs_ramp_ = VK_NULL_HANDLE;
  // Index 4 * fxaa + 2 * grading + exact: 0 bilinear (like guest_output_bilinear_ps), 1 exact texel
  // (output the same size as the source); +2 with the saturation, vibrance, vignette or scanline
  // post-processing; +4 with FXAA. Those other than 0 and 1 are created on request (PipelineRamp).
  std::array<VkPipeline, 8> pipelines_ramp_{};
  std::array<bool, 8> pipelines_ramp_failed_{};
  bool warned_ramp_bilinear_ = false;
  // The home screen's credits line (CreditsReady).
  struct Credits {
    VkImage image = VK_NULL_HANDLE;
    VkDeviceMemory memory = VK_NULL_HANDLE;
    VkImageView view = VK_NULL_HANDLE;
    VkBuffer staging = VK_NULL_HANDLE;
    VkDeviceMemory staging_memory = VK_NULL_HANDLE;
    VkDescriptorPool pool = VK_NULL_HANDLE;
    VkDescriptorSet set = VK_NULL_HANDLE;
    VkShaderModule fs = VK_NULL_HANDLE;
    VkPipeline pipeline = VK_NULL_HANDLE;
    bool uploaded = false;
    bool failed = false;
  } credits_;
  // The game's ramp in 10 bits (red, green, blue). Until the game loads its own, the identity the SDK starts
  // with (i * 1023 / 255).
  std::array<std::array<uint16_t, 3>, 256> ramp_game_ = [] {
    std::array<std::array<uint16_t, 3>, 256> t{};
    for (uint32_t i = 0; i < 256; ++i) {
      t[i].fill(uint16_t(i * 0x3FF / 0xFF));
    }
    return t;
  }();
  // The output UBO in std140: one vec4 per entry (red, green and blue in 0-1, with the single-channel
  // post-processing applied), followed by the mix (saturation, vibrance, 1 / gamma) and effects (vignette,
  // scanlines). Filled by RecomputeRamp.
  static constexpr size_t kFloatsRamp = 256 * 4 + 8;
  std::array<float, kFloatsRamp> ramp_values_{};
  uint64_t version_ramp_ = 0;
  // Post-processing in effect and the settings version it was computed from.
  bo2zm::settings::Postprocess postprocess_{};
  uint64_t version_postprocess_ = 0;
  bool grading_ = false;  // shader variant with saturation, vibrance, vignette or scanlines
  bool fxaa_annotated_ = false;  // bo2zm_antialiasing at the last output (to log changes)
  std::array<Framebuffer, VulkanPresenter::kMaxActiveGuestOutputImageVersions> framebuffers_{};
  size_t next_framebuffer_ = 0;

  std::chrono::steady_clock::time_point start_alternation_ = std::chrono::steady_clock::now();
  bool alternation_annotated_ = false;
  uint64_t swaps_ = 0;    // resolves without a copy
  uint64_t restores_ = 0;
  uint64_t restores_for_resolve_ = 0;  // bo2zm_native_resolve_valid_content
  uint64_t resolve_no_source_ = 0;           // the content was in no texture
  uint64_t resolve_content_old_ = 0;      // with the setting off, resolves of the stale image
  uint64_t swaps_no_clear_ = 0;  // the ones that used to copy the whole map
  uint64_t swaps_color_ = 0;  // color targets resolved without a copy
  // Copies and clears removed, and what they cost.
  std::unordered_map<const OwnedImage*, TargetState> state_target_;
  // CopyConverted: resolves that are not a plain bit copy.
  static constexpr uint32_t kConvSets = 256;
  VkShaderModule conv_fs_ = VK_NULL_HANDLE;
  VkDescriptorPool conv_pool_ = VK_NULL_HANDLE;
  std::array<VkDescriptorSet, kConvSets> conv_sets_{};
  uint64_t conv_next_ = 0;
  bool conv_failed_ = false;
  std::unordered_map<VkFormat, VkRenderPass> conv_passes_;
  std::unordered_map<VkFormat, VkPipeline> conv_pipelines_;
  std::unordered_map<VkImageView, VkFramebuffer> conv_framebuffers_;
  uint64_t copies_converted_ = 0;
  std::unordered_set<uint64_t> conv_views_;
  // Resolves through an alias (Copy): surface pairs already logged, and blits not done
  std::unordered_set<uint64_t> alias_seen_;
  uint32_t alias_failed_ = 0;
  // 3D textures built from 2D resolves (TextureResolved3D), by address.
  struct ResolvedVolume {
    OwnedImage image;  // VK_IMAGE_TYPE_3D: width x height x depth
    uint32_t depth3d = 0;
    uint32_t pitch = 0;        // of the 3D fetch, in texels
    uint32_t pitch_flat = 0;  // of the resolve it comes from
    uint32_t block = 0;       // block size of the regions: width | height << 16
    uint64_t write = 0;    // writes_resolved_ value it was built from
    VkImage source = VK_NULL_HANDLE;
    std::vector<VkImageCopy> regions;
  };
  std::unordered_map<uint32_t, ResolvedVolume> volumes_;
  std::unordered_map<uint32_t, uint64_t> writes_resolved_;  // resolve address -> its last write
  uint64_t counter_writes_ = 0;
  uint64_t volumes_done_ = 0;
  uint64_t clears_skipped_ = 0;             // color ones
  uint64_t clears_skipped_depth_ = 0;
  uint64_t pixels_clears_skipped_ = 0;
  uint64_t clears_useless_ = 0;             // clears wiped out by a swap with no draw in between
  uint64_t pixels_clears_useless_ = 0;
  uint64_t restores_clipped_ = 0;     // restores that did not copy the whole image
  uint64_t pixels_restored_ = 0;           // the ones actually copied (so the report adds up)
  uint64_t pixels_restore_saved_ = 0;   // the ones not copied, due to useful area or swap
  uint64_t loans_ = 0;                     // restorations done by swapping again
  uint64_t lent_read_ = 0;              // times a lent resolved texture was requested (must be 0)
  std::unordered_set<uint32_t> lent_;     // bases whose content is lent to the render target
  // bo2zm_native_shadow_min. State of the shadow map cycle and phase of the guard.
  enum : uint32_t { kSmFree = 0, kSmAfterBase = 1, kSmOverlay = 2 };
  enum : uint32_t { kSmObserving = 0, kSmApplying = 1, kSmOff = 2 };
  static constexpr uint64_t kShadowMinCycles = 300;  // consecutive clean cycles before applying (~10 s of play)
  uint32_t sm_phase_ = kSmObserving;
  uint32_t sm_state_ = kSmFree;
  bool sm_on_ = false;                  // cvar, alternation and cheap PCF, read at the start of each cycle
  bool sm_span_annotated_ = true;               // bo2zm_native_shadow_min_alternate_s: last interval written to the log
  std::chrono::steady_clock::time_point sm_start_alternation_ = std::chrono::steady_clock::now();
  const OwnedImage* sm_target_cycle_ = nullptr;   // the current cycle: the shadow target...
  uint32_t sm_base_cycle_ = 0;                // ...and the texture that received the world (no clear)
  bool sm_cycle_clean_ = false;               // nothing watched has failed in this cycle
  bool sm_cycle_clear_ = false;              // this cycle's overlay pass draws on the cleared target
  bool sm_base_rewritten_ = false;            // the world texture was rewritten within the cycle
  const OwnedImage* sm_target_ = nullptr;         // learned while observing: the target...
  uint32_t sm_base_ = 0;                      // ...the texture without the overlay (texture[1])...
  uint32_t sm_overlay_ = 0;                     // ...and the texture with the overlay (texture[0])
  bool sm_virtual_ = false;                    // sm_overlay_ holds only the overlay: sampled with the minimum
  bool sm_virtual_valid_ = false;             // and sm_base_ still holds the world of that same cycle
  uint64_t sm_clean_consecutive_ = 0;
  uint64_t sm_cycles_ = 0;
  uint64_t sm_cycles_applied_ = 0;
  uint64_t sm_pixels_saved_ = 0;
  uint64_t sm_pixels_saved_previous_ = 0;
  uint64_t sm_draws_overlay_ = 0;
  uint64_t sm_draws_no_exact_ = 0;
  uint64_t sm_reads_min_ = 0;            // of the overlay-only texture, paired with the base map
  uint64_t sm_reads_yes_same_ = 0;          // of the usual texture, paired with itself (observing)
  uint64_t sm_reads_normal_ = 0;          // of the usual texture, without the minimum
  uint64_t sm_reads_a_offbeat_ = 0;       // between the two resolves of a cycle
  uint64_t sm_reads_unable_ = 0;         // from a shader without tfetch2DShadowMin on that register
  uint64_t sm_mismatches_ = 0;
  // bo2zm_native_diag_clears (see ClearUse).
  std::unordered_map<const OwnedImage*, ClearUse> use_clears_;  // targets_ and depths_ never erase: the key stays valid
  bool diag_clears_ = REXCVAR_GET(bo2zm_native_diag_clears);
  std::chrono::steady_clock::time_point report_clears_{};
  uint64_t presented_report_clears_ = 0;
  // bo2zm_native_clear_useful_area.
  bool clear_area_useful_ = REXCVAR_GET(bo2zm_native_clear_useful_area);
  bool area_useful_global_off_ = false;
  uint32_t bands_pending_ = 0;  // targets with an uncleared band: AnnotateUseClear checks them even when not measuring
  uint64_t area_useful_clears_ = 0;
  uint64_t area_useful_completed_ = 0;
  uint64_t area_useful_misses_ = 0;
  uint64_t area_useful_pixels_ = 0;
  uint64_t area_useful_pixels_previous_ = 0;
  static constexpr uint64_t kAreaUsefulCycles = 120;  // measured clears of a target before trimming its own
  // bo2zm_native_lazy_depth. Deferred depth copies, by texture address.
  struct PendingCopy {
    const OwnedImage* source = nullptr;  // depth target (depths_ never erases: the pointer stays valid)
    VkImage source_vk = VK_NULL_HANDLE;
    VkImage target_vk = VK_NULL_HANDLE;
    VkImageCopy copy{};
    uint64_t frame = 0;     // presented_ when postponing it
    bool read_dead = false;  // the composite without blur already requested it
  };
  std::unordered_map<uint32_t, PendingCopy> pending_;
  std::unordered_map<uint32_t, uint64_t> last_read_live_;    // per address, in presented_
  std::unordered_map<uint32_t, uint64_t> last_read_dead_;
  std::unordered_set<uint32_t> expired_;     // dropped copies: their texture lacks the last resolve
  bool reads_depth_dead_ = false;  // set by VulkanDraws (DeadDepthReads)
  bool lazy_off_ = false;              // the guard saw a late read
  uint64_t lazy_postponed_ = 0;
  uint64_t lazy_copied_read_ = 0;
  uint64_t lazy_copied_write_ = 0;
  uint64_t lazy_replaced_ = 0;
  uint64_t lazy_dropped_ = 0;
  uint64_t lazy_reads_late_ = 0;
  uint64_t lazy_pixels_saved_ = 0;
  uint64_t lazy_pixels_saved_previous_ = 0;
  static constexpr uint64_t kLazyFrames = 30;  // ~1 s: window for "requested" and "nobody samples it"
  // bo2zm_native_lazy_front (see PendingFront).
  std::unordered_map<uint32_t, PendingFront> fronts_pending_;  // per texture address
  std::vector<FrontImage> fronts_images_;                         // spares and retained ones
  std::unordered_map<uint32_t, uint64_t> fronts_presented_;  // address -> presented_ at its last Swap
  std::unordered_map<uint32_t, uint64_t> fronts_read_;       // address -> last sampling by a draw
  // Logical targets survive image swaps and unordered_map rehashes; Destroy removes their ownership entry.
  std::unordered_map<const NativeImage*, FrontComposition> front_compositions_;
  std::unordered_set<uint32_t> front_composition_sampled_;  // sticky until the resolved allocation changes shape
  uint64_t front_partial_clears_ = 0;
  bool front_partial_clear_failed_ = false;
  bool front_trace_active_ = false;
  uint64_t front_trace_lines_ = 0, front_trace_suppressed_ = 0;
  std::unordered_set<uint32_t> fronts_expired_;              // missing a copy that can no longer be made
  bool front_off_ = false;
  uint64_t front_postponed_ = 0;
  uint64_t front_painted_target_ = 0;
  uint64_t front_painted_retained_ = 0;
  uint64_t front_rotations_ = 0;
  uint64_t front_no_spare_ = 0;
  uint64_t front_copied_read_ = 0;
  uint64_t front_copied_write_ = 0;
  uint64_t front_copied_swap_ = 0;
  uint64_t front_replaced_ = 0;
  uint64_t front_reads_late_ = 0;
  uint64_t front_pixels_saved_ = 0;
  uint64_t front_pixels_saved_previous_ = 0;
  static constexpr uint64_t kFrontFrames = 8;    // window for "the Swap presents it" and "nobody samples it"
  static constexpr size_t kFrontImagesMax = 3;     // spares of the front buffer's target (~4 MB each at 1040)
  // The values above as they were at the previous report: the new lines are per frame.
  uint64_t pixels_restored_previous_ = 0;
  uint64_t pixels_restore_saved_previous_ = 0;
  uint64_t pixels_clears_skipped_previous_ = 0;
  uint64_t pixels_clears_useless_previous_ = 0;
  uint64_t presented_report_copies_ = 0;
  // State of the previous frame, for the stutter dump.
  uint64_t hitch_copies_ = 0, hitch_clears_ = 0, hitch_resolves_ = 0;
  uint64_t hitch_restores_ = 0, hitch_waits_ = 0;
  // Snapshot of the waits at the previous Swap, for the "[hitch] waits" line.
  uint64_t hitch_waits_ns_[bo2zm::waits::kNumTypes] = {};
  uint64_t hitch_waits_times_[bo2zm::waits::kNumTypes] = {};
  uint64_t hitch_ns_waits_gpu_ = 0, hitch_ns_wait_output_ = 0;
  uint64_t hitch_bytes_hash_ = 0, hitch_hashes_postponed_ = 0;
  uint64_t hitch_draws_ = 0, hitch_ns_ring_ = 0, hitch_ns_textures_ = 0;
  uint64_t hitch_textures_uploads_ = 0, hitch_bytes_uploaded_ = 0, hitch_textures_created_ = 0;
  uint64_t hitch_ns_hash_raw_ = 0, hitch_ns_hash_data_ = 0;
  uint64_t hitch_ns_waiting_copies_ = 0, hitch_waits_copies_ = 0;
  uint64_t hitch_ns_helping_copies_ = 0, hitch_copies_helped_ = 0;
  uint64_t hitch_ns_create_textures_ = 0, hitch_textures_bound_thread_ = 0;
  uint64_t hitch_ns_waiting_bindings_ = 0;
  uint64_t hitch_textures_hash_thread_ = 0, hitch_ns_hash_thread_ = 0, hitch_ns_hash_snapshot_ = 0;
  uint64_t hitch_textures_hash_ring_ = 0, hitch_ns_hash_ring_ = 0, hitch_ns_waiting_hashes_ = 0;
  // Why the restore does not swap. See the block in RestoreContent.
  uint64_t no_swap_no_transfer_dst_ = 0;
  uint64_t no_swap_target_no_prepare_ = 0;
  uint64_t no_swap_resolved_no_prepare_ = 0;
  uint64_t no_swap_sizes_different_ = 0;
  uint64_t hitch_gpu_ns_ = 0;      // GPU timestamps when the previous frame closed
  uint64_t hitch_ns_record_ = 0;   // ns recording when the previous frame closed
  uint32_t warnings_hitch_ = 0;
  uint64_t copies_ = 0;
  uint64_t copies_depth_ = 0;  // those that force keeping the depth tile
  uint64_t clears_depth_ = 0;  // depth clears (clears_ only counts color ones)
  uint64_t clears_in_pass_failed_ = 0;  // clears through a pass that could not be opened
  uint64_t clears_ = 0;
  uint64_t presented_ = 0;
  // bo2zm_show_fps
  std::chrono::steady_clock::time_point fps_since_{};
  uint64_t fps_presented_ = 0;
  uint32_t fps_tenths_ = 0;
  uint64_t rejections_ = 0;
  std::unordered_set<uint32_t> warned_;
};

}  // namespace

std::unique_ptr<NativeTargets> NativeTargets::Create(const VulkanDevice* vulkan_device,
                                                        rex::memory::Memory* guest_memory) {
  if (!vulkan_device || !guest_memory) {
    return nullptr;
  }
  auto targets = std::make_unique<VulkanTargets>(vulkan_device, guest_memory);
  if (!targets->Init()) {
    REXLOG_ERROR("[native] C2: could not prepare the target presentation");
    return nullptr;
  }
  return targets;
}

}  // namespace bo2zm::native
