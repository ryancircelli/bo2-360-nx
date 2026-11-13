// Derived from GPL-3.0 code; modified for bo2-zm-nx in 2026.
//
// Graphics settings for the native renderer and the settings menu (see bo2zm_graphics_settings.h).

#include "bo2zm_graphics_settings.h"

#include "bo2zm_native_render.h"

#include <rex/cvar.h>
#include <rex/logging.h>
#include <rex/ui/overlay/settings_overlay.h>

#include <atomic>
#include <cstdlib>
#include <string>
#include <vector>

REXCVAR_DEFINE_STRING(bo2zm_internal_resolution, "auto", "Graphics",
                      "Video mode passed to the game. It does nothing in BO2: nothing applies it, and the game draws "
                      "its 3D view at 880x720 at every setting and stretches it to 1280x720 itself. auto: 1920x1080 "
                      "docked and 1280x720 handheld, switching on the fly when the console goes in and out of the "
                      "dock, and also following Reverse-NX. 1280x720 is the Xbox 360's output. 1920x1080 always forces "
                      "it. 1024x576 (asks for a restart), 640x360 and 640x480 are lower modes")
    .allowed({"auto", "1280x720", "1920x1080", "1024x576", "640x360", "640x480"});

/*
 * GPU clock in handheld mode.
 *
 * The console starts games with the GPU at 307.2 MHz. Nintendo later added a 460.8 MHz profile for games that need
 * it. Requesting it is not an overclock: no KIP or sysmodule is involved, it goes through the official API (apm), the
 * same one retail games use. Games that do not need it simply leave it alone for the battery's sake. 307.2 to 460.8
 * MHz is 50 % more GPU, at the cost of battery life and heat.
 *
 * The profiler prints the real frequency in every report ("clocks: CPU 1020.0 MHz, GPU 307.2 MHz, ..."); if it still
 * says 307.2 after a request, the request did not take effect.
 *
 * The default is 0: the game requests no clock at all, so the system's clocks apply, or whatever sys-clk (or one of
 * its forks) has been configured to set, with nothing in between.
 */
REXCVAR_DEFINE_INT32(bo2zm_switch_gpu_mhz, 0, "Graphics",
                     "Switch: GPU MHz requested from the system in handheld mode (0 = touch nothing: the system's "
                     "clocks, or sys-clk's; 384, 460). 460.8 MHz is an official Horizon profile, not an overclock; it "
                     "uses more battery and runs hotter")
    .allowed({"0", "384", "460"});

/*
 * The memory clock that comes with the high GPU profile.
 *
 * The high GPU configurations come in two flavors: memory at 1331.2 MHz (0x92220008) and memory at 1600 MHz
 * (0x92220007). The first is preferred, because a higher memory clock costs battery and heat without giving anything
 * back here: bandwidth use was measured well below the limit, and what saturates is the fragment ALU. If the firmware
 * lacks the first profile, the default leaves the GPU alone rather than raise the memory clock; setting this to true
 * accepts the second profile instead.
 */
REXCVAR_DEFINE_BOOL(bo2zm_switch_ram_1600, false, "Graphics",
                    "Switch: accept the high GPU profile even if it raises the memory to 1600 MHz. Default NO: if there "
                    "is none that leaves the RAM where it was, the GPU stays as it was. The [apm] log says which one "
                    "was applied");

REXCVAR_DEFINE_BOOL(bo2zm_switch_saltynx, true, "Graphics",
                    "Switch: publishes the FPS and the resolution where the console's overlays read them (SaltyNX) and "
                    "follows Reverse-NX's handheld/docked mode. Without SaltyNX it does nothing");

REXCVAR_DEFINE_BOOL(bo2zm_switch_clocks_reverse, false, "Graphics",
                    "Switch: make the clocks follow Reverse-NX when it does not match the hardware, in BOTH directions, "
                    "by asking the sysmodule (Horizon OC and other sys-clk forks). If it fakes DOCKED with the console "
                    "in your hands, YOUR docked column is applied (the game profile's or the global one): that is an "
                    "overclock and is no good for measuring. If it fakes HANDHELD with the console in the dock, it "
                    "drops to the handheld column and, if yours is empty, to the stock clocks (GPU 307.2 MHz and memory "
                    "1331.2). It is released when they match again and on exit");
