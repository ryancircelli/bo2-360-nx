# replay-gate

Picture-regression gate over saved GPU traces. No game boot.

    replay-gate check BUILD_DIR [--only A,B] [--jobs 3]     # PASS/FAIL screen, exit 1 on regression
    replay-gate golden BUILD_DIR [--only A,B]               # (re)make goldens from a known-good build
    replay-gate accept TRACE...                             # accept the last check's output as golden

BUILD_DIR holds `rexgpu-replay`, `librexgpu-metal.dylib`, `librexruntime.dylib` (flat, or per-game subdirs bo1/bo2/fh1/nfsmw).
Replays go through runq as ordinary non-solo `harness` jobs (solo windows freeze them, so they cannot spoil a timed run).
`--direct` runs niced outside runq: avoid. Stores: `~/Games/Xbox360/bo2-build/replay-gate/{traces,golden,runs}`.

Verdicts: IDENTICAL (every per-draw hash equal; replays are bit-exact run to run), PASS (hashes drift but final image
within tolerance), FAIL (final image out of tolerance, draw count/failed readbacks changed, crash, timeout),
CHANGED (listed in expected.tsv, not failing). FAIL prints the first diverging draw with its vs/ps hashes and keeps
`diff_<target>.png` (golden | new | heat) under runs/<ts>/<trace>/.

Intended picture change: run `check`, look at the diff images, then `accept` that trace and say why in expected.tsv/commit.
Not covered: CPU/PPC-side fixes (e.g. dot-product), flags that only act on the live path (constant-override file,
FH1 blur threshold), anything the saved traces don't draw. BO1 traces replay slowly (per-draw readback, up to ~15 min under load), they do not deadlock.

Glitch traces: `add-glitches [RUN_DIR...]` copies `logs/glitches/*/*.xtr` (glitch-capture branch) into the store as `glitch-*`; `check`
replays them automatically and reports NEW (not a failure) until `golden --only NAME` is run after the fix.
`sync-laptop` rsyncs bo2-*.xtr (<200 MB) to the laptop CI. New captures need a recorder-enabled plugin (REXGLUE_BUILD_GPUTEST) swapped
into a scratch copy of the app (plugin + runtime for BO2), then REX_GPU_TRACE_FRAME/TRIGGER; see reports/replay-regression.md.
