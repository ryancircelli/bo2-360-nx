# Campaign (bo2cp)

The campaign lives in `default.xex`, a separate program from `default_mp.xex` (multiplayer and Zombies). This folder
recompiles the TU18 `default.xex` as `bo2cp`, for Windows and the Switch, with the Zombies program's native renderer and
native code built in from `games/bo2/src` and `core`.

```
REXGLUE_BIN=sdk/out/win-amd64 bash campaign/codegen.sh                # code generator + fix-ups
python -I campaign/build_shaders.py                                   # bo2cp_shaders.spvpack from your game files
bash campaign/build_windows.sh                                        # campaign/out/win/bo2cp.exe
bash campaign/build_switch.sh                                         # campaign/out/switch/bo2cp.nro
bash campaign/run_windows.sh 60 --bo2zm_renderer=native               # one PC run, stopped after 60 s
```

How the Zombies code builds here unchanged:

- `src/native/bo2cp_game_remap.h` is forced into those files and renames each default_mp function they hook to the
  campaign's function that does the same job.
- Numeric addresses are written `BO2_GAME_ADDR(zombies, campaign)` (`games/bo2/src/bo2zm_game.h`).
- `src/native/bo2cp_native_stubs.cpp` stands in for the Zombies-only systems (cheats, presets, credits, the benchmark).

On the Switch, `bo2cp.nro`, `bo2cp.toml` and `bo2cp_shaders.spvpack` go next to `bo2zm.nro` in `/switch/bo2zm`. Back on
the Zombies home menu opens the game's Campaign / Multiplayer choice; Campaign starts `bo2cp.nro`, and the campaign's
Zombies choice starts `bo2zm.nro` again. Both programs share the game files, the profile and the saves.

Test helpers (all off by default): `bo2cp_autoexec` and `bo2cp_autoexec2` (console commands after a delay, `{map}`
taking the next mission of `bo2cp_test_maps`), `bo2cp_watchdog_s`, `bo2cp_test_seconds` and `bo2cp_restart_on_crash`
(restart the application for unattended loops), `bo2cp_sv_cheats`.
