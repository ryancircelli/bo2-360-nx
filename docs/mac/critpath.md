# Main / GPU fence critical-path trace

Set `REX_BO2ZM_CRITPATH=1` (or init-only `bo2zm_critpath=true`) and enable `bo2zm_perf_log`.
The SDK renderer writes two sibling TSVs beside perf.tsv. The default is off. Guest hooks then do only a cached
boolean check; the SDK fence notification has a null callback. No waits or scheduling policies are changed.

All `t_*`, wait start/end and duration values use host `steady_clock` nanoseconds. `critpath_frames.tsv` uses exactly
the `frame` assigned by perf.tsv; join its `cpu_ms` to obtain the CP's busy time for that frame.

`critpath_fences.tsv` contains append-only snapshots. Keep the **last row per (address,value)**, including rows with
zero fields (zero means unobserved). `address` is the physical completed-fence word, not the tagged PM4 address.
Issue is sampled just before sub_821610C0 emits the packet, using dev+11036 and dev+11024. CP completion is sampled
after EVENT_WRITE_SHD's actual store, with the logical value captured before endian conversion. Only the matching
address/value updates an issued fence. Pending/pass observations come from sub_82161DC0 called at 0x828AA5D4 inside
sub_828AA590; paths that bypass that predicate are not treated as fence passes. Pending-call counts are exact.

`main_wait_ns` is Main's wall time inside sub_82493690 intersected with the interval since the preceding present;
nested waits count once. `main_wait_calls` counts all invocations started in that interval. The longest wait records its
full observed start/end, with `ongoing=1` when end is the present boundary rather than an observed return. Its fence
is the most recent pending test seen by any guest thread during that wait, or zero. This is a temporal association,
not proof that that fence belongs to the waited group. The global association clock has microsecond precision;
first pending/pass/issue/execute timestamps retain nanosecond units. Association stamps support runs up to 71 minutes.

Hot paths use a fixed 1,048,576-slot array indexed by `(value>>1)&mask` and bounded SPSC wait/frame rings; they never
allocate or lock. Fence slots are not recycled during a process. A background writer scans once per second and
flushes both files. Check `fence_collisions`, `wait_drops`, and `frame_drops` before interpreting a run; any nonzero
value makes the corresponding coverage incomplete. An abruptly killed process can lose its last second of output.
No optional ring-kick timestamp is recorded, so issue-to-execute delay includes submission delay.

For slow frames, compare the longest wait with its fence's `t_exec-t_issue` and `t_pass-t_exec`. An exec time inside
the wait followed promptly by a pass supports a fence on the critical path. A fence issued late suggests guest-side
pacing; an early issue and late execution needs further tracing to distinguish late submission from slow CP progress.
