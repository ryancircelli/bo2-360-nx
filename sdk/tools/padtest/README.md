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
| `rex-padtest --hid` | every HID device with its USB port or Bluetooth address (macOS), then exit |

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
* Pads are recognised by the first of these they have:
  1. serial number, when the OS reports one;
  2. the USB port they are plugged into (macOS: the IOKit LocationID, found
     through SDL's path or the GameController framework). Wired Xbox Series
     pads on macOS report no serial, so this is what keeps four identical pads
     apart;
  3. the Bluetooth address, for Bluetooth pads (macOS: the IOKit
     DeviceAddress of the pad's HID device, found the same ways). Xbox pads
     over Bluetooth report no serial either;
  4. SDL's device path, when it names a port (not macOS `DevSrvsID:`, Linux
     `/dev/input/eventN` or `XInput#N`, which change on every replug);
  5. SDL GUID. Identical pads share it, so connection order decides.
  A pad known by port that comes back in a different port falls back to step
  5. Pads known by port or Bluetooth address that are connected at launch take
  players in port order, then address order, so the same pads give the same
  players every launch.
* `--list` prints, per pad, the identity actually used (`id=...`), which step
  it came from and why the stronger ones were missing, and for pads known by
  port or Bluetooth address a ready `input_player_slots` pin.
* Virtual pads (Steam Input's virtual controllers, SDL virtual joysticks) are
  never player 1 unless pinned there.

## Settings

All are cvars: pass `--name=value` to the game or this tool, put them in the
game's config, or set `REX_<NAME>` in the environment.

| Setting | Default | Meaning |
| --- | --- | --- |
| `input_player_slots` | empty | pin pads to players, e.g. `1=<serial>;2=054c:0ce6;3=Xbox Wireless;4=port:14200000` or `4=bt:98:7a:14:01:02:03`. A match is a serial, an SDL GUID, a `vendor:product` id, a port (`port:<location>` as `--list` prints it), a Bluetooth address (`bt:<address>`), or part of the name. |
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

### Waiting for the game

Fixed times break when loading takes longer than usual. A wait step pauses
the script until the game gets somewhere, and the times of later steps count
from the end of the wait:

* `wait_event <name> [timeout_s]` waits until the game calls
  `rex::input::SignalScriptEvent("<name>")`
  (`#include <rex/input/script/script_events.h>`).
* `wait_log <text> [timeout_s]` waits for a log line containing `<text>`, so
  the game needs no change. Put text holding `,` or `;` in double quotes.

A wait starts once the steps before it are done (or at `<s>` with
`<s>:wait_event <name>`). Without a timeout it waits up to 120 s; then it logs a
warning and the script goes on. A wait ends at the next `,`, `;` or new line.
Each pad waits on its own, and a signal sent before a pad starts waiting still
counts for it.

```sh
rex-padtest --hid_virtual_pads=1 \
  --hid_virtual_pad1="1:a, wait_log Local lobby open 60, 2:start, 4-6:stick_up"
```

A script file holds `key = value` lines: `players` (pad count), `pad1` to
`pad4` (or `p1` to `p4`), and `look_speed`. Route files that use
`BO2ZM_TEST_BUTTONS` and `BO2ZM_TEST_BUTTONS_P2` to `_P4` also load as is.