/*
 * The FPS limit (applied only by ApplySettingsGraphics, which BO2 does not call).
 *
 * bo2zm_fps_limit ends up in video_mode_refresh_rate, and LoopVblank (bo2zm_native_render.cpp) reads it from there
 * to know how often to fire the guest's vblank interrupt; at "60" the vblank thread ticks at a steady 60.1 Hz. The
 * setting neither throttles nor drops work for a game whose pace comes from its own work rather than from counting
 * vblanks: "30" only lowers the guest interrupt to 30 Hz (0.25 % of a core) and tells the game the panel runs at 30,
 * which changes nothing while a frame takes longer than 33.3 ms.
 */
REXCVAR_DEFINE_STRING(bo2zm_fps_limit, "60", "Graphics",
                      "Maximum game FPS. 60: no limit of its own (the game runs at the pace of a 60 Hz vblank). 30: a "
                      "fixed 30 FPS pace, without ups and downs, with the game at its normal speed. Applied on restart")
    .allowed({"60", "30"})
    .lifecycle(rex::cvar::Lifecycle::kRequiresRestart);

/*
 * The presentation interval, also applied only by ApplySettingsGraphics. (BO2's
 * own VSYNC option sets the interval through RexSwitchPerfSwapIntervalForce instead.)
 *
 * The FPS limit above does not limit presentation: it sets the rate of the guest's vblank interrupt, and a game that
 * paces itself by counting vblanks then runs on two free-running 30 Hz clocks (the process timer and the panel) that
 * beat against each other, giving mostly 33.3 ms frames with one at 50 ms every few seconds. The interval is the other
 * approach: the compositor shows one image every N vblanks, so with 2 there is a single clock, the panel's, and its
 * cadence reaches the ring thread through back-pressure.
 *
 * The default of 0 matters. The console's swapchain is created in IMMEDIATE mode, which in the Horizon WSI means
 * nwindowSetSwapInterval(nw, 0): the queue no longer blocks the producer. That closes a gap of several ms in which the
 * GPU sat idle while the ring thread was inside presentation. The SDK reapplies the interval on every presentation
 * (on purpose: the swapchain is recreated when the console goes in or out of the dock), so any non-zero value here
 * overwrote the WSI's 0 on the first frame and undid IMMEDIATE. With 0 the swapchain's mode is left alone. An interval
 * of 2 still works for locking to the panel, but only pays when the work really fits in 33.3 ms.
 */
REXCVAR_DEFINE_UINT32(bo2zm_swap_interval, 0, "Graphics",
                      "Switch: vblanks between images. 0 = leave it alone, the swapchain's mode decides (the usual "
                      "IMMEDIATE, that is interval 0, which is what removes the gap). 2 = 30 FPS "
                      "locked by the panel, only useful if the work fits in 33.3 ms. 1 = up to 60");

// Optional antialiasing on the output, applied live. The Xbox 360 draws the scene with MSAA; here there is none (it
// costs GPU time, and it has hung NVK before), and FXAA smooths the edges for little work.
REXCVAR_DEFINE_STRING(bo2zm_antialiasing, "off", "Graphics",
                      "Image antialiasing. off: as until now. fxaa: smooths jagged edges (the Xbox 360 uses 2x MSAA, "
                      "which is not available here); costs some GPU")
    .allowed({"off", "fxaa"});

// Sky glow, applied live, to the bright pass the renderer recognizes by hash (kHashBrightPass in
// bo2zm_native_draws.cpp; no BO2 shader has that hash, so in BO2 this changes nothing). That pass subtracts its
// threshold per channel, so with an intense blue sky only the blue channel passes and leaves blue edges. "natural"
// keeps the same glow energy with the hue of the source color; "soft" applies the threshold to luminance, with much
// less halo. natural is the default: compared on the console, the original still showed a noticeable blue halo.
REXCVAR_DEFINE_STRING(bo2zm_sky_glow, "natural", "Graphics",
                      "Sky glow, for a bright pass the renderer recognizes by hash (no BO2 shader has it, so this "
                      "does nothing in BO2). natural (default): the glow without the saturated blue that leaves edges "
                      "against the sky; colored lights unchanged. original: like the Xbox 360, with those blue edges. "
                      "soft: almost no sky halo; single-color lights glow less. No cost")
    .allowed({"original", "natural", "soft"});

