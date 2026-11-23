#!/bin/bash
# runq laptop job: run the SDK unit suite (ctest -L unit) in the laptop's existing Linux build. CPU only, no game.
# usage: runq submit --where laptop --name X --out DIR -- bash ~/bo2-worker/jobs/unit-suite.sh {OUT}
set -u
root=$HOME/bo2-worker/drawer-linux
out=${1:?out dir}
cd "$root/sdk-linux-tests-build" || exit 2
ctest -L unit -j2 --output-on-failure > "$out/ctest.log" 2>&1
code=$?
tail -5 "$out/ctest.log" > "$out/summary.txt"
exit $code
