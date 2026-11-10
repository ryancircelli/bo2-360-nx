# rex-padtest

Shows each controller, the player it gets in a game, and what it is pressing,
using the same input code a game uses. No game or window is needed.

## Build

```sh
cmake --preset mac-arm64
cmake --build out/build/mac-arm64 --config Release --target rex-padtest
out/mac-arm64/Release/rex-padtest
```

The tool builds by default when the SDK is the top-level project
(`REXGLUE_BUILD_PADTEST`).

## Use

| Command | What it does |
| --- | --- |
| `rex-padtest` | live table; Ctrl+C quits |
| `rex-padtest --list` | print the controllers once and exit |
| `rex-padtest --plain` | print each change as a line, with the log |
| `rex-padtest --seconds=30` | stop after 30 seconds |
| `rex-padtest --no-sdl` | scripted pads only |

Each row shows the player (P1 to P4), name, USB vendor:product id, link (usb or
bluetooth), whether it can rumble, its serial and SDL GUID, and live buttons,
triggers and sticks. When a controller connects it buzzes once (left motor for
players 1 and 3, right motor for 2 and 4), and pulling a trigger drives that
motor, so you can tell which physical pad is which player.

## How players are chosen

* Pads get players 1 to 4 in the order they connect.
* A pad that disconnects keeps its player while it is away. When it reconnects
  it gets the same player back, and nobody else moves. If a new pad arrives
  while all other players are taken, it may take the waiting player.
* Pads are recognised by serial number when the OS reports one, otherwise by
  SDL GUID (two identical pads share a GUID, so those fall back to connection
  order).
* Virtual pads (Steam Input's virtual controllers, SDL virtual joysticks) are
  never player 1 unless pinned there.

## Settings

All are cvars: pass `--name=value` to the game or this tool, put them in the
game's config, or set `REX_<NAME>` in the environment.

| Setting | Default | Meaning |
| --- | --- | --- |
| `input_player_slots` | empty | pin pads to players, e.g. `1=<serial>;2=054c:0ce6;3=Xbox Wireless`. A match is a serial, an SDL GUID, a `vendor:product` id, or part of the name. |
| `input_virtual_pads` | `after` | `after`: virtual pads never take player 1; `normal`: treat them like real pads; `ignore`: hide them |
| `input_remember_slots` | `true` | keep a disconnected pad's player for it |
| `input_log_state` | `false` | log every pad's player and state every 0.5 s |
| `mnk_player` | `0` | give keyboard and mouse its own player (1 to 4); 0 shares player 1 |
| `vibration` | `true` | rumble on or off |
| `hid_mappings_file` | `gamecontrollerdb.txt` | extra SDL controller mappings |
| `hid_virtual_pads` | `0` | number of scripted pads (0 to 4), players 1 up |
| `hid_virtual_pad1` to `4` | empty | script for each scripted pad |
| `hid_virtual_pad_script` | empty | file with the scripts (see below) |
| `hid_virtual_pad_look_speed` | `35` | right stick tilt for `look_*`, percent |

## Steam

When Steam is running with Steam Input on for Xbox, PlayStation, Switch or
generic pads, Steam can hide the real controller and show a virtual one, or
leave the game a pad that never reports input. The game logs a warning at
start, and the tool prints the same text on its first line. Fix: quit Steam,
or turn Steam Input off for the game (Properties, Controller, Disable Steam
Input), or in Steam Settings, Controller, untick the controller types you use.

## Scripted pads

Scripted pads let a test run play split screen without controllers. Each step
is `<seconds>:<controls>` (a 0.25 s tap) or `<from>-<to>:<controls>` (a hold).
Controls are `a b x y start back up down left right lb rb lt rt ls rs`,
`stick_up stick_down stick_left stick_right`, and
`look_up look_down look_left look_right`, joined with `+`. A pad can unplug
with `<from>-<to>:disconnect`. Steps are separated by commas, semicolons or
spaces.

```sh
rex-padtest --hid_virtual_pads=2 --hid_virtual_pad1="1:a, 2-4:stick_up+look_left" \
            --hid_virtual_pad2="3-5:disconnect"
```

A script file holds `key = value` lines: `players` (pad count), `pad1` to
`pad4` (or `p1` to `p4`), and `look_speed`. Route files that use
`BO2ZM_TEST_BUTTONS` and `BO2ZM_TEST_BUTTONS_P2` to `_P4` also load as is.