// A color filter, applied live: the "visual treatment" (the color curves, the desaturation and the vignette) of a
// final composite the renderer recognizes by hash (kHashComposite in bo2zm_native_draws.cpp; no BO2 shader has that
// hash, so in BO2 this changes nothing).
REXCVAR_DEFINE_STRING(bo2zm_visual_treatment, "original", "Graphics",
                      "Color filter of a final composite the renderer recognizes by hash (color curves, desaturation "
                      "and vignette); no BO2 shader has it, so this does nothing in BO2. original (default): like the "
                      "Xbox 360. soft: at half strength. off: no filter; the glow and the fades stay the same. No cost")
    .allowed({"original", "soft", "off"});

// Optional post-processing, applied live (the change is visible with the menu still open). The values are option
// lists, which are easy to change with a gamepad. The presets' values come from a public-domain post-processing
// filter set.
REXCVAR_DEFINE_STRING(bo2zm_postprocess, "off", "Graphics/Postprocess",
                      "Image postprocessing. off: like the Xbox 360. Presets: cinema, sepia, noir, cold, warm, vivid, "
                      "matrix and crt. custom: the values below. Grading of a single channel costs nothing; saturation, "
                      "vibrance, vignette and scanlines do some work per pixel")
    .allowed({"off", "cinema", "sepia", "noir", "cold", "warm", "vivid", "matrix", "crt", "custom"});
REXCVAR_DEFINE_STRING(bo2zm_postprocess_brightness, "0.00", "Graphics/Postprocess",
                      "Custom: brightness added (0.00 = no change)")
    .allowed({"-0.20", "-0.15", "-0.10", "-0.05", "0.00", "+0.05", "+0.10", "+0.15", "+0.20"});
REXCVAR_DEFINE_STRING(bo2zm_postprocess_contrast, "1.00", "Graphics/Postprocess",
                      "Custom: contrast around middle gray (1.00 = no change)")
    .allowed({"0.70", "0.80", "0.90", "1.00", "1.10", "1.20", "1.30", "1.40"});
REXCVAR_DEFINE_STRING(bo2zm_postprocess_saturation, "1.00", "Graphics/Postprocess",
                      "Custom: saturation (0.00 = black and white, 1.00 = no change)")
    .allowed({"0.00", "0.25", "0.50", "0.75", "1.00", "1.10", "1.20", "1.35", "1.50", "2.00"});
REXCVAR_DEFINE_STRING(bo2zm_postprocess_vibrance, "0.00", "Graphics/Postprocess",
                      "Custom: vibrance, boosts the less saturated colors more (0.00 = no change)")
    .allowed({"-0.50", "-0.25", "0.00", "+0.15", "+0.25", "+0.50", "+0.75", "+1.00"});
REXCVAR_DEFINE_STRING(bo2zm_postprocess_temperature, "0.00", "Graphics/Postprocess",
                      "Custom: temperature, warm (+) or cold (-) (0.00 = no change)")
    .allowed({"-1.00", "-0.75", "-0.50", "-0.25", "0.00", "+0.25", "+0.50", "+0.75", "+1.00"});
REXCVAR_DEFINE_STRING(bo2zm_postprocess_gamma, "1.00", "Graphics/Postprocess",
                      "Custom: gamma, above 1 brightens the midtones (1.00 = no change)")
    .allowed({"0.70", "0.80", "0.90", "1.00", "1.10", "1.20", "1.30"});
REXCVAR_DEFINE_STRING(bo2zm_postprocess_vignette, "0.00", "Graphics/Postprocess",
                      "Custom: vignette, darkens the edges (0.00 = no vignette)")
    .allowed({"0.00", "0.20", "0.30", "0.40", "0.55", "0.70", "1.00"});
REXCVAR_DEFINE_STRING(bo2zm_postprocess_scanlines, "0.00", "Graphics/Postprocess",
                      "Custom: old television scanlines, one row in three (0.00 = no scanlines)")
    .allowed({"0.00", "0.25", "0.50", "0.75", "1.00"});

