# Third-party notices

bo2-zm-nx (Black Ops II Zombies for the Nintendo Switch) is licensed under the GNU General Public License v3.0
(`LICENSE`). That license covers everything this project wrote: `app/`, its changes in `sdk/`, `tools/`, the Mesa patch
and the documentation. The port is built on the work of the projects and libraries below. Their code keeps its own
copyright and license notices, exactly as they are, and every one of those licenses is compatible with GPL-3.0.

## Projects this port is derived from

| Project | Used for | License |
|---|---|---|
| [ReXGlue SDK](https://github.com/rexglue/rexglue-sdk) v0.10.0 (commit `c94f5eb`) by Tom Clay, with portions from [Xenia](https://xenia.jp) (Ben Vanik and the Xenia contributors) | `sdk/`: code generator, Xbox 360 kernel and runtime, GPU command processing. See `sdk/README.md` for which files are the official release and which are changed | BSD-3-Clause (`sdk/LICENSE`) |
| [Fable II Recomp for macOS](https://github.com/andrew17182/Fable-2-Recomp-macOS) by andrew17182 (commit `6f5459e`), adapting the [XeniOS](https://github.com/xenios-jp/XeniOS) Metal backend (commit `87b176a`) | `sdk/src/graphics/metal/`: the optional native Metal GPU plugin for macOS (`REXGLUE_USE_METAL`, off by default). Its `licenses/` folder holds every notice; `provenance.json` records the sources | MIT (Fable II), BSD-3-Clause (XeniOS, Xenia), MIT (D3D11 tessellator) |
| [XenosRecomp](https://github.com/hedge-dev/XenosRecomp) by hedge-dev and contributors | `tools/xenosrecomp/translator/`: Xenos microcode to HLSL translator | MIT (`tools/xenosrecomp/translator/LICENSE.md`) |
| [mesa-switch](https://github.com/danfromtico/mesa-switch) by danfromtico (commit `1a8c1a66d6f`), on top of [Mesa](https://mesa3d.org) | The Vulkan driver (NVK) and shader compiler (NAK) for Horizon, linked into the NRO. The official base is fetched by `tools/build_mesa.sh`; this project's changes are `patches/mesa-bo2zm.patch` | MIT (Mesa's licenses per file) |

Parts of `app/` and `tools/` are derived from GPL-3.0 code, so GPL-3.0 would apply to them in any case; each such file
says so in its header. The upstream code in this repository keeps its own license: the ReXGlue SDK with its Xenia
portions and the Switch layer in `sdk/` under BSD-3-Clause (`sdk/LICENSE`), the translator under MIT
(`tools/xenosrecomp/translator/LICENSE.md`), and Mesa with the changes to it under its per-file licenses (MIT).

## Libraries

Libraries used by the SDK, the NRO and the code generator, fetched from the submodules of ReXGlue SDK v0.10.0
(`tools/fetch_thirdparty.py`):

| Library | License |
|---|---|
| [FFmpeg](https://ffmpeg.org) (libavcodec, libavutil: the WMV3 decoder for movies, the VP8 decoder for the loading videos), from `wmarti/FFmpeg` at `0604b464c7cb` | LGPL-2.1-or-later (configured without GPL parts) |
| [libmspack](https://www.cabextract.org.uk/libmspack/) by Stuart Caie (LZX decompression) | LGPL-2.1 |
| [glslang](https://github.com/KhronosGroup/glslang) | BSD-3-Clause and others (see its LICENSE.txt) |
| [SPIRV-Tools](https://github.com/KhronosGroup/SPIRV-Tools) | Apache-2.0 |
| [SPIRV-Headers](https://github.com/KhronosGroup/SPIRV-Headers), [Vulkan-Headers](https://github.com/KhronosGroup/Vulkan-Headers) | MIT, Apache-2.0 |
| [Vulkan Memory Allocator](https://github.com/GPUOpen-LibrariesAndSDKs/VulkanMemoryAllocator) | MIT |
| [fmt](https://github.com/fmtlib/fmt), [spdlog](https://github.com/gabime/spdlog) | MIT |
| [xxHash](https://github.com/Cyan4973/xxHash) | BSD-2-Clause |
| [SIMDe](https://github.com/simd-everywhere/simde) | MIT |
| [toml++](https://github.com/marzer/tomlplusplus) | MIT |
| [Dear ImGui](https://github.com/ocornut/imgui) | MIT |
| [o1heap](https://github.com/pavel-kirienko/o1heap) | MIT |
| [utfcpp](https://github.com/nemtrif/utfcpp) | BSL-1.0 |
| [CLI11](https://github.com/CLIUtils/CLI11) | BSD-3-Clause |
| AES-128 by LuoPeng (`thirdparty/aes_128`) | MIT |
| [zstd](https://github.com/facebook/zstd) (`tools/build_zstd_switch.sh`) | BSD-3-Clause |
| [libnx](https://github.com/switchbrew/libnx) and the devkitA64 runtime (newlib, libstdc++) | ISC; newlib and GCC runtime licenses |

### FFmpeg and libmspack (LGPL)

These files of the SDK's third-party tree are modified; the modified copies are in `sdk/thirdparty`:

- `FFmpeg/libavcodec/fft_template.c` and `FFmpeg/libavcodec/mdct_template.c`: cache of the permutation and cosine tables
- `FFmpeg/config.h`, plus `FFmpeg/config_switch_aarch64.h`, `FFmpeg/config_bo2zm_wmv3.h` and `FFmpeg/config_bo2zm_vp8.h`
  (new): the configuration for AArch64 Horizon and for a build limited to the WMV3 and VP8 decoders
- `ffmpeg-overlay/codec_list.c`: registers the WMV3 and VP8 decoders
- `CMakeLists.txt`: builds the libraries for the Switch

The rest of FFmpeg and libmspack is the unmodified upstream source at the commits pinned by ReXGlue SDK v0.10.0.

## Artwork in the program

The home screen's credits line (`app/src/ui/credits`, drawn by `tools/make_credits.py`; details in its README):

| Part | Source | License / terms |
|---|---|---|
| GitHub's mark | The `mark-github` path of [Octicons](https://github.com/primer/octicons) by GitHub | MIT License (Octicons). The mark is GitHub's trademark, used as [GitHub's logo guidelines](https://github.com/logos) allow (pointing to a GitHub profile, unmodified) |
| Roboto Medium (the text's glyphs) | Google, as Dear ImGui carries it in `misc/fonts` | Apache License 2.0 |

## Build-time tools (not shipped)

The shader library is built on the user's machine from their own game data with the translator above and
[DirectXShaderCompiler](https://github.com/microsoft/DirectXShaderCompiler) from the Vulkan SDK (University of
Illinois/NCSA Open Source License, with LLVM's license terms).

## Trademarks

Call of Duty and Call of Duty: Black Ops II are trademarks of Activision Publishing, Inc. Nintendo Switch is a
trademark of Nintendo. Xbox 360 is a trademark of Microsoft. GitHub and the GitHub mark are trademarks of GitHub, Inc. This project is not affiliated with or endorsed by any of them.
