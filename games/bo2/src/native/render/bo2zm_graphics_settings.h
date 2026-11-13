// Derived from GPL-3.0 code; modified for bo2-zm-nx in 2026.
//
// Graphics settings for the native renderer, and their place in the settings menu (F4).
//
// The SDK's menu lists every option the Xenos emulation reads. With the native renderer most of them do nothing:
// resolution_scale, draw_resolution_scale_x/y, vsync, anisotropic_override and swap_post_effect are only read by the
// emulated path, so HideSettingsNoEffect removes them from the menu. What remains are settings this renderer
// implements itself (antialiasing on the output, the sky glow, the game's color filter and an optional
// post-processing chain, all applied live) and the Switch clock and overlay settings read at start-up.
//
// The internal resolution and FPS limit settings are for a game that draws at the size of the video mode and is
// paced by the vblank rate; they are offered as lists because free text and decimals are awkward to edit with a
// gamepad. BO2 ignores the video mode and always draws its 3D scene at 880x720, so ApplySettingsGraphics, which would
// pass them on, is currently not called.

#pragma once

#include <cstdint>

namespace bo2zm::settings {

// Optional post-processing in the Graphics/Postprocess category, modeled on a public-domain filter set: color
// grading (temperature, brightness, contrast, tint, saturation, vibrance and gamma), vignette and scanlines. Off by
// default, in which case the output stays bit-identical. Applied by the native renderer's output pass.
struct Postprocess {
  bool active = false;
  float brightness = 0.0f;
  float contrast = 1.0f;
  float saturation = 1.0f;
  float vibrance = 0.0f;
  float temperature = 0.0f;
  float gamma = 1.0f;
  float tint_r = 1.0f;
  float tint_g = 1.0f;
  float tint_b = 1.0f;
  float tint = 0.0f;
  float vignette = 0.0f;
  float lines = 0.0f;
};

// Values in effect (the chosen preset or the custom ones). Reads the cvars with the registry lock: call
// only when VersionPostprocess changes.
Postprocess ReadPostprocess();

// Increments every time the menu changes a post-processing cvar (lock-free: it can be checked every frame).
uint64_t VersionPostprocess();

// bo2zm_antialiasing = fxaa (lock-free: it can be checked every frame).
bool AntialiasingFxaa();

// bo2zm_sky_glow: 0 original, 1 natural, 2 soft (lock-free: it can be checked on every draw).
int GlowSky();

// bo2zm_visual_treatment: 0 original, 1 soft, 2 off (lock-free: it can be checked on every draw).
int TreatmentVisual();

// Registers the change notifications for the live Graphics settings (post-processing, antialiasing, sky glow
// and the color filter), once at startup.
void WatchSettingsInLive();

// Passes bo2zm_internal_resolution and bo2zm_fps_limit to the video mode (video_mode_width/height,
// resolution and video_mode_refresh_rate), except the ones that come from the command line. Must run
// before the game requests the video mode. Not called for BO2 (see above).
void ApplySettingsGraphics();
// The start-up part that does apply to BO2: the handheld GPU clock request (none by default) and the overlay's FPS
// block.
void RequestBootClocks();

// With the native renderer, removes from the menu the FPS, resolution and graphics settings that do
// nothing, and the video mode settings that the ones above replace.
void HideSettingsNoEffect();

}  // namespace bo2zm::settings
