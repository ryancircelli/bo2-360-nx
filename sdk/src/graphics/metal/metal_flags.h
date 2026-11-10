#pragma once
DECLARE_int32(anisotropic_override);
DECLARE_bool(async_shader_compilation);
DECLARE_bool(clear_memory_page_state);
DECLARE_bool(depth_float24_convert_in_pixel_shader);
DECLARE_bool(depth_float24_round);
DECLARE_bool(depth_transfer_not_equal_test);
DECLARE_path(dump_shaders);
DECLARE_bool(gamma_render_target_as_unorm16);
DECLARE_bool(gpu_3d_to_2d_texture);
DECLARE_bool(gpu_allow_invalid_fetch_constants);
DECLARE_bool(metal_allow_gamma_unorm16);
DECLARE_bool(metal_allow_tearing);
DECLARE_bool(metal_backend_telemetry);
DECLARE_int32(metal_backend_telemetry_interval);
DECLARE_bool(metal_constant_payload_cache);
DECLARE_bool(metal_dedupe_gpu_access);
DECLARE_bool(metal_tile_resolve_half_bits);
DECLARE_bool(metal_fetch_constants_shared);
DECLARE_bool(metal_bindless_index_memo);
DECLARE_bool(metal_register_write_fast_path);
DECLARE_bool(metal_3d_as_2d_refresh);
DECLARE_bool(metal_encode_thread);
DECLARE_bool(metal_direct_host_resolve);
DECLARE_bool(metal_float_constants_dirty_on_change);
DECLARE_bool(metal_force_bc_decompress);
DECLARE_int32(metal_heap_min_bytes);
DECLARE_bool(metal_pipeline_binary_archive);
DECLARE_int32(metal_pipeline_creation_threads);
DECLARE_bool(metal_presenter_debug_markers);
DECLARE_bool(metal_presenter_force_10bpc);
DECLARE_int32(metal_presenter_metalfx_color_processing);
DECLARE_int32(metal_presenter_metalfx_scale_x);
DECLARE_int32(metal_presenter_metalfx_scale_y);
DECLARE_bool(metal_presenter_use_backing_scale);
DECLARE_bool(metal_presenter_use_metalfx);
DECLARE_string(metal_residency_sets);
DECLARE_bool(metal_root_rebuild_detail_telemetry);
DECLARE_bool(metal_shader_disk_cache);
DECLARE_bool(metal_shared_memory_direct_write);
DECLARE_bool(metal_texture_cache_use_private);
DECLARE_bool(metal_texture_upload_via_blit);
DECLARE_bool(metal_transfer_fast_divmod);
DECLARE_bool(metal_transfer_in_draw_pass);
DECLARE_bool(metal_transfer_msaa_sample_id);
DECLARE_bool(metal_transfer_native_stencil_output);
DECLARE_bool(metal_use_heaps);
DECLARE_bool(occlusion_query_log);
DECLARE_bool(snorm16_render_target_full_range);
DECLARE_bool(submit_on_primary_buffer_end);

DECLARE_bool(ac6_ground_fix);

// Settings formerly read from FABLE2_* environment variables (README.md,
// "Settings"). User-facing ones are the cvars below (env REX_METAL_<NAME>, or
// the legacy FABLE2_<NAME> through ApplyLegacyMetalEnv).
DECLARE_bool(metal_native_first);
DECLARE_int32(metal_native_points);
DECLARE_bool(metal_native_shaders);
DECLARE_int32(metal_native_tess);
DECLARE_bool(metal_native_xlate);
DECLARE_string(metal_ns_cache_toolchain);
DECLARE_bool(metal_ns_specialize);
DECLARE_string(metal_msc_min_target);
DECLARE_bool(metal_occlusion_real);
DECLARE_int32(metal_present_drawables);
DECLARE_string(metal_res_scale);
DECLARE_bool(metal_resolve_to_texture);
DECLARE_bool(metal_retina);
DECLARE_string(metal_shader_prep);
DECLARE_double(metal_shader_prep_timeout_s);
DECLARE_bool(metal_ssm_merge);
DECLARE_bool(metal_tile_resolve);
DECLARE_string(metal_title_quirks);
DECLARE_bool(metal_vsync_phase_lock);
DECLARE_int32(metal_warmup_threads);

// Debug and diagnostic switch <name>: the environment variable
// REX_METAL_<name>, else the legacy FABLE2_<name>; nullptr when neither is set.
// Not cached: call it once (static or setup), never per draw.
const char* MetalDebugEnv(const char* name);

// True when metal_title_quirks lists `quirk` (comma or space separated, e.g.
// "fable2"). Parsed once on first use.
bool MetalTitleQuirk(const char* quirk);

// Fable II-only workarounds (metal_title_quirks=fable2). Off for other titles.
inline bool MetalFable2Quirks() {
  static const bool on = MetalTitleQuirk("fable2");
  return on;
}

// Maps legacy FABLE2_<NAME> variables onto the user-facing cvars above when the
// cvar was not set by a config file, REX_METAL_<NAME> or the command line.
void ApplyLegacyMetalEnv();
