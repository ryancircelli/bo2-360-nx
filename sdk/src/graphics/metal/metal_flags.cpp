#include "compat.h"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include <rex/logging.h>
DEFINE_bool(metal_backend_telemetry, false,
            "Log concise Metal backend decision counters for render encoder "
            "lifetime, resolve/transfer planning, bindless binding, and "
            "texture upload/load behavior.",
            "Metal");
DEFINE_int32(metal_backend_telemetry_interval, 120,
             "Number of guest swaps between Metal backend telemetry summaries. "
             "Set to 0 to log only on shutdown.",
             "Metal");
DEFINE_bool(metal_constant_payload_cache, false,
            "Reuse identical Metal constant/descriptor payload uploads across "
            "draws within a frame. Disable to A/B hash/cache CPU cost against "
            "extra CBV uploads and root argument churn.",
            "Metal");
DEFINE_bool(metal_pipeline_binary_archive, true,
            "Use MTLBinaryArchive for Metal pipeline compilation caching. "
            "Requires store_shaders and a compatible OS/driver.",
            "Metal");
DEFINE_string(
    metal_residency_sets, "auto",
    "Use Metal residency sets for stable Metal allocations where supported.\n"
    "  auto: Enable when the runtime exposes MTLResidencySet.\n"
    "  true: Require creating a residency set, falling back only if creation "
    "fails.\n"
    "  false: Use per-encoder useResource/useHeap only.",
    "Metal");
DEFINE_bool(
    metal_root_rebuild_detail_telemetry, false,
    "Collect expensive Metal root argument rebuild diagnostics, including slot "
    "change histograms and CBV resource identity details. Leave disabled for "
    "normal profiling.",
    "Metal");
DEFINE_bool(metal_shader_disk_cache, true,
            "Cache translated Metal shader artifacts and binding metadata in "
            "the packed Metal artifact store.",
            "Metal");
DEFINE_bool(occlusion_query_log, false,
            "Log occlusion query lifetime and summary stats.", "GPU");
DEFINE_bool(submit_on_primary_buffer_end, true,
            "Submit the command buffer when a PM4 primary buffer ends if it's "
            "possible to submit immediately to try to reduce frame latency.",
            "GPU");

DEFINE_bool(ac6_ground_fix, false, "XeniOS AC6-specific texture workaround (leave disabled for Fable II)", "HACKS");
DEFINE_bool(half_pixel_offset, true, "Adjust pixel centers between guest and host graphics APIs", "GPU");
DEFINE_int32(anisotropic_override, -1, "Anisotropic filtering override (-1 follows the game)", "GPU");
DEFINE_bool(gpu_3d_to_2d_texture, true, "Handle 3D textures sampled as 2D using the first guest slice", "GPU");

// ---------------------------------------------------------------------------
// Settings that were FABLE2_* environment variables (README.md, "Settings").
// Defaults match the old unset behaviour.
// ---------------------------------------------------------------------------
DEFINE_bool(metal_native_shaders, true,
            "Optimize guest pixel shaders with metal-opt -O2 (native shaders). "
            "Off also turns off the native translator and everything under it.",
            "Metal");
DEFINE_bool(metal_native_xlate, true,
            "Translate guest shaders to MSL directly (native translator) "
            "instead of going through the shader converter.",
            "Metal");
DEFINE_bool(metal_native_first, true,
            "Compile guest shaders from the native translation directly; off "
            "compiles the converter output first and swaps in native later.",
            "Metal");
DEFINE_int32(metal_native_tess, 1,
             "Native tessellation: 0 = emulation, 1 = exact native, "
             "2 = Metal tessellator for all patches, 3 = reference pattern.",
             "Metal");
DEFINE_int32(metal_native_points, 2,
             "Native point lists: 0 = emulation, 1 = per-corner expansion, "
             "2 = mesh function (vertex shader once per point).",
             "Metal");
