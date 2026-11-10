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

## Known limits

- Some code paths are tuned for Fable II and key on its exact shader hashes and addresses; they stay inactive for
  other titles but should become per-title hooks.
- Runtime switches still use Fable II's `FABLE2_*` environment variable names (see `metal_flags.h`).
- No fallback for a shader the native translator cannot handle in this configuration.
