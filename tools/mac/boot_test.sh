#!/bin/bash
# Boot and performance test for the packed app (or a bare executable). Adapted from peggle1-nx tools/win/boot_test.ps1.
#
#   boot_test.sh --app "Black Ops II.app" [--route routes/town_benchmark.route] [--out DIR]
#                [--pads sdk|game] [--port-offset N] [--reference ref.mp4] [--baseline perf.tsv] [--video] [--open] [--keep-running]
#   boot_test.sh --exe path/to/bo2zm --data DIR ...      (no app: runs the executable with the data folder)
#
# Muted, started minimized, with window activation suppressed so the run does not take focus. The route (routes/*.route)
# sets duration and REX_ cvars (scripted input via BO2ZM_TEST_BUTTONS[_P2.._P4]). Results land in --out
# (default /tmp/bo2-boot-test/<route>-<time>/): game.log, perf.tsv, summary.txt, run.mp4, compare.json.
# Pads: sdk (default) drives the route's scripts through the SDK's scripted pads (REX_HID_VIRTUAL_PAD_SCRIPT=<route>) and does
# NOT export BO2ZM_TEST_BUTTONS*, so no player gets two pads; game uses the game's own test pads instead.
# Every route also gets BO2ZM_TEST_EXIT_ON_ERROR=true (an in-game error restarts the game under the same PID otherwise);
# the run fails on that exit, on a "[relaunch] restarting" line (unless the route says allow_relaunch = true) and on
# the error patterns in CHECK_PATTERNS below.
# Exit: 0 pass, 1 regression, compare or log-check failure, 2 setup error, 3 the game crashed or never produced frames.
set -uo pipefail
here="$(cd "$(dirname "$0")" && pwd)"
app=""; exe=""; data=""; route="$here/routes/smoke.route"; out=""; ref=""; base=""; video=0; useopen=0; keep=0; pads=sdk; strict=0; portoff=${BO2_NET_PORT_OFFSET:-}
while [ $# -gt 0 ]; do case "$1" in
  --app) app="$2"; shift 2;; --exe) exe="$2"; shift 2;; --data) data="$2"; shift 2;; --route) route="$2"; shift 2;;
  --out) out="$2"; shift 2;; --reference) ref="$2"; shift 2;; --baseline) base="$2"; shift 2;;
  --video) video=1; shift;; --open) useopen=1; shift;; --keep-running) keep=1; shift;; --pads) pads="$2"; shift 2;; --strict) strict=1; shift;; --port-offset) portoff="$2"; shift 2;;
  *) echo "unknown option $1" >&2; exit 2;; esac; done
[ -f "$route" ] || { [ -f "$here/routes/$route.route" ] && route="$here/routes/$route.route"; }
[ -f "$route" ] || { [ -f "$here/routes/$route.route.timed" ] && route="$here/routes/$route.route.timed"; }
[ -f "$route" ] || { echo "no route file: $route" >&2; exit 2; }
[ -n "$app$exe" ] || { echo "--app or --exe is required" >&2; exit 2; }
# Event routes (wait_log steps) need an SDK with the wait_log step. Look for the string in the binary and its dylibs;
# without it (or with --pads game) the route's .timed twin is used, when it has one.
_xd="$([ -n "$exe" ] && dirname "$exe" || echo "$app/Contents/MacOS")"
_bin="$([ -n "$exe" ] && echo "$exe" || ls "$app"/Contents/MacOS/bo2zm 2>/dev/null)"
if grep -q '^BO2ZM_TEST_BUTTONS.*wait_log' "$route" 2>/dev/null; then
  if [ "$pads" = sdk ] && grep -l 'wait_log' "$_bin" "$_xd"/*.dylib 2>/dev/null | grep -q .; then
    echo "wait_log: supported by this build, using the event route $(basename "$route")"
  elif [ -f "$route.timed" ]; then
    echo "wait_log: NOT supported by this build (or --pads game), using $(basename "$route").timed"; route="$route.timed"
  else
    echo "wait_log: NOT supported by this build and $(basename "$route") has no .timed twin" >&2; exit 2
  fi
fi
rname="$(basename "$route" .timed)"; rname="$(basename "$rname" .route)"
[ -n "$out" ] || out="/tmp/bo2-boot-test/$rname-$(date +%Y%m%d-%H%M%S)"
mkdir -p "$out/support" "$out/logs" "$out/captures"
[ -z "$ref" ] && [ -n "$video" ] || true
grep -q '^PLACEHOLDER *= *true' "$route" && echo "note: $rname is a placeholder route, its input script is empty"

# ---- route -> environment ----
duration=60; players=1; quad=0; envs=(); allow_relaunch=0; scripted=0
CHECK_PATTERNS='unregistered function|MISMATCH|lost wake'
# known defects of the 23:37 build, logged on every run: counted and printed, they fail the run only with --strict
KNOWN_PATTERNS='signal delivery failed|not in the library|primitive restart'
while IFS= read -r line; do
  line="${line%%#*}"; [[ "$line" == *=* ]] || continue
  k="$(echo "${line%%=*}" | tr -d ' ')"; v="$(echo "${line#*=}" | sed 's/^ *//;s/ *$//')"
  case "$k" in duration_s) duration="$v";; players) players="$v";; compare_quadrants) [ "$v" = true ] && quad=1;;
    allow_relaunch) [ "$v" = true ] && allow_relaunch=1;; PLACEHOLDER) ;;
    BO2ZM_TEST_BUTTONS*) [ -n "$v" ] && scripted=1; [ "$pads" = game ] && [ -n "$v" ] && envs+=("${k#REX_}=$v");;
    BO2ZM_*|AUDIO_*|REX_*) [ -n "$v" ] && envs+=("${k#REX_}=$v");; esac