DEFINE_bool(metal_ns_specialize, true,
            "Specialize hot optimized pixel shaders by baking their constants.",
            "Metal");
DEFINE_string(metal_ns_cache_toolchain, "",
              "Toolchain hash (hex) of a bundled native-shader cache. When "
              "metal-opt/metallib are missing, optimized shaders then come "
              "from that cache only (portable builds).",
              "Metal");
DEFINE_string(metal_msc_min_target, "",
              "Shader converter minimum target: empty or 'portable' = "
              "apple7:15.0 (cache survives OS updates and works on any Apple "
              "Silicon Mac), 'native' = this Mac, or e.g. 'apple8:14.0'.",
              "Metal");
DEFINE_int32(metal_warmup_threads, -1,
             "Total pipeline warm-up threads at startup; -1 = automatic "
             "(performance cores, 2 to 6).",
             "Metal");
DEFINE_string(metal_shader_prep, "",
              "Headless shader preparation: '1' or a path ending in .json "
              "(status file) warms every shader and pipeline cache, saves them "
              "and exits without running the game. Empty or '0' = off.",
              "Metal");
DEFINE_double(metal_shader_prep_timeout_s, 1800.0,
              "Guard timeout of metal_shader_prep, seconds.", "Metal");
DEFINE_string(metal_res_scale, "",
              "Draw resolution scale: empty = native 1x, 1 to 3 = both axes, "
              "'cvar' = follow draw_resolution_scale_x/y.",
              "Metal");
DEFINE_bool(metal_resolve_to_texture, true,
            "Resolve render targets straight into textures instead of through "
            "guest memory where possible.",
            "Metal");
DEFINE_bool(metal_tile_resolve, true,
            "Run colour resolves as tile functions inside the draw pass.",
            "Metal");
DEFINE_bool(metal_ssm_merge, false,
            "Merge stencil-only alias clear transfers into the next draw pass. "
            "Exact; can help bandwidth-bound Macs, slightly slower on others.",
            "Metal");
DEFINE_bool(metal_occlusion_real, true,
            "Answer guest occlusion queries with real Metal visibility results; "
            "off reports a fixed 1000 samples passed.",
            "Metal");
DEFINE_bool(metal_vsync_phase_lock, true,
            "Phase-lock presentation to the display refresh.", "Metal");
DEFINE_int32(metal_present_drawables, 2,
             "CAMetalLayer maximumDrawableCount: 2 (lower latency) or 3 "
             "(0 also means 3).",
             "Metal");
DEFINE_bool(metal_retina, true,
            "Render at the display's backing scale (Retina); off uses scale 1.",
            "Metal");
DEFINE_string(metal_title_quirks, "",
              "Per-title workarounds to enable, comma separated. 'fable2' = "
              "Fable II: texture-space and shadow passes at native scale, "
              "built-in shader lists, morph readback, terrain tessellation, "
              "transfer prewarm keys. Empty = none.",
              "Metal");

const char* MetalDebugEnv(const char* name) {
  std::string key = "REX_METAL_";
  key += name;
  if (const char* v = std::getenv(key.c_str())) {  // getenv-ok: callers read once
    return v;
  }
  key = "FABLE2_";
  key += name;
  return std::getenv(key.c_str());  // getenv-ok: callers read once
}

bool MetalTitleQuirk(const char* quirk) {
  static const std::vector<std::string> quirks = [] {
    std::vector<std::string> list;
    std::string token;
    for (char c : REXCVAR_GET(metal_title_quirks) + ",") {
      if (c == ',' || c == ' ' || c == ';') {
        if (!token.empty()) list.push_back(std::move(token));
        token.clear();
      } else {
        token += char(std::tolower(static_cast<unsigned char>(c)));
      }
    }
    if (!list.empty()) {
      std::string joined;
      for (const auto& q : list) joined += (joined.empty() ? "" : ",") + q;
      REXLOG_INFO("[title-quirks] metal_title_quirks = {}", joined);
    }
    return list;
  }();
  return std::find(quirks.begin(), quirks.end(), quirk) != quirks.end();
}

