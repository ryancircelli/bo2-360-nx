// Diagnostics of the draw path, kept out of metal_command_processor.cc so the
// hot IssueDrawImpl stays readable: env-gated traces and dumps (FABLE2_*),
// each a no-op unless its switch is set, and the periodic backend telemetry.
#include "rex/graphics/metal/metal_command_processor.h"

#include <rex/graphics/swap_hook.h>
#include "rex/graphics/metal/metal_frame_log.h"
#include "rex/graphics/metal/metal_readiness.h"
#include "rex/graphics/metal/metal_trace.h"
#include "rex/graphics/metal/metal_hitch.h"
#include "rex/graphics/metal/metal_converter_census.h"
#include "rex/graphics/metal/metal_air_optimizer.h"
#include "rex/graphics/metal/metal_ns_specialize.h"
#include "rex/graphics/metal/metal_hle_census.h"
#include <rex/graphics/metal_runtime.h>
#include <rex/thread.h>
#include "rex/graphics/metal/metal_emu_draw_stats.h"
#include "rex/graphics/metal/metal_ref_tessellator.h"
#include "rex/graphics/shaders/bytecode/metal/resolve_downscale_cs.h"
#include <rex/kernel/xboxkrnl/video.h>
#include <fstream>
#include "rex/graphics/metal/metal_backend_telemetry.h"

