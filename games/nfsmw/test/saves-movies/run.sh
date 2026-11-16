#!/bin/bash
set -uo pipefail
exe="$1"; out="$2"; duration="${3:-300}"
mkdir -p "$out/support" "$out/logs" "$out/shots"
[ -z "${REX_GPU_TRACE_DIR:-}" ] || mkdir -p "$REX_GPU_TRACE_DIR"
env REX_AUDIO_MUTE=true REX_LOG_LEVEL=${NFSMW_LOG_LEVEL:-info} \
    SDL_WINDOW_ACTIVATE_WHEN_SHOWN=0 SDL_WINDOW_ACTIVATE_WHEN_RAISED=0 SDL_HINT_MAC_BACKGROUND_APP=1 \
    MVK_CONFIG_FAST_MATH_ENABLED=0 \
    "$exe" ${NFSMW_EXTRA_ARGS:-} --window_start_minimized=true --user_data_root="${NFSMW_SUPPORT:-$out/support}" --log_file="$out/logs/nfsmw.log" \
    >"$out/stdout.txt" 2>&1 &
pid=$!
echo "pid $pid, ${duration}s"
crashed=0
for ((i = 0; i < duration; i++)); do
  sleep 1
  if ! kill -0 "$pid" 2>/dev/null; then
    wait "$pid"; gs=$?
    echo "the game exited by itself at ${i}s with status $gs"
    [ "$gs" != 0 ] && crashed=1
    break
  fi
  # NFSMW_TRACE_PLAN="150:3 250:3 420:300": at second T arm a trace of N frames (no desktop screenshots)
  for ent in ${NFSMW_TRACE_PLAN:-}; do
    if [ "$i" = "${ent%%:*}" ]; then
      printf '%s\n' "${ent##*:}" > "${REX_GPU_TRACE_TRIGGER:?trace trigger required}"
      echo "trace armed at ${i}s: ${ent##*:} frames"
    fi
  done
done
if kill -0 "$pid" 2>/dev/null; then
  kill "$pid" 2>/dev/null
  for _ in 1 2 3 4 5; do kill -0 "$pid" 2>/dev/null || break; sleep 1; done
  kill -9 "$pid" 2>/dev/null
  wait "$pid" 2>/dev/null
fi
cp "$out/logs/nfsmw.log" "$out/game.log" 2>/dev/null || true
echo "shots: $(ls "$out/shots" 2>/dev/null | grep -c '\.jpg$') jpg"
exit "$crashed"
