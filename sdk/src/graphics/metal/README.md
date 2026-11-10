# rexgpu-metal: native Metal GPU plugin

A Xenos GPU emulation plugin that draws with Metal on Apple Silicon Macs, so ReXGlue titles do not need MoltenVK.
It is built next to `rexgpu-xenos` and loaded the same way.

## Origin

- Adapted from the Metal backend of [Fable II Recomp for macOS](https://github.com/andrew17182/Fable-2-Recomp-macOS)
  (MIT, commit `6f5459e`), which adapts the [XeniOS](https://github.com/xenios-jp/XeniOS) Metal backend
  (BSD-3-Clause, commit `87b176a`) to the ReXGlue GPU plugin ABI. Its Xenos to MSL translator
  (`port/rex/graphics/metal/metal_msl_translator.cc`) is Fable II's own work.
- `licenses/` holds every upstream notice; `provenance.json` records what came from where.
- This copy is the `metal-renderer` branch of ryancircelli/rexglue-sdk, rebuilt for this repository's SDK fork.
  `patches/` is regenerated against this fork's graphics sources: four hunks of `sdk-metal.patch` were fixed by hand
  (`pipeline/shader/interpreter.cpp`, `primitive_processor.cpp`, and two in `command_processor.cpp`, where this fork
  had already changed the same lines).

## Building

```sh
xcodebuild -downloadComponent MetalToolchain   # once, if `xcrun metal` is missing
cmake --preset mac-arm64 -DREXGLUE_USE_METAL=ON
cmake --build out/build/mac-arm64 --config Release --target rexgpu-metal
```

`REXGLUE_USE_METAL` is OFF by default and only exists on macOS; configuring it on an Intel Mac fails.

What configure does:

1. Downloads the pinned, hash-checked dependencies in `dependencies.json` (metal-cpp, the Metal Shader Converter
   runtime headers, XeniOS source for the built-in helper shaders) into the build directory.
2. Copies `include/rex/graphics`, `src/graphics` and `thirdparty/dxbc` into `<build>/src/graphics/metal/sdk-private`
   and applies `patches/*.patch` there. Only this plugin compiles that copy; the runtime and `rexgpu-xenos` keep
   the stock sources. The copy is remade when any SDK graphics file or patch changes.
3. Adds a build step that compiles the XeniOS helper shaders to metallib headers with the Xcode Metal toolchain
   (`tools/compile_metal_helpers.py`).

The SDK changes every module must see the same way (in `xenos.h`, `texture/info.h`, `math.h`) are in the SDK
itself, not in the patches.

This is the converter-free configuration (`FABLE2_NO_CONVERTER=1`): guest shaders are translated by the native
Xenos to MSL translator only. The DXBC to DXIL to Metal Shader Converter sources are kept in `port/` but not
compiled, so neither dxilconv nor Apple's converter library is needed.

`rexgpu-metal-msl-xlate` (not built by default) runs the translator offline on dumped Xenos shaders.

## Using it

Stage it with the game: `rexglue_configure_target(<target> GPU_PLUGINS metal)`, then run with
`--gpu_plugin=metal` (or set `gpu_plugin = "metal"` in the title's config).

## Settings

User-facing switches are SDK cvars (category `Metal`, defined in `metal_flags.cpp`). Set them in the title's
config, on the command line (`--metal_retina=false`) or through the environment (`REX_METAL_RETINA=0`).

| Setting | Default | What it does |
| --- | --- | --- |
| `metal_title_quirks` | empty | Per-title workarounds, comma separated. Only `fable2` exists (see below). |
| `metal_native_shaders` | true | Optimize guest pixel shaders with metal-opt. Off also turns off everything below it. |
| `metal_native_xlate` | true | Translate guest shaders to MSL directly (native translator). |
| `metal_native_first` | true | Compile the native translation first instead of swapping it in later. |
| `metal_native_tess` | 1 | Tessellation: 0 emulation, 1 exact native, 2 Metal tessellator for all patches, 3 reference pattern. |
| `metal_native_points` | 2 | Point lists: 0 emulation, 1 per-corner expansion, 2 mesh function. |
| `metal_ns_specialize` | true | Specialize hot optimized pixel shaders by baking their constants. |
| `metal_ns_cache_toolchain` | empty | Toolchain hash of a bundled native-shader cache, for builds without metal-opt. |
| `metal_msc_min_target` | empty | Shader converter target: empty or `portable` (apple7:15.0), `native`, or e.g. `apple8:14.0`. |
| `metal_warmup_threads` | -1 | Pipeline warm-up threads at startup; -1 picks from the performance core count. |
| `metal_shader_prep` | empty | `1` or a `.json` status path: warm every shader and pipeline cache, save, exit. |
| `metal_shader_prep_timeout_s` | 1800 | Guard timeout for `metal_shader_prep`. |
| `metal_res_scale` | empty | Draw resolution scale: empty is native, 1 to 3, or `cvar` to follow `draw_resolution_scale_x/y`. |
| `metal_resolve_to_texture` | true | Resolve render targets straight into textures where possible. |
| `metal_tile_resolve` | true | Run colour resolves as tile functions inside the draw pass. |
| `metal_ssm_merge` | false | Merge stencil-only alias clears into the next draw pass (helps bandwidth-bound Macs). |
| `metal_occlusion_real` | true | Real occlusion query results; off reports a fixed sample count. |
| `metal_vsync_phase_lock` | true | Phase-lock presentation to the display refresh. |
| `metal_present_drawables` | 2 | CAMetalLayer drawable count: 2 (lower latency) or 3. |
| `metal_retina` | true | Render at the display backing scale. |

The old Fable II names (`FABLE2_RETINA=0` and so on) still work: when a cvar was not set any other way, its
`FABLE2_<NAME>` variable is applied at plugin load and a line in the log says so.

Debug, trace and tuning switches (about 200, for example `TILE_RESOLVE_VERIFY`, `RTT_TRACE`,
`VSYNC_LOCK_MARGIN_MS`) are not cvars. Each is read once from `REX_METAL_<NAME>`, falling back to the legacy
`FABLE2_<NAME>`, through `MetalDebugEnv()` in `metal_flags.cpp`.

### Title quirks

`metal_title_quirks=fable2` turns on Fable II-only behaviour, which is off for every other title:

- texture-space and shadow-map passes kept at native resolution (Fable II pixel shader hashes and shadow heuristic)
- built-in shader lists for native-shader specialization, its skip list and the keep-plain list
- the export-only memexport geometry path for Fable II's vertex shader
- morph texture readback (`metal_fable_morph_readback`, copy destination 0x12704000) and keeping the morph draw
- uniform terrain tessellation (`metal_fable_terrain_uniform_tessellation`)
- transfer pipeline prewarm defaults, the automatic shader bank path, and the "Loading Fable II" startup text

The matching debug switches (`SHADOWS_NATIVE`, `TEXSPACE_NATIVE`, `NS_SPECIALIZE_LIST`, ...) still override the
quirk default when set explicitly.

## Known limits

- The trace hooks still use Fable II's exported symbol names (`fable2_fr_api`, `fable2_zpd_*` and similar).
- No fallback for a shader the native translator cannot handle in this configuration.