void ApplyLegacyMetalEnv() {
  // How the old variable was parsed, so old launch scripts keep their meaning.
  enum class Kind {
    kRaw,        // passed through unchanged
    kZeroOff,    // on unless the value starts with '0'
    kOneOn,      // on only when the value starts with '1'
    kOffWords,   // off for 0, off or false
    kExactZero,  // off only for exactly "0"
  };
  struct Entry {
    const char* cvar;
    const char* legacy;
    Kind kind;
  };
  static constexpr Entry kEntries[] = {
      {"metal_native_shaders", "FABLE2_NATIVE_SHADERS", Kind::kZeroOff},
      {"metal_native_xlate", "FABLE2_NATIVE_XLATE", Kind::kZeroOff},
      {"metal_native_first", "FABLE2_NATIVE_FIRST", Kind::kZeroOff},
      {"metal_native_tess", "FABLE2_NATIVE_TESS", Kind::kRaw},
      {"metal_native_points", "FABLE2_NATIVE_POINTS", Kind::kRaw},
      {"metal_ns_specialize", "FABLE2_NS_SPECIALIZE", Kind::kZeroOff},
      {"metal_ns_cache_toolchain", "FABLE2_NS_CACHE_TOOLCHAIN", Kind::kRaw},
      {"metal_msc_min_target", "FABLE2_MSC_MIN_TARGET", Kind::kRaw},
      {"metal_warmup_threads", "FABLE2_WARMUP_THREADS", Kind::kRaw},
      {"metal_shader_prep", "FABLE2_SHADER_PREP", Kind::kRaw},
      {"metal_shader_prep_timeout_s", "FABLE2_SHADER_PREP_TIMEOUT_S", Kind::kRaw},
      {"metal_res_scale", "FABLE2_RES_SCALE", Kind::kRaw},
      {"metal_resolve_to_texture", "FABLE2_RESOLVE_TO_TEXTURE", Kind::kZeroOff},
      {"metal_tile_resolve", "FABLE2_TILE_RESOLVE", Kind::kZeroOff},
      {"metal_ssm_merge", "FABLE2_SSM_MERGE", Kind::kOneOn},
      {"metal_occlusion_real", "FABLE2_OCCLUSION_REAL", Kind::kZeroOff},
      {"metal_vsync_phase_lock", "FABLE2_VSYNC_PHASE_LOCK", Kind::kOffWords},
      {"metal_present_drawables", "FABLE2_PRESENT_DRAWABLES", Kind::kRaw},
      {"metal_retina", "FABLE2_RETINA", Kind::kExactZero},
  };
  for (const Entry& e : kEntries) {
    const char* v = std::getenv(e.legacy);  // getenv-ok: once, at plugin creation
    if (!v || !*v) continue;
    if (rex::cvar::GetFlagSource(e.cvar) != rex::cvar::Source::kDefault) continue;
    std::string value = v;
    switch (e.kind) {
      case Kind::kRaw:
        break;
      case Kind::kZeroOff:
        value = v[0] == '0' ? "false" : "true";
        break;
      case Kind::kOneOn:
        value = v[0] == '1' ? "true" : "false";
        break;
      case Kind::kOffWords:
        value = (value == "0" || value == "off" || value == "false") ? "false" : "true";
        break;
      case Kind::kExactZero:
        value = value == "0" ? "false" : "true";
        break;
    }
    if (rex::cvar::SetFlagByName(e.cvar, value)) {
      REXLOG_INFO("[settings] {}={} -> {} = {} (legacy name; prefer the cvar)", e.legacy, v,
                  e.cvar, value);
    } else {
      REXLOG_WARN("[settings] {}={} ignored: not a valid {}", e.legacy, v, e.cvar);
    }
  }
}