done < "$route"
envs+=(LOG_LEVEL=info BO2ZM_TEST_EXIT_ON_ERROR=true AUDIO_MUTE=true WINDOW_START_MINIMIZED=true BO2ZM_PERF_LOG="$out/perf.tsv" BO2ZM_CAPTURE_EVERY_S=${CAPTURE_EVERY_S:-15} BO2ZM_CAPTURE_MAX=${CAPTURE_MAX:-60})
[ "$video" = 1 ] && envs+=(BO2ZM_VIDEO_RGBA="$out/capture.rgba")   # hook for the build agent, see docs/mac/README.md
fullenv=(); for e in "${envs[@]}"; do fullenv+=("REX_$e"); done
[ "$pads" = sdk ] && [ "$scripted" = 1 ] && fullenv+=(REX_HID_VIRTUAL_PAD_SCRIPT="$(cd "$(dirname "$route")" && pwd)/$(basename "$route")")
[ -n "$portoff" ] && fullenv+=(REX_NET_SYSTEM_PORT_OFFSET="$portoff")   # explicit offset; otherwise a free one is picked below. other instances bind UDP 31000/31001; each test run uses its own ports
fullenv+=(SDL_WINDOW_ACTIVATE_WHEN_SHOWN=0 SDL_WINDOW_ACTIVATE_WHEN_RAISED=0 SDL_HINT_MAC_BACKGROUND_APP=1
          MVK_CONFIG_FAST_MATH_ENABLED=0 BO2_NO_DIALOG=1 BO2_SUPPORT_DIR="$out/support" BO2_LOG_DIR="$out/logs"
          BO2ZM_ROUTE="$rname" BO2ZM_PLAYERS="$players")   # the perf log's route and players header lines
[ -n "$data" ] && fullenv+=(BO2_DATA="$data")
# Another copy of the game on the default ports makes the socket bind fail, and the game then reboots itself over and
# over (an empty perf log). Unless the caller or the route chose one, pick an offset whose UDP ports (offset + 1000 and
# + 1001) are free.
if [ -z "${REX_NET_SYSTEM_PORT_OFFSET:-}" ] && [[ " ${envs[*]} " != *" NET_SYSTEM_PORT_OFFSET="* ]]; then
  port_offset=50000
  while lsof -nP -iUDP:$((port_offset + 1000)) >/dev/null 2>&1 || lsof -nP -iUDP:$((port_offset + 1001)) >/dev/null 2>&1; do
    port_offset=$((port_offset + 1000))
    [ "$port_offset" -le 63000 ] || { echo "no free port offset between 50000 and 63000" >&2; exit 2; }
  done
  fullenv+=(REX_NET_SYSTEM_PORT_OFFSET="$port_offset")
  echo "port offset $port_offset (UDP $((port_offset + 1000)), $((port_offset + 1001)))"
fi

# ---- stop leftovers ----
pat="${app:-$exe}"; pat="$(cd "$(dirname "$pat")" && pwd)/$(basename "$pat")"
if pgrep -f "$pat" >/dev/null 2>&1; then echo "stopping a leftover instance"; pkill -f "$pat"; sleep 2; pkill -9 -f "$pat" 2>/dev/null; fi

# the game writes captures/ next to the executable (bo2zm_capture_every_s); start clean, collect afterwards
exedir="$([ -n "$exe" ] && dirname "$exe" || echo "$app/Contents/MacOS")"; rm -rf "$exedir/captures"
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
crashed=0; early=0
mkdir -p "$out/shots"
for ((i = 0; i < duration; i++)); do
  sleep 1
  # a screenshot every SHOT_EVERY_S seconds (default 20) and one just before the end; needs Screen Recording permission
  if [ $((i % ${SHOT_EVERY_S:-20})) = 5 ] || [ "$i" = "$((duration - 2))" ]; then screencapture -x -t jpg "$out/shots/t$(printf %04d $i).jpg" 2>/dev/null; fi
  if ! kill -0 "$pid" 2>/dev/null && ! pgrep -f "$pat" >/dev/null 2>&1; then
    [ "$i" -lt "$((duration - 5))" ] && crashed=1
    early=1; break
  fi