namespace rex {
namespace graphics {
namespace metal {

namespace {
const char* MetalTelemetryCbvSlotName(size_t slot) {
  switch (slot) {
    case 0:
      return "system";
    case 1:
      return "float";
    case 2:
      return "bool_loop";
    case 3:
      return "fetch";
    case 4:
      return "descriptor_indices";
    default:
      return "invalid";
  }
}

const char* MetalTelemetryShaderStageName(size_t stage) {
  switch (stage) {
    case 0:
      return "vertex";
    case 1:
      return "pixel";
    default:
      return "invalid";
  }
}

const char* MetalTelemetryRootScopeName(size_t stage) {
  switch (stage) {
    case 0:
      return "graphics";
    case 1:
      return "unused";
    default:
      return "invalid";
  }
}

const char* MetalTelemetryRenderResourceSetName(size_t set) {
  switch (set) {
    case 0:
      return "fixed";
    case 1:
      return "texture";
    case 2:
      return "root";
    default:
      return "invalid";
  }
}

const char* MetalTelemetryRootRebuildReasonName(size_t reason) {
  switch (reason) {
    case 0:
      return "frame_open";
    case 1:
      return "descriptor_indices_pointer_tuple";
    case 2:
      return "other_cbv_pointer_tuple";
    case 3:
      return "shared_memory_uav_mode";
    default:
      return "invalid";
  }
}

const char* MetalTelemetryRootSlotsChangedName(size_t bin) {
  switch (bin) {
    case 0:
      return "0";
    case 1:
      return "1";
    case 2:
      return "2";
    case 3:
      return "3";
    case 4:
      return "4";
    case 5:
      return "5_plus";
    default:
      return "invalid";
  }
}

const char* MetalTelemetryRootRebuildDetailName(size_t detail) {
  switch (detail) {
    case 0:
      return "same_buffer_offset_changed";
    case 1:
      return "different_buffer";
    case 2:
      return "descriptor_indices_only";
    case 3:
      return "other_cbv_only";
    case 4:
      return "mixed_descriptor_and_other";
    case 5:
      return "resource_identity_changed";
    case 6:
      return "resource_identity_same";
    default:
      return "invalid";
  }
}

const char* MetalTelemetryDrawMaterializationSourceName(size_t source) {
  switch (source) {
    case 0:
      return "vertex_fetch";
    case 1:
      return "guest_index";
    case 2:
      return "memexport";
    case 3:
      return "texture_source";
    default:
      return "invalid";
  }
}

const char* MetalPreparedDrawFlushReasonName(size_t reason) {
  switch (reason) {
    case 0:
      return "manual";
    case 1:
      return "rt_update";
    case 2:
      return "rt_key_mismatch";
    case 3:
      return "queue_budget";
    case 4:
      return "queue_reject";
    case 5:
      return "prepare_wait";
    case 6:
      return "swap";
    case 7:
      return "copy";
    case 8:
      return "transfer_request";
    case 9:
      return "render_encoder_end";
    case 10:
      return "command_buffer_end";
    case 11:
      return "query";
    default:
      return "invalid";
  }
}

const char* MetalPreparedDrawQueueRejectReasonName(size_t reason) {
  switch (reason) {
    case 0:
      return "none";
    case 1:
      return "resident_no_active_queue";
    case 2:
      return "no_shared_memory_ranges";
    case 3:
      return "memexport";
    case 4:
      return "texture_upload";
    case 5:
      return "texture_request_load_data";
    case 6:
      return "pending_draw_pass_transfers";
    case 7:
      return "zpd_active";
    case 8:
      return "rt_key_mismatch";
    case 9:
      return "queue_budget";
    default:
      return "invalid";
  }
}

const char* MetalTelemetryRootArgSlotName(size_t slot) {
  switch (slot) {
    case 0:
      return "srv0";
    case 1:
      return "srv1";
    case 2:
      return "srv2";
    case 3:
      return "srv3";
    case 4:
      return "srv10";
    case 5:
      return "uav0";
    case 6:
      return "uav1";
    case 7:
      return "uav2";
    case 8:
      return "uav3";
    case 9:
      return "sampler0";
    case 10:
      return "cbv_system";
    case 11:
      return "cbv_float";
    case 12:
      return "cbv_bool_loop";
    case 13:
      return "cbv_fetch";
    case 14:
      return "cbv_descriptor_indices";
    case 15:
      return "cbv_hull_float";
    case 16:
      return "cbv_hull_fetch";
    case 17:
      return "cbv_hull_descriptor_indices";
    case 18:
      return "cbv_domain_float";
    case 19:
      return "cbv_domain_fetch";
    case 20:
      return "cbv_domain_descriptor_indices";
    case 21:
      return "cbv_pixel_float";
    case 22:
      return "cbv_pixel_fetch";
    case 23:
      return "cbv_pixel_descriptor_indices";
    default:
      return "unused";
  }
}

const char* MetalTelemetryRenderEncoderBufferStageName(size_t stage) {
  switch (stage) {
    case 0:
      return "vertex";
    case 1:
      return "fragment";
    case 2:
      return "object";
    case 3:
      return "mesh";
    default:
      return "invalid";
  }
}
}  // namespace

void MetalCommandProcessor::DiagLogMemexportFormats(const RegisterFile& regs, Shader* vertex_shader, bool memexport_used_vertex) {
  // Diagnostic (FABLE2_LOG_MEMEXPORT_FORMATS=1, docs/native-translator.md):
  // log each distinct memexport stream constant (format, signedness) once.
  if (memexport_used_vertex) {
    static const bool log_streams = [] {
      const char* v = MetalDebugEnv("LOG_MEMEXPORT_FORMATS");
      return v && v[0] == '1';
    }();
    if (log_streams) {
      static std::mutex log_mutex;
      static std::unordered_set<uint64_t> seen;
      for (uint32_t c : vertex_shader->memexport_stream_constants()) {
        const uint32_t z = regs.values[XE_GPU_REG_SHADER_CONSTANT_000_X + c * 4 + 2];
        const uint64_t key = (vertex_shader->ucode_data_hash() << 8) ^ (uint64_t(c) << 40) ^ z;
        std::lock_guard<std::mutex> lock(log_mutex);
        if (seen.insert(key).second) {
          REXLOG_INFO(
              "[memexport] VS {:016X} stream c{}: z {:08X} format {} signed {} "
              "integer {} endian {} red/blue swap {}",
              vertex_shader->ucode_data_hash(), c, z, (z >> 8) & 0x3F,
              (z >> 16) & 1, (z >> 17) & 1, z & 7, (z >> 19) & 1);
        }
      }
    }
  }
}

void MetalCommandProcessor::DiagMainDepthDump(const RegisterFile& regs, Shader* vertex_shader) {
  // [main-depth] diagnostic EDRAM dumps at three points of the screen-space
  // shadow-mask sequence in one frame (FABLE2_MAIN_DEPTH_DUMP=<frame>,
  // docs/main-depth-20261014.md): the z = 0.75 alias clear, the shadow-mask
  // draw and the first main colour pass draw, each before its own handling.
  if (render_target_cache_ && MetalRenderTargetCache::MainDepthDumpFrame() &&
      frame_log::CurrentFrame() == MetalRenderTargetCache::MainDepthDumpFrame()) {
    static uint32_t dumped = 0;
    const uint64_t vs_hash = vertex_shader->ucode_data_hash();
    const char* tag = nullptr;
    uint32_t bit = 0;
    if (vs_hash == UINT64_C(0x1E6883FCCDE1F688) &&
        regs[XE_GPU_REG_RB_SURFACE_INFO] == 0x08C20230u &&
        regs[XE_GPU_REG_RB_DEPTH_INFO] == 0) {
      tag = "a-preclear";
      bit = 1;
    } else if (vs_hash == UINT64_C(0x7A6E981FC4D0A375)) {
      tag = "b-ssmask";
      bit = 2;
    } else if (vs_hash == UINT64_C(0xBEAD84BD72072E0E)) {
      tag = "c-mainpass";
      bit = 4;
    }
    // FABLE2_MAIN_DEPTH_DUMP_SKIP=<bits>: checkpoints left out (2 = b: the b
    // dump flushes work [ssm-merge] would draw in the shadow-mask pass).
    static const uint32_t skip = [] {
      const char* v = MetalDebugEnv("MAIN_DEPTH_DUMP_SKIP");  // getenv-ok: once
      return v ? uint32_t(std::strtoul(v, nullptr, 10)) : 0u;
    }();
    if (tag && !(dumped & bit) && !(skip & bit)) {
      dumped |= bit;
      if (FlushPreparedDrawQueue(PreparedDrawFlushReason::kRenderTargetUpdate)) {
        render_target_cache_->MainDepthDumpEdram(tag);
      }
    }
  }
}

void MetalCommandProcessor::DiagAliasClearTrace(const RegisterFile& regs, xenos::PrimitiveType primitive_type, uint32_t index_count, Shader* vertex_shader, Shader* pixel_shader, bool is_rasterization_done, bool memexport_used_vertex) {
  // [alias-clear] diagnostic (FABLE2_ALIAS_CLEAR_TRACE=1 inside the
  // FABLE2_SHADOW_TRACE window): full state of clear-like draws.
  {
    static const bool ac_trace = [] {
      const char* v = MetalDebugEnv("ALIAS_CLEAR_TRACE");
      return v && v[0] == '1';
    }();
    if (ac_trace && MetalRenderTargetCache::ShadowTraceActive()) {
      const auto prim = primitive_type;
      const bool cand =
          vertex_shader->ucode_data_hash() == UINT64_C(0x1E6883FCCDE1F688) ||
          prim == xenos::PrimitiveType::kRectangleList ||
          (prim == xenos::PrimitiveType::kPointList && !pixel_shader);
      if (cand) {
        std::string vtx;
        for (const auto& binding : vertex_shader->vertex_bindings()) {
          xenos::xe_gpu_vertex_fetch_t vf =
              regs.GetVertexFetch(binding.fetch_constant);
          uint32_t addr = vf.address << 2;
          uint32_t len = std::min<uint32_t>(vf.size << 2, 64);
          vtx += fmt::format(" vb{}@{:08X}/s{}:", binding.fetch_constant,
                             addr, binding.stride_words * 4);
          const auto* g = static_cast<const uint8_t*>(
              memory_->TranslatePhysical(addr));
          for (uint32_t o = 0; g && o + 4 <= len; o += 4) {
            uint32_t w = (uint32_t(g[o]) << 24) | (uint32_t(g[o + 1]) << 16) |
                         (uint32_t(g[o + 2]) << 8) | g[o + 3];
            float f;
            std::memcpy(&f, &w, 4);
            vtx += fmt::format(" {:g}", f);
          }
        }
        std::string vc, pc;
        for (uint32_t i = 0; i < 4 * 8; ++i) {
          vc += fmt::format("{}{:g}", i ? "," : "",
                            regs.Get<float>(XE_GPU_REG_SHADER_CONSTANT_000_X + i));
        }
        for (uint32_t i = 0; i < 4 * 4; ++i) {
          pc += fmt::format("{}{:g}", i ? "," : "",
                            regs.Get<float>(XE_GPU_REG_SHADER_CONSTANT_256_X + i));
        }
        REXLOG_INFO(
            "[alias-clear] f{} cand prim={} n={} vs={:016X} ps={:016X} "
            "surf={:08X} ci0={:08X} ci1={:08X} di={:08X} cm={:08X} "
            "zc={:08X} sref={:08X} blend0={:08X} cc={:08X} hiz={:08X} "
            "mode={:08X} vte={:08X} clip={:08X} su={:08X} woff={:08X} "
            "ws={:08X}-{:08X} ss={:08X}-{:08X} vp={:g},{:g},{:g},{:g},{:g},{:g} "
            "aa={:08X} dclr={:08X} cclr={:08X} ps_writes={:X} rast={} "
            "mex={}{} vc=[{}] pc=[{}]",
            frame_log::CurrentFrame(), uint32_t(prim), index_count,
            vertex_shader->ucode_data_hash(),
            pixel_shader ? pixel_shader->ucode_data_hash() : 0,
            regs[XE_GPU_REG_RB_SURFACE_INFO], regs[XE_GPU_REG_RB_COLOR_INFO],
            regs[XE_GPU_REG_RB_COLOR1_INFO], regs[XE_GPU_REG_RB_DEPTH_INFO],
            regs[XE_GPU_REG_RB_COLOR_MASK], regs[XE_GPU_REG_RB_DEPTHCONTROL],
            regs[XE_GPU_REG_RB_STENCILREFMASK],
            regs[XE_GPU_REG_RB_BLENDCONTROL0], regs[XE_GPU_REG_RB_COLORCONTROL],
            regs[XE_GPU_REG_RB_HIZCONTROL], regs[XE_GPU_REG_RB_MODECONTROL],
            regs[XE_GPU_REG_PA_CL_VTE_CNTL], regs[XE_GPU_REG_PA_CL_CLIP_CNTL],
            regs[XE_GPU_REG_PA_SU_SC_MODE_CNTL],
            regs[XE_GPU_REG_PA_SC_WINDOW_OFFSET],
            regs[XE_GPU_REG_PA_SC_WINDOW_SCISSOR_TL],
            regs[XE_GPU_REG_PA_SC_WINDOW_SCISSOR_BR],
            regs[XE_GPU_REG_PA_SC_SCREEN_SCISSOR_TL],
            regs[XE_GPU_REG_PA_SC_SCREEN_SCISSOR_BR],
            regs.Get<float>(XE_GPU_REG_PA_CL_VPORT_XSCALE),
            regs.Get<float>(XE_GPU_REG_PA_CL_VPORT_XOFFSET),
            regs.Get<float>(XE_GPU_REG_PA_CL_VPORT_YSCALE),
            regs.Get<float>(XE_GPU_REG_PA_CL_VPORT_YOFFSET),
            regs.Get<float>(XE_GPU_REG_PA_CL_VPORT_ZSCALE),
            regs.Get<float>(XE_GPU_REG_PA_CL_VPORT_ZOFFSET),
            regs[XE_GPU_REG_PA_SC_AA_MASK], regs[XE_GPU_REG_RB_DEPTH_CLEAR],
            regs[XE_GPU_REG_RB_COLOR_CLEAR],
            pixel_shader ? pixel_shader->writes_color_targets() : 0,
            is_rasterization_done ? 1 : 0, memexport_used_vertex ? 1 : 0,
            vtx, vc, pc);
      }
    }
  }
}

void MetalCommandProcessor::DiagTessDraw(const RegisterFile& regs, uint64_t vs_hash, Shader* pixel_shader, xenos::TessellationMode mode, bool tess_blended, bool tess_exact, bool have_factors, bool memexport_used_vertex, bool native_tess, bool native_tess_cpu, uint32_t blend0, reg::RB_DEPTHCONTROL normalized_depth_control, float factor_min, float factor_max, const PrimitiveProcessor::ProcessingResult& primitive_processing_result) {
  // FABLE2_NATIVE_TESS_TRACE=1 (diagnostic, docs/native-translator.md):
  // which path every tessellated draw took and why, once per combination,
  // plus counts every 600 frames.
  if (NativeTessTraceEnabled()) {
    const char* path;
    if (native_tess) {
      path = native_tess_cpu ? "reference" : "tessellator";
    } else if (!NativeTessMode()) {
      path = "emulation:off";
    } else if (!have_factors) {
      path = "emulation:no-factors";
    } else if (memexport_used_vertex) {
      path = "emulation:memexport";
    } else if (!tess_exact) {
      path = tess_blended ? "emulation:blended" : "emulation:inexact";
    } else {
      path = "emulation:pipeline-or-upload-failed";
    }
    float fmin_seen = 0.0f, fmax_seen = 0.0f;
    bool bad_factor = false;
    if (have_factors && !tess_cp_scratch_.empty()) {
      fmin_seen = fmax_seen = tess_cp_scratch_[0];
      for (float f : tess_cp_scratch_) {
        fmin_seen = std::fmin(fmin_seen, f);
        fmax_seen = std::fmax(fmax_seen, f);
        bad_factor = bad_factor || !(f >= 1.0f && f <= 64.0f);
      }
    }
    NativeTessTrace(vs_hash,
                    pixel_shader ? pixel_shader->ucode_data_hash() : 0,
                    path, uint32_t(mode), tess_blended,
                    uint32_t(tess_cp_scratch_.size() / 4), fmin_seen,
                    fmax_seen, bad_factor,
                    uint32_t(normalized_depth_control.zfunc),
                    uint32_t(normalized_depth_control.z_write_enable));
  }
  // FABLE2_TESS_FRAME_DUMP=<file> (diagnostic, default off,
  // docs/adaptive-terrain-flashes-20261014.md): every tessellated draw of
  // selected frames as one JSON line: shaders, path, depth state, the
  // patch indices (continuous) or per-patch factors (adaptive) and the
  // terrain constants, so tools/tess_frame_match.py can pair the patches
  // of the opaque pass with the blended layers over the same ground.
  // FABLE2_TESS_FRAME_DUMP_FRAMES=<first>:<every>:<count> (600:60:20).
  static FILE* tess_dump_file = [] () -> FILE* {
    const char* v = MetalDebugEnv("TESS_FRAME_DUMP");
    return (v && *v) ? std::fopen(v, "w") : nullptr;
  }();
  if (tess_dump_file) {
    static const std::array<uint64_t, 3> sel = [] {
      std::array<uint64_t, 3> s = {600, 60, 20};
      if (const char* v = MetalDebugEnv("TESS_FRAME_DUMP_FRAMES")) {
        unsigned long long a = 0, b = 0, c = 0;
        if (std::sscanf(v, "%llu:%llu:%llu", &a, &b, &c) == 3 && b) {
          s = {a, b, c};
        }
      }
      return s;
    }();
    const uint64_t f = frame_current_;
    if (f >= sel[0] && (f - sel[0]) % sel[1] == 0 &&
        (f - sel[0]) / sel[1] < sel[2]) {
      static uint64_t draw_in_frame_frame = 0;
      static uint32_t draw_in_frame = 0;
      if (draw_in_frame_frame != f) {
        draw_in_frame_frame = f;
        draw_in_frame = 0;
      }
      std::string line = fmt::format(
          "{{\"frame\":{},\"draw\":{},\"vs\":\"{:016X}\",\"ps\":\"{:016X}\","
          "\"mode\":{},\"blended\":{},\"blend0\":\"{:08X}\","
          "\"path\":\"{}\",\"zfunc\":{},\"zwrite\":{},\"zenable\":{},"
          "\"depth_info\":\"{:08X}\",\"surface_info\":\"{:08X}\","
          "\"color0_info\":\"{:08X}\",\"indx_offset\":{},\"min_index\":{},"
          "\"max_index\":{},\"fmin\":{},\"fmax\":{},\"count\":{},"
          "\"ib_type\":{},\"ib_fmt\":{},\"ib_endian\":{},"
          "\"su_mode\":\"{:08X}\",\"poly_front\":[{},{}],"
          "\"poly_back\":[{},{}],\"vport_z\":[{},{}]",
          f, draw_in_frame++, vs_hash,
          pixel_shader ? pixel_shader->ucode_data_hash() : 0,
          uint32_t(mode), tess_blended ? 1 : 0,
          blend0,
          native_tess ? (native_tess_cpu ? "reference" : "tessellator")
                      : "emulation",
          uint32_t(normalized_depth_control.zfunc),
          uint32_t(normalized_depth_control.z_write_enable),
          uint32_t(normalized_depth_control.z_enable),
          regs[XE_GPU_REG_RB_DEPTH_INFO], regs[XE_GPU_REG_RB_SURFACE_INFO],
          regs[XE_GPU_REG_RB_COLOR_INFO], regs[XE_GPU_REG_VGT_INDX_OFFSET],
          regs[XE_GPU_REG_VGT_MIN_VTX_INDX],
          regs[XE_GPU_REG_VGT_MAX_VTX_INDX], factor_min, factor_max,
          primitive_processing_result.host_draw_vertex_count,
          uint32_t(primitive_processing_result.index_buffer_type),
          uint32_t(primitive_processing_result.host_index_format),
          uint32_t(primitive_processing_result.host_shader_index_endian),
          regs[XE_GPU_REG_PA_SU_SC_MODE_CNTL],
          regs.Get<float>(XE_GPU_REG_PA_SU_POLY_OFFSET_FRONT_SCALE),
          regs.Get<float>(XE_GPU_REG_PA_SU_POLY_OFFSET_FRONT_OFFSET),
          regs.Get<float>(XE_GPU_REG_PA_SU_POLY_OFFSET_BACK_SCALE),
          regs.Get<float>(XE_GPU_REG_PA_SU_POLY_OFFSET_BACK_OFFSET),
          regs.Get<float>(XE_GPU_REG_PA_CL_VPORT_ZSCALE),
          regs.Get<float>(XE_GPU_REG_PA_CL_VPORT_ZOFFSET));
      // Constants the terrain domain shaders read (patch grid, scales,
      // offsets, selectors) and the first 4 (the transform).
      static const uint32_t kConsts[] = {0,   1,   2,   3,   8,   11,
                                         14,  46,  47,  72,  113, 114,
                                         115, 253, 254, 255};
      line += ",\"c\":{";
      for (size_t i = 0; i < std::size(kConsts); ++i) {
        const uint32_t c = kConsts[i];
        line += fmt::format(
            "{}\"{}\":[{},{},{},{}]", i ? "," : "", c,
            regs.Get<float>(XE_GPU_REG_SHADER_CONSTANT_000_X + c * 4),
            regs.Get<float>(XE_GPU_REG_SHADER_CONSTANT_000_X + c * 4 + 1),
            regs.Get<float>(XE_GPU_REG_SHADER_CONSTANT_000_X + c * 4 + 2),
            regs.Get<float>(XE_GPU_REG_SHADER_CONSTANT_000_X + c * 4 + 3));
      }
      line += "},\"tf\":{";
      for (uint32_t t = 16; t <= 19; ++t) {
        line += fmt::format(
            "{}\"{}\":\"{:08X}\"", t > 16 ? "," : "", t,
            regs.values[XE_GPU_REG_SHADER_CONSTANT_FETCH_00_0 + t * 6 + 1]);
      }
      line += "}";
      // The factors each patch was drawn with (adaptive; continuous
      // layers after FABLE2_TERRAIN_LAYER_MATCH), then the indices.
      if (have_factors) {
        line += ",\"factors\":[";
        for (size_t i = 0; i < tess_cp_scratch_.size(); ++i) {
          line += fmt::format("{}{}", i ? "," : "", tess_cp_scratch_[i]);
        }
        line += "]";
      }
      if (mode != xenos::TessellationMode::kAdaptive &&
          primitive_processing_result.index_buffer_type ==
              PrimitiveProcessor::ProcessedIndexBufferType::kGuestDMA) {
        const uint32_t n = primitive_processing_result.host_draw_vertex_count;
        const bool i32 = primitive_processing_result.host_index_format ==
                         xenos::IndexFormat::kInt32;
        const uint8_t* ib = static_cast<const uint8_t*>(
            memory_->TranslatePhysical(
                primitive_processing_result.guest_index_base));
        const xenos::Endian endian =
            primitive_processing_result.host_shader_index_endian;
        line += ",\"indices\":[";
        for (uint32_t i = 0; ib && i < n; ++i) {
          uint32_t v;
          if (i32) {
            std::memcpy(&v, ib + i * 4, 4);
          } else {
            uint16_t h;
            std::memcpy(&h, ib + i * 2, 2);
            v = h;
          }
          if (endian == xenos::Endian::k8in16 ||
              endian == xenos::Endian::k8in32) {
            v = ((v & 0x00FF00FFu) << 8) | ((v >> 8) & 0x00FF00FFu);
          }
          if (endian == xenos::Endian::k8in32 ||
              endian == xenos::Endian::k16in32) {
            v = (v >> 16) | (v << 16);
          }
          line += fmt::format("{}{}", i ? "," : "", v);
        }
        line += "]";
      }
      line += "}\n";
      std::fwrite(line.data(), 1, line.size(), tess_dump_file);
      std::fflush(tess_dump_file);
    }
  }
}

void MetalCommandProcessor::DiagNsLogFetch(const RegisterFile& regs, Shader* pixel_shader, const DxbcShaderTranslator::Modification& pixel_shader_modification) {
  // Trace-only (docs/native-shaders.md): FABLE2_NS_LOG_FETCH=<PS hash hex>
  // logs the fetch constants used by the first 16 draws of that pixel
  // shader, for specialization experiments.
  {
    static const uint64_t log_ps = [] {
      const char* v = MetalDebugEnv("NS_LOG_FETCH");
      return v ? std::strtoull(v, nullptr, 16) : 0ull;
    }();
    static uint32_t logged = 0;
    if (log_ps && pixel_shader && logged < 16 &&
        pixel_shader->ucode_data_hash() == log_ps) {
      ++logged;
      std::string line;
      for (const auto& binding : pixel_shader->texture_bindings()) {
        const uint32_t* f =
            &regs.values[XE_GPU_REG_SHADER_CONSTANT_FETCH_00_0 +
                         binding.fetch_constant * 6];
        line += fmt::format(" tf{}:{:08X},{:08X},{:08X},{:08X},{:08X},{:08X}",
                            binding.fetch_constant, f[0], f[1], f[2], f[3],
                            f[4], f[5]);
      }
      REXLOG_INFO("[ns-fetch] ps {:016X} mod {:X} draw {}:{}",
                  pixel_shader->ucode_data_hash(),
                  pixel_shader_modification.value, logged, line);
    }
  }
}

void MetalCommandProcessor::DiagShmemCheck(Shader* vertex_shader, Shader* pixel_shader, const PrimitiveProcessor::ProcessingResult& primitive_processing_result, const VertexBindingRange* vertex_ranges, uint32_t vertex_range_count, bool guest_dma_index_buffer_read, bool shader_primitive_index_load) {
  // Trace-only (FABLE2_SHMEM_CHECK=1): for vertex/index ranges the shared
  // memory considers valid, compare the GPU-visible shared buffer contents with
  // guest RAM at command-processor time. A mismatch means the GPU would read
  // data that differs from what the guest wrote (stale upload / missed
  // invalidation / not-yet-executed staged upload / GPU-written memexport).
  {
    static const bool shmem_check = [] {
      const char* v = MetalDebugEnv("SHMEM_CHECK");
      return v && v[0] == '1';
    }();
    if (shmem_check && shared_memory_ && shared_memory_->GetBuffer()) {
      static uint32_t shmem_check_logged = 0;
      auto* gpu_base =
          static_cast<const uint8_t*>(shared_memory_->GetBuffer()->contents());
      auto check = [&](const char* kind, uint32_t start, uint32_t length) {
        if (!length || !gpu_base || uint64_t(start) + length > SharedMemory::kBufferSize) {
          return;
        }
        if (!shared_memory_->IsRangeValid(start, length)) {
          return;
        }
        auto* guest = static_cast<const uint8_t*>(memory_->TranslatePhysical(start));
        if (!guest) return;
        const uint8_t* gpu = gpu_base + start;
        if (std::memcmp(guest, gpu, length) == 0) return;
        uint32_t first = 0, diff = 0;
        for (uint32_t i = 0; i < length; ++i) {
          if (guest[i] != gpu[i]) { if (!diff) first = i; ++diff; }
        }
        frame_log::Record(kind, frame_log::CurrentFrame(), diff);
        if (shmem_check_logged < 400) {
          ++shmem_check_logged;
          REXLOG_WARN(
              "[shmem-check] frame={} {} start=0x{:08X} len={} first_diff=+{} "
              "diff_bytes={} vs={:016X} ps={:016X} prim={} verts={}",
              frame_log::CurrentFrame(), kind, start, length, first, diff,
              vertex_shader->ucode_data_hash(),
              pixel_shader ? pixel_shader->ucode_data_hash() : 0,
              uint32_t(primitive_processing_result.host_primitive_type),
              primitive_processing_result.host_draw_vertex_count);
        }
      };
      for (uint32_t i = 0; i < vertex_range_count; ++i) {
        check("sc_vb_mismatch", vertex_ranges[i].offset, vertex_ranges[i].length);
      }
      if (guest_dma_index_buffer_read || shader_primitive_index_load) {
        uint32_t index_stride =
            primitive_processing_result.host_index_format == xenos::IndexFormat::kInt16 ? 2 : 4;
        uint32_t count = shader_primitive_index_load
                             ? primitive_processing_result.guest_draw_vertex_count
                             : primitive_processing_result.host_draw_vertex_count;
        check("sc_ib_mismatch", uint32_t(primitive_processing_result.guest_index_base),
              count * index_stride);
      }
    }
  }
}

void MetalCommandProcessor::DiagDrawDump(const RegisterFile& regs, Shader* vertex_shader, Shader* pixel_shader, const PrimitiveProcessor::ProcessingResult& primitive_processing_result, const DxbcShaderTranslator::Modification& vertex_shader_modification, const DxbcShaderTranslator::Modification& pixel_shader_modification, const VertexBindingRange* vertex_ranges, uint32_t vertex_range_count, bool memexport_used_vertex) {
    // Trace-only (FABLE2_DRAW_DUMP=<file>): append one fixed-size record per
    // draw for frames in FABLE2_DRAW_DUMP_FRAMES=<first>-<last>. Default off.
    {
      struct DrawDumpConfig {
        FILE* file = nullptr;
        uint64_t first = 0, last = 0;
      };
      static DrawDumpConfig* dump_config = [] {
        const char* path = MetalDebugEnv("DRAW_DUMP");
        if (!path || !path[0]) return static_cast<DrawDumpConfig*>(nullptr);
        auto* c = new DrawDumpConfig;
        c->file = std::fopen(path, "wb");
        if (c->file) std::setvbuf(c->file, nullptr, _IOFBF, 16 << 20);
        unsigned long long a = 0, b = 0;
        const char* fr = MetalDebugEnv("DRAW_DUMP_FRAMES");
        if (fr && std::sscanf(fr, "%llu-%llu", &a, &b) == 2) {
          c->first = a; c->last = b;
        } else {
          c->first = 0; c->last = UINT64_MAX;
        }
        REXLOG_INFO("[draw-dump] {} frames {}-{} ok={}", path, c->first, c->last,
                    c->file != nullptr);
        return c;
      }();
      const uint64_t dump_frame = frame_log::CurrentFrame();
      if (dump_config && dump_config->file && dump_frame >= dump_config->first &&
          dump_frame <= dump_config->last) {
  #pragma pack(push, 1)
        struct DrawDumpRecord {
          uint32_t magic;            // 'FDD1'
          uint32_t frame;
          uint32_t draw_index;       // within frame
          uint32_t prim_guest, prim_host;
          uint64_t vs_hash, ps_hash, vs_mod, ps_mod;
          uint32_t guest_vertex_count, host_vertex_count;
          uint32_t index_buffer_type, index_size;  // VGT index_size (0=16,1=32)
          uint32_t guest_index_base, vgt_dma_size, vgt_indx_offset;
          uint32_t idx_min_be, idx_max_be, idx_min_le, idx_max_le;
          uint64_t ib_hash_guest, ib_hash_gpu;
          uint32_t vb_count;
          uint32_t vb_addr[4], vb_size[4], vb_stride[4];
          uint64_t vb_hash_guest[4], vb_hash_gpu[4];
          uint32_t vb_valid_mask;
          uint64_t vsc_hash;         // all 256 VS float constants
          float vsc_maxabs64;        // max |c| in constants 0..63
          uint32_t vsc_nan64, vsc_huge64;  // NaN/Inf count, |c|>1e6 count
          float vsc64[64 * 4];       // first 64 VS float constants
          uint32_t blend0, depthctl, colorctl, colormask, su_sc_mode;
          uint32_t flags;            // bit0 memexport vs, bit1 geom emu, bit2 has ps
        };
  #pragma pack(pop)
        static uint64_t last_frame = UINT64_MAX;
        static uint32_t draw_index = 0;
        if (dump_frame != last_frame) { last_frame = dump_frame; draw_index = 0; }
        DrawDumpRecord r = {};
        r.magic = 0x31444446;
        r.frame = uint32_t(dump_frame);
        r.draw_index = draw_index++;
        r.prim_guest = uint32_t(primitive_processing_result.guest_primitive_type);
        r.prim_host = uint32_t(primitive_processing_result.host_primitive_type);
        r.vs_hash = vertex_shader->ucode_data_hash();
        r.ps_hash = pixel_shader ? pixel_shader->ucode_data_hash() : 0;
        r.vs_mod = vertex_shader_modification.value;
        r.ps_mod = pixel_shader_modification.value;
        r.guest_vertex_count = primitive_processing_result.guest_draw_vertex_count;
        r.host_vertex_count = primitive_processing_result.host_draw_vertex_count;
        r.index_buffer_type = uint32_t(primitive_processing_result.index_buffer_type);
        r.index_size = uint32_t(regs.Get<reg::VGT_DRAW_INITIATOR>().index_size);
        r.guest_index_base = uint32_t(primitive_processing_result.guest_index_base);
        r.vgt_dma_size = regs[XE_GPU_REG_VGT_DMA_SIZE];
        r.vgt_indx_offset = regs[XE_GPU_REG_VGT_INDX_OFFSET];
        const uint8_t* gpu_base =
            (shared_memory_ && shared_memory_->GetBuffer())
                ? static_cast<const uint8_t*>(shared_memory_->GetBuffer()->contents())
                : nullptr;
        auto hash_range = [&](uint32_t start, uint32_t length, uint64_t& guest_hash,
                              uint64_t& gpu_hash) {
          if (!length || uint64_t(start) + length > SharedMemory::kBufferSize) return;
          uint32_t n = std::min<uint32_t>(length, 1u << 20);
          if (auto* g = static_cast<const uint8_t*>(memory_->TranslatePhysical(start))) {
            guest_hash = XXH3_64bits(g, n);
          }
          if (gpu_base) gpu_hash = XXH3_64bits(gpu_base + start, n);
        };
        if (primitive_processing_result.index_buffer_type !=
            PrimitiveProcessor::ProcessedIndexBufferType::kNone) {
          const bool idx32 = r.index_size != 0;
          const uint32_t count = primitive_processing_result.guest_draw_vertex_count;
          const uint32_t bytes = count * (idx32 ? 4u : 2u);
          if (uint64_t(r.guest_index_base) + bytes <= SharedMemory::kBufferSize) {
            auto* g = static_cast<const uint8_t*>(
                memory_->TranslatePhysical(r.guest_index_base));
            if (g && count) {
              uint32_t mn_be = UINT32_MAX, mx_be = 0, mn_le = UINT32_MAX, mx_le = 0;
              for (uint32_t i = 0; i < count; ++i) {
                uint32_t be, le;
                if (idx32) {
                  const uint8_t* q = g + i * 4;
                  be = (uint32_t(q[0]) << 24) | (uint32_t(q[1]) << 16) |
                       (uint32_t(q[2]) << 8) | q[3];
                  le = (uint32_t(q[3]) << 24) | (uint32_t(q[2]) << 16) |
                       (uint32_t(q[1]) << 8) | q[0];
                } else {
                  const uint8_t* q = g + i * 2;
                  be = (uint32_t(q[0]) << 8) | q[1];
                  le = (uint32_t(q[1]) << 8) | q[0];
                }
                mn_be = std::min(mn_be, be); mx_be = std::max(mx_be, be);
                mn_le = std::min(mn_le, le); mx_le = std::max(mx_le, le);
              }
              r.idx_min_be = mn_be; r.idx_max_be = mx_be;
              r.idx_min_le = mn_le; r.idx_max_le = mx_le;
            }
            hash_range(r.guest_index_base, bytes, r.ib_hash_guest, r.ib_hash_gpu);
          }
        }
        r.vb_count = vertex_range_count;
        for (uint32_t i = 0; i < vertex_range_count && i < 4; ++i) {
          r.vb_addr[i] = vertex_ranges[i].offset;
          r.vb_size[i] = vertex_ranges[i].length;
          r.vb_stride[i] = vertex_ranges[i].stride;
          hash_range(vertex_ranges[i].offset, vertex_ranges[i].length,
                     r.vb_hash_guest[i], r.vb_hash_gpu[i]);
          if (shared_memory_ && vertex_ranges[i].length &&
              shared_memory_->IsRangeValid(vertex_ranges[i].offset,
                                           vertex_ranges[i].length)) {
            r.vb_valid_mask |= 1u << i;
          }
        }
        const uint32_t* vsc = &regs.values[XE_GPU_REG_SHADER_CONSTANT_000_X];
        r.vsc_hash = XXH3_64bits(vsc, 256 * 4 * sizeof(uint32_t));
        std::memcpy(r.vsc64, vsc, sizeof(r.vsc64));
        for (uint32_t i = 0; i < 64 * 4; ++i) {
          float f = r.vsc64[i];
          if (!std::isfinite(f)) { ++r.vsc_nan64; continue; }
          float a = std::fabs(f);
          if (a > 1e6f) ++r.vsc_huge64;
          r.vsc_maxabs64 = std::max(r.vsc_maxabs64, a);
        }
        r.blend0 = regs[XE_GPU_REG_RB_BLENDCONTROL0];
        r.depthctl = regs[XE_GPU_REG_RB_DEPTHCONTROL];
        r.colorctl = regs[XE_GPU_REG_RB_COLORCONTROL];
        r.colormask = regs[XE_GPU_REG_RB_COLOR_MASK];
        r.su_sc_mode = regs[XE_GPU_REG_PA_SU_SC_MODE_CNTL];
        r.flags = (memexport_used_vertex ? 1u : 0u) |
                  (pixel_shader ? 4u : 0u);
        std::fwrite(&r, sizeof(r), 1, dump_config->file);
        static uint64_t flushed_frame = 0;
        if (dump_frame != flushed_frame && (dump_frame & 63) == 0) {
          flushed_frame = dump_frame;
          std::fflush(dump_config->file);
        }
        if (dump_frame == dump_config->last) std::fflush(dump_config->file);
      }
    }
}

void MetalCommandProcessor::DiagShadowTrace(const RegisterFile& regs, xenos::PrimitiveType primitive_type, uint32_t index_count, IndexBufferInfo* index_buffer_info, Shader* vertex_shader, Shader* pixel_shader, uint32_t used_texture_mask) {
  // [shadows-native] diagnostic: one line per draw in the trace window, with
  // the bound render targets and every fetched texture (FABLE2_SHADOW_TRACE).
  // FABLE2_SHADOW_TRACE_STENCIL=1 narrows the draw lines to stencil-writing
  // draws and stencil-tested colour draws (long windows stay small).
  static const bool trace_stencil_only = [] {
    const char* v = MetalDebugEnv("SHADOW_TRACE_STENCIL");  // getenv-ok: once
    return v && v[0] == '1';
  }();
  if (render_target_cache_ && MetalRenderTargetCache::ShadowTraceActive() &&
      (!trace_stencil_only ||
       ((regs[XE_GPU_REG_RB_DEPTHCONTROL] & 1u) &&
        ((regs[XE_GPU_REG_RB_STENCILREFMASK] & 0x00FF0000u) == 0x00200000u ||
         (regs[XE_GPU_REG_RB_STENCILREFMASK] & 0x00FFFF00u) == 0x00003000u)))) {
    std::string tex;
    uint32_t remaining = used_texture_mask;
    uint32_t ti;
    while (rex::bit_scan_forward(remaining, &ti)) {
      remaining &= ~(uint32_t(1) << ti);
      const auto fetch = regs.GetTextureFetch(ti);
      const uint32_t base = uint32_t(fetch.base_address << 12);
      tex += fmt::format(
          " T{}=0x{:08X}/fmt{}/dim{}/{}x{}/p{}/m0x{:08X}/mip{}-{}/bias{}", ti,
          base, uint32_t(fetch.format), uint32_t(fetch.dimension),
          uint32_t(fetch.size_2d.width) + 1, uint32_t(fetch.size_2d.height) + 1,
          uint32_t(fetch.pitch) * 32, uint32_t(fetch.mip_address << 12),
          uint32_t(fetch.mip_min_level), uint32_t(fetch.mip_max_level),
          int32_t(fetch.lod_bias));
    }
    tex += fmt::format(" ib=0x{:08X} n={}",
                       index_buffer_info ? index_buffer_info->guest_base : 0u,
                       index_count);
    REXLOG_INFO("[shadow-trace] f{} draw {} vs={:016X} ps={:016X} zc={:08X} "
                "cm={:08X} bc0={:08X} cc={:08X} srm={:08X} prim={} sc={:08X}-{:08X} vp={}x{}{}{}",
                frame_log::CurrentFrame(),
                render_target_cache_->DescribeCurrentTargetsForTrace(),
                vertex_shader->ucode_data_hash(),
                pixel_shader ? pixel_shader->ucode_data_hash() : 0,
                regs[XE_GPU_REG_RB_DEPTHCONTROL], regs[XE_GPU_REG_RB_COLOR_MASK],
                regs[XE_GPU_REG_RB_BLENDCONTROL0], regs[XE_GPU_REG_RB_COLORCONTROL],
                regs[XE_GPU_REG_RB_STENCILREFMASK], uint32_t(primitive_type),
                regs[XE_GPU_REG_PA_SC_WINDOW_SCISSOR_TL],
                regs[XE_GPU_REG_PA_SC_WINDOW_SCISSOR_BR],
                regs.Get<float>(XE_GPU_REG_PA_CL_VPORT_XSCALE) * 2.0f,
                regs.Get<float>(XE_GPU_REG_PA_CL_VPORT_YSCALE) * -2.0f, tex,
                hle_census::Enabled()
                    ? fmt::format(" site={} prim={} n={}", census_draw_site_,
                                  uint32_t(primitive_type), index_count)
                    : std::string());
  }
  // Diagnostic (FABLE2_TRACE_VSC=<vs hash hex>:<c>,<c>,...): log these VS
  // float constants for that vertex shader's draws in the shadow-trace window.
  if (render_target_cache_ && MetalRenderTargetCache::ShadowTraceActive()) {
    static const std::pair<uint64_t, std::vector<uint32_t>> trace_vsc = [] {
      std::pair<uint64_t, std::vector<uint32_t>> r{0, {}};
      const char* v = MetalDebugEnv("TRACE_VSC");  // getenv-ok: once
      if (!v) return r;
      char* end = nullptr;
      r.first = std::strtoull(v, &end, 16);
      while (end && (*end == ':' || *end == ',')) {
        const char* b = end + 1;
        const unsigned long c = std::strtoul(b, &end, 10);
        if (end == b) break;
        r.second.push_back(uint32_t(c));
      }
      return r;
    }();
    if (trace_vsc.first && vertex_shader->ucode_data_hash() == trace_vsc.first) {
      std::string cs;
      for (uint32_t c : trace_vsc.second) {
        const uint32_t base = XE_GPU_REG_SHADER_CONSTANT_000_X + c * 4;
        cs += fmt::format(" c{}=({},{},{},{})", c, regs.Get<float>(base),
                          regs.Get<float>(base + 1), regs.Get<float>(base + 2),
                          regs.Get<float>(base + 3));
      }
      // FABLE2_TRACE_TEXDUMP=<dir>: raw guest bytes of each fetched texture
      // (base level region, 1 MB max) as <dir>/<base hex>.bin, once each.
      static const char* texdump_dir = MetalDebugEnv("TRACE_TEXDUMP");  // getenv-ok: once
      if (texdump_dir) {
        static std::vector<uint32_t> dumped;
        uint32_t m = used_texture_mask, ti;
        while (rex::bit_scan_forward(m, &ti)) {
          m &= ~(uint32_t(1) << ti);
          const auto fetch = regs.GetTextureFetch(ti);
          for (uint32_t which = 0; which < 2; ++which) {
            const uint32_t base = which ? uint32_t(fetch.mip_address << 12)
                                        : uint32_t(fetch.base_address << 12);
            if (!base || std::find(dumped.begin(), dumped.end(), base) != dumped.end()) {
              continue;
            }
            dumped.push_back(base);
            const uint8_t* src = memory_->TranslatePhysical<const uint8_t*>(base);
            const std::string path = fmt::format("{}/{:08X}.bin", texdump_dir, base);
            if (FILE* f = std::fopen(path.c_str(), "wb")) {
              std::fwrite(src, 1, 1u << 20, f);
              std::fclose(f);
            }
            REXLOG_INFO("[trace-texdump] T{} {:08X} fetch {:08X} {:08X} {:08X} {:08X} {:08X} {:08X}",
                        ti, base,
                        regs.values[XE_GPU_REG_SHADER_CONSTANT_FETCH_00_0 + ti * 6],
                        regs.values[XE_GPU_REG_SHADER_CONSTANT_FETCH_00_0 + ti * 6 + 1],
                        regs.values[XE_GPU_REG_SHADER_CONSTANT_FETCH_00_0 + ti * 6 + 2],
                        regs.values[XE_GPU_REG_SHADER_CONSTANT_FETCH_00_0 + ti * 6 + 3],
                        regs.values[XE_GPU_REG_SHADER_CONSTANT_FETCH_00_0 + ti * 6 + 4],
                        regs.values[XE_GPU_REG_SHADER_CONSTANT_FETCH_00_0 + ti * 6 + 5]);
          }
        }
      }
      std::string pcs;
      for (uint32_t c : {20u, 31u, 44u, 45u, 46u, 47u}) {
        const uint32_t base = XE_GPU_REG_SHADER_CONSTANT_256_X + c * 4;
        pcs += fmt::format(" pc{}=({},{},{},{})", c, regs.Get<float>(base),
                           regs.Get<float>(base + 1), regs.Get<float>(base + 2),
                           regs.Get<float>(base + 3));
      }
      REXLOG_INFO("[trace-vsc] f{} vs={:016X} ps={:016X} n={} aref={} cc={:08X}{}{}",
                  frame_log::CurrentFrame(), vertex_shader->ucode_data_hash(),
                  pixel_shader ? pixel_shader->ucode_data_hash() : 0,
                  index_count, regs.Get<float>(XE_GPU_REG_RB_ALPHA_REF),
                  regs[XE_GPU_REG_RB_COLORCONTROL], cs, pcs);
    }
  }
}

void MetalCommandProcessor::MaybeDumpBackendTelemetry(const char* reason,
                                                      bool force) {
  if (!REXCVAR_GET(metal_backend_telemetry)) {
    return;
  }
  int32_t interval_config = REXCVAR_GET(metal_backend_telemetry_interval);
  uint64_t interval = interval_config > 0 ? uint64_t(interval_config) : 0;
  if (!force &&
      (!interval || backend_telemetry_.swaps <
                        backend_telemetry_last_dump_swap_ + interval)) {
    return;
  }

  MetalRenderTargetCache::TelemetryStats rt_stats = {};
  if (render_target_cache_) {
    rt_stats = render_target_cache_->GetAndResetTelemetryStats();
  }

  const std::string end_reasons = MetalFormatNamedCounts(
      backend_telemetry_.end_reasons, MetalRenderEncoderEndReasonName);
  const std::string transfer_request_sources = MetalFormatNamedTriplets(
      backend_telemetry_.transfer_request_sources_total,
      backend_telemetry_.transfer_request_sources_active,
      backend_telemetry_.transfer_request_sources_no_active,
      MetalTransferRequestSourceName);
  const std::string transfer_request_render_end_sources =
      MetalFormatNamedCounts(
          backend_telemetry_.transfer_request_render_encoder_ends,
          MetalTransferRequestSourceName);
  const std::string shared_memory_request_upload_bytes = MetalFormatNamedCounts(
      backend_telemetry_.shared_memory_request_upload_bytes,
      MetalSharedMemoryRequestReasonName);
  const std::string shared_memory_request_failures =
      MetalFormatNamedCounts(backend_telemetry_.shared_memory_request_failures,
                             MetalSharedMemoryRequestReasonName);
  const std::string shared_memory_request_outcomes =
      MetalFormatNamedCounts(backend_telemetry_.shared_memory_request_outcomes,
                             MetalSharedMemoryRequestOutcomeName);
  const std::string shared_memory_upload_route_counts = MetalFormatNamedCounts(
      backend_telemetry_.shared_memory_upload_route_counts,
      MetalSharedMemoryUploadRouteName);
  const std::string shared_memory_upload_route_bytes = MetalFormatNamedCounts(
      backend_telemetry_.shared_memory_upload_route_bytes,
      MetalSharedMemoryUploadRouteName);
  const std::string shared_memory_direct_write_rejects = MetalFormatNamedCounts(
      backend_telemetry_.shared_memory_direct_write_reject_counts,
      MetalSharedMemoryDirectWriteRejectReasonName);
  const std::string shared_memory_direct_write_reject_bytes =
      MetalFormatNamedCounts(
          backend_telemetry_.shared_memory_direct_write_reject_bytes,
          MetalSharedMemoryDirectWriteRejectReasonName);
  const std::string texture_upload_source_route_counts = MetalFormatNamedCounts(
      backend_telemetry_.texture_upload_source_route_counts,
      MetalTextureUploadSourceRouteName);
  const std::string texture_upload_source_route_bytes = MetalFormatNamedCounts(
      backend_telemetry_.texture_upload_source_route_bytes,
      MetalTextureUploadSourceRouteName);
  const std::string texture_upload_source_fallback_reasons =
      MetalFormatNamedCounts(
          backend_telemetry_.texture_upload_source_fallback_reasons,
          MetalTextureUploadSourceFallbackReasonName);
  const std::string shared_memory_upload_encoder_end_reasons =
      MetalFormatNamedCounts(
          backend_telemetry_.shared_memory_upload_encoder_end_reasons,
          MetalSharedMemoryUploadEncoderEndReasonName);
  const std::string draw_materialization_source_ranges = MetalFormatNamedCounts(
      backend_telemetry_.draw_materialization_source_ranges,
      MetalTelemetryDrawMaterializationSourceName);
  const std::string draw_materialization_source_bytes = MetalFormatNamedCounts(
      backend_telemetry_.draw_materialization_source_bytes,
      MetalTelemetryDrawMaterializationSourceName);
  const std::string draw_materialization_source_invalid_ranges =
      MetalFormatNamedCounts(
          backend_telemetry_.draw_materialization_source_invalid_ranges,
          MetalTelemetryDrawMaterializationSourceName);
  const std::string draw_materialization_source_invalid_bytes =
      MetalFormatNamedCounts(
          backend_telemetry_.draw_materialization_source_invalid_bytes,
          MetalTelemetryDrawMaterializationSourceName);
  const std::string prepared_draw_queue_flush_reasons = MetalFormatNamedCounts(
      backend_telemetry_.prepared_draw_queue_flush_reasons,
      MetalPreparedDrawFlushReasonName);
  const std::string prepared_draw_queue_reject_reasons = MetalFormatNamedCounts(
      backend_telemetry_.prepared_draw_queue_reject_reasons,
      MetalPreparedDrawQueueRejectReasonName);
  const std::string cbv_uploads = MetalFormatNamedCounts(
      backend_telemetry_.cbv_uploads, MetalTelemetryCbvSlotName);
  const std::string cbv_reuse_hits = MetalFormatNamedCounts(
      backend_telemetry_.cbv_reuse_hits, MetalTelemetryCbvSlotName);
  const std::string cbv_upload_bytes = MetalFormatNamedCounts(
      backend_telemetry_.cbv_upload_bytes, MetalTelemetryCbvSlotName);
  const std::string descriptor_index_uploads =
      MetalFormatNamedCounts(backend_telemetry_.descriptor_index_uploads,
                             MetalTelemetryShaderStageName);
  const std::string bindless_root_allocations =
      MetalFormatNamedCounts(backend_telemetry_.bindless_root_allocations,
                             MetalTelemetryRootScopeName);
  const std::string bindless_root_reuse_hits = MetalFormatNamedCounts(
      backend_telemetry_.bindless_root_reuse_hits, MetalTelemetryRootScopeName);
  const std::string bindless_root_arg_noop_updates =
      MetalFormatNamedCounts(backend_telemetry_.bindless_root_arg_noop_updates,
                             MetalTelemetryRootScopeName);
  const std::string bindless_root_arg_slots_patched =
      MetalFormatNamedCounts(backend_telemetry_.bindless_root_arg_slots_patched,
                             MetalTelemetryRootScopeName);
  const std::string bindless_root_arg_bytes_copied =
      MetalFormatNamedCounts(backend_telemetry_.bindless_root_arg_bytes_copied,
                             MetalTelemetryRootScopeName);
  const std::string bindless_root_arg_slot_patches =
      MetalFormatNamedCounts(backend_telemetry_.bindless_root_arg_slot_patches,
                             MetalTelemetryRootArgSlotName);
  const std::string bindless_root_rebuild_reasons =
      MetalFormatNamedCounts(backend_telemetry_.bindless_root_rebuild_reasons,
                             MetalTelemetryRootRebuildReasonName);
  const std::string encoder_buffer_full_binds = MetalFormatNamedCounts(
      backend_telemetry_.render_encoder_buffer_full_binds,
      MetalTelemetryRenderEncoderBufferStageName);
  const std::string encoder_buffer_offset_binds = MetalFormatNamedCounts(
      backend_telemetry_.render_encoder_buffer_offset_binds,
      MetalTelemetryRenderEncoderBufferStageName);
  const std::string encoder_buffer_noop_binds = MetalFormatNamedCounts(
      backend_telemetry_.render_encoder_buffer_noop_binds,
      MetalTelemetryRenderEncoderBufferStageName);
  const std::string encoder_buffer_null_binds = MetalFormatNamedCounts(
      backend_telemetry_.render_encoder_buffer_null_binds,
      MetalTelemetryRenderEncoderBufferStageName);
  const std::string encoder_buffer_untracked_binds = MetalFormatNamedCounts(
      backend_telemetry_.render_encoder_buffer_untracked_binds,
      MetalTelemetryRenderEncoderBufferStageName);
  const std::string render_resource_set_applies =
      MetalFormatNamedCounts(backend_telemetry_.render_resource_set_applies,
                             MetalTelemetryRenderResourceSetName);
  const std::string render_resource_set_skips =
      MetalFormatNamedCounts(backend_telemetry_.render_resource_set_skips,
                             MetalTelemetryRenderResourceSetName);
  const std::string render_resource_set_resources =
      MetalFormatNamedCounts(backend_telemetry_.render_resource_set_resources,
                             MetalTelemetryRenderResourceSetName);
  const std::string render_resource_registry_serial_skips =
      MetalFormatNamedCounts(
          backend_telemetry_.render_resource_registry_serial_skips,
          MetalTelemetryRenderResourceSetName);
  const std::string render_resource_registry_builds =
      MetalFormatNamedCounts(backend_telemetry_.render_resource_registry_builds,
                             MetalTelemetryRenderResourceSetName);
  const std::string render_resource_registry_registers = MetalFormatNamedCounts(
      backend_telemetry_.render_resource_registry_registers,
      MetalTelemetryRenderResourceSetName);
  const auto& direct_host_stats = rt_stats.resolve_direct_host;
  MetalPipelineRuntimeStats pipeline_runtime_stats = {};
  if (pipeline_cache_) {
    pipeline_runtime_stats = pipeline_cache_->GetAndResetRuntimeStats();
  }

  REXLOG_INFO(
      "MetalTelemetry[{}]: work swaps={} draws={} pipelines set/skip={}/{} "
      "texture_requests before/after_encoder={}/{}",
      reason, backend_telemetry_.swaps - backend_telemetry_last_dump_swap_,
      backend_telemetry_.draw_calls, backend_telemetry_.pipeline_sets,
      backend_telemetry_.pipeline_set_skips,
      backend_telemetry_.texture_requests_before_encoder,
      backend_telemetry_.texture_requests_after_encoder_begin);
  REXLOG_INFO(
      "MetalTelemetry[{}]: render_encoder begin_calls={} reused={} created={} "
      "descriptor_restarts={} resource_resets={} desc_fail={} create_fail={} "
      "end active/no_active={}/{} reasons={{ {} }}",
      reason, backend_telemetry_.begin_encoder_calls,
      backend_telemetry_.begin_encoder_reused_compatible,
      backend_telemetry_.begin_encoder_created,
      backend_telemetry_.begin_encoder_descriptor_restarts,
      backend_telemetry_.begin_encoder_resource_usage_resets,
      backend_telemetry_.begin_encoder_descriptor_failures,
      backend_telemetry_.begin_encoder_creation_failures,
      backend_telemetry_.end_encoder_active,
      backend_telemetry_.end_encoder_no_active, end_reasons);
  REXLOG_INFO(
      "MetalTelemetry[{}]: transfer_request sources "
      "total/active/no_active={{ {} }} render_end={{ {} }}",
      reason, transfer_request_sources, transfer_request_render_end_sources);
  REXLOG_INFO("MetalTelemetry[{}]: shared_memory request failures={{ {} }}", reason,
         shared_memory_request_failures);
  REXLOG_INFO(
      "MetalTelemetry[{}]: shared_memory upload bytes={{ {} }} "
      "outcomes={{ {} }}",
      reason, shared_memory_request_upload_bytes,
      shared_memory_request_outcomes);
  REXLOG_INFO(
      "MetalTelemetry[{}]: shared_memory upload_batches requests={} "
      "ranges input/coalesced={}/{} bytes={} upload_encoder "
      "acquire/reuse/copies={}/{}/{} closes={{ {} }} routes counts={{ {} }} "
      "bytes={{ {} }}",
      reason, backend_telemetry_.shared_memory_upload_batches,
      backend_telemetry_.shared_memory_upload_batch_input_ranges,
      backend_telemetry_.shared_memory_upload_batch_coalesced_ranges,
      backend_telemetry_.shared_memory_upload_batch_bytes,
      backend_telemetry_.shared_memory_upload_encoder_acquisitions,
      backend_telemetry_.shared_memory_upload_encoder_reuses,
      backend_telemetry_.shared_memory_upload_encoder_copies,
      shared_memory_upload_encoder_end_reasons,
      shared_memory_upload_route_counts, shared_memory_upload_route_bytes);
  REXLOG_INFO(
      "MetalTelemetry[{}]: shared_memory direct_write "
      "eligible/staged_bytes={}/{} rejects={{ {} }} reject_bytes={{ {} }}",
      reason, backend_telemetry_.shared_memory_direct_write_eligible_bytes,
      backend_telemetry_.shared_memory_direct_write_staged_required_bytes,
      shared_memory_direct_write_rejects,
      shared_memory_direct_write_reject_bytes);
  REXLOG_INFO(
      "MetalTelemetry[{}]: shared_memory lazy_upload batches "
      "no_upload/direct_only/mixed/staged_only={}/{}/{}/{} "
      "render_active direct_only/mixed/staged_only={}/{}/{}",
      reason, backend_telemetry_.shared_memory_lazy_upload_no_upload_batches,
      backend_telemetry_.shared_memory_lazy_upload_direct_only_batches,
      backend_telemetry_.shared_memory_lazy_upload_mixed_batches,
      backend_telemetry_.shared_memory_lazy_upload_staged_only_batches,
      backend_telemetry_.shared_memory_lazy_upload_direct_only_active,
      backend_telemetry_.shared_memory_lazy_upload_mixed_active,
      backend_telemetry_.shared_memory_lazy_upload_staged_only_active);
  REXLOG_INFO(
      "MetalTelemetry[{}]: texture_upload_source routes counts={{ {} }} "
      "bytes={{ {} }} fallback_reasons={{ {} }}",
      reason, texture_upload_source_route_counts,
      texture_upload_source_route_bytes,
      texture_upload_source_fallback_reasons);
  REXLOG_INFO(
      "MetalTelemetry[{}]: draw_materialization ranges={{ {} }} bytes={{ {} }} "
      "invalid_ranges={{ {} }} invalid_bytes={{ {} }} "
      "per_draw requests/invalid/resident_skip={}/{}/{}",
      reason, draw_materialization_source_ranges,
      draw_materialization_source_bytes,
      draw_materialization_source_invalid_ranges,
      draw_materialization_source_invalid_bytes,
      backend_telemetry_.draw_materialization_per_draw_requests,
      backend_telemetry_.draw_materialization_per_draw_invalid_requests,
      backend_telemetry_.draw_materialization_per_draw_resident_skips);
  REXLOG_INFO(
      "MetalTelemetry[{}]: prepared_draw_queue appends={} flushes={} "
      "single_draw_flushes={} draws_flushed={} ranges_flushed={} "
      "bytes_flushed={} invalid_flushes={} texture_plans={} "
      "texture_loads planned/executed={}/{} flush_reasons={{ {} }} "
      "rejects={{ {} }}",
      reason, backend_telemetry_.prepared_draw_queue_appends,
      backend_telemetry_.prepared_draw_queue_flushes,
      backend_telemetry_.prepared_draw_queue_single_draw_flushes,
      backend_telemetry_.prepared_draw_queue_draws_flushed,
      backend_telemetry_.prepared_draw_queue_ranges_flushed,
      backend_telemetry_.prepared_draw_queue_bytes_flushed,
      backend_telemetry_.prepared_draw_queue_invalid_flushes,
      backend_telemetry_.prepared_draw_queue_texture_plans_flushed,
      backend_telemetry_.prepared_draw_queue_texture_loads_planned,
      backend_telemetry_.prepared_draw_queue_texture_loads_executed,
      prepared_draw_queue_flush_reasons, prepared_draw_queue_reject_reasons);
  REXLOG_INFO(
      "MetalTelemetry[{}]: constants cbv_uploads={{ {} }} "
      "cbv_reuse={{ {} }} cbv_upload_bytes={{ {} }} "
      "descriptor_index_uploads={{ {} }} "
      "root_allocations={{ {} }} root_reuse={{ {} }} "
      "root_rebuild_reasons={{ {} }} frame_slot_waits={} "
      "waited_submissions={} last_wait_submission={}",
      reason, cbv_uploads, cbv_reuse_hits, cbv_upload_bytes,
      descriptor_index_uploads,
      bindless_root_allocations, bindless_root_reuse_hits,
      bindless_root_rebuild_reasons, backend_telemetry_.frame_slot_waits,
      backend_telemetry_.frame_slot_wait_submission_count,
      backend_telemetry_.frame_slot_wait_submission_last);
  REXLOG_INFO(
      "MetalTelemetry[{}]: root_args allocs={{ {} }} reuse={{ {} }} "
      "noop={{ {} }} slots_patched={{ {} }} bytes_copied={{ {} }} "
      "slot_patches={{ {} }} rebuild_reasons={{ {} }}",
      reason, bindless_root_allocations, bindless_root_reuse_hits,
      bindless_root_arg_noop_updates, bindless_root_arg_slots_patched,
      bindless_root_arg_bytes_copied, bindless_root_arg_slot_patches,
      bindless_root_rebuild_reasons);
  if (REXCVAR_GET(metal_root_rebuild_detail_telemetry)) {
    const std::string bindless_root_slots_changed =
        MetalFormatNamedCounts(backend_telemetry_.bindless_root_slots_changed,
                               MetalTelemetryRootSlotsChangedName);
    const std::string bindless_root_rebuild_details =
        MetalFormatNamedCounts(backend_telemetry_.bindless_root_rebuild_details,
                               MetalTelemetryRootRebuildDetailName);
    REXLOG_INFO(
        "MetalTelemetry[{}]: root_rebuild_detail slots_changed={{ {} }} "
        "details={{ {} }}",
        reason, bindless_root_slots_changed, bindless_root_rebuild_details);
  }
  REXLOG_INFO(
      "MetalTelemetry[{}]: encoder_bindings full={{ {} }} offset={{ {} }} "
      "noop={{ {} }} null={{ {} }} untracked={{ {} }} resource_use "
      "calls/skips/upgrades={}/{}/{} batches/resources/skips={}/{}/{} "
      "resource_sets apply={{ {} }} skip={{ {} }} resources={{ {} }}",
      reason, encoder_buffer_full_binds, encoder_buffer_offset_binds,
      encoder_buffer_noop_binds, encoder_buffer_null_binds,
      encoder_buffer_untracked_binds,
      backend_telemetry_.render_encoder_use_resource_calls,
      backend_telemetry_.render_encoder_use_resource_skips,
      backend_telemetry_.render_encoder_use_resource_upgrades,
      backend_telemetry_.render_encoder_use_resources_batches,
      backend_telemetry_.render_encoder_use_resources_requested,
      backend_telemetry_.render_encoder_use_resources_skips,
      render_resource_set_applies, render_resource_set_skips,
      render_resource_set_resources);
  REXLOG_INFO(
      "MetalTelemetry[{}]: resource_registry serial_skip={{ {} }} "
      "build={{ {} }} register={{ {} }}",
      reason, render_resource_registry_serial_skips,
      render_resource_registry_builds, render_resource_registry_registers);
  REXLOG_INFO(
      "MetalTelemetry[{}]: residency_set supported/enabled/attached={}/{}/{} "
      "allocations added/duplicates/commits={}/{}/{} "
      "resource_refs covered/fallback={}/{} "
      "use_resource covered/fallback={}/{} "
      "use_heap covered/fallback={}/{} live_allocations={}",
      reason, residency_set_supported_ ? 1 : 0, residency_set_enabled_ ? 1 : 0,
      residency_set_attached_ ? 1 : 0,
      backend_telemetry_.residency_set_allocations_added,
      backend_telemetry_.residency_set_allocation_duplicates,
      backend_telemetry_.residency_set_commits,
      backend_telemetry_.residency_set_resource_refs_covered,
      backend_telemetry_.residency_set_resource_refs_fallback,
      backend_telemetry_.residency_set_use_resources_covered,
      backend_telemetry_.residency_set_use_resources_fallback,
      backend_telemetry_.residency_set_use_heaps_covered,
      backend_telemetry_.residency_set_use_heaps_fallback,
      residency_set_ ? uint64_t(residency_set_->allocationCount()) : 0);
  REXLOG_INFO(
      "MetalTelemetry[{}]: resolve_direct_host attempt/success={}/{} "
      "reject gamma/exp_bias/format/sample/depth_no_fast={}/{}/{}/{}/{}",
      reason, direct_host_stats.direct_host_attempt,
      direct_host_stats.direct_host_success,
      direct_host_stats.direct_host_reject_gamma,
      direct_host_stats.direct_host_reject_exp_bias,
      direct_host_stats.direct_host_reject_format_mismatch,
      direct_host_stats.direct_host_reject_sample_select,
      direct_host_stats.direct_host_reject_depth_no_fast);
  REXLOG_INFO(
      "MetalTelemetry[{}]: shader_prep dxil requests/hits/misses/failures="
      "{}/{}/{}/{} "
      "bytes dxbc/dxil={}/{} ms total/max={}/{} libraries "
      "requests/failures={}/{} bytes={} ms total/max={}/{} render_pso "
      "requests/failures={}/{} ms total/max={}/{}",
      reason, pipeline_runtime_stats.dxil_convert_requests,
      pipeline_runtime_stats.dxil_cache_hits,
      pipeline_runtime_stats.dxil_cache_misses,
      pipeline_runtime_stats.dxil_convert_failures,
      pipeline_runtime_stats.dxil_convert_dxbc_bytes,
      pipeline_runtime_stats.dxil_convert_dxil_bytes,
      pipeline_runtime_stats.dxil_convert_ms_total,
      pipeline_runtime_stats.dxil_convert_ms_max,
      pipeline_runtime_stats.library_requests,
      pipeline_runtime_stats.library_failures,
      pipeline_runtime_stats.library_bytes,
      pipeline_runtime_stats.library_ms_total,
      pipeline_runtime_stats.library_ms_max,
      pipeline_runtime_stats.render_pipeline_requests,
      pipeline_runtime_stats.render_pipeline_failures,
      pipeline_runtime_stats.render_pipeline_ms_total,
      pipeline_runtime_stats.render_pipeline_ms_max);
  ResetBackendTelemetry();
}

}  // namespace metal
}  // namespace graphics
}  // namespace rex
