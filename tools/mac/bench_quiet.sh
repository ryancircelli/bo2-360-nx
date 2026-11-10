#!/usr/bin/env bash
# Run a benchmark with the Mac's background build work moved off the performance cores.
#
#   tools/mac/bench_quiet.sh <command> [args...]
#
# Every running compiler, linker, build tool, shader tool, python3 and the OrbStack VM is put in the background
# QoS band (`taskpolicy -b`: efficiency cores, throttled I/O), the command runs, and on exit (also on Ctrl-C) each
# demoted process is restored (`taskpolicy -B`). The game (bo2zm) and WindowServer are never touched. Nothing is
# stopped, so other work keeps going, only slower. Processes started after the demotion are demoted too, by a
# sweep every 5 seconds. The load average at the start and end is printed so the numbers can be reported with it.
set -u

if [ $# -eq 0 ]; then
  echo "usage: $0 <command> [args...]" >&2
  exit 2
fi

NAMES='^(clang|clang\+\+|clang-[0-9]+|ld|ld64|ld-prime|ninja|cmake|make|metal|metallib|dxc|xcrun|python3|python3\.[0-9]+|cc1|cc1plus|swift-frontend|lua)$'
ORB='OrbStack Helper|/MacOS/OrbStack$|^orb$'
STATE=$(mktemp -t bench_quiet)
SELF=$$

candidates() {
  ps -axo pid=,comm= | while read -r pid comm; do
    [ "$pid" = "$SELF" ] && continue
    base=${comm##*/}
    case "$base" in bo2zm|WindowServer) continue ;; esac
    if [[ "$base" =~ $NAMES ]] || [[ "$comm" =~ $ORB ]]; then
      echo "$pid"
    fi
  done
}

demote() {
  for pid in $(candidates); do
    grep -qx "$pid" "$STATE" && continue
    if taskpolicy -b -p "$pid" 2>/dev/null; then
      echo "$pid" >> "$STATE"
    fi
  done
}

restore() {
  [ -n "${SWEEP:-}" ] && kill "$SWEEP" 2>/dev/null
  local n=0
  while read -r pid; do
    taskpolicy -B -p "$pid" 2>/dev/null && n=$((n + 1))
  done < "$STATE"
  rm -f "$STATE"
  echo "bench_quiet: restored $n processes; load average at end: $(sysctl -n vm.loadavg)" >&2
}
trap restore EXIT
trap 'exit 130' INT TERM

demote
echo "bench_quiet: demoted $(wc -l < "$STATE" | tr -d ' ') processes; load average at start: $(sysctl -n vm.loadavg)" >&2
( while sleep 5; do demote; done ) &
SWEEP=$!

"$@"
