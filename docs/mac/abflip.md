# Runtime benchmark factors

`bo2zm_benchmark_ab_s` keeps the existing 2x2 arm order. Set
`REX_BO2ZM_BENCHMARK_AB_FACTORS=waits_fence_wake,metal_encode_thread`
to map the first factor to bit 0 and the second to bit 1. At startup the
app rejects unknown, duplicate, empty, or more than two names. The Metal
factors are available on macOS. Each switch returns to its startup value
when the benchmark ends.

Every arm transition, including the final `arm=-1`, writes a normal perf.tsv
`#event` named `abarm`, with `arm`, `settle_s`, and comma-separated
`factors=name=0|1,...` fields. Events share the frame rows' time base.
`REX_BO2ZM_BENCHMARK_AB_SETTLE_S` defaults to 3. The benchmark summary
excludes entire frame intervals overlapping this settle period.

Fence-wake uses a relaxed atomic initialized from `bo2zm_waits_fence_wake`.
A transition cannot invalidate a sleep: its mask is local, notifications
are advisory, the guest rechecks its predicate, and the existing maximum
wait timeout still applies. A sleep started while off can miss the first
new fence notification; a sleep started while on and then disabled can
last to the normal timeout. The settle period excludes this transition.

Metal's encode-thread cvar exists and defaults to false. Requests live in
rexruntime, shared by the app and the dynamically loaded Metal plugin,
with explicitly exported getter/setter declarations. The CP initializes
its local mode from the cvar and latches subsequent atomic requests only
in IssueSwap after flushing prepared draws, ending/draining/releasing the
render encoder, flushing upload encoders, and committing/releasing the
current command buffer. All encoders in the following frame use that
CP-owned mode. DeferredRenderEncoder::endEncoding/release drain the replay
queue. GPU submissions may still be executing, but they no longer own CPU
recorder state. The worker remains parked when the mode is off. A log line
confirms each actual mode change at the drained boundary; an abarm event
records the request, which may precede application by one frame.

For a 600-second measurement use `tools/mac/routes/splitscreen4-long.route`
and `REX_BO2ZM_BENCHMARK_AB_S=30`. Setup matches splitscreen4.route;
its 760-second process window preserves the original setup allowance.
All game runs must go through runq, with audio muted.

Analyze with `python3 tools/mac/abflip.py <perf.tsv>`. Data rows use frame_ms
at index 2. Frames before the first active abarm or after the ending event
are skipped, as are whole frame intervals crossing the settle boundary.
The analyzer prints arm statistics, factor on/off statistics pooled over
the other bit, and adjacent segment p90 comparisons. Its per-factor sign
test holds the other factor fixed, excludes ties, and reports an exact
two-sided binomial p value. Adjacent comparisons overlap and route drift
can correlate them, so these p values are descriptive. The slowest 1% low
FPS is 1000 / mean(slowest ceil(1% of frames) frame_ms). `--settle-s` can
override the duration recorded in events.

`metal_type0_register_batching` is also registered (existing cvar default false).
It selects the existing sequential type-0 range writer versus per-register
writes. Both finish synchronously before the next packet; repeated-register
packets keep the ordered per-register path, and side-effecting/invalid ranges
retain the existing fallback. No batching state persists between packets.
Requests use exported rexruntime atomics, and the Metal CP latches its protected
base-class mode at the same drained IssueSwap boundary as the encoder mode.
Only that CP thread reads/writes the active mode. This avoids changing a cvar
concurrently with ExecutePacketType0. A log confirms each actual change.

The preferred latency experiment is
`REX_BO2ZM_BENCHMARK_AB_FACTORS=metal_encode_thread,metal_type0_register_batching`
(encoder bit 0, batching bit 1). Fence-wake remains selectable in either slot.
