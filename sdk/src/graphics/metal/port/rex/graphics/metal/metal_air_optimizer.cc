// See metal_air_optimizer.h and docs/native-shaders.md.

#include "rex/graphics/metal/metal_air_optimizer.h"

#include "rex/logging.h"

namespace rex {
namespace graphics {
namespace metal {

bool ConverterBuiltIn() {
#if FABLE2_NO_CONVERTER
  return false;
#else
  return true;
#endif
}

bool NoConverterForcesNative() {
  // FABLE2_NOCONV_ALLOW_OFF=1 (diagnostic only, no launcher toggle): honour
  // the native-path switches in a no-converter build anyway, to test how the
  // renderer copes with refused draws.
  static const bool force = [] {
    if (ConverterBuiltIn()) {
      return false;
    }
    const char* v = MetalDebugEnv("NOCONV_ALLOW_OFF");
    return !(v && v[0] == '1');
  }();
  return force;
}

namespace {

// docs/no-converter.md "Forced switches": in a build without the converter
// these native paths are the only shader paths, so turning one off would
// refuse every draw it covers (black frames). Returns the value to use: the
// environment value, or nullptr (the default, i.e. on) when the value turns
// the path off in a no-converter build. Logs one line per switch.
const char* NativeSwitchValue(const char* name) {
  const char* v = MetalDebugEnv(name);
  if (!v || v[0] != '0' || !NoConverterForcesNative()) {
    return v;
  }
  REXLOG_WARN("[no-converter] {}={} ignored: no converter fallback in this "
              "build",
              name, v);
  return nullptr;
}

// The same rule for the native-path cvars: a value that turns the path off
// (0 / false) is replaced by `forced` in a no-converter build.
int32_t NativeSwitchCvar(const char* name, int32_t value, int32_t forced) {
  if (value != 0 || !NoConverterForcesNative()) {
    return value;
  }
  REXLOG_WARN("[no-converter] {}=0 ignored: no converter fallback in this "
              "build",
              name);
  return forced;
}

}  // namespace

uint32_t NativeShadersMode() {
  static const uint32_t mode = [] {
    return NativeSwitchCvar("metal_native_shaders",
                            REXCVAR_GET(metal_native_shaders) ? 1 : 0,
                            int32_t(kNativeShadersDefault))
               ? 1u
               : 0u;
  }();
  return mode;
}

bool NativeXlateMode() {
  static const bool on = [] {
    if (!NativeShadersMode()) {
      return false;
    }
    // Default on since 2026-10-13 (docs/native-translator.md); =0 restores
    // the converter path.
    return NativeSwitchCvar("metal_native_xlate",
                            REXCVAR_GET(metal_native_xlate) ? 1 : 0, 1) != 0;
  }();
  return on;
}

bool NativeFirstMode() {
  static const bool on = [] {
    if (!NativeXlateMode()) {
      if (!ConverterBuiltIn()) {
        REXLOG_ERROR("[no-converter] FABLE2_NATIVE_XLATE / FABLE2_NATIVE_SHADERS "
                     "are off (FABLE2_NOCONV_ALLOW_OFF=1), but this build has "
                     "no converter: every guest shader will fail");
      }
      return false;
    }
    const bool mode = NativeSwitchCvar("metal_native_first",
                                       REXCVAR_GET(metal_native_first) ? 1 : 0,
                                       1) != 0;
    REXLOG_INFO("[native-first] {} (metal_native_first={}; converter {})",
                mode ? "on: guest shaders are compiled from the native "
                       "translation directly"
                     : "off: converter library first, native swap later",
                mode, ConverterBuiltIn() ? "built in" : "not built");
    return mode;
  }();
  return on;
}

bool NativeXlateVertexMode() {
  static const bool on = [] {
    if (!NativeXlateMode()) {
      return false;
    }
    const char* v = NativeSwitchValue("NATIVE_XLATE_VS");
    return !(v && v[0] == '0');
  }();
  return on;
}

uint32_t NativeTessMode() {
  static const uint32_t mode = [] {
    if (!NativeXlateMode()) {
      return 0u;
    }
    // Default 1 (exact only) since 2026-10-13; =0 restores the emulation.
    const int32_t v =
        NativeSwitchCvar("metal_native_tess", REXCVAR_GET(metal_native_tess), 1);
    return v == 3 ? 3u : (v == 2 ? 2u : (v == 0 ? 0u : 1u));
  }();
  return mode;
}

uint32_t NativePointsMode() {
  static const uint32_t mode = [] {
    if (!NativeXlateVertexMode()) {
      return 0u;
    }
    // Default 2 (mesh function, VS once per point) since 2026-10-13: exact
    // and not slower. =1 per-corner expansion (slower), =0 emulation.
    const int32_t v = NativeSwitchCvar("metal_native_points",
                                       REXCVAR_GET(metal_native_points), 2);
    return v == 0 ? 0u : (v == 1 ? 1u : 2u);
  }();
  return mode;
}

bool NativePointsMemexportMode() {
  static const bool on = [] {
    if (NativePointsMode() != 2 || !NativeFirstMode()) {
      return false;
    }
    const char* v = NativeSwitchValue("NATIVE_POINTS_MEMEXPORT");
    return !(v && v[0] == '0');
  }();
  return on;
}

bool NativeRectsMode() {
  static const bool on = [] {
    if (!NativeXlateVertexMode()) {
      return false;
    }
    // Default on since 2026-10-13 (bit-identical, neutral); =0 emulates.
    const char* v = NativeSwitchValue("NATIVE_RECTS");
    return !(v && v[0] == '0');
  }();
  return on;
}

bool NativeTessBlendedMode() {
  static const bool on = [] {
    if (!NativeTessMode()) {
      return false;
    }
    const char* v = NativeSwitchValue("NATIVE_TESS_BLENDED");
    return !(v && v[0] == '0');
  }();
  return on;
}

bool NativeTessSnapUvMode() {
  static const bool on = [] {
    const char* v = MetalDebugEnv("NATIVE_TESS_SNAP_UV");
    const bool enabled = v && v[0] == '1';
    // Confirms in the log that the switch reached the game (flash kit, docs/flash-kit.md); the translator
    // logs each domain shader compiled with the snap ([tess-trace] snap-uv ... compiled).
    REXLOG_INFO("[tess-trace] snap-uv: {} (FABLE2_NATIVE_TESS_SNAP_UV={})", enabled ? "on" : "off",
                v ? v : "unset");
    return enabled;
  }();
  return on;
}

bool EffectsFootprintMode() {
  static const bool on = [] {
    const char* v = MetalDebugEnv("EFFECTS_FOOTPRINT");
    const bool enabled = !(v && v[0] == '0');
    REXLOG_INFO(
        "[effects-footprint] offset texture fetches cover their guest "
        "texel at scaled resolution: {} (FABLE2_EFFECTS_FOOTPRINT={})",
        enabled ? "on" : "off", v ? v : "unset");
    return enabled;
  }();
  return on;
}

bool EffectsFootprintVariantMode() {
  static const bool on = [] {
    const char* v = MetalDebugEnv("EFFECTS_FOOTPRINT_VARIANT");
    const bool enabled = !(v && v[0] == '0');
    REXLOG_INFO(
        "[effects-footprint] footprint code only in pixel-shader variants of "
        "draws it can apply to: {} (FABLE2_EFFECTS_FOOTPRINT_VARIANT={})",
        enabled ? "on" : "off", v ? v : "unset");
    return enabled;
  }();
  return on;
}

const char* NativeXlateDumpDir() {
  static const char* dir = [] {
    const char* v = MetalDebugEnv("NATIVE_XLATE_DUMP");
    return (v && v[0]) ? v : nullptr;
  }();
  return dir;
}

}  // namespace metal
}  // namespace graphics
}  // namespace rex
