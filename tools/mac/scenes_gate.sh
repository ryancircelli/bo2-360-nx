#!/bin/bash
# The regress.sh "scenes" stage: boots each short scene through runq and passes when every scene passes its log
# checks and its golden-frame diff (scenediff.py). Optional: it only runs with REGRESS_SCENES=1 (exit 77 = SKIP
# otherwise), because each scene is a real game boot of about 2-3 minutes plus its wait in the queue.
#
#   REGRESS_SCENES=1 tools/gputest/regress.sh regress-bo2.conf --only scenes
#
# Env: SCENES_APP (default ~/Games/Launchers/BO2 Zombies.app), SCENES (space list, default town4_fence_pistol),
# SCENES_RENDERER (metal), SCENES_SOLO (1: run them solo; the fence/pistol speckle depends on frame rate),
# REGRESS_OUT (regress.sh sets it). The last line is the one-line verdict.
set -u
B="$(cd "$(dirname "$0")" && pwd)"
if [ "${REGRESS_SCENES:-0}" != 1 ]; then
  echo "scenes: SKIP (optional stage; set REGRESS_SCENES=1 to boot the scenes through runq)"; exit 77
fi
app="${SCENES_APP:-$HOME/Games/Launchers/BO2 Zombies.app}"
[ -d "$app" ] || { echo "scenes: SKIP (no app at $app)"; exit 77; }
out="${REGRESS_OUT:-/tmp/gputest-regress}/scenes"; mkdir -p "$out"
solo=(); [ "${SCENES_SOLO:-1}" = 1 ] && solo=(--solo)
ids=(); names=()
for s in ${SCENES:-town4_fence_pistol}; do
  f="$B/scenes/$s.json"; [ -f "$f" ] || { echo "scenes: FAIL (no scene $f)"; exit 1; }
  rm -rf "$out/$s"
  line=$("$B/runq" submit --app "$app" --scene "$f" --renderer "${SCENES_RENDERER:-metal}" --class harness \
         --owner regress "${solo[@]}" --name "regress-$s" --out "$out/$s") || { echo "scenes: FAIL (submit $s)"; exit 1; }
  echo "$line"; ids+=("$(echo "$line" | awk '{print $2}')"); names+=("$s")
done
"$B/runq" wait "${ids[@]}"
pass=0; fail=0; skip=0; parts=()
for i in "${!ids[@]}"; do
  s="${names[$i]}"; r="$out/$s/scene_result.json"
  v=$(python3 -I - "$r" <<'EOF'
import json, sys
try:
    r = json.load(open(sys.argv[1]))
except OSError:
    print('FAIL no-result'); sys.exit()
d = r.get('diff') or {}
w = d.get('worst_by_view') or []
sc = ' '.join(f'v{i + 1}:' + ','.join(f'{k}={v}' for k, v in x.items()) for i, x in enumerate(w))
v = r.get('verdict', 'FAIL')
if v == 'PASS' and d.get('verdict') == 'NO_GOLDEN':
    v = 'NO_GOLDEN'
print(v, sc)
EOF
)
  echo "$s: $v"; parts+=("$s ${v%% *}")
  case "${v%% *}" in PASS|XPASS) pass=$((pass+1));; NO_GOLDEN|XFAIL) skip=$((skip+1));; *) fail=$((fail+1));; esac
done
total=${#ids[@]}
if [ $fail -gt 0 ]; then echo "scenes: FAIL $fail/$total ($(IFS=';'; echo "${parts[*]}"))"; exit 1; fi
if [ $pass = 0 ]; then echo "scenes: SKIP (no golden frames yet: $(IFS=';'; echo "${parts[*]}"))"; exit 77; fi
echo "scenes: PASS $pass/$total ($(IFS=';'; echo "${parts[*]}"))"
