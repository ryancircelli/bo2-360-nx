# Standard gates (tools/gates)

One command per gate. Every gate prints ONE line, `PASS|FAIL <gate> <game> <facts> | <evidence path>`, writes the same
line to `<out>/gate.txt` and `gate.json`, and exits 0 / 1 (2 = setup error). Do not write your own boot.sh.

```
G="python3 ~/dev/bo2-360-nx/tools/gates/gate.py"       # or the copy in your worktree
$G boot  nfsmw                    # control channel up, frames drawn, alive 45 s, screenshot, crash patterns
$G shot  fh1 --ff 2               # route to a scene at 2x guest time, 3 screenshots, non-black check
$G drive nfsmw                    # whole drive route, alive to the end, shots every 60 s, new warning/error kinds
$G guide nfsmw                    # Guide opens + closes ([guide] opened/closed, XN_SYS_UI 1 then 0), overlay shots
$G open  fh1                      # `open -n` Finder-style launch of a scratch clone, max fds < 256
$G set                            # boot gate for bo2 nfsmw fh1 together (3 queue slots)
$G speed bo2 --a OLD.app --b NEW.app            # town4 pairs, class perf solo, abverdict (max 3 pairs)
$G speed nfsmw --a OLD.app --b NEW.app          # same through the drive route's frame csv (perf.tsv built from it)
$G install nfsmw --app NEW.app --repo ~/dev/bo2-360-nx-<topic> [--integration origin/nfs-tonight] [--dry-run]
```
GAME = `bo2 nfsmw fh1 nfshp bo1`. `--app` defaults to the installed app in ~/Games/Launchers (runq clones it; it is
never touched). Everything but `install`/`set`/`speed` is a runq job (class harness, `--class proof|ship` to change), so
it is muted, minimized, on its own UDP ports, SDL_HIDAPI off, and waits its turn. `--now` runs inline (debug only).

Evidence goes to `~/Games/Xbox360/bo2-build/runs/gates/<game>-<gate>-<time>/` (outside /tmp): `gate.txt`,
`shots/*.png` (look at them for the final proof; the gate only checks they are not black/flat), `logs/`, `kinds.txt`
(normalized warning/error kinds; pass an earlier one as `--kinds-from` to get "new-kinds N").

* Fast by default: `boot` is 45 s after the first frame; `shot` and `guide` run the route at `--ff 2` and drop to 1x
  before looking; the Guide gate writes its own pad script (Guide, RB, LB, Guide at the settle time, default 60-75 s).
  Use `--at S` for another scene time, `--route FILE` for another route (bare names: tools/gates/routes, then
  ~/Games/Xbox360/bo2-build/runq/bin/routes).
* `install` refuses when: the installed app is running (the owner may be playing), the build tree does not contain the
  integration line (`git merge-base --is-ancestor origin/main HEAD`), the new app fails `codesign --verify --deep --strict`,
  or its bundle id differs from the installed one. It then clones a restore copy (`--restore-dir`, default inside the
  gate out dir), swaps in place with rsync --delete (no "(previous)" copy), and re-verifies codesign and identity.
  `--dest DIR` installs somewhere else (use it for proofs; never test into ~/Games/Launchers).
* Per-game table (installed app name, drive route, scene time, profile folder) is `GAMES` in gate.py. nfshp and bo1
  have no drive route yet: boot, guide, open work; pass `--route` for the rest.