done
if [ "$keep" = 0 ]; then
  pkill -TERM -f "$pat" 2>/dev/null; for _ in 1 2 3 4 5 6 7 8 9 10; do pgrep -f "$pat" >/dev/null 2>&1 || break; sleep 1; done
  pkill -9 -f "$pat" 2>/dev/null
fi
[ -d "$exedir/captures" ] && cp -R "$exedir/captures/." "$out/captures/" && echo "captures: $(ls "$out/captures" | wc -l) in $out/captures"
log="$(ls -t "$out"/logs/*.log 2>/dev/null | head -1)"; [ -n "$log" ] && cp "$log" "$out/game.log"
for r in ~/Library/Logs/DiagnosticReports/*bo2zm* ~/Library/Logs/DiagnosticReports/*Black*Ops*; do
  [ -f "$r" ] || continue
  if [ "$(stat -f %m "$r")" -ge "$start" ]; then cp "$r" "$out/"; echo "crash report: $(basename "$r")"; crashed=1; fi
done
# the game exits with EXIT_FAILURE on an in-game error (BO2ZM_TEST_EXIT_ON_ERROR); the shell sees that status
if [ "$early" = 1 ] && [ "$useopen" = 0 ]; then
  wait "$pid" 2>/dev/null; gs=$?   # status of a game that ended by itself: 1 = EXIT_FAILURE, 128+n = killed by a signal
  [ "$gs" != 0 ] && { echo "the game ended by itself with status $gs (1 = in-game error, 134/139 = crash)"; crashed=1; }
fi
logfail=0
if [ -f "$out/game.log" ]; then
  if [ "$allow_relaunch" = 0 ] && grep -q '\[relaunch\] restarting' "$out/game.log"; then echo "FAIL: the game relaunched itself"; logfail=1; fi
  # STUB lines for the sign-in calls fail a run (join test plan, splitscreen-signin.md)
  if grep -E 'XamUserGetAgeGroup|XamUserGetMembershipTier' "$out/game.log" | grep -q 'STUB'; then echo "FAIL: STUB line for XamUserGetAgeGroup/XamUserGetMembershipTier"; logfail=1; fi
  # event check: pads run on a clock from process start, so say when a join landed before its event
  t0="$(head -1 "$out/game.log" | sed -n 's/^\[[0-9-]* \([0-9:]*\)\..*/\1/p')"
  tl="$(grep -m1 'Local lobby open' "$out/game.log" | sed -n 's/^\[[0-9-]* \([0-9:]*\)\..*/\1/p')"
  if [ -n "$t0" ] && [ -n "$tl" ]; then
    sec() { IFS=: read -r h m x <<<"$1"; echo $((10#$h*3600+10#$m*60+10#$x)); }
    lobby_s=$(( $(sec "$tl") - $(sec "$t0") ))
    first_join="$(grep -E '^BO2ZM_TEST_BUTTONS_P[2-4]' "$route" | sed -E 's/^[^=]*= *//; s/:.*//; s/-.*//' | sort -n | head -1)"
    echo "event: Local lobby open at ${lobby_s} s on the script clock (log start to lobby)"
    grep -q wait_log "$route" && first_join=""   # event routes wait for the lobby themselves
    [ -n "$first_join" ] && [ "$first_join" -le "$lobby_s" ] && echo "WARN: first join at ${first_join} s is not after the lobby opened (${lobby_s} s): raise the join times in the route"
  fi
  grep -q '\[benchmark\] [0-9]* frames in' "$out/game.log" && echo "route reached its end: benchmark summary written"
  known="$(grep -Ec "$KNOWN_PATTERNS" "$out/game.log")"
  if [ "${known:-0}" -gt 0 ]; then echo "known defects: $known log lines ($KNOWN_PATTERNS)"; [ "$strict" = 1 ] && logfail=1; fi
  # "verify mismatches 0" is an info line of the Metal plugin, not an error
  hits="$(grep -E "$CHECK_PATTERNS" "$out/game.log" | grep -Evic "mismatches 0")"
  if [ "${hits:-0}" -gt 0 ]; then echo "FAIL: $hits log lines match: $CHECK_PATTERNS"; grep -E "$CHECK_PATTERNS" "$out/game.log" | grep -Evi "mismatches 0" | sort | uniq -c | sort -rn | head -5; logfail=1; fi
fi

# ---- results ----
rc=0
if [ "$crashed" = 1 ]; then echo "the game ended early or crashed"; rc=3; fi
[ "$logfail" = 1 ] && [ "$rc" = 0 ] && rc=1
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
else
  [ "$quad" = 1 ] && echo "quadrant comparison: NOT MEASURED (no video; the game lacks the bo2zm_video_rgba setting or --video was not given)"
  [ -n "$ref" ] && echo "reference comparison: NOT MEASURED (no capture)"
  [ -f "$out/perf.tsv" ] || echo "perf log: NOT MEASURED (the game lacks the bo2zm_perf_log setting; only the [benchmark] line is used)"
  [ -n "$ref$quad" ] && [ "$ref$quad" != "0" ] && echo "not_measured: video" >> "$out/summary.txt"
fi
echo "result: exit $rc  ($out)"; exit $rc
