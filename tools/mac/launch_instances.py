#!/usr/bin/env python3
"""Local multiplayer by instances: run 2-8 copies of one game on this Mac as a split-screen grid.

The engine does the rest from REX_INSTANCE / REX_INSTANCE_COUNT (sdk/include/rex/instance_env.h): each copy gets its
own profile, console id, saves, settings and caches (<user>/instances/<i>), its own network ports
(instance_ports.h), its own pads (instance_pads per copy, first come first kept), and a borderless tile of the main
display. The copies find each other through the game's own System Link / LAN mode. Nothing here knows about a game.

Usage: launch_instances.py K [APP.app ...] [--audio first|all] [--env I:NAME=VALUE ...]
  K copies (2-8) of APP (or of $REX_APP / $BO2_MAC_APP); give K apps to run different bundles.
  --audio first (default) plays the first copy only; all mixes every copy.
  --env sets NAME=VALUE for copy I only (for example a test pad script).
The launcher stays until the copies exit; when one copy quits (Guide > Quit, or a crash) it closes the others.
"""

import json
import os
import plistlib
import signal
import subprocess
import sys
import tempfile
import time
from pathlib import Path

RUNQ = Path.home() / "Games/Xbox360/bo2-build/runq"
MAX_COPIES = 8  # rex::kMaxInstanceCount


def active_measured_jobs():
    """Running runq jobs that measure speed; N extra copies would spoil them."""
    running = RUNQ / "running"
    if not running.is_dir():
        return []
    own, found = os.environ.get("RUNQ_JOB"), []
    for path in running.glob("*.json"):
        if path.name.endswith(".run.json"):
            continue
        try:
            job = json.loads(path.read_text())
        except (OSError, ValueError):
            continue
        if job.get("id") != own and (job.get("measured") or job.get("solo") or job.get("quiet_machine")):
            found.append(f"{job.get('id', path.stem)} {job.get('name', '')}".strip())
    return found


def executable(app):
    info = plistlib.loads((app / "Contents/Info.plist").read_bytes())
    exe = app / "Contents/MacOS" / info["CFBundleExecutable"]
    if not os.access(exe, os.X_OK):
        raise ValueError(f"not executable: {exe}")
    return exe


def stop(children):
    for child in children:
        if child.poll() is None:
            child.terminate()
    deadline = time.time() + 10
    for child in children:
        try:
            child.wait(timeout=max(0.1, deadline - time.time()))
        except subprocess.TimeoutExpired:
            child.kill()
            child.wait()


def parse(argv):
    count, apps, audio, extra = None, [], "first", {}
    it = iter(argv)
    for arg in it:
        if arg == "--wait":
            continue  # old flag: the launcher always waits now
        if arg == "--audio":
            audio = next(it, "")
            if audio not in ("first", "all"):
                raise ValueError("--audio is first or all")
        elif arg == "--env":
            spec = next(it, "")
            index, _, pair = spec.partition(":")
            name, eq, value = pair.partition("=")
            if not index.isdigit() or not name or not eq:
                raise ValueError(f"--env wants I:NAME=VALUE, got {spec!r}")
            extra.setdefault(int(index), {})[name] = value
        elif count is None:
            count = int(arg)
        else:
            apps.append(Path(arg).expanduser().resolve())
    if count is None or not 2 <= count <= MAX_COPIES:
        raise ValueError(f"K must be 2 to {MAX_COPIES}")
    if not apps:
        default = os.environ.get("REX_APP") or os.environ.get("BO2_MAC_APP")
        if not default:
            raise ValueError("give the app bundle (or set REX_APP)")
        apps = [Path(default).expanduser().resolve()]
    if len(apps) == 1:
        apps *= count
    if len(apps) != count:
        raise ValueError(f"give one app or exactly K apps (got {len(apps)} for K={count})")
    return count, apps, audio, extra


def main(argv):
    if not argv or argv[0] in ("-h", "--help"):
        print(__doc__)
        return 0 if argv else 2
    try:
        count, apps, audio, extra = parse(argv)
        exes = [executable(app) for app in apps]
    except (ValueError, OSError, KeyError) as exc:
        print(f"launch: {exc}", file=sys.stderr)
        return 2
    if jobs := active_measured_jobs():
        print("launch: refusing while speed runs are active: " + ", ".join(jobs), file=sys.stderr)
        return 3

    # Pad claims are first come, first kept and never pruned: every session starts with none.
    (Path(tempfile.gettempdir()) / "rex-sdl-pad-ownership-v1.txt").unlink(missing_ok=True)
    children = []
    signal.signal(signal.SIGTERM, lambda *_: (stop(children), sys.exit(143)))
    for instance, exe in enumerate(exes):
        env = os.environ.copy()
        env["REX_INSTANCE"] = env["BO2_INSTANCE"] = str(instance)  # BO2_ names: builds before the core move
        env["REX_INSTANCE_COUNT"] = env["BO2_INSTANCE_COUNT"] = str(count)
        if audio == "first" and instance:
            env["REX_AUDIO_MUTE"] = "true"
        env.update(extra.get(instance, {}))
        children.append(subprocess.Popen([str(exe)], env=env, cwd=str(exe.parent)))
        if instance + 1 < count:
            time.sleep(2)  # the first copy reserves its memory and claims its pads before the next starts
            if children[-1].poll() is not None:
                print(f"launch: copy {instance} exited during start-up ({children[-1].returncode})", file=sys.stderr)
                stop(children)
                return 5
    print("launch: copies " + ", ".join(str(c.pid) for c in children), flush=True)

    # One quit closes all: the first copy to exit takes the others down with it.
    while all(child.poll() is None for child in children):
        time.sleep(0.5)
    first = next(child for child in children if child.poll() is not None)
    stop(children)
    return first.returncode or 0


if __name__ == "__main__":
    raise SystemExit(main(sys.argv[1:]))
