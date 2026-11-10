# Pipeline state list (PSO list), format version 1

The file that lets the game prepare every pipeline it will need before the first frame, so no frame waits for a
compile. The owner's rule: zero compiles in the middle of the game. This page defines the file; recording it and
loading it are the renderer's job.

It follows the plan in `~/dev/bo2-mac-port/research/metal-renderer-plan.md` (section on compile stalls): shader and
pipeline compiles never stall a frame, a draw whose pipeline is not ready is skipped for that frame, and an
on-disk pipeline cache is warmed before the next launch's first frame. The list is the input to that warm-up.

## Why a list and not only a cache

A Metal binary archive or a Vulkan pipeline cache holds compiled code but not the question "which pipelines does
this game use". The list answers that question, independently of the GPU and OS version: it can be recorded on one
Mac, shipped, and compiled on another (after a macOS update the archive is stale but the list is still right). It
also makes a regression test possible: `boot_test.sh` replays the benchmark route and fails if any pipeline is
created that the list did not contain.

## Files

| File | Written by | Contents |
|---|---|---|
| `bo2zm_pso.psolist` | the game, in record mode; or `tools/mac/pso_list.py merge` | the list (below) |
| `bo2zm_pso.mtlarchive` | the prewarm step | the compiled pipelines for this GPU and OS (a Metal binary archive; for Vulkan a `VkPipelineCache` blob named `bo2zm_pso.vkcache`) |

The list ships with the game data (it holds hashes and state, no game code or assets); the archive is made on the
player's Mac at install time or first launch and stored under `~/Library/Caches/bo2-360-nx/`.

## Recording

* `--bo2zm_pso_record=<path>` (env `REX_BO2ZM_PSO_RECORD`): every pipeline created during the run is appended to
  `<path>` the first time its key is seen. Off by default.
* Record from the routes in `tools/mac/routes/` (smoke, town benchmark, split screen), several rounds and maps,
  then merge: `python3 tools/mac/pso_list.py merge a.psolist b.psolist -o bo2zm_pso.psolist` (de-duplicates by key,
  keeps the first-seen order, sums `uses`).
* `--bo2zm_pso_verify` makes the game log an error line `[pso] unlisted pipeline <key>` for every pipeline created
  after loading that is not in the loaded list, and write a `compile` event to the perf log
  (`perf-log-format.md`).

## Loading

At start, after the shader library index is read and before the first frame, the renderer reads
`bo2zm_pso.psolist`, creates every listed pipeline on worker threads (`min(cores - 2, 6)`), and writes the result
to the archive. The title screen is shown meanwhile and the game does not leave the loading screen before the
queue is empty (a progress line in the log: `[pso] prewarm 812/3400`). With a valid archive the work is a lookup and
takes a few seconds; without one it is a full compile and is the reason for a first-launch "preparing shaders" step.

## Layout of the list

Plain text, UTF-8, LF line ends, one pipeline per line. It stays diffable and mergeable with ordinary tools.

```
# bo2pso 1
# build	3f9c2ab
# renderer	native-metal
# shaderpack	bo2zm_shaders.spvpack	<sha256 of the pack's index>
G	<key>	<vs>	<ps>	<topology>	<state>	<uses>
C	<key>	<cs>	<uses>
```

1. The first line is exactly `# bo2pso 1`. Header lines `# key<TAB>value` follow. `shaderpack` ties the list to
   the shader library it was recorded against: if the hash differs, the loader ignores the list and logs why
   (shader indexes changed, so keys mean something else), and the pipelines are compiled on demand as today.
2. `G` is a graphics pipeline, `C` a compute pipeline. Fields are tab-separated.
3. `key` is a 16-digit lowercase hex 64-bit hash (XXH3) of all other fields of the line except `uses`. It is the
   identity: two lines with the same key are the same pipeline.
4. `vs`, `ps`, `cs` are shader indexes into the shader library (`bo2zm_shaders.spvidx`): the 16-digit hex content
   hash of the entry, not its position, so a rebuilt library with the same shaders keeps its list. `-` for none
   (a depth-only draw has no pixel shader).
5. `topology` is the Xenos primitive type name lower-cased as the renderer sees it after conversion: `triangle_list`,
   `triangle_strip`, `line_list`, `line_strip`, `point_list`, `rect_list`. Anything a backend rewrites (a rect
   list becomes a triangle strip) is recorded as the original name; the renderer applies the rewrite when it
   builds the pipeline.
