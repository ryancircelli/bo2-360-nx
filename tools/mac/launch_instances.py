#!/usr/bin/env python3
"""Launch 2-4 isolated Mac BO2 instances with per-process tiled windows."""

import json
import os
import re
import subprocess
import sys
import time
from pathlib import Path

BUILD = Path.home() / "Games/Xbox360/bo2-build"
RUNQ = BUILD / "runq"


def measured(job):
    if job.get("measured") is not None:
        return bool(job["measured"])
    if job.get("solo") or job.get("quiet_machine"):
        return True
    if job.get("scene"):
        try:
            route = json.loads(Path(job["scene"]).read_text()).get("route", {})
            return str(route.get("BO2ZM_BENCHMARK", route.get("REX_BO2ZM_BENCHMARK", ""))).lower() in ("true", "1")
        except (OSError, ValueError, TypeError):
            return False
    for arg in job.get("cmd", []):
        if str(arg).endswith(".route"):
            try:
                return any(re.match(r"\s*(REX_)?BO2ZM_BENCHMARK\s*=\s*(true|1)\b", line, re.I)
                           for line in Path(arg).read_text().splitlines())
            except OSError:
                return False
    return False


def active_measured_jobs():
    running = RUNQ / "running"
    if not running.is_dir():
        raise RuntimeError(f"runq running directory not found: {running}")
    found = []
    own_job = os.environ.get("RUNQ_JOB")
    for path in running.glob("*.json"):
        if path.name.endswith(".run.json"):
            continue
        try:
            job = json.loads(path.read_text())
        except (OSError, ValueError) as exc:
            raise RuntimeError(f"cannot read active runq record {path}: {exc}")
        if job.get("id") != own_job and measured(job):
            found.append(f"{job.get('id', path.stem)} {job.get('name', '(unnamed)')}")
    return found


def stop_children(children):
    for child in children:
        if child.poll() is None:
            child.terminate()
    for child in children:
        if child.poll() is None:
            try:
                child.wait(timeout=5)
            except subprocess.TimeoutExpired:
                child.kill()
                child.wait()


def main(argv):
    wait_for_exit = "--wait" in argv
    argv = [arg for arg in argv if arg != "--wait"]
    if not argv or argv == ["--help"]:
        print("Usage: tools/mac/launch_instances.py K [--wait] [APP.app ...]\n"
              "K is 2-4; provide K app bundles or set BO2_MAC_APP to the app bundle to copy.\n"
              "The app tiles windows using BO2_INSTANCE_COUNT; --wait keeps runq active until the games exit.")
        return 0 if argv else 2
    try:
        count = int(argv[0])
        if count < 2 or count > 4:
            raise ValueError
    except ValueError:
        print("launch: K must be 2, 3, or 4", file=sys.stderr)
        return 2
    apps = [Path(p).expanduser().resolve() for p in argv[1:]]
    if not apps:
        configured = os.environ.get("BO2_MAC_APP")
        if not configured:
            print("launch: set BO2_MAC_APP or provide K app bundle paths", file=sys.stderr)
            return 2
        default = Path(configured).expanduser().resolve()
        apps = [default] * count
    if len(apps) != count:
        print(f"launch: provide exactly K app paths (got {len(apps)} for K={count})", file=sys.stderr)
        return 2
    executables = []
    for app in apps:
        exe = app / "Contents/MacOS/Black Ops II"
        if not exe.is_file() or not os.access(exe, os.X_OK):
            print(f"launch: missing executable in app bundle: {exe}", file=sys.stderr)
            return 2
        executables.append(exe)
    try:
        jobs = active_measured_jobs()
    except RuntimeError as exc:
        print(f"launch: refusing because runq state is unavailable: {exc}", file=sys.stderr)
        return 2
    if jobs:
        print("launch: refusing while measured proof jobs are active: " + ", ".join(jobs), file=sys.stderr)
        return 3

    capture_root = Path(os.environ.get(
        "BO2_CAPTURE_ROOT", f"/tmp/bo2/syslink/captures/{time.strftime('%Y%m%d-%H%M%S')}"
    )).expanduser().resolve()
    capture_root.mkdir(parents=True, exist_ok=True)
    children = []
    for instance, exe in enumerate(executables):
        # Repeat the guard before each process to catch proof work queued during startup.
        try:
            jobs = active_measured_jobs()
        except RuntimeError as exc:
            print(f"launch: stopping; runq state unavailable: {exc}", file=sys.stderr)
            return 2
        if jobs:
            print("launch: stopping; measured proof job became active: " + ", ".join(jobs), file=sys.stderr)
            return 3
        env = os.environ.copy()
        env["BO2_INSTANCE"] = str(instance)
        env["BO2_INSTANCE_COUNT"] = str(count)
        env["BO2_LOG_DIR"] = str(capture_root / f"instance-{instance}")
        env["REX_BO2ZM_CAPTURE_EVERY_S"] = "10"
        env["REX_BO2ZM_CAPTURE_MAX"] = "30"
        for suffix in ("", "_P2", "_P3", "_P4"):
            script = os.environ.get(f"BO2_INSTANCE_BUTTONS{suffix}_{instance}")
            if script:
                env[f"REX_BO2ZM_TEST_BUTTONS{suffix}"] = script
        child = subprocess.Popen([str(exe)], env=env, cwd=str(exe.parent))
        children.append(child)
        if instance + 1 < count:
            # Let the first guest reserve its memory arena before starting the
            # next process. This also gives startup failures a clear owner.
            time.sleep(2)
            if child.poll() is not None:
                print(f"launch: instance {instance} exited during startup with status {child.returncode}",
                      file=sys.stderr)
                stop_children(children)
                return 5

    print("Started BO2 instances: " + ", ".join(str(child.pid) for child in children))
    print(f"Game frame captures: {capture_root}/instance-N/captures")
    if wait_for_exit:
        for child in children:
            child.wait()
    return 0


if __name__ == "__main__":
    raise SystemExit(main(sys.argv[1:]))
