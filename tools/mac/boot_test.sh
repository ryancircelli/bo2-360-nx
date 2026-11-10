#!/bin/bash
# Boot and performance test for the packed app (or a bare executable). Adapted from peggle1-nx tools/win/boot_test.ps1.
#
#   boot_test.sh --app "Black Ops II.app" [--route routes/town_benchmark.route] [--out DIR]
#                [--reference ref.mp4] [--baseline perf.tsv] [--video] [--open] [--keep-running]
#   boot_test.sh --exe path/to/bo2zm --data DIR ...      (no app: runs the executable with the data folder)
#
# Muted, started minimized, with window activation suppressed so the run does not take focus. The route (routes/*.route)
# sets duration and REX_ cvars (scripted input via BO2ZM_TEST_BUTTONS[_P2.._P4]). Results land in --out
# (default /tmp/bo2-boot-test/<route>-<time>/): game.log, perf.tsv, summary.txt, run.mp4, compare.json.
# Exit: 0 pass, 1 regression or compare failure, 2 setup error, 3 the game crashed or never produced frames.
set -uo pipefail
here="$(cd "$(dirname "$0")" && pwd)"
app=""; exe=""; data=""; route="$here/routes/smoke.route"; out=""; ref=""; base=""; video=0; useopen=0; keep=0
while [ $# -gt 0 ]; do case "$1" in
  --app) app="$2"; shift 2;; --exe) exe="$2"; shift 2;; --data) data="$2"; shift 2;; --route) route="$2"; shift 2;;
  --out) out="$2"; shift 2;; --reference) ref="$2"; shift 2;; --baseline) base="$2"; shift 2;;
  --video) video=1; shift;; --open) useopen=1; shift;; --keep-running) keep=1; shift;;
  *) echo "unknown option $1" >&2; exit 2;; esac; done
[ -f "$route" ] || { [ -f "$here/routes/$route.route" ] && route="$here/routes/$route.route"; }
[ -f "$route" ] || { echo "no route file: $route" >&2; exit 2; }
[ -n "$app$exe" ] || { echo "--app or --exe is required" >&2; exit 2; }
rname="$(basename "$route" .route)"
[ -n "$out" ] || out="/tmp/bo2-boot-test/$rname-$(date +%Y%m%d-%H%M%S)"
mkdir -p "$out/support" "$out/logs" "$out/captures"
[ -z "$ref" ] && [ -n "$video" ] || true
grep -q '^PLACEHOLDER *= *true' "$route" && echo "note: $rname is a placeholder route, its input script is empty"

# ---- route -> environment ----
duration=60; players=1; quad=0; envs=()
while IFS= read -r line; do
  line="${line%%#*}"; [[ "$line" == *=* ]] || continue
  k="$(echo "${line%%=*}" | tr -d ' ')"; v="$(echo "${line#*=}" | sed 's/^ *//;s/ *$//')"
  case "$k" in duration_s) duration="$v";; players) players="$v";; compare_quadrants) [ "$v" = true ] && quad=1;;
    PLACEHOLDER) ;; BO2ZM_*|AUDIO_*|REX_*) [ -n "$v" ] && envs+=("${k#REX_}=$v");; esac
done < "$route"
envs+=(AUDIO_MUTE=true WINDOW_START_MINIMIZED=true BO2ZM_PERF_LOG="$out/perf.tsv" BO2ZM_CAPTURE_EVERY_S=0)
[ "$video" = 1 ] && envs+=(BO2ZM_VIDEO_RGBA="$out/capture.rgba")   # hook for the build agent, see docs/mac/README.md
fullenv=(); for e in "${envs[@]}"; do fullenv+=("REX_$e"); done
fullenv+=(SDL_WINDOW_ACTIVATE_WHEN_SHOWN=0 SDL_WINDOW_ACTIVATE_WHEN_RAISED=0 SDL_HINT_MAC_BACKGROUND_APP=1
          MVK_CONFIG_FAST_MATH_ENABLED=0 BO2_NO_DIALOG=1 BO2_SUPPORT_DIR="$out/support" BO2_LOG_DIR="$out/logs"
          BO2ZM_ROUTE="$rname" BO2ZM_PLAYERS="$players")   # the perf log's route and players header lines
[ -n "$data" ] && fullenv+=(BO2_DATA="$data")