6. `state` is a comma-separated list of `name=value` that make the pipeline different, in this fixed order (all
   present, no omissions, so lines are comparable):

   | Name | Values |
   |---|---|
   | `fmt` | colour attachment formats `r0.r1.r2.r3` (`-` unused), as `rgba8`, `bgra8`, `rgba16f`, `rg11b10f`, `r32f`, ... |
   | `zfmt` | `d24s8`, `d32f`, `d32fs8`, `-` |
   | `samples` | `1`, `2`, `4` |
   | `blend` | per attachment `src.dst.op.asrc.adst.aop.mask` as hex digits, attachments joined with `/` |
   | `depth` | `test.write.func` (`0.0.0` off) |
   | `stencil` | `en.func.fail.zfail.pass.rmask.wmask.ref` for front, `/`, back, or `0` |
   | `cull` | `none`, `front`, `back` |
   | `front` | `cw`, `ccw` |
   | `fill` | `solid`, `line`, `point` |
   | `bias` | `0` or `slope.const` as hex floats |
   | `vtx` | vertex layout hash, 16 hex digits (of the fetch constants' formats and strides, not the addresses) |
   | `alpha2c` | `0` or `1` alpha to coverage |

   Values must not depend on addresses, resource handles or frame state, only on what MSL/SPIR-V pipeline creation
   needs. A backend may carry more state in a trailing `x:<name>=<value>` list; the loader passes unknown names
   through and the verifier treats them as part of the key.
7. `uses` is how often the pipeline was seen in the recording (decimal). The loader compiles in descending `uses`
   order, so the pipelines needed first finish first. The merge tool sums it.
8. `# end` line with `pipelines=N` closes a complete file. A loader that does not find it uses what it read and
   warns.

## Metal-specific notes

* Use `MTLBinaryArchive` (`addRenderPipelineFunctionsWithDescriptor`, `serializeToURL`) written once after warm-up,
  and give the archive to every `MTLRenderPipelineDescriptor.binaryArchives` so later creations hit it. Tie the
  archive's file name to the GPU registry id, the macOS build and the list's hash, so a stale archive is replaced
  and not trusted.
* Shaders for the Metal renderer reach the device as a metallib built from the `.spvpack` entries. A list line names
  shader entries, so the prewarm can load only the functions it needs instead of the whole library.
* On the generic Xenos path (`rexgpu-metal`, option A in the plan) the same list works with a different key space:
  the Xenos microcode hash replaces the shader index, and the plugin writes `# renderer	xenos-metal`. The loader refuses
  a list whose `renderer` differs from the running one.

## The native Vulkan renderer (MoltenVK), as built

The native renderer already had its own binary list and driver cache in one file (`bo2zm_native_pipelines.bin`, the
list plus `vkGetPipelineCacheData`) and a precompile that runs behind a progress screen before the game starts when
the build, driver or shader pack changed (`app/src/native/render/bo2zm_native_precompile.h`). The text list sits on
top of that:

* `--bo2zm_pso_record=<path>` or `REX_BO2ZM_PSO_RECORD` writes every pipeline the session creates, once, flushed
  line by line, so a run the harness stops still leaves a usable list (only the `# end` line is missing then).
* `bo2zm_pso.psolist` beside the executable, in `Contents/Resources/` or `Contents/Resources/runtime/` (or
  `--bo2zm_pso_list=<path>`) is read at start-up. Its pipelines join the binary list, so the precompile builds them
  before the first match. Its name and size are part of the precompile stamp, so a new list triggers a pass.
* `--bo2zm_pso_verify` or `REX_BO2ZM_PSO_VERIFY=1` logs `[pso] unlisted pipeline <key> (<ms> ms)` and writes a
  `compile` perf log event with `unlisted=1`.
* `renderer` is `native-vulkan`. `shaderpack` holds the shader library's 16-hex identity (the same value the
  pipelines file's stamp uses), not a sha256: the loader checks each pipeline's shader content hashes anyway, so a
  line whose shaders left the pack is skipped one by one.
* Each `G` line's state ends in backend extras: `x:spec`, `x:restart` and `x:rec=<hex>`, the renderer's own pipeline
  record (key, shader hashes, vertex attributes and strides). The loader rebuilds the pipeline from `x:rec`; the
  standard fields are there for `tools/mac/pso_list.py diff` and for other backends. A record from another build of
  the renderer (other size) is ignored.
* The pipeline cache goes to `cache/` beside the executable, or to `~/Library/Caches/bo2-360-nx/` when the game runs
  from an `.app` bundle (`--bo2zm_native_cache_dir` overrides both).
* MoltenVK's `VkPipelineCache` only holds the SPIR-V to MSL translation; the Metal pipeline state is made again every
  time a pipeline is created (about 1 to 10 ms each, from Metal's own shader cache when it is warm). So on macOS the
  precompile keeps the pipelines it creates and hands them to the ring (`bo2zm_native_precompile_keep`, on by
  default on macOS), and runs on every start, not only when the stamp changed; with warm caches the pass is short.
  While recording a list the pipelines are not kept, so the record still sees what the session asks for.

## Open questions for the renderer author

1. Whether the native renderer creates pipelines lazily at draw time (then recording is a hook in that function) or
   in bulk per material (then the list is smaller and can be derived offline from the shader library).
2. Whether the key should include the render pass compatibility (load/store actions do not affect Metal pipeline
   creation, so the list leaves them out).
3. Where `bo2zm_pso.psolist` is stored for a packaged app: the plan is `Contents/Resources/` for the shipped list
   and `~/Library/Caches/bo2-360-nx/` for the archive.