namespace bo2zm::settings {
namespace {

std::atomic<uint64_t> g_version_postprocess{1};
std::atomic<bool> g_fxaa{false};
std::atomic<int> g_glow_sky{0};  // 0 original, 1 natural, 2 soft
std::atomic<int> g_treatment_visual{0};  // 0 original, 1 soft, 2 off

constexpr const char* kCvarsPostprocess[] = {
    "bo2zm_postprocess",           "bo2zm_postprocess_brightness",      "bo2zm_postprocess_contrast",
    "bo2zm_postprocess_saturation", "bo2zm_postprocess_vibrance",  "bo2zm_postprocess_temperature",
    "bo2zm_postprocess_gamma",     "bo2zm_postprocess_vignette",      "bo2zm_postprocess_scanlines",
};

// bo2zm_sky_glow: 0 original, 1 natural, 2 soft.
int ModeGlow(std::string_view value) {
  return value == "natural" ? 1 : value == "soft" ? 2 : 0;
}

// bo2zm_visual_treatment: 0 original, 1 soft, 2 off.
int ModeTreatment(std::string_view value) {
  return value == "soft" ? 1 : value == "off" ? 2 : 0;
}

float Number(const char* label, float per_default) {
  const std::string text = rex::cvar::GetFlagByName(label);
  char* end = nullptr;
  const double value = std::strtod(text.c_str(), &end);
  return end && end != text.c_str() ? float(value) : per_default;
}

// The command line takes precedence (PC tests with --video_mode_width=...). Whatever comes from the
// configuration file is replaced: SaveConfig saves the cvars that differ from their default value, and an
// old video mode must not win over the new settings.
void Put(const char* label, const std::string& value) {
  if (rex::cvar::GetFlagInfo(label) == nullptr) {
    REXLOG_WARN("[settings] cvar {} does not exist: {} is not applied", label, value);
    return;
  }
  if (rex::cvar::GetFlagSource(label) == rex::cvar::Source::kCommandLine) {
    REXLOG_INFO("[settings] {} comes from the command line ({}): left alone", label,
                rex::cvar::GetFlagByName(label));
    return;
  }
  if (!rex::cvar::SetFlagByName(label, value)) {
    REXLOG_WARN("[settings] could not set {} = {}", label, value);
  }
}

}  // namespace

#if REX_PLATFORM_SWITCH
// Provided by the SaltyNX module so that the console overlays can show the resolution.
extern "C" void RexSwitchPerfResolution(unsigned width, unsigned height);
extern "C" void RexSwitchPerfSwapInterval(unsigned vblanks);
extern "C" void RexSwitchSaltyNxEnable(int enabled);
extern "C" void RexSwitchClocksReverseEnable(int enabled);
extern "C" void RexSwitchApmRequestGpuMhz(int mhz);
extern "C" void RexSwitchApmAllowRam1600(int allow);
extern "C" void RexSwitchApmApply(void);
extern "C" void RexSwitchApmWatchMemory(int watch);
#endif

// The clock and overlay part of ApplySettingsGraphics, run at start-up. In handheld mode it requests the GPU clock
// bo2zm_switch_gpu_mhz names (none by default; 460 is the official 460.8 MHz profile, configuration 0x92220008, a
// stock mode retail games use, while homebrew starts at 307.2 MHz). Docked, the console keeps its own 768 MHz. Nothing
// is overclocked, and the memory watchdog stays off so that clocks raised with sys-clk are not undone. It also lets
// Status Monitor show the game's FPS (the SaltyNX block).
void RequestBootClocks() {
#if REX_PLATFORM_SWITCH
  RexSwitchSaltyNxEnable(REXCVAR_GET(bo2zm_switch_saltynx) ? 1 : 0);
  RexSwitchClocksReverseEnable(REXCVAR_GET(bo2zm_switch_clocks_reverse) ? 1 : 0);
  RexSwitchApmRequestGpuMhz(REXCVAR_GET(bo2zm_switch_gpu_mhz));
  RexSwitchApmAllowRam1600(REXCVAR_GET(bo2zm_switch_ram_1600) ? 1 : 0);
  RexSwitchApmWatchMemory(0);
  RexSwitchApmApply();
  REXLOG_INFO("[settings] clocks: GPU requested {} MHz in handheld mode (0 = the system's), memory not watched",
              REXCVAR_GET(bo2zm_switch_gpu_mhz));
#endif
}

void ApplySettingsGraphics() {
  const std::string resolution = REXCVAR_GET(bo2zm_internal_resolution);
  // 1920x1080 is not requested through the video mode: the video mode stays at 720p, which is what is presented, and
  // a 1920x1080 video mode would not enlarge the front buffer, which is registered earlier. A game whose render
  // targets follow a mode table of its own takes the internal size from this same cvar instead.
  const bool internal_1080p = resolution == "1920x1080" || resolution == "auto";
  const std::string mode_video = internal_1080p ? std::string("1280x720") : resolution;
  const size_t x = mode_video.find('x');
  if (x != std::string::npos) {
    Put("video_mode_width", mode_video.substr(0, x));
    Put("video_mode_height", mode_video.substr(x + 1));
  }
#if REX_PLATFORM_SWITCH
  // FPS and resolution for the console overlays, and honoring Reverse-NX. Can be turned off if it gets in the way.
  RexSwitchSaltyNxEnable(REXCVAR_GET(bo2zm_switch_saltynx) ? 1 : 0);
  RexSwitchClocksReverseEnable(REXCVAR_GET(bo2zm_switch_clocks_reverse) ? 1 : 0);
  // GPU profile in handheld mode. See bo2zm_switch_gpu_mhz.
  RexSwitchApmRequestGpuMhz(REXCVAR_GET(bo2zm_switch_gpu_mhz));
  RexSwitchApmAllowRam1600(REXCVAR_GET(bo2zm_switch_ram_1600) ? 1 : 0);
  RexSwitchApmApply();
  RexSwitchPerfSwapInterval(unsigned(REXCVAR_GET(bo2zm_swap_interval)));
#endif
  Put("resolution", "");  // empty preset: the width and height above apply
  const std::string limit = REXCVAR_GET(bo2zm_fps_limit);
  Put("video_mode_refresh_rate", limit == "30" ? "30" : "60");
  REXLOG_INFO("[settings] internal resolution {} and FPS limit {}: video mode {}x{} at {} Hz", resolution, limit,
              rex::cvar::GetFlagByName("video_mode_width"), rex::cvar::GetFlagByName("video_mode_height"),
              rex::cvar::GetFlagByName("video_mode_refresh_rate"));
#if REX_PLATFORM_SWITCH
  {
    // The overlay shows the resolution the game draws at, so with the 1080p internal mode it is told
    // 1920x1080, not the video mode.
    unsigned width = unsigned(std::strtoul(rex::cvar::GetFlagByName("video_mode_width").c_str(), nullptr, 10));
    unsigned height = unsigned(std::strtoul(rex::cvar::GetFlagByName("video_mode_height").c_str(), nullptr, 10));
    // Only the start-up value is set here: with "auto" the size changes on the fly, and whoever draws at it
    // publishes the real size every time it changes.
    if (resolution == "1920x1080") {
      width = 1920;
      height = 1080;
    }
    if (width && height) {
      RexSwitchPerfResolution(width, height);  // for the console overlay
    }
  }
#endif
}

void HideSettingsNoEffect() {
  if (!bo2zm::native::IsActive()) {
    return;  // with emulation, all of them do something
  }
  const std::vector<std::string> hidden = {
      // From the emulated path: they do nothing with the native renderer.
      "resolution_scale", "draw_resolution_scale_x", "draw_resolution_scale_y", "vsync", "anisotropic_override",
      "swap_post_effect",
      // Replaced by bo2zm_internal_resolution and bo2zm_fps_limit.
      "video_mode_width", "video_mode_height", "resolution", "video_mode_refresh_rate",
  };
  // On PC, the emulated path settings are registered by the GPU plugin, which is not loaded with the
  // native renderer: they do not exist there. On the Switch they are built into the executable and do show
  // up in the menu. They are hidden by name in both cases.
  std::string registered;
  std::string not_registered;
  for (const std::string& label : hidden) {
    std::string& list = rex::cvar::GetFlagInfo(label) ? registered : not_registered;
    list += (list.empty() ? "" : ", ") + label;
  }
  rex::ui::HideSettingsInMenu(hidden);
  REXLOG_INFO("[settings] menu: removed, because they do nothing with the native renderer or the Graphics category "
              "replaces them: {}; not registered in this build: {}",
              registered.empty() ? "none" : registered, not_registered.empty() ? "none" : not_registered);
}

Postprocess ReadPostprocess() {
  struct Preset {
    const char* label;
    Postprocess values;
  };
  // The values of the public-domain filter set: brightness, contrast, saturation, vibrance, temperature, gamma, tint
  // (red, green, blue and strength), vignette and scanlines.
  static const Preset kPresets[] = {
      {"cinema", {true, -0.04f, 1.15f, 1.05f, 0.15f, -0.10f, 1.0f, 1.0f, 0.95f, 0.85f, 0.12f, 0.40f, 0.0f}},
      {"sepia", {true, -0.02f, 1.05f, 0.20f, 0.0f, 0.25f, 1.0f, 1.0f, 0.82f, 0.55f, 0.50f, 0.30f, 0.0f}},
      {"noir", {true, -0.03f, 1.35f, 0.0f, 0.0f, 0.0f, 1.0f, 1.0f, 1.0f, 1.0f, 0.0f, 0.55f, 0.0f}},
      {"cold", {true, 0.0f, 1.05f, 1.0f, 0.10f, -0.55f, 1.0f, 0.70f, 0.85f, 1.0f, 0.15f, 0.18f, 0.0f}},
      {"warm", {true, 0.02f, 1.05f, 1.05f, 0.15f, 0.55f, 1.0f, 1.0f, 0.85f, 0.60f, 0.12f, 0.18f, 0.0f}},
      {"vivid", {true, 0.0f, 1.10f, 1.20f, 0.50f, 0.0f, 1.0f, 1.0f, 1.0f, 1.0f, 0.0f, 0.0f, 0.0f}},
      {"matrix", {true, -0.03f, 1.10f, 0.85f, 0.20f, 0.0f, 1.0f, 0.55f, 1.0f, 0.60f, 0.30f, 0.30f, 0.25f}},
      {"crt", {true, -0.02f, 1.05f, 1.10f, 0.10f, 0.0f, 1.0f, 0.90f, 1.0f, 0.90f, 0.10f, 0.20f, 0.50f}},
  };
  const std::string mode = rex::cvar::GetFlagByName("bo2zm_postprocess");
  for (const Preset& p : kPresets) {
    if (mode == p.label) {
      return p.values;
    }
  }
  Postprocess result;
  if (mode != "custom") {
    return result;  // off
  }
  result.active = true;
  result.brightness = Number("bo2zm_postprocess_brightness", 0.0f);
  result.contrast = Number("bo2zm_postprocess_contrast", 1.0f);
  result.saturation = Number("bo2zm_postprocess_saturation", 1.0f);
  result.vibrance = Number("bo2zm_postprocess_vibrance", 0.0f);
  result.temperature = Number("bo2zm_postprocess_temperature", 0.0f);
  result.gamma = Number("bo2zm_postprocess_gamma", 1.0f);
  result.vignette = Number("bo2zm_postprocess_vignette", 0.0f);
  result.lines = Number("bo2zm_postprocess_scanlines", 0.0f);
  return result;
}

uint64_t VersionPostprocess() {
  return g_version_postprocess.load(std::memory_order_relaxed);
}

bool AntialiasingFxaa() {
  return g_fxaa.load(std::memory_order_relaxed);
}

int GlowSky() {
  return g_glow_sky.load(std::memory_order_relaxed);
}

int TreatmentVisual() {
  return g_treatment_visual.load(std::memory_order_relaxed);
}

void WatchSettingsInLive() {
  // The notification arrives with the registry lock held (SetFlagFromSource): only atomics are touched.
  for (const char* label : kCvarsPostprocess) {
    rex::cvar::RegisterChangeCallback(label, [](std::string_view, std::string_view) {
      g_version_postprocess.fetch_add(1, std::memory_order_relaxed);
    });
  }
  g_fxaa.store(rex::cvar::GetFlagByName("bo2zm_antialiasing") == "fxaa", std::memory_order_relaxed);
  rex::cvar::RegisterChangeCallback("bo2zm_antialiasing", [](std::string_view, std::string_view value) {
    g_fxaa.store(value == "fxaa", std::memory_order_relaxed);
  });
  g_glow_sky.store(ModeGlow(rex::cvar::GetFlagByName("bo2zm_sky_glow")),
                          std::memory_order_relaxed);
  rex::cvar::RegisterChangeCallback("bo2zm_sky_glow", [](std::string_view, std::string_view value) {
    g_glow_sky.store(ModeGlow(value), std::memory_order_relaxed);
  });
  g_treatment_visual.store(ModeTreatment(rex::cvar::GetFlagByName("bo2zm_visual_treatment")),
                             std::memory_order_relaxed);
  rex::cvar::RegisterChangeCallback("bo2zm_visual_treatment", [](std::string_view, std::string_view value) {
    g_treatment_visual.store(ModeTreatment(value), std::memory_order_relaxed);
  });
}

}  // namespace bo2zm::settings
