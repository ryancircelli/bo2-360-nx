# Mac app and test harness

Files in this branch (all new, nothing existing is edited):

| Path | Purpose |
|---|---|
| `tools/mac/pack_app.py` | Builds `Black Ops II.app` from the build output, shaders and data folder. |
| `tools/mac/app/launcher.c` | The app's `CFBundleExecutable`: finds the data, sets paths, starts `bo2zm`. |
| `tools/mac/boot_test.sh` | Launches the app muted and in the background, plays a route, collects stats and video. |
| `tools/mac/routes/*.route` | smoke, town_benchmark, splitscreen4 (placeholder). |
| `tools/mac/perf_summary.py` | Frame-time statistics and regression check from a perf log. |
| `tools/mac/make_video.sh`, `compare_video.py` | Raw capture to mp4, and frame comparison against a reference. |
| `tools/mac/pso_list.py` | Merge and check pipeline lists. |
| `docs/mac/perf-log-format.md`, `pso-list-format.md` | The two file formats the renderer should write and read. |

## Packing

    python3 -I tools/mac/pack_app.py --build $BO2_BUILD/out/mac-arm64 --shaders <dir with bo2zm_shaders.spvpack> \
        --data <data dir with game/ update/ content/> [--data-mode external --data-path DIR]

Layout: real files in `Contents/MacOS` (launcher, `bo2zm`, `librex*.dylib`), non-system libraries in
`Contents/Frameworks`, and the shader library, `bo2zm.toml` and `vulkan/` in `Contents/Resources/runtime`, linked
from `Contents/MacOS` (codesign refuses plain data files there). Rpaths are `@loader_path` and
`@executable_path/../Frameworks` only. The app is ad-hoc signed. Data is APFS-cloned into `Resources/data`, or
read from an external path (`data_path.txt` in Application Support, `BO2_DATA`, or `BO2DataPath` in Info.plist).
Saves go to `~/Library/Application Support/bo2-360-nx`, logs to `~/Library/Logs/bo2-360-nx` (newest 20 kept).
Extra command-line options for every launch go in `Application Support/bo2-360-nx/args.txt` (`name = value`).
Tested only on a dummy binary so far.

## Testing

    tools/mac/boot_test.sh --app "Black Ops II.app" --route town_benchmark [--baseline old/perf.tsv] [--reference ref.mp4] [--video]

Exit 0 pass, 1 regression or log-check failure, 2 setup error, 3 crash or no stats.
Every route gets `BO2ZM_TEST_EXIT_ON_ERROR=true`; an in-game error, a `[relaunch] restarting` line (unless the route
says `allow_relaunch = true`) or an error pattern in the log (unregistered function, MISMATCH, lost wake, ...) fails the
run. Pads: by default the SDK's scripted pads run the route (`REX_HID_VIRTUAL_PAD_SCRIPT`, SDK branch mac-input) and
`BO2ZM_TEST_BUTTONS*` are not exported; `--pads game` uses the game's own test pads. Missing perf log or video is
printed as NOT MEASURED. Routes: smoke, town_benchmark, town4, splitscreen4, teardown (menu sequence is a guess).
`tools/mac/test_harness.sh` self-tests the harness with fake games (pass, crash, error exit, relaunch, pad env). Output in `/tmp/bo2-boot-test/<route>-<time>/`.

## What the build agent must hook in

Existing cvars already cover input and the benchmark (`bo2zm_test_start`, `bo2zm_test_buttons[_p2.._p4]`,
`bo2zm_test_god`, `bo2zm_benchmark`). Missing, in priority order:

1. `bo2zm_perf_log` (env `REX_BO2ZM_PERF_LOG`): write the log in `perf-log-format.md`. Without it the harness falls
   back to the `[benchmark]` line (average, median, p99 only).
2. `window_start_minimized` (env `REX_WINDOW_START_MINIMIZED`): start the window minimized; also honour
   `SDL_WINDOW_ACTIVATE_WHEN_SHOWN=0`. Until then the window may appear in front.
3. `bo2zm_video_rgba` (env `REX_BO2ZM_VIDEO_RGBA`): record presented frames as raw rgba plus `<name>.txt`
   (`w h fps count`), as peggle1-nx does with `REX_PEGGLE1_VIDEO_*`. Needed for `--video` and `--reference`.
4. `bo2zm_pso_record`, `bo2zm_pso_verify`, loading of `bo2zm_pso.psolist` (`pso-list-format.md`).
5. Route timings: `town_benchmark.route` button timings and the whole split screen route need real values from a run.

## Credits

`pack_app.py` borrows its clone copy, icon, signing and atomic swap logic from switch2mac
(github.com/azmasm/switch2mac, MIT, Copyright (c) 2026 seedds); the harness follows peggle1-nx's boot test, video
and compare tools.
