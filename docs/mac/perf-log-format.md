# Performance log format (version 1)

What the game writes during a test run and what `tools/mac/perf_summary.py` reads. One format for every run on the
Mac, so two runs (two builds, two renderers, a Metal change before and after) can be compared by a tool instead of
by eye. The build emits it; this repository only reads it.

## Turning it on

| Switch | Meaning |
|---|---|
| `--bo2zm_perf_log=<path>` (env `REX_BO2ZM_PERF_LOG`) | Write the log to this file. Empty (the default) writes nothing and costs nothing. |

`tools/mac/boot_test.sh` sets it for every run. The file is created at the first presented frame, flushed once a
second, and closed on a normal exit. A run that is killed leaves a file that lacks the `# end` line; the reader
accepts that and says so.

## File layout

Plain text, tab-separated, one frame per line. Lines starting with `#` are header, event or footer lines.

```
# bo2perf 1
# build	3f9c2ab+dirty
# renderer	native-metal
# gpu	Apple M4 Pro
# os	macOS 27.2
# resolution	1920x1080
# players	1
# route	town_benchmark
frame	t_ms	frame_ms	cpu_ms	gpu_ms	present_wait_ms	compile_ms	draws
1	8123.402	-	11.8	9.1	0.4	0	1432
2	8140.118	16.716	12.2	9.4	0.3	0	1440
#event	8123.402	loading_done
#event	9001.500	segment	name=walk_left
...
# end	frames=5400	duration_ms=180001.2
```

1. The first line is exactly `# bo2perf 1`. A reader that sees another version stops with an error.
2. `# key<TAB>value` header lines come before the column line. Unknown keys are kept and ignored. Keys the tools
   print when present: `build`, `renderer`, `gpu`, `os`, `resolution`, `players`, `route`.
3. The column line is the first line that does not start with `#`. Columns are looked up by name, so the build may
   add columns at the end and may leave out the optional ones. A value that was not measured is `-`.
4. `#event<TAB>t_ms<TAB>name[<TAB>key=value ...]` lines may appear anywhere, in time order. They are not frames.
5. The last line is `# end<TAB>frames=N<TAB>duration_ms=D`.

## Columns

All times are milliseconds as a decimal number with a dot. `frame` and `t_ms` are required; the rest are required
for the statistics they feed and optional otherwise.

| Column | Required | Definition |
|---|---|---|
| `frame` | yes | Index of the presented game frame (one per `D3DDevice_Swap`, or per Metal `presentDrawable`). Starts at 1. A gap means a frame was dropped by the presenter. |
| `t_ms` | yes | Time of the present call, milliseconds since the process started, from a monotonic clock (`mach_continuous_time` or `std::chrono::steady_clock`). |
| `frame_ms` | yes | Interval to the previous present (`t_ms` minus the previous `t_ms`). `-` on the first frame. This is the number frame rate statistics use. |
| `cpu_ms` | yes | CPU time the frame's work took on the thread that submits it, from the start of the guest frame to the end of command encoding, with blocking waits (`present_wait_ms`, synchronous compile) subtracted. Wall time of that thread, not summed over threads. |
| `gpu_ms` | yes | GPU execution time of the frame: the sum over its command buffers of `GPUEndTime - GPUStartTime` on Metal (read in the completion handler), or the span of the timestamp queries on Vulkan/MoltenVK. It belongs to the frame whose commands it ran, and so may be written one or two frames late; the writer holds a frame back until it is known rather than writing `-`. |
| `present_wait_ms` | yes | Time the submit thread was blocked waiting for something outside its control: the next drawable, the vsync, or a frame-in-flight fence. |
| `compile_ms` | recommended | Time spent compiling a shader or pipeline synchronously inside this frame. The goal of the pipeline prewarm list (`pso-list-format.md`) is that this is `0` in every frame after loading. |
| `draws` | optional | Draw calls issued. |
| `rt_switches` | optional | Render target (pass) changes. |
| `mem_mb` | optional | Resident memory of the process. |

A frame is `bound` by whichever is largest of `cpu_ms`, `gpu_ms` (the tools report the shares) when `frame_ms` is
above the display interval. A run in which `present_wait_ms` is close to `frame_ms` is limited by vsync, not by the
game; compare runs only with the same frame cap.

## Events

Names the tools know. The build may add others; they are listed in the summary and otherwise ignored.

| Event | Extra fields | Meaning |
|---|---|---|
| `loading_done` | | The map finished loading; statistics skip everything before it (use `--skip-s` to override). |
| `segment` | `name=<text>` | Starts a named part of a route. The summary gives statistics per segment as well as for the whole run. A segment lasts until the next `segment` event or the end. |
| `compile` | `kind=pso\|shader`, `ms=<n>`, `key=<hash>` | A synchronous compile happened. One line per compile; `compile_ms` carries the per-frame total. |
| `hitch` | `ms=<n>` | The writer saw a frame over 100 ms. |
| `route_end` | | The route finished; statistics stop here. |

## Statistics

`tools/mac/perf_summary.py` reads a log and prints, over the window from `loading_done` (or `--skip-s`) to
`route_end` (or the last frame):

* frames, duration, average FPS (frames divided by duration)
* frame time: average, median, p90, p99, p99.9, worst
* **1 % low FPS**: 1000 divided by the mean `frame_ms` of the slowest 1 % of frames (not the 99th percentile
  converted, which reads higher); **0.1 % low** likewise
* share of frames over 16.7, 33.3 and 50 ms
* `cpu_ms`, `gpu_ms`, `present_wait_ms`: average, p99, worst
* synchronous compiles: count of frames with `compile_ms > 0` and their total
* the same for each `segment`

`perf_summary.py` also reads the one-line `[benchmark]` summary the existing `bo2zm_benchmark` writes into the game
log (average FPS, median, p90, p99, worst, share over 33.3 and 50 ms), so old runs and Windows runs stay
comparable on those numbers; it cannot give 1 % low, CPU, GPU or present wait from that line.

## Comparing two runs

`perf_summary.py a.tsv --baseline b.tsv` prints both and the change in each number, and exits with status 1 when
the average FPS, 1 % low or p99 is worse than the baseline by more than `--tolerance` percent (default 5, which
is above the 1 to 2 % run-to-run noise the Switch notes measured), or when `compile` frames appear that the
baseline did not have.

## What the build has to do

1. Define `bo2zm_perf_log` and write the file as above from the place that already counts frames
   (`bo2zm::benchmark::AnnotateFrame`, called once per presented frame).
2. Measure `cpu_ms`, `present_wait_ms` around the existing present/acquire call, and `gpu_ms` from the command
   buffer completion handler (Metal) or timestamp queries (Vulkan).
3. Emit `loading_done` when the match is loaded, and `segment` events from the benchmark route (each
   30-second turn is one).
4. Emit `compile` events and `compile_ms` from every synchronous pipeline or shader creation. Together with the
   pipeline list (`pso-list-format.md`) this tells the owner's "zero mid-game compiles" goal as a number.
