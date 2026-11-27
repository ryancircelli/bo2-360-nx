# sdk: ReXGlue SDK

The [ReXGlue SDK](https://github.com/rexglue/rexglue-sdk) release **v0.10.0** (commit `c94f5eb`) by Tom Clay, which
carries Xenia's implementation of the Xbox 360's kernel, system libraries and GPU, plus this project's changes. `README.upstream.md` is the release's README. Upstream code here
keeps its BSD 3-Clause license (`LICENSE`); this project's changes are GPL-3.0 under the top-level
[`LICENSE`](../LICENSE). See [`../THIRD_PARTY_NOTICES.md`](../THIRD_PARTY_NOTICES.md).

Against the release (line endings ignored): 96 of its files carry changes and some files are new (the Switch layer this
fork once carried was removed; tag `rejected/switch-support` keeps it). The release's tests, CI and developer scripts are left out. `thirdparty/` holds only the files this
project changed; `python tools/fetch_thirdparty.py` fetches the rest. To see every change:

```bash
git clone https://github.com/rexglue/rexglue-sdk.git rexglue-sdk && git -C rexglue-sdk checkout c94f5eb
git diff --no-index --ignore-cr-at-eol rexglue-sdk/src sdk/src
```


## Changes to the release's files

| Area | What changed |
|---|---|
| Code generator | Register options, the plain guest memory form, and a fix to `vpkuwus`/`vpkuhus`, which corrupted their result when the destination was also a source |
| Sync objects and threads | Handle lookups without the global lock; audio thread placement |
| Clock | A lock-free guest clock |
| Memory | A crash log on Windows |
| Files | A read cache, paced image pack reads, reports of the most-read files |
| Content and profiles | DLC listed and mounted as the game expects, XSession support so the solo lobby can start, profile settings, text input |
| Sockets | One receive buffer per thread; post-sleep network errors reported as "no data yet" |
| Audio | The decoded XMA frame cache; no per-frame FFmpeg side data |
| GPU | Hooks for the native renderer; changes to the PC's Vulkan back end |
| Presenter and UI | The output pass's FPS counter and credits line |
| Logging | The writer thread, the quiet player log, rotation |

In `thirdparty/`, only FFmpeg's files changed: a WMV3/VP8-only build configuration, the
codec list, and a cache of the FFT and MDCT tables.

## How it is built

Not on its own: the game build scripts point `REXSDK_DIR` here and CMake builds the SDK with
the app. `tools/build_host.sh` builds the host tools (`rexglue.exe`, `xexdump`). See
[`../docs/building.md`](../docs/building.md).
