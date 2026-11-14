#!/usr/bin/env bash
# Runs the PC build against the game files in the data folder (tools/data_dir.sh: $BO2_DATA, bo2_data.txt or data/; layout in docs/building.md).
# usage: [LOG_LEVEL=debug] [BO2_VOLUME=percent] [BO2_FOREGROUND=1] games/bo2/run_windows.sh [seconds] [extra --cvar=value ...]
# LOG_LEVEL=none leaves --log_level out, so the log follows DEBUG as with a player's toml (the quiet log,
# games/bo2/src/bo2zm_debug_options.cpp); BO2_NUMBERED_LOGS=1 leaves --log_file out: the SDK's numbered logs, as on the
# Switch, in games/bo2/out/win*/logs. Both are for testing those two features; leave them unset otherwise.
# Test runs stay out of the way of whoever is using the PC: the window opens on the second monitor when there is one
# (--monitor=2) without taking the focus. Sound: 0 % while the file gaming.flag exists in the data folder (create it
# while playing something else on this PC), else 5 % of the game's volume (bo2zm_volume). BO2_VOLUME sets the volume;
# BO2_FOREGROUND=1 (games/bo2/play_windows.cmd) opens a normal window on the main monitor that takes the focus.
# Do not repeat a cvar this script already passes: the parser rejects the
# duplicate and then ignores every argument ("--game_data_root was not provided").
# With seconds > 0 the game is stopped after that long (for unattended tests).
#
# One game at a time on this PC. Two copies collide (the second cannot bind its socket and relaunches every few
# seconds), and overlapping runs share the CPU, so their numbers do not compare. Runs from every worktree
# (tools/bootstrap_worktree.sh) therefore take turns through a lock in the data folder they share (pc-run.lock), and a run
# also waits until no bo2zm.exe is left from anything else. A run ends when no process of its own exe remains
# (the game relaunches itself as a new process on a mode switch).
set -uo pipefail
APP="$(cd "$(dirname "$0")" && pwd -P)"
SECS="${1:-0}"; shift || true
DATA="$(bash "$APP/../../tools/data_dir.sh")"
EXE="$APP/out/win${BO2_BUILD_TAG:+-$BO2_BUILD_TAG}/bo2zm.exe"
EXE_W="$(cygpath -w "$EXE")"
COMPANION_W="$(cygpath -w "$(dirname "$EXE")/bo2cp.exe")"
LOCK="$DATA/pc-run.lock"

# Keep a handover to the companion in this folder under the same run lock.
own_pids() {
  powershell -NoProfile -Command "Get-CimInstance Win32_Process -Filter \"Name='bo2zm.exe' OR Name='bo2cp.exe'\" | Where-Object { \$_.ExecutablePath -eq '$EXE_W' -or \$_.ExecutablePath -eq '$COMPANION_W' } | ForEach-Object { \$_.ProcessId }" | tr -d '\r'
}
any_game() { tasklist //FI "IMAGENAME eq bo2zm.exe" //NH 2>/dev/null | grep -qi "bo2zm.exe" ||
             tasklist //FI "IMAGENAME eq bo2cp.exe" //NH 2>/dev/null | grep -qi "bo2cp.exe"; }
alive() { [ -n "$1" ] && tasklist //FI "PID eq $1" //NH 2>/dev/null | grep -q " $1 "; }

mkdir -p "$(dirname "$LOCK")"
announced=0
while ! mkdir "$LOCK" 2>/dev/null; do
  holder_pid="$(cat "$LOCK/winpid" 2>/dev/null || true)"
  if [ -n "$holder_pid" ] && ! alive "$holder_pid"; then
    rm -rf "$LOCK"  # its script is gone without releasing it
    continue
  fi
  if [ "$announced" = 0 ]; then
    echo "waiting for another PC run: $(cat "$LOCK/owner" 2>/dev/null)"
    announced=1
  fi
  sleep 5
done
echo "${BO2_BUILD_TAG:-main} since $(date +%H:%M:%S): $EXE_W" > "$LOCK/owner"
cat "/proc/$$/winpid" > "$LOCK/winpid" 2>/dev/null || true
trap 'rm -rf "$LOCK"' EXIT
while any_game; do  # a game started by other means (a debugger, RenderDoc) also gets to finish
  if [ "$announced" != 2 ]; then
    echo "waiting for a running bo2zm.exe to exit"
    announced=2
  fi
  sleep 5
done

DESKTOP="$(powershell -NoProfile -ExecutionPolicy Bypass -File "$(cygpath -w "$APP/../../tools/pc_desktop_state.ps1")" 2>/dev/null | tr -d '\r')"
VOLUME="${BO2_VOLUME:-}"
if [ -z "$VOLUME" ]; then
  if [ -f "$DATA/gaming.flag" ]; then VOLUME=0; else VOLUME=5; fi
fi
export BO2_PC_TEST_VOLUME="$VOLUME"
PLACE=()
if [ "${BO2_FOREGROUND:-0}" != 1 ]; then
  export SDL_WINDOW_ACTIVATE_WHEN_SHOWN=0 SDL_WINDOW_ACTIVATE_WHEN_RAISED=0
  case " $* " in
    *" --monitor="*) ;;
    *) case "$DESKTOP" in screens=[2-9]*) PLACE=(--monitor=2) ;; esac ;;
  esac
fi

# Test runs play as the console does: the native renderer (the PC's default is the emulated GPU, xenos) and god mode
# (the player otherwise dies while a test stands in Town). Passing --bo2zm_renderer=... or --bo2zm_test_god=...
# (games/bo2/play_windows.cmd passes both) decides instead; never both (see the top).
DEFAULTS=()
case " $* " in
  *" --bo2zm_renderer="*) ;;
  *) DEFAULTS+=(--bo2zm_renderer=native) ;;
esac
case " $* " in
  *" --bo2zm_test_god="*) ;;
  *) DEFAULTS+=(--bo2zm_test_god=true) ;;
esac

mkdir -p "$APP/logs"
LOG="$APP/logs/run_$(date +%Y%m%d_%H%M%S).log"
LOG_ARGS=()
if [ "${BO2_NUMBERED_LOGS:-0}" = 1 ]; then
  LOG="$(dirname "$EXE")/logs (numbered)"
else
  LOG_ARGS+=(--log_file="$(cygpath -w "$LOG")")
fi
if [ "${LOG_LEVEL:-info}" != none ]; then
  LOG_ARGS+=(--log_level="${LOG_LEVEL:-info}")
fi
"$EXE" \
  --game_data_root="$DATA/game" --update_data_root="$DATA/update" \
  --gpu_plugin=xenos \
  "${LOG_ARGS[@]}" --log_max_file_size_mb=200 --bo2zm_volume="$VOLUME" \
  "${PLACE[@]}" "${DEFAULTS[@]}" "$@" &
PID=$!
if [ "$SECS" -gt 0 ]; then
  # Up to SECS seconds, or until the game is gone (closed, crashed or stopped by hand): the lock is not held for
  # nothing.
  end=$((SECONDS + SECS))
  while [ "$SECONDS" -lt "$end" ]; do
    if ! kill -0 "$PID" 2>/dev/null && [ -z "$(own_pids)" ]; then
      break
    fi
    sleep 2
  done
  for p in $(own_pids); do
    taskkill //F //PID "$p" >/dev/null 2>&1
  done
  kill "$PID" 2>/dev/null
fi
wait "$PID" 2>/dev/null
while [ -n "$(own_pids)" ]; do  # a relaunched copy of the game is still running
  sleep 2
done
echo "log: $LOG"
