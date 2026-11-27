# Standard gates (tools/gates)

One command per gate. Every gate prints ONE line, `PASS|FAIL <gate> <game> <facts> | <evidence path>`, writes the same
line to `<out>/gate.txt` and `gate.json`, and exits 0 / 1 (2 = setup error). Do not write your own boot.sh.

```
G="python3 ~/dev/bo2-360-nx/tools/gates/gate.py"       # or the copy in your worktree
$G boot  nfsmw                    # control channel up, frames drawn, alive 45 s, screenshot, crash patterns
$G shot  fh1 --ff 2               # route to a scene at 2x guest time, 3 screenshots, non-black check
$G drive nfsmw                    # whole route, verified gameplay movement, screenshots, new warning/error kinds
$G guide nfsmw                    # Guide opens + closes ([guide] opened/closed, XN_SYS_UI 1 then 0), overlay shots
$G open  fh1                      # `open -n` Finder-style launch of a scratch clone, max fds < 256
$G set                            # boot gate for bo2 nfsmw fh1 together (3 queue slots)
$G speed bo2 --a OLD.app --b NEW.app            # town4 pairs, class perf solo, abverdict (max 3 pairs)
$G speed bo2 --bench --a OLD.app --b NEW.app     # deterministic bench (apps built from deterministic-bench only; class bench solo, 2+ runs per side, use --pairs 3+)
$G speed nfsmw --a OLD.app --b NEW.app          # same through the drive route's frame csv (perf.tsv built from it)
$G install nfsmw --app NEW.app --repo ~/dev/bo2-360-nx-<topic> [--integration origin/nfs-tonight] [--dry-run]
$G replay BUILD_DIR --repo ~/dev/bo2-360-nx-<topic> [--only a,b]   # picture-regression replay of the saved GPU traces (sdk/tools/gputest/replay-gate); run first for any sdk/src/graphics change
```
GAME = `bo2 nfsmw fh1 nfshp bo1 nfsmw2012` (nfsmw2012: boot and install, no drive route yet). `--app` defaults to the installed app in ~/Games/Launchers (runq clones it; it is
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
* Per-game table (installed app name, drive route, scene time, profile folder) is `GAMES` in gate.py. Drive routes exist for bo2, nfsmw, fh1, nfshp (Career to the first race), bo1 (Zombies Kino). Speed noise: `SPEED_SIGMA` in gate.py (nfsmw 2%); fh1 has none measured, so `speed fh1` returns RERUN until someone measures two valid same-build runs.

Drive fails unless `driving.json` records gameplay and scene movement in a pair of detailed captures. Racing games need the lower-right MPH/KM/H driving HUD in both frames and changed scene pixels. Menu text, static scenes, loading frames, missing OCR, and unknown gameplay state fail closed. BO2 additionally requires its in-match log marker; BO1 requires its Kino zone-load marker and the lower-right ammo HUD. Text recognition uses macOS Vision via `osascript`; no extra Python packages are needed. Regression check: `python3 -I tools/gates/test_driving.py`.

NFSMW entry stops on `CAREER_SAFEHOUSE.LZC`, then accepts Resume Free Roam, selects Yes with Left, and accepts. Throttle follows `INGAMEB.LZC`. These are load markers; final driving proof is still mandatory. Hot Pursuit instead supports its unitless HUD with a timer advancing and Miles remaining decreasing in the top-right, together with changed road pixels.

Speed readings are recorded when OCR can read them. NFSMW uses segmented digits that Vision can miss, so proof uses the driving HUD together with world-frame differences, rather than accepting process survival. NFSMW captures every two seconds for 60 seconds after its in-game load marker to retain initial movement before a straight-line route reaches a wall.

Forza recognizes the large three-digit speed in its lower-right HUD (at least 5); two moving-world captures must have that HUD. NFSMW permits a missed OCR frame between two positively recognized driving-HUD frames. `driving.json` records the exact endpoint files and deltas. The BO2 in-match marker excludes the script wait echo. All launchers receive private support/log paths under both their declared prefix and `METAL360_*`, including older BO1 apps with no prefix metadata.
