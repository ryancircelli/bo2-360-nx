#!/bin/bash
# Self-test of boot_test.sh with fake games (no real build needed). Run: tools/mac/test_harness.sh
here="$(cd "$(dirname "$0")" && pwd)"; t="$(mktemp -d /tmp/bo2-harness-test.XXXXXX)"; fail=0
printf 'duration_s = 6\nBO2ZM_TEST_BUTTONS_P2 = 3:start\n' > "$t/r.route"
mk() { printf '#!/bin/bash\n%s\n' "$2" > "$t/$1"; chmod +x "$t/$1"; }
good='cp "'"${PERF_FIXTURE:-$here/fixtures/perf_ok.tsv}"'" "$REX_BO2ZM_PERF_LOG"; trap "exit 0" TERM; sleep 100 & wait'
mk ok.sh "$good"
mk crash.sh 'cp "'"$here/fixtures/perf_ok.tsv"'" "$REX_BO2ZM_PERF_LOG"; sleep 1; kill -SEGV $$'
mk err.sh 'cp "'"$here/fixtures/perf_ok.tsv"'" "$REX_BO2ZM_PERF_LOG"; sleep 1; exit 1'
mk relaunch.sh 'cp "'"$here/fixtures/perf_ok.tsv"'" "$REX_BO2ZM_PERF_LOG"; echo "[relaunch] restarting the application" >> "$BO2_LOG_DIR/bo2zm.log"; trap "exit 0" TERM; sleep 100 & wait'
mk pads.sh 'env | grep -E "^REX_(HID_VIRTUAL_PAD_SCRIPT|BO2ZM_TEST_BUTTONS|BO2ZM_TEST_EXIT_ON_ERROR)" > "$BO2_SUPPORT_DIR/../envdump.txt"; cp "'"$here/fixtures/perf_ok.tsv"'" "$REX_BO2ZM_PERF_LOG"; trap "exit 0" TERM; sleep 100 & wait'
L='"$BO2_LOG_DIR/bo2zm.log"'; sig='echo "QueueUserCallback: signal delivery failed (3)" >> '"$L"; pr='echo "mvk-warn: Metal does not support disabling primitive restart." >> '"$L"
mk shutdown-signals.sh "$pr; trap '$sig; $sig; exit 0' TERM; cp \"$here/fixtures/perf_ok.tsv\" \"\$REX_BO2ZM_PERF_LOG\"; sleep 100 & wait"
mk early-signal.sh "$sig; echo later >> $L; trap 'exit 0' TERM; cp \"$here/fixtures/perf_ok.tsv\" \"\$REX_BO2ZM_PERF_LOG\"; sleep 100 & wait"
mk two-restarts.sh "$pr; $pr; trap 'exit 0' TERM; cp \"$here/fixtures/perf_ok.tsv\" \"\$REX_BO2ZM_PERF_LOG\"; sleep 100 & wait"
check() { # name expected-rc script
  "$here/boot_test.sh" --exe "$t/$3" --route "$t/r.route" --out "$t/out-$1" >"$t/log-$1" 2>&1; rc=$?
  if [ "$rc" = "$2" ]; then echo "ok   $1 (exit $rc)"; else echo "FAIL $1: exit $rc, wanted $2"; tail -5 "$t/log-$1"; fail=1; fi; }
check pass 0 ok.sh
check segv-crash 3 crash.sh
check error-exit 3 err.sh
check relaunch 1 relaunch.sh
check pads 0 pads.sh
check shutdown-signals 0 shutdown-signals.sh
check early-signal 1 early-signal.sh
check two-restarts 1 two-restarts.sh
if grep -q HID_VIRTUAL_PAD_SCRIPT "$t/out-pads/envdump.txt" && ! grep -q 'TEST_BUTTONS' "$t/out-pads/envdump.txt" && grep -q EXIT_ON_ERROR=true "$t/out-pads/envdump.txt"; then echo "ok   sdk pads: script set, TEST_BUTTONS not exported"; else echo "FAIL pads env"; cat "$t/out-pads/envdump.txt"; fail=1; fi
rm -rf "$t"; exit $fail
