"""python3 tools/mac/test_launch_instances.py: argument parsing of the instance launcher."""
import sys
from pathlib import Path
sys.path.insert(0, str(Path(__file__).parent))
from launch_instances import parse

count, apps, audio, extra = parse(["2", "/x/G.app", "--audio", "all", "--env", "1:A=b=c", "--wait"])
assert (count, audio, extra) == (2, "all", {1: {"A": "b=c"}}) and apps == [Path("/x/G.app").resolve()] * 2
assert parse(["3", "/a.app", "/b.app", "/c.app"])[1][2] == Path("/c.app").resolve()
for bad in (["1", "/a.app"], ["9", "/a.app"], ["2", "/a.app", "/b.app", "/c.app"], ["2", "/a.app", "--env", "x"],
            ["2", "/a.app", "--audio", "loud"]):
    try:
        parse(bad)
    except ValueError:
        continue
    raise AssertionError(bad)
print("ok")