# ---- stop leftovers ----
pat="${app:-$exe}"; pat="$(cd "$(dirname "$pat")" && pwd)/$(basename "$pat")"
if pgrep -f "$pat" >/dev/null 2>&1; then echo "stopping a leftover instance"; pkill -f "$pat"; sleep 2; pkill -9 -f "$pat" 2>/dev/null; fi

# ---- launch ----
echo "route $rname, ${duration}s, out $out"
start=$(date +%s)
if [ -n "$app" ] && [ "$useopen" = 1 ]; then
  oargs=(); for e in "${fullenv[@]}"; do oargs+=(--env "$e"); done
  open -g -j -n "${oargs[@]}" "$app" || { echo "open failed" >&2; exit 2; }
  sleep 3; pid="$(pgrep -f "$app/Contents/MacOS/bo2zm" | head -1)"
elif [ -n "$app" ]; then
  env "${fullenv[@]}" "$app/Contents/MacOS/Black Ops II" >"$out/stdout.txt" 2>&1 & pid=$!
else
  args=(); [ -n "$data" ] && args=(--game_data_root="$data/game" --update_data_root="$data/update")
  env "${fullenv[@]}" "$exe" ${args[@]+"${args[@]}"} --user_data_root="$out/support" --log_file="$out/logs/bo2zm.log" >"$out/stdout.txt" 2>&1 & pid=$!
fi
[ -n "${pid:-}" ] || { echo "the game did not start" >&2; exit 3; }
# wait for the run: the duration, or an early exit
crashed=0
for ((i = 0; i < duration; i++)); do
  sleep 1
  if ! kill -0 "$pid" 2>/dev/null && ! pgrep -f "$pat" >/dev/null 2>&1; then
    [ "$i" -lt "$((duration - 5))" ] && crashed=1
    break
  fi
done
if [ "$keep" = 0 ]; then
  pkill -TERM -f "$pat" 2>/dev/null; for _ in 1 2 3 4 5 6 7 8 9 10; do pgrep -f "$pat" >/dev/null 2>&1 || break; sleep 1; done
  pkill -9 -f "$pat" 2>/dev/null
fi
log="$(ls -t "$out"/logs/*.log 2>/dev/null | head -1)"; [ -n "$log" ] && cp "$log" "$out/game.log"
ls ~/Library/Logs/DiagnosticReports 2>/dev/null | grep -i "bo2zm\|Black Ops" | while read -r r; do
  [ "$(stat -f %m ~/Library/Logs/DiagnosticReports/"$r")" -ge "$start" ] && { cp ~/Library/Logs/DiagnosticReports/"$r" "$out/"; echo "crash report: $r"; crashed=1; }; done

# ---- results ----
rc=0
if [ "$crashed" = 1 ]; then echo "the game ended early or crashed"; rc=3; fi
if [ -f "$out/perf.tsv" ]; then
  sargs=("$out/perf.tsv" --json "$out/summary.json"); [ -n "$base" ] && sargs+=(--baseline "$base")
  python3 -I "$here/perf_summary.py" "${sargs[@]}" | tee "$out/summary.txt"; s=${PIPESTATUS[0]}
  [ "$s" = 1 ] && [ "$rc" = 0 ] && rc=1; [ "$s" = 2 ] && [ "$rc" = 0 ] && rc=3
elif [ -f "$out/game.log" ] && grep -q '\[benchmark\]' "$out/game.log"; then
  python3 -I "$here/perf_summary.py" "$out/game.log" | tee "$out/summary.txt"
else
  echo "no perf log and no [benchmark] line: the build does not emit them yet (docs/mac/perf-log-format.md)"; [ "$rc" = 0 ] && rc=3
fi
if [ -f "$out/capture.rgba" ]; then
  "$here/make_video.sh" "$out/capture.rgba" "$out/run.mp4" && rm -f "$out/capture.rgba"
  if [ -n "$ref" ] && [ -f "$out/run.mp4" ]; then
    cargs=("$out/run.mp4" "$ref" --json "$out/compare.json"); [ "$quad" = 1 ] && cargs+=(--quadrants)
    python3 -I "$here/compare_video.py" "${cargs[@]}" || { [ "$rc" = 0 ] && rc=1; }
  fi
elif [ -n "$ref" ]; then echo "no capture to compare (the build has no video hook yet)"; fi
echo "result: exit $rc  ($out)"; exit $rc
