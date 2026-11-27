# Installing runq-aging (manager only; the worker did not install it)

Branch `runq-aging` of ryancircelli/bo2-360-nx, based on `runq-review-refactor` (== the installed runq at the time).
Changes: `tools/mac/runq` (sched_key aging, owner share cap, aged windows), `tools/mac/test_runq_sim.py` (3 new scenarios).

1. Confirm the base: `cmp ~/dev/bo2-360-nx-runq-review/tools/mac/runq ~/Games/Xbox360/bo2-build/runq/bin/runq` must print nothing
   (if the installed runq moved on, rebase `runq-aging` onto it first and rerun the simulator).
2. `cd ~/dev/bo2-360-nx-runq-aging && python3 tools/mac/test_runq_sim.py && python3 tools/mac/test_runq_slot_hygiene.py` (pure Python, seconds).
3. Copy: `cp tools/mac/runq ~/Games/Xbox360/bo2-build/runq/bin/runq`
4. In a foreground terminal, never `launchctl submit`: `~/Games/Xbox360/bo2-build/runq/bin/runq restart`
   (hands the daemon over in ~1 s, adopts running jobs, queued jobs just wait).
5. Check `daemon.log` for `daemon up`. Jobs already queued over 90 min jump the line at once (about 36 today), oldest first:
   expect the bench / sweep backlog to start in the next ordinary window; that is the fix working.

Config (config.json, optional): `max_wait_s` (default 5400, 0 = aging off), `owner_slot_share` (default 0.5: with 3 slots an owner
holds 1 while another owner waits; no idling for it, bumped and aged jobs exempt).
Rollback: `cp` the previous runq back and `runq restart`.
