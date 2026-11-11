# Mac app and test harness

Files in this branch (all new, nothing existing is edited):

| Path | Purpose |
|---|---|
| `tools/mac/pack_app.py` | Builds `Black Ops II.app` from the build output, shaders and data folder. |
| `tools/mac/app/launcher.c` | The app's `CFBundleExecutable`: finds the data, sets paths, starts `bo2zm`. |
| `tools/mac/boot_test.sh` | Launches the app muted and in the background, plays a route, collects stats and video. |
| `tools/mac/routes/*.route` | smoke, town_benchmark, town4, splitscreen4, teardown, signin_four, signin_gap, signin_rejoin. |
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

## Multiple local instances

`tools/mac/launch_instances.py K [--wait] [APP.app ...]` starts 2–4 native Mac app instances and assigns
`BO2_INSTANCE=0..K-1` plus `BO2_INSTANCE_COUNT=K`. Each process sizes and positions its own SDL window in a main-display
tile, without Accessibility access. The environment also enables in-game PNG captures in an instance-specific
directory under `/tmp/bo2/syslink/captures/<time>/instance-N/captures`. Each process has isolated user data, caches,
settings, logs, and network ports. Provide K app paths or set `BO2_MAC_APP` to the app bundle produced by
`tools/mac/pack_app.py`. For example:

    BO2_MAC_APP="$HOME/Games/Black Ops II.app" tools/mac/launch_instances.py 4
    tools/mac/launch_instances.py 2 "$HOME/Games/BO2 copy A.app" "$HOME/Games/BO2 copy B.app"

For scripted menu control, set `BO2_INSTANCE_BUTTONS_0`, `BO2_INSTANCE_BUTTONS_1` (and so on) to the standard
`bo2zm_test_buttons` time/button sequences; `_P2` through `_P4` suffixes address local split-screen users.

The launcher refuses to start if runq state cannot be read or a measured FPS job is active. It reads running-job
records under `~/Games/Xbox360/bo2-build/runq/running`; queued proof work does not block launch. Use `--wait` when
running it as a runq job so that runq continues to track the copies until they exit. Run the job in a non-measured
class and wait for measured FPS jobs to finish before starting an interactive System Link session.

## Testing

    tools/mac/boot_test.sh --app "Black Ops II.app" --route town_benchmark [--baseline old/perf.tsv] [--reference ref.mp4] [--video]

Exit 0 pass, 1 regression or log-check failure, 2 setup error, 3 crash or no stats.
Every route gets `BO2ZM_TEST_EXIT_ON_ERROR=true`; an in-game error, a `[relaunch] restarting` line (unless the route
says `allow_relaunch = true`) or an error pattern in the log (unregistered function, MISMATCH, lost wake, ...) fails the
run. Pads: by default the SDK's scripted pads run the route (`REX_HID_VIRTUAL_PAD_SCRIPT`, SDK branch mac-input) and
`BO2ZM_TEST_BUTTONS*` are not exported; `--pads game` uses the game's own test pads. Missing perf log or video is
printed as NOT MEASURED. Routes: smoke, town_benchmark, town4, splitscreen4, teardown (pause menu recorded on the real game), signin_four,
signin_gap, signin_rejoin (join tests from research/splitscreen-signin.md; a STUB line for XamUserGetAgeGroup or
XamUserGetMembershipTier fails them).

Timing notes recorded on the real game: players 2 to 4 join the Local lobby with A (Start does not); the script clock
starts with the process, and the lobby opens at about 15 s unloaded and 17 to 40 s on a busy Mac, so joins are set
at 44 to 48 s and `BO2ZM_TEST_START_WAIT_S` to 55 s. The pause menu is RESUME GAME, RESTART LEVEL, OPTIONS, EXTRAS,
END GAME (4 downs); the END GAME confirm defaults to No (press up, then A); the game then returns to the Local lobby.
Every run uses `--net_system_port_offset` (`--port-offset N`, default 42000) so parallel instances do not collide on
UDP 31000/31001. Known defects of the current build (signal delivery failed, shaders not in the library, primitive
restart) are counted and printed but fail a run only with `--strict`. Game captures (`captures/`, every
`CAPTURE_EVERY_S`, default 15 s) are copied into the run directory: macOS blocks `screencapture` of the game window.
Under load, run through `tools/mac/bench_quiet.sh` (mac-port) and quote the load average with any FPS.
`tools/mac/test_harness.sh` self-tests the harness with fake games (pass, crash, error exit, relaunch, pad env). Output in `/tmp/bo2-boot-test/<route>-<time>/`.

## What the build agent must hook in

Existing cvars already cover input and the benchmark (`bo2zm_test_start`, `bo2zm_test_buttons[_p2.._p4]`,
`bo2zm_test_god`, `bo2zm_benchmark`). Done:

* `bo2zm_perf_log` (env `REX_BO2ZM_PERF_LOG`, `app/src/bo2zm_perf_log.cpp`): the log in `perf-log-format.md`, from
  the native renderer. `segment` and `route_end` come from the benchmark route (`bo2zm_benchmark`); the `route` and
  `players` header lines from `BO2ZM_ROUTE` and `BO2ZM_PLAYERS`, which `boot_test.sh` exports. Without the cvar the
  harness falls back to the `[benchmark]` line (average, median, p99 only).
* `window_start_minimized` (env `REX_WINDOW_START_MINIMIZED`, SDK `src/ui/window_sdl.cpp`): the window starts
  minimized, windowed, and without activating the app.

Missing, in priority order:

1. `bo2zm_video_rgba` (env `REX_BO2ZM_VIDEO_RGBA`): record presented frames as raw rgba plus `<name>.txt`
   (`w h fps count`), as peggle1-nx does with `REX_PEGGLE1_VIDEO_*`. Needed for `--video` and `--reference`.
2. `bo2zm_pso_record`, `bo2zm_pso_verify`, loading of `bo2zm_pso.psolist` (`pso-list-format.md`).
3. Route timings: `town_benchmark.route` button timings and the whole split screen route need real values from a run.

## Credits

`pack_app.py` borrows its clone copy, icon, signing and atomic swap logic from switch2mac
(github.com/azmasm/switch2mac, MIT, Copyright (c) 2026 seedds); the harness follows peggle1-nx's boot test, video
and compare tools.
