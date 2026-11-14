#!/usr/bin/env bash
# Runs the campaign bring-up (campaign/out/win/bo2cp.exe) against the game files in the data folder, with the
# emulated GPU. The same rules as games/bo2/run_windows.sh: one game at a time on this PC (the pc-run.lock in the data
# folder, shared with bo2zm), a small window (960x540) on the second monitor without focus (BO2_FOREGROUND=1: a
# normal window on the main monitor), 5 % volume (BO2_VOLUME sets it).
# usage: campaign/run_windows.sh [seconds] [extra --cvar=value ...]   (seconds > 0 stops the game after that long)
set -uo pipefail
CP="$(cd "$(dirname "$0")" && pwd)"
SECS="${1:-0}"; shift || true
DATA="$(bash "$CP/../tools/data_dir.sh")"
EXE="$CP/out/win/bo2cp.exe"
EXE_W="$(cygpath -w "$EXE")"
COMPANION_W="$(cygpath -w "$(dirname "$EXE")/bo2zm.exe")"
LOCK="$DATA/pc-run.lock"
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
    rm -rf "$LOCK"
    continue
  fi
  [ "$announced" = 0 ] && echo "waiting for another PC run: $(cat "$LOCK/owner" 2>/dev/null)" && announced=1
  sleep 5
done
echo "campaign since $(date +%H:%M:%S): $EXE_W" > "$LOCK/owner"
cat "/proc/$$/winpid" > "$LOCK/winpid" 2>/dev/null || true
trap 'rm -rf "$LOCK"' EXIT
while any_game; do
  [ "$announced" != 2 ] && echo "waiting for a running game to exit" && announced=2
  sleep 5
done

PLACE=()
if [ "${BO2_FOREGROUND:-0}" != 1 ]; then
  export SDL_WINDOW_ACTIVATE_WHEN_SHOWN=0 SDL_WINDOW_ACTIVATE_WHEN_RAISED=0
  DESKTOP="$(powershell -NoProfile -ExecutionPolicy Bypass -File "$(cygpath -w "$CP/../tools/pc_desktop_state.ps1")" 2>/dev/null | tr -d '\r')"
  case "$DESKTOP" in screens=[2-9]*) PLACE=(--monitor=2) ;; esac
fi
mkdir -p "$CP/logs"
LOG="$CP/logs/run_$(date +%Y%m%d_%H%M%S).log"
VOLUME="${BO2_VOLUME:-}"
if [ -z "$VOLUME" ]; then
  if [ -f "$DATA/gaming.flag" ]; then VOLUME=0; else VOLUME=5; fi
fi
export BO2_PC_TEST_VOLUME="$VOLUME"
"$EXE" \
  --game_data_root="$DATA/game" --update_data_root="$DATA/update" \
  --gpu_plugin=xenos --log_file="$(cygpath -w "$LOG")" --log_level="${LOG_LEVEL:-info}" --log_max_file_size_mb=200 \
  --fullscreen=false --window_width=960 --window_height=540 --bo2cp_volume="$VOLUME" "${PLACE[@]}" "$@" &
PID=$!
if [ "$SECS" -gt 0 ]; then
  end=$((SECONDS + SECS))
  while [ "$SECONDS" -lt "$end" ]; do
    if ! kill -0 "$PID" 2>/dev/null && [ -z "$(own_pids)" ]; then break; fi
    sleep 2
  done
  for p in $(own_pids); do taskkill //F //PID "$p" >/dev/null 2>&1; done
fi
wait "$PID" 2>/dev/null
while [ -n "$(own_pids)" ]; do sleep 2; done
echo "log: $LOG"
