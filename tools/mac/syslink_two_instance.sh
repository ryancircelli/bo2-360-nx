#!/bin/bash
# Two-instance System Link lobby test (REX_INSTANCE 0 hosts, 1 joins, loopback). runq-submittable:
#   runq submit --name syslink2 --out DIR --timeout 600 -- tools/mac/syslink_two_instance.sh "{APP}" "{OUT}"
# Env: HOST_PAD / JOIN_PAD = pad script text (see rex::input::script::ParseScript), DURATION_S (default 240),
#      EXPECT_HOST / EXPECT_JOIN = log text each copy must show (defaults: lobby markers below).
# Both copies run muted + minimized, own support/log dirs under OUT/inst0, OUT/inst1. Exit 0 = both logs have their
# marker and the joiner's log shows the host's address; 1 = a marker is missing; 2 = setup error; 3 = a copy died early.
set -uo pipefail
app="${1:?app}"; out="${2:?out}"
dur="${DURATION_S:-240}"
exe="$app/Contents/MacOS/Black Ops II"
[ -x "$exe" ] || { echo "no executable $exe" >&2; exit 2; }
HOST_PAD="${HOST_PAD:-wait_log Main menu 60, 2:a}"
JOIN_PAD="${JOIN_PAD:-wait_log Main menu 60, 2:a}"
mkdir -p "$out"
pids=()
launch() {  # n pad
  local n="$1" pad="$2" d="$out/inst$1"
  mkdir -p "$d/support" "$d/logs"
  printf 'players = 1\nduration_s = %s\npad1 = %s\n' "$dur" "$pad" > "$d/pad.route"
  env REX_INSTANCE="$n" REX_INSTANCE_COUNT=2 REX_AUDIO_MUTE=true REX_WINDOW_START_MINIMIZED=true REX_LOG_LEVEL="${LOG_LEVEL:-info}" \
      REX_BO2ZM_TEST_EXIT_ON_ERROR=true REX_BO2ZM_CAPTURE_EVERY_S="${CAPTURE_EVERY_S:-10}" REX_BO2ZM_CAPTURE_MAX=40 \
      REX_BO2ZM_PERF_LOG="$d/perf.tsv" REX_HID_VIRTUAL_PAD_SCRIPT="$d/pad.route" \
      SDL_WINDOW_ACTIVATE_WHEN_SHOWN=0 SDL_WINDOW_ACTIVATE_WHEN_RAISED=0 SDL_HINT_MAC_BACKGROUND_APP=1 \
      MVK_CONFIG_FAST_MATH_ENABLED=0 BO2_NO_DIALOG=1 BO2_SUPPORT_DIR="$d/support" BO2_LOG_DIR="$d/logs" \
      BO2ZM_ROUTE=syslink2 BO2ZM_PLAYERS=1 "$exe" > "$d/stdout.txt" 2>&1 &
  pids+=($!)
  echo "instance $n pid $!"
}
launch 0 "$HOST_PAD"
sleep "${JOIN_DELAY_S:-5}"
launch 1 "$JOIN_PAD"
died=0
for ((i = 0; i < dur; i += 5)); do
  sleep 5
  for p in "${pids[@]}"; do kill -0 "$p" 2>/dev/null || died=1; done
  [ "$died" = 1 ] && { echo "a copy exited early at ${i} s"; break; }
done
for p in "${pids[@]}"; do kill -TERM "$p" 2>/dev/null; done; sleep 3
for p in "${pids[@]}"; do kill -KILL "$p" 2>/dev/null; done
for n in 0 1; do
  d="$out/inst$n"; cp "$(ls -t "$d"/logs/*.log 2>/dev/null | head -1)" "$d/game.log" 2>/dev/null
  mkdir -p "$d/captures"; [ -d "$d/logs/captures" ] && cp -R "$d/logs/captures/." "$d/captures/"
done
[ "$died" = 1 ] && exit 3
rc=0
chk() {  # n text
  grep -qF -- "$2" "$out/inst$1/game.log" 2>/dev/null && echo "PASS inst$1: $2" || { echo "FAIL inst$1: missing '$2'"; rc=1; }
}
[ -n "${EXPECT_HOST:-}" ] && chk 0 "$EXPECT_HOST"
[ -n "${EXPECT_JOIN:-}" ] && chk 1 "$EXPECT_JOIN"
exit $rc
