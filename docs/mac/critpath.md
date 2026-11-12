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

## LIGHT mode

`REX_BO2ZM_CRITPATH=2` records only Main's `sub_82493690` wait at LR `0x828ABCCC`, which passes through
`sub_822E9CA8` and `sub_824937A8`. Its filter can be an explicit group or a type descriptor's counter pair at +16.
Initial queue pushes and successful job callback returns maintain fixed per-filter/type outstanding counts.
Five callback-return probes cover both worker callback sites, the helper site, and both type-wait sites. Failed
callbacks/put-backs do no recording. After generating a **private** `app/generated` copy, run
`python3 tools/critpath_light_probes.py` to install/verify those five probes. The script refuses symlinked generated trees.

LIGHT does not install the SDK write callback, collect fences, scan periodically, or write during rendering. CP
records only its frame index and monotonic present time. Main records completed target waits in bounded append-only
buffers (65,536 presents and waits). At exit the recorder joins these intervals and writes:

- `critpath_light_frames.tsv`: per-frame wait overlap, longest intersecting wait, its filter, guest queued/running
  counts at start/end, last matching successful callback's timestamp/type/function/record/data/thread, and member types.
- `critpath_light_waits.tsv`: one row per completed target wait, with the same job information.

`types` is a semicolon-separated list of `descriptor:function:outstanding_start:outstanding_end:completed_during_wait`.
Only types outstanding at a boundary or observed completing during the wait are included. The last job's timestamp
is sampled on its successful callback return, just before the scheduler releases its queued/running counters; it
is not the later picker notification. `last_record` is the scheduler's stack copy; `last_data` is the first word
of that job record. `#thread` footer rows map indexes to host IDs and guest thread names. A zero last timestamp
means no matching completion was observed during the wait. `#drops` includes capacities and count underflows;
nonzero counters indicate incomplete tracking. Lifetime type registries have 4,096 group/filter slots, 32 types per
filter, and at most 32 contributing threads. No published wait/frame row is overwritten.

The route harness ends by sending SIGTERM. In LIGHT mode its handler sets only a lock-free flag; a sleeping exit
thread dumps immutable published rows and exits successfully. Normal application shutdown also dumps the buffers.
SIGKILL/crashes cannot produce an exit dump. An unfinished final wait is omitted. Per-frame longest spans are joined
offline at exit, so their full end time may be later than that frame's present. Fence tracing mode 1 is unchanged.

For exit-only recording on macOS, give runq a canonical output path (`/private/tmp/...` for `/tmp/...`). The current
installed `boot_test.sh` compares the cloned app path literally with the process argv, while the launcher resolves
its executable with `realpath`. A logical `/tmp` clone path can therefore miss the harness's TERM and leave runq to
SIGKILL the process, losing the exit dump. Both names refer to the same output directory.

## Group-own overrun diagnostics

The group-own LIGHT variant adds report-only `kRoomGroup` outcomes to each traced target wait:
`group_sleep_warned`, `group_sleep_timeout`, `group_sleep_direct`, total sleep ns, and the last sleep's start/end
and warned result. `warned` is the existing wait predicate's success: either a direct signal **or a global warning
counter change**. `direct` counts the subset whose sleeper flag was actually signalled; a change-only return must
not be described as a matching-group direct wake. These observations do not change the sleep predicate or timeout.

With `bo2zm_jobs_report_s=5`, `[jobs] group-sleep` lines give exact per-thread counts for the report window. Their
`ended_warnings` counts calls made by that thread to `Warn(kWarningEnded,key)` where `key` is Main's most recently
observed target group (`watched_group`). Group-sleep warned/timeout/direct counters on those log lines include all
that thread's `kRoomGroup` sleeps; the LIGHT TSV fields are restricted to the traced Main wait.

For the report-enabled overrun build, the group hook captures the LIGHT return before `AnnotateWait` takes the
shared jobs-report map mutex. The return timestamp therefore excludes post-return reporting lock contention.
