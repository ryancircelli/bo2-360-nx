"""The bo2-360-nx build engine: builds the game for the Nintendo Switch and for Windows from your own copy of Black Ops II.

The kit's double-click front end (kit_build.py) asks its questions and runs this; the game files and shader extractor
(gamefiles_shader_extractor.py) runs a few of its steps. It can also be run directly:

  python install.py --game-files <folder> --out <folder> [--target switch,windows] [--modes zm,mp,cp] [options]

  --game-files FOLDER   a folder with your game files, in any layout: the Xbox 360 disc image (.iso) or the folder
                        extracted from it, Title Update 18 and your DLC packages (repeat it for several folders;
                        --disc, --title-update and --dlc are the same option, kept for older command lines)
  --no-dlc              build without the DLC even when packages are found
  --out FOLDER          where the finished game goes: <out>/switch/bo2zm and <out>/windows/bo2zm
  --work FOLDER         where the build works (default <out>/work; a path without spaces)
  --target T            switch, windows or both (comma-separated; "all" means both). Default: switch
  --modes M             zm (Zombies), mp (Multiplayer), cp (Campaign), comma-separated. Default: zm
  --bots-pack FILE      a bo2zm_bots.gscpack to put next to the game (found by itself in the game files folders)
  --pgo FOLDER          a profile-guided optimization profile for the Switch build (optional)
  --jobs N              parallel compile jobs (default: what the PC's memory allows)
  --no-lto              build without link-time optimization: less memory and time, a slower game
  --low-memory          --jobs 2 and --no-lto, for PCs with 16 GB of memory or less
  --redo STEP           run STEP and every step that depends on it again (see --list-steps)
  --list-steps          show the steps and which are done

Each step records when it is done in <work>/state.json, so running the same command again carries on where it stopped;
a step whose inputs changed (other modes, other DLC) runs again with everything that depends on it. Each step's full
output goes to <work>/logs/<step>.log. Nothing is uploaded, and nothing is written outside the output and work
folders: the kit's source tree is copied (as hard links when it can) into <work>/src and built there.

The two programs: Zombies and Multiplayer are bo2zm (the disc's default_mp.xex), with one shader library for both
modes; Campaign is bo2cp (default.xex), with its own library. Only the chosen modes' files are laid out and only their
shaders are built (game_files.py has the split).

The kit holds this project's source and a prebuilt runtime package (runtime/): the host tools, the shader compilers
and, for the Switch, the Mesa/NVK Vulkan driver. Everything game-derived (the generated code, the shader libraries,
the NROs and the executables) is made here, on your PC, from your files.
"""
import argparse
import ctypes
import hashlib
import json
import os
import re
import shutil
import struct
import subprocess
import sys
import time
import zlib
from concurrent.futures import ThreadPoolExecutor

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import game_files  # noqa: E402

NAME = "bo2-360-nx"
TU18_IMAGE_SHA256 = "ecb2d7ce653be4c4fc8945e0083e222570266b5a770896819427be69d463f72e"  # default_mp, decrypted
TU18_CAMPAIGN_IMAGE_SHA256 = "277c4f21169dd0e0e9c5dc818afb0e0ecc889bb8563e3d8a3ec39580e71d6298"  # default
OTHER_LANGUAGE = re.compile(r"^(bp|fj|fr|ge|it|ja|ms|po|ru|sp)_")
ANY_LANGUAGE = re.compile(r"^(en|bp|fj|fr|ge|it|ja|ms|po|ru|sp)_")
BOTS_PACK = "bo2zm_bots.gscpack"
# The most shaders one library holds: the packer and the game's loader both refuse more
# (kMaxShaders in app/src/native/render/bo2zm_shader_library.cpp).
MAX_SHADERS = 65536

# What the source copy leaves out: a checkout's git-ignored folders and its top-level dot folders (a kit has none).
SKIP_DIRS = {"out", "work", "refs", "data", os.path.join("app", "out"), os.path.join("app", "generated"),
             os.path.join("app", "logs"), os.path.join("sdk", "out"), os.path.join("campaign", "out"),
             os.path.join("campaign", "generated")}
SKIP_FILES = {"bo2_data.txt"}
# Variables of a developer's shell that would send the builds elsewhere or change them.
DEV_VARIABLES = ("BO2_BUILD_TAG", "BO2_PGO", "BO2_PGO_DIR", "BO2_LTO", "BO2_SDK_DIR", "BO2_HEAP_TRACK",
                 "BO2_MEMCPY_CALLERS", "BO2_DEBUG_LINES", "BO2_GUEST_MEMORY_PLAIN", "BO2ZM_BUILD_ID",
                 "BO2ZM_NRO_ICON", "BO2ZM_EXE_ICON", "MESA_SDK", "BO2_DEVLINK", "REXGLUE_BIN")

PC_RUNTIME_FILES = ("rexruntimerd.dll", "TracyClientrd.dll", "rexgpu-xenosrd.dll")
PC_TOML = """# {name} for Windows: settings. The names are the same as the Switch version's (see its bo2zm.toml) and as the
# command-line options (--name=value), so a line here works as an option on the shortcut and the other way round.

# The native Vulkan renderer, with the shader library next to this file. "xenos" emulates the Xbox 360's GPU through
# Direct3D 12 instead: exact, but slow.
gpu_plugin = "xenos"
bo2zm_renderer = "native"

# The mode the game starts in the first time (afterwards it remembers the last one chosen): true Zombies, false
# Multiplayer.
bo2zm_boot_zombies = {boot_zombies}

# Frames are presented in step with the display, so window capture (Discord streaming, OBS) stays live while the game
# has focus. Set both to true for the lowest latency when you don't stream.
vulkan_allow_present_mode_immediate = false
vulkan_allow_present_mode_mailbox = false

# Internet play (ZOMBIES MULTIPLAYER): the free public MQTT servers the consoles meet through to find each other (they
# only pass addresses along; the game itself goes straight between the consoles). If they ever stop working, put other
# public MQTT servers here (host:port, comma-separated). Every player should use the same list.
net_online_brokers = "broker.hivemq.com:1883,broker.emqx.io:1883,broker.mqtt.cool:1883,test.mosquitto.org:1883"

# Frames per second in the top-left corner; the graphics preset only shows for a moment after it changes.
bo2zm_show_fps = true
bo2zm_preset_on_screen = false

# No log files. Remove this line to get the small warnings-and-errors log back, for example to report a crash.
log_level = "off"
"""
PC_CAMPAIGN_TOML = """# {name} for Windows: the campaign's settings (bo2cp.exe). The same names work on its command line.

gpu_plugin = "xenos"
bo2zm_renderer = "native"

# Window capture (Discord streaming, OBS) stays live while the game has focus.
vulkan_allow_present_mode_immediate = false
vulkan_allow_present_mode_mailbox = false

# No log file. Remove this line to get the small log back, for example to report a crash.
log_level = "off"
# Frames per second in the top-left corner; the graphics preset only shows for a moment after it changes.
bo2zm_show_fps = true
bo2zm_preset_on_screen = false
"""
PC_LAUNCHER = """@echo off
rem Starts {name} with its saves, settings and DLC kept in the user folder next to it.
start "" "%~dp0{exe}" --user_data_root="%~dp0user" %*
"""


# --- Output ----------------------------------------------------------------------------------------------------------

def say(text=""):
    print(text, flush=True)


class BuildError(Exception):
    pass


def fail(text):
    raise BuildError(text)


def seconds_text(s):
    s = int(round(s))
    return f"{s // 3600} h {s % 3600 // 60:02d} min" if s >= 3600 else f"{s // 60} min {s % 60:02d} s"


# --- Files -----------------------------------------------------------------------------------------------------------

sha256 = game_files.sha256
link_or_copy = game_files.link_or_copy


def mirror(src, dst, skip=lambda rel: False):
    """Every file of the src tree into dst (hard links or copies); files already there are kept. Empty folders are
    made too: the SDK's third-party folders each need their (empty) .git entry."""
    count = 0
    for dp, dn, fn in os.walk(src):
        rel_dir = os.path.relpath(dp, src)
        dn[:] = [d for d in dn if not skip(os.path.normpath(os.path.join(rel_dir, d)))]
        os.makedirs(os.path.normpath(os.path.join(dst, rel_dir)), exist_ok=True)
        for n in fn:
            rel = os.path.normpath(os.path.join(rel_dir, n))
            if skip(rel):
                continue
            target = os.path.join(dst, rel)
            if not os.path.exists(target):
                link_or_copy(os.path.join(dp, n), target)
                count += 1
    return count


def memory_gb():
    class MemoryStatus(ctypes.Structure):
        _fields_ = [("length", ctypes.c_ulong), ("load", ctypes.c_ulong), ("total", ctypes.c_ulonglong),
                    ("avail", ctypes.c_ulonglong), ("total_page", ctypes.c_ulonglong),
                    ("avail_page", ctypes.c_ulonglong), ("total_virtual", ctypes.c_ulonglong),
                    ("avail_virtual", ctypes.c_ulonglong), ("avail_extended", ctypes.c_ulonglong)]
    try:
        status = MemoryStatus()
        status.length = ctypes.sizeof(MemoryStatus)
        ctypes.windll.kernel32.GlobalMemoryStatusEx(ctypes.byref(status))
        return status.total / (1 << 30)
    except (AttributeError, OSError):
        try:
            return os.sysconf("SC_PAGE_SIZE") * os.sysconf("SC_PHYS_PAGES") / (1 << 30)
        except (ValueError, OSError, AttributeError):
            return 16.0


def short_path(path):
    """Windows' 8.3 form of an existing path (no spaces), or the path itself."""
    if os.name != "nt":
        return path
    buf = ctypes.create_unicode_buffer(1024)
    n = ctypes.windll.kernel32.GetShortPathNameW(path, buf, 1024)
    return buf.value if 0 < n < 1024 else path


def work_folder(wanted):
    """wanted, or its short form when it has spaces (the compilers and build scripts need a path without them), or a
    folder at the root of its drive when Windows gives it no short form."""
    os.makedirs(wanted, exist_ok=True)
    if " " not in wanted:
        return wanted
    short = short_path(wanted)
    if " " not in short:
        return short
    # One folder per kit location, so two kits never share a work folder.
    tag = hashlib.sha1(os.path.normcase(os.path.abspath(wanted)).encode("utf-8")).hexdigest()[:8]
    fallback = os.path.join(os.path.splitdrive(os.path.abspath(wanted))[0] + os.sep, f"{NAME}-work-{tag}")
    os.makedirs(fallback, exist_ok=True)
    try:
        os.rmdir(wanted)  # left empty: the work goes to the fallback
    except OSError:
        pass
    return fallback


# --- Icons -----------------------------------------------------------------------------------------------------------

def png_pixels(png):
    """(width, height, RGBA rows) of an 8-bit RGBA or RGB, non-interlaced PNG, or None."""
    width, height, depth, color, _, _, interlace = struct.unpack(">IIBBBBB", png[16:29])
    if depth != 8 or color not in (2, 6) or interlace:
        return None
    data, pos = b"", 8
    while pos < len(png):
        length, kind = struct.unpack(">I4s", png[pos:pos + 8])
        if kind == b"IDAT":
            data += png[pos + 8:pos + 8 + length]
        pos += 12 + length
    raw = zlib.decompress(data)
    bpp = 4 if color == 6 else 3
    stride = width * bpp
    rows, prev = [], bytearray(stride)
    for y in range(height):
        f = raw[y * (stride + 1)]
        line = bytearray(raw[y * (stride + 1) + 1:(y + 1) * (stride + 1)])
        for i in range(stride):
            a = line[i - bpp] if i >= bpp else 0
            b = prev[i]
            c = prev[i - bpp] if i >= bpp else 0
            if f == 1:
                line[i] = (line[i] + a) & 255
            elif f == 2:
                line[i] = (line[i] + b) & 255
            elif f == 3:
                line[i] = (line[i] + (a + b) // 2) & 255
            elif f == 4:
                p = a + b - c
                pa, pb, pc = abs(p - a), abs(p - b), abs(p - c)
                line[i] = (line[i] + (a if pa <= pb and pa <= pc else b if pb <= pc else c)) & 255
        prev = line
        if bpp == 3:
            line = bytearray(b"".join(bytes(line[i:i + 3]) + b"\xff" for i in range(0, stride, 3)))
        rows.append(bytes(line))
    return width, height, rows


def write_ico(png, path):
    """An .ico with the PNG as a 32-bit bitmap (what every resource compiler and Windows version reads)."""
    decoded = png_pixels(png)
    if not decoded:
        return False
    width, height, rows = decoded
    pixels = b"".join(b"".join(bytes((r[i + 2], r[i + 1], r[i], r[i + 3])) for i in range(0, len(r), 4))
                      for r in reversed(rows))
    mask = b"\0" * (((width + 31) // 32) * 4 * height)
    image = struct.pack("<IiiHHIIiiII", 40, width, height * 2, 1, 32, 0, len(pixels) + len(mask), 0, 0, 0, 0) + \
        pixels + mask
    with open(path, "wb") as f:
        f.write(struct.pack("<HHH", 0, 1, 1))
        f.write(struct.pack("<BBBBHHII", width % 256, height % 256, 0, 0, 1, 32, len(image), 22))
        f.write(image)
    return True


def powershell(script, **env):
    e = dict(os.environ)
    e.update({k: str(v) for k, v in env.items()})
    return subprocess.run(["powershell", "-NoProfile", "-ExecutionPolicy", "Bypass", "-Command", script],
                          env=e, capture_output=True, text=True)


def write_jpeg(png_path, jpg_path, size=256):
    """A size x size JPEG of the PNG (the Switch's Homebrew Menu icon), through Windows' own image library."""
    script = ("Add-Type -AssemblyName System.Drawing; $img = [System.Drawing.Image]::FromFile($env:BO2_ICON_IN); "
              "$size = [int]$env:BO2_ICON_SIZE; $bmp = New-Object System.Drawing.Bitmap $size, $size; "
              "$g = [System.Drawing.Graphics]::FromImage($bmp); "
              "$g.InterpolationMode = [System.Drawing.Drawing2D.InterpolationMode]::HighQualityBicubic; "
              "$g.DrawImage($img, 0, 0, $bmp.Width, $bmp.Height); "
              "$bmp.Save($env:BO2_ICON_OUT, [System.Drawing.Imaging.ImageFormat]::Jpeg)")
    r = powershell(script, BO2_ICON_IN=png_path, BO2_ICON_OUT=jpg_path, BO2_ICON_SIZE=size)
    return r.returncode == 0 and os.path.isfile(jpg_path)


def make_shortcut(lnk, target, arguments, icon, description):
    script = ("$s = (New-Object -ComObject WScript.Shell).CreateShortcut($env:BO2_LNK); "
              "$s.TargetPath = $env:BO2_TARGET; $s.Arguments = $env:BO2_ARGS; "
              "$s.WorkingDirectory = Split-Path -Parent $env:BO2_TARGET; "
              "if ($env:BO2_ICON) { $s.IconLocation = $env:BO2_ICON + ',0' }; "
              "$s.Description = $env:BO2_DESCRIPTION; $s.Save()")
    r = powershell(script, BO2_LNK=lnk, BO2_TARGET=target, BO2_ARGS=arguments, BO2_ICON=icon or "",
                   BO2_DESCRIPTION=description)
    return r.returncode == 0 and os.path.isfile(lnk)


# --- Running tools ---------------------------------------------------------------------------------------------------

class Runner:
    """Runs commands with their output in a log file; progress lines (Ninja's [n/total], step banners) show here."""

    PROGRESS = re.compile(r"^\[\d+/\d+\]|^== |^translate:|^compile:")

    def __init__(self, log_path):
        self.log_path = log_path
        os.makedirs(os.path.dirname(log_path), exist_ok=True)

    def run(self, args, cwd=None, env=None, quiet=False):
        with open(self.log_path, "a", encoding="utf-8", errors="replace") as log:
            log.write(f"\n$ {' '.join(map(str, args))}\n")
            log.flush()
            proc = subprocess.Popen(args, cwd=cwd, env=env, stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                                    text=True, encoding="utf-8", errors="replace")
            last = 0.0
            for line in proc.stdout:
                log.write(line)
                if quiet:
                    continue
                now = time.time()
                if self.PROGRESS.match(line) and (now - last > 2 or not line.startswith("[")):
                    last = now
                    print("    " + line.rstrip()[:150], flush=True)
            proc.wait()
        if proc.returncode != 0:
            tail = open(self.log_path, encoding="utf-8", errors="replace").read().splitlines()[-25:]
            fail(f"{os.path.basename(str(args[0]))} failed (exit {proc.returncode}). The end of "
                 f"{self.log_path}:\n" + "\n".join("    " + t for t in tail))

    def run_parallel(self, commands, workers):
        """Runs independent commands, up to `workers` at once; each one's output goes to the log in order, after all
        have ended. Stops at the first that failed, as run does."""
        def one(args):
            proc = subprocess.run(args, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True, encoding="utf-8",
                                  errors="replace")
            return args, proc.returncode, proc.stdout
        with ThreadPoolExecutor(max_workers=max(1, workers)) as pool:
            results = list(pool.map(one, commands))
        with open(self.log_path, "a", encoding="utf-8", errors="replace") as log:
            for args, _, output in results:
                log.write(f"\n$ {' '.join(map(str, args))}\n{output}")
        for args, code, output in results:
            if code != 0:
                tail = output.splitlines()[-25:]
                fail(f"{os.path.basename(str(args[0]))} failed (exit {code}). The end of its output:\n" +
                     "\n".join("    " + t for t in tail))


# --- The steps -------------------------------------------------------------------------------------------------------

class Step:
    def __init__(self, name, what, method, needs=(), when=lambda: True):
        self.name, self.what, self.method, self.needs, self.when = name, what, method, needs, when


class Installer:
    def __init__(self, args):
        self.args = args
        self.kit = os.path.abspath(args.kit) if args.kit else self.find_kit(HERE)
        self.source = os.path.join(self.kit, "source") if os.path.isdir(os.path.join(self.kit, "source")) else \
            os.path.abspath(os.path.join(HERE, "..", ".."))
        self.runtime = os.path.join(self.kit, "runtime")
        self.out = os.path.abspath(args.out)
        self.work = work_folder(os.path.abspath(args.work) if args.work else os.path.join(self.out, "work"))
        self.src = os.path.join(self.work, "src")
        self.data = os.path.join(self.work, "data")
        self.state_path = os.path.join(self.work, "state.json")
        self.state = {}
        if os.path.isfile(self.state_path):
            with open(self.state_path, encoding="utf-8") as f:
                self.state = json.load(f)
        self.targets = parse_list(args.target, ("switch", "windows"), {"all": ["switch", "windows"]})
        self.modes = parse_list(args.modes, tuple(game_files.MODES), {"all": list(game_files.MODES)})
        self.bash = None
        self.tools = {}
        self.found = None
        self.game = None
        self.packages = []
        self.bots_pack = None

    @staticmethod
    def find_kit(here):
        # In a kit: <kit>/source/tools/install/this file. In a checkout: the runtime package is named with --kit.
        candidate = os.path.abspath(os.path.join(here, "..", "..", ".."))
        if os.path.isdir(os.path.join(candidate, "runtime")) and os.path.isdir(os.path.join(candidate, "source")):
            return candidate
        return os.path.abspath(os.path.join(here, "..", ".."))

    def main_program(self):
        return bool(set(self.modes) & {"zm", "mp"})

    def campaign(self):
        return "cp" in self.modes

    def steps(self):
        main, cp = self.main_program, self.campaign
        sw = lambda: "switch" in self.targets  # noqa: E731
        win = lambda: "windows" in self.targets  # noqa: E731
        # bo2zm runs Zombies and Multiplayer; the messages name the ones chosen.
        zm_mp = " and ".join(game_files.MODES[m] for m in ("zm", "mp") if m in self.modes)
        all_steps = [
            Step("source", "copy the kit's source tree into the work folder", self.step_source),
            Step("game", "lay out the game files, Title Update 18 and the DLC", self.step_game),
            Step("decrypt", "decrypt the game's programs and check they are TU18's", self.step_decrypt,
                 ("source", "game")),
            Step("codegen", f"generate the C++ of bo2zm ({zm_mp})", self.step_codegen, ("decrypt",), main),
            Step("codegen-campaign", "generate the C++ of the campaign", self.step_codegen_campaign, ("decrypt",),
                 cp),
            Step("shaders", f"build the shader library of {zm_mp}", self.step_shaders,
                 ("game", "decrypt"), main),
            Step("shaders-campaign", "build the campaign's shader library", self.step_shaders_campaign,
                 ("game", "decrypt"), cp),
            Step("icons", "make the icons from your game's tile", self.step_icons, ("game",)),
            Step("build-switch", f"build bo2zm.nro ({zm_mp})", self.step_build_switch,
                 ("codegen", "icons"), lambda: main() and sw()),
            Step("build-switch-campaign", "build bo2cp.nro (campaign)", self.step_build_switch_campaign,
                 ("codegen-campaign",), lambda: cp() and sw()),
            Step("build-windows", f"build bo2zm.exe ({zm_mp})", self.step_build_windows,
                 ("codegen", "icons"), lambda: main() and win()),
            Step("build-windows-campaign", "build bo2cp.exe (campaign)", self.step_build_windows_campaign,
                 ("codegen-campaign",), lambda: cp() and win()),
            Step("layout-switch", "put together the Switch folder", self.step_layout_switch,
                 ("game", "shaders", "shaders-campaign", "build-switch", "build-switch-campaign"), sw),
            Step("layout-windows", "put together the Windows folder", self.step_layout_windows,
                 ("game", "shaders", "shaders-campaign", "build-windows", "build-windows-campaign"), win),
        ]
        return [s for s in all_steps if s.when()]

    def save_state(self):
        tmp = self.state_path + ".tmp"
        with open(tmp, "w", encoding="utf-8") as f:
            json.dump(self.state, f, indent=1)
        os.replace(tmp, self.state_path)

    def env(self, **extra):
        e = {k: v for k, v in os.environ.items() if k not in DEV_VARIABLES}
        e.update({"BO2_DATA": self.data.replace("\\", "/"), "PYTHON": sys.executable, "PYTHONUTF8": "1",
                  "BO2_SPECS_DIR": self.specs_dir(), "JOBS": str(self.jobs()), "BO2_DEVLINK": "0"})
        if os.path.isdir(os.path.join(self.runtime, "switch", "portlibs")):
            e["MESA_SDK"] = os.path.join(self.runtime, "switch", "portlibs").replace("\\", "/")
        e.update({k: str(v) for k, v in extra.items()})
        return e

    def specs_dir(self):
        d = os.path.join(self.work, "specs")
        os.makedirs(d, exist_ok=True)
        return d.replace("\\", "/")

    def jobs(self):
        if self.args.jobs:
            return self.args.jobs
        if self.args.low_memory:
            return 2
        # About 3.5 GB per job for the generated code with LTO, and some for everything else.
        return max(1, min(os.cpu_count() or 4, int((memory_gb() - 4) / 3.5)))

    def lto(self):
        return not (self.args.no_lto or self.args.low_memory)

    # --- Checks ------------------------------------------------------------------------------------------------

    def find_bash(self):
        candidates = [os.environ.get("BO2_BASH", ""), r"C:\Program Files\Git\bin\bash.exe",
                      os.path.join(os.environ.get("BO2_DEVKITPRO", r"C:\devkitPro"), "msys2", "usr", "bin", "bash.exe"),
                      shutil.which("bash") or ""]
        for c in candidates:
            if c and os.path.isfile(c):
                return c
        return None

    def toolchain(self):
        """What tools/toolchain.sh finds (the same lookup the build scripts use)."""
        script = ('ROOT="$1"; . "$ROOT/tools/toolchain.sh"; '
                  'echo "cmake=$(command -v cmake)"; echo "ninja=$NINJA_EXE"; echo "devkitpro=$DEVKITPRO"; '
                  'echo "clang=$(command -v clang++)"')
        r = subprocess.run([self.bash, "-c", script, "toolchain", self.source.replace("\\", "/")],
                           capture_output=True, text=True)
        found = {}
        for line in r.stdout.splitlines():
            k, _, v = line.partition("=")
            found[k] = v.strip()
        return found

    def check_tools(self):
        """The problems that stop the build, as a list of plain sentences (empty when everything is there)."""
        problems = []
        if sys.version_info < (3, 11):
            problems.append(f"Python 3.11 or later is needed (this is {sys.version.split()[0]}): python.org")
        self.bash = self.find_bash()
        if not self.bash:
            problems.append("bash was not found: install Git for Windows (git-scm.com)")
        if not os.path.isfile(os.path.join(self.source, "app", "CMakeLists.txt")):
            problems.append(f"the source tree was not found at {self.source}")
        host = os.path.join(self.runtime, "host")
        for name in ("rexglue.exe", "xexdump.exe", "rexruntime.dll", "bo2zm_hlsl.exe", "bo2zm_pack.exe", "dxc.exe",
                     "dxcompiler.dll", "spirv-val.exe"):
            if not os.path.isfile(os.path.join(host, name)):
                problems.append(f"the kit's runtime folder lacks {name} (runtime/host): unpack the kit again")
        if "switch" in self.targets and not os.path.isfile(
                os.path.join(self.runtime, "switch", "portlibs", "lib", "libvulkan.a")):
            problems.append("the kit's runtime folder has no Switch driver (runtime/switch/portlibs)")
        if self.bash and not problems:
            tc = self.toolchain()
            self.tools = tc
            if not tc.get("cmake"):
                problems.append("CMake was not found: install Kitware CMake (cmake.org)")
            elif "devkitpro" in tc["cmake"].lower():
                problems.append("the CMake found is devkitPro's older one: install Kitware CMake (cmake.org)")
            if not tc.get("ninja"):
                problems.append("Ninja was not found: install it (winget install Ninja-build.Ninja)")
            if "switch" in self.targets:
                dkp = tc.get("devkitpro", "")
                if not os.path.isfile(os.path.join(dkp, "devkitA64", "bin", "aarch64-none-elf-g++.exe")):
                    problems.append("devkitPro was not found: install it with the switch-dev group (devkitpro.org)")
                else:
                    for lib, pkg in (("libnx/lib/libnx.a", "libnx"), ("portlibs/switch/lib/libz.a", "switch-zlib"),
                                     ("portlibs/switch/lib/libexpat.a", "switch-libexpat")):
                        if not os.path.isfile(os.path.join(dkp, lib)):
                            problems.append(f"devkitPro lacks {pkg}: install it with devkitPro's pacman")
            if "windows" in self.targets and not tc.get("clang"):
                problems.append("clang was not found: install LLVM 20 or later (winget install LLVM.LLVM) and Visual "
                                "Studio's \"Desktop development with C++\" build tools")
        if not problems:
            r = subprocess.run([os.path.join(host, "xexdump.exe")], capture_output=True)
            if r.returncode in (0xC0000135, -1073741515):
                problems.append("the kit's tools cannot start: install the Microsoft Visual C++ Redistributable (x64)")
        return problems

    def find_inputs(self):
        """Scans the game files folders (unless the front end already did); returns the problems that stop the
        build."""
        if not self.found:
            found = game_files.Found()
            for folder in self.args.game_files or []:
                if os.path.isfile(folder):
                    folder = os.path.dirname(os.path.abspath(folder))
                if not os.path.isdir(folder):
                    return [f"{folder} is not a folder"]
                part = game_files.scan(folder, skip=[self.out, self.work])
                found.games += part.games
                found.tu18 = found.tu18 or part.tu18
                found.other_updates += part.other_updates
                names = {p.file_name.upper() for p in found.dlc}
                found.dlc += [p for p in part.dlc if p.file_name.upper() not in names]
                found.problems += part.problems
            self.found = found
            self.packages = [] if self.args.no_dlc else found.dlc
        self.game, problem = game_files.choose_game(self.found)
        problems = [problem] if problem else []
        if not self.found.tu18:
            problems.append("Title Update 18 was not found. It is required: put the TU18 package "
                            f"({game_files.TU18_NAME}) in the game files folder")
        if self.args.bots_pack:
            if not os.path.isfile(self.args.bots_pack):
                problems.append(f"--bots-pack {self.args.bots_pack} was not found")
            self.bots_pack = os.path.abspath(self.args.bots_pack)
        for folder in self.args.game_files or []:
            for dp, _, fn in os.walk(folder if os.path.isdir(folder) else os.path.dirname(folder)):
                if not self.bots_pack and BOTS_PACK in fn:
                    self.bots_pack = os.path.join(dp, BOTS_PACK)
        if not self.bots_pack and os.path.isfile(os.path.join(self.kit, "extras", BOTS_PACK)):
            self.bots_pack = os.path.join(self.kit, "extras", BOTS_PACK)
        if self.args.pgo and not os.path.isdir(self.args.pgo):
            problems.append(f"--pgo {self.args.pgo} is not a folder")
        return problems

    def use(self, found, packages):
        """The front end's scan and DLC choice, so the build does not scan again."""
        self.found, self.packages = found, packages

    def check(self):
        say("Checking the tools and your files")
        problems = self.check_tools() + self.find_inputs()
        if problems:
            fail("the build cannot start:\n" + "\n".join("  - " + p for p in problems))
        need = 4e9 + game_files.estimate(self.game, self.modes, self.found) * (0 if self.same_drive() else 1)
        free = game_files.free_space(self.work)
        if free < need:
            say(f"  warning: {game_files.gb(free)} free in {self.work}, about {game_files.gb(need)} needed")
        say(f"  memory {memory_gb():.0f} GB: {self.jobs()} parallel jobs, link-time optimization "
            f"{'on' if self.lto() else 'off'}")

    def same_drive(self):
        try:
            return os.stat(self.game.path).st_dev == os.stat(self.work).st_dev
        except OSError:
            return False

    # --- Steps -------------------------------------------------------------------------------------------------

    def host(self):
        return os.path.join(self.runtime, "host")

    def step_source(self, run):
        # The tree is copied, so codegen and the builds write into the copy, never into the kit or a checkout.
        n = mirror(self.source, self.src, skip=lambda rel: rel in SKIP_DIRS or rel in SKIP_FILES
                   or (rel.startswith(".") and os.sep not in rel))
        for name in ("rexglue.exe", "rexruntime.dll"):  # codegen.sh runs the code generator from sdk/out/win-amd64
            link_or_copy(os.path.join(self.host(), name), os.path.join(self.src, "sdk", "out", "win-amd64", name))
        say(f"  {n} files")

    def step_game(self, run):
        installed = game_files.prepare(self.found, self.game, self.modes, self.packages, self.data,
                                       os.path.join(self.work, "game-work"), run.log_path)
        self.state["dlc"] = [p.display_name for p in installed]

    def decrypt(self, run, xex, expected):
        load = os.path.join(self.src, "work", "load_tu18")
        stem = xex[:-4]
        for name in (xex, stem + ".xexp"):
            link_or_copy(os.path.join(self.data, "game", name), os.path.join(load, name))
        dump = os.path.join(self.src, "work", "dump_tu18")
        run.run([os.path.join(self.host(), "xexdump.exe"), load, xex, dump],
                env=self.env(PATH=self.host() + os.pathsep + os.environ.get("PATH", "")))
        image = os.path.join(dump, stem + ".image.bin")
        if not os.path.isfile(image) or sha256(image) != expected:
            fail(f"the decrypted {xex} is not TU18's (checksum differs)")
        say(f"  {xex}: TU18, checksum matches")

    def step_decrypt(self, run):
        if self.main_program():
            self.decrypt(run, "default_mp.xex", TU18_IMAGE_SHA256)
        if self.campaign():
            self.decrypt(run, "default.xex", TU18_CAMPAIGN_IMAGE_SHA256)

    def step_codegen(self, run):
        run.run([self.bash, "tools/codegen.sh"], cwd=self.src, env=self.env())

    def step_codegen_campaign(self, run):
        run.run([self.bash, "campaign/codegen.sh"], cwd=self.src, env=self.env())

    def zone_files(self, program):
        """The fastfiles whose shaders go into a program's library. bo2zm: the zones of Zombies and/or Multiplayer
        (the chosen ones), with the zones every mode shares and the DLC's; other languages' zones have no shaders of
        their own. bo2cp: the campaign's zones without any language zone, and patch_ui_zm, whose 2D shaders the
        campaign creates at startup from data in none of its own zones (see campaign/build_shaders.py)."""
        modes = ({"zm", "mp"} & set(self.modes)) if program == "bo2zm" else {"cp"}
        found = []
        for folder in ("game", "update", "content"):
            for dp, _, fn in os.walk(os.path.join(self.data, folder)):
                for n in sorted(fn):
                    stem = n[:-3].lower()
                    if not n.lower().endswith(".ff"):
                        continue
                    if (OTHER_LANGUAGE if program == "bo2zm" else ANY_LANGUAGE).match(stem):
                        continue
                    if game_files.zone_modes(stem) & modes:
                        found.append(os.path.join(dp, n))
        return found

    def build_library(self, run, program, image):
        shaders = os.path.join(self.work, "shaders", program)
        zones_dir, unique, hlsl, spirv = (os.path.join(shaders, d) for d in ("zones", "unique", "hlsl", "spirv"))
        for d in (zones_dir, unique, hlsl, spirv):
            shutil.rmtree(d, ignore_errors=True)
            os.makedirs(d)
        zones = self.zone_files(program)
        # Each zone is decompressed and searched on one core, so several run at once (half the cores, at most 8: each
        # holds a zone in memory). In batches of at most 40, so the command line stays well under Windows' 32,767
        # characters with long paths.
        workers = max(1, min(8, (os.cpu_count() or 2) // 2))
        say(f"  extracting the shaders of {len(zones)} zones ({workers} at a time)")
        size = max(1, min(40, -(-len(zones) // (workers * 3))))
        ff_shaders = [sys.executable, "-I", os.path.join(self.src, "tools", "inspect", "ff_shaders.py")]
        commands = [[*ff_shaders, *zones[i:i + size], "--dump", "--out", zones_dir, "--xex-image", image]
                    for i in range(0, len(zones), size)]
        if program == "bo2zm":
            # The shaders linked into the executable itself, in no zone: BO2 creates six of them at renderer start.
            commands.append([*ff_shaders, "--image-shaders", "--dump", "--out", zones_dir, "--xex-image", image])
        run.run_parallel(commands, workers)
        # The blobs are named by content, so one folder of them all is the set without duplicates.
        for d in os.listdir(zones_dir):
            path = os.path.join(zones_dir, d)
            if d.endswith("_shaders") and os.path.isdir(path):
                for n in os.listdir(path):
                    if n.endswith(".bin") and not os.path.exists(os.path.join(unique, n)):
                        os.replace(os.path.join(path, n), os.path.join(unique, n))
        shutil.rmtree(zones_dir, ignore_errors=True)  # the decompressed zones: gigabytes, not needed any more
        count = len(os.listdir(unique))
        if count > MAX_SHADERS:
            fail(f"the zones of these modes hold {count} shaders, more than one library can hold ({MAX_SHADERS}): "
                 "choose fewer modes or leave out the DLC")
        say(f"  {count} shaders; translating")
        common = os.path.join(self.src, "tools", "xenosrecomp", "translator", "shader_common.h")
        run.run([os.path.join(self.host(), "bo2zm_hlsl.exe"), unique, hlsl, common], quiet=True)
        sources = sorted(n for n in os.listdir(hlsl) if n.endswith(".hlsl"))
        say(f"  {len(sources)} translated; compiling to SPIR-V")
        failed = self.compile_spirv(hlsl, spirv, sources, run.log_path)
        # The packer packs what compiled and lists the rest; a shader that is missing only skips its draws. More than a
        # few failures means something is wrong with the tools, not with a shader.
        if len(failed) > len(sources) // 50:
            fail(f"{len(failed)} of {len(sources)} shaders did not compile: see {run.log_path}")
        if failed:
            say(f"  {len(failed)} shaders did not compile and are left out ({', '.join(failed[:3])}...)")
        pack = os.path.join(shaders, f"{program}_shaders.spvpack")
        if os.path.exists(pack):
            os.remove(pack)
        run.run([os.path.join(self.host(), "bo2zm_pack.exe"), unique, spirv, pack])
        say(f"  shader library: {os.path.getsize(pack) / 1e6:.0f} MB")
        self.shader_index(run, program)
        for d in (unique, hlsl, spirv):  # about 2 GB that only the packing needed
            shutil.rmtree(d, ignore_errors=True)

    def shader_index(self, run, program):
        """The library's index (<program>_shaders.spvidx beside the pack), which the game otherwise makes on its first
        start (about a minute on a console). It depends only on the pack, so it is made here, with the game's own
        code, when it is missing or older than the pack. None if the runtime's packer is too old to make it: the game
        then makes it itself."""
        pack = os.path.join(self.work, "shaders", program, f"{program}_shaders.spvpack")
        index = os.path.splitext(pack)[0] + ".spvidx"
        if not os.path.isfile(pack):
            return None
        if os.path.isfile(index) and os.path.getmtime(index) >= os.path.getmtime(pack):
            return index
        if os.path.exists(index):
            os.remove(index)
        with open(run.log_path, "a", encoding="utf-8", errors="replace") as log:
            log.write(f"\n$ bo2zm_pack.exe --index {pack}\n")
            r = subprocess.run([os.path.join(self.host(), "bo2zm_pack.exe"), "--index", pack], stdout=log,
                               stderr=subprocess.STDOUT)
        if r.returncode != 0 or not os.path.isfile(index):
            say("  (this runtime's packer cannot index the shader library: the game indexes it on its first start)")
            return None
        say(f"  shader index: {os.path.basename(index)}")
        return index

    def lay_out_shaders(self, run, program, dst):
        """The program's shader library and its index into the install folder."""
        shaders = os.path.join(self.work, "shaders", program)
        link_or_copy(os.path.join(shaders, f"{program}_shaders.spvpack"),
                     os.path.join(dst, f"{program}_shaders.spvpack"))
        index = self.shader_index(run, program)
        if index:
            link_or_copy(index, os.path.join(dst, f"{program}_shaders.spvidx"))

    def key_image(self):
        # Every zone is encrypted with the same key, which ff_shaders.py rebuilds from either decrypted program.
        dump = os.path.join(self.src, "work", "dump_tu18")
        return os.path.join(dump, "default_mp.image.bin" if self.main_program() else "default.image.bin")

    def step_shaders(self, run):
        self.build_library(run, "bo2zm", self.key_image())

    def step_shaders_campaign(self, run):
        self.build_library(run, "bo2cp", self.key_image())

    def compile_spirv(self, hlsl, spirv, sources, log_path):
        dxc = os.path.join(self.host(), "dxc.exe")
        val = os.path.join(self.host(), "spirv-val.exe")
        done = [0]
        started = time.time()

        def one(name):
            base = name[:-5]
            kind, extra = ("ps_6_6", []) if base.startswith("p_") else ("vs_6_6", ["-fvk-invert-y"])
            out = os.path.join(spirv, base + ".spv")
            r = subprocess.run([dxc, "-spirv", "-T", kind, "-E", "main", "-HV", "2021", "-fspv-target-env=vulkan1.2",
                                "-fvk-use-dx-layout", *extra, "-Fo", out, os.path.join(hlsl, name)],
                               capture_output=True, text=True, errors="replace")
            if r.returncode == 0:
                r = subprocess.run([val, "--target-env", "vulkan1.2", "--scalar-block-layout", out],
                                   capture_output=True, text=True, errors="replace")
            done[0] += 1
            if done[0] % 2000 == 0:
                say(f"    {done[0]} of {len(sources)} ({seconds_text(time.time() - started)})")
            return base, r.returncode, (r.stdout + r.stderr).strip()[:300]

        failed = []
        with ThreadPoolExecutor(max_workers=os.cpu_count() or 4) as pool, \
                open(log_path, "a", encoding="utf-8", errors="replace") as log:
            for base, rc, text in pool.map(one, sources):
                if rc != 0:
                    failed.append(base)
                    log.write(f"{base}: {text}\n")
        return failed

    def step_icons(self, run):
        """bo2zm.exe's icon and the Homebrew Menu's from the game's own tile, in the title update package's header.
        The build scripts pick them up from the data folder's art/."""
        art = os.path.join(self.data, "art")
        os.makedirs(art, exist_ok=True)
        png = game_files.title_thumbnail(self.found.tu18) if self.found.tu18 else None
        if not png:
            say("  no tile in the title update package: the programs get no icon")
            return
        png_path = os.path.join(art, "tile.png")
        with open(png_path, "wb") as f:
            f.write(png)
        ico = write_ico(png, os.path.join(art, "icon_pc.ico"))
        jpg = os.name == "nt" and write_jpeg(png_path, os.path.join(art, "icon_switch.jpg"))
        say(f"  Windows icon {'made' if ico else 'not made'}, Switch icon {'made' if jpg else 'not made'}")

    def build_env(self):
        extra = {}
        if self.args.build_id:
            extra["BO2ZM_BUILD_ID"] = self.args.build_id
        return extra

    def step_build_switch(self, run):
        extra = dict(self.build_env(), BO2_LTO="1" if self.lto() else "0")
        if self.args.pgo:
            extra.update(BO2_PGO="use", BO2_PGO_DIR=os.path.abspath(self.args.pgo).replace("\\", "/"))
        run.run([self.bash, "app/build_switch.sh"], cwd=self.src, env=self.env(**extra))

    def step_build_switch_campaign(self, run):
        run.run([self.bash, "campaign/build_switch.sh"], cwd=self.src,
                env=self.env(BO2_LTO="1" if self.lto() else "0"))

    def step_build_windows(self, run):
        run.run([self.bash, "app/build_windows.sh"], cwd=self.src, env=self.env(**self.build_env()))

    def step_build_windows_campaign(self, run):
        run.run([self.bash, "campaign/build_windows.sh"], cwd=self.src, env=self.env())

    def lay_out_game(self, dst):
        """game/, update/ and the DLC in user/ (hard links when on the same drive)."""
        game_files.sync_folder(self.tree(os.path.join(self.data, "game")), os.path.join(dst, "game"))
        game_files.sync_folder(self.tree(os.path.join(self.data, "update")), os.path.join(dst, "update"))
        dlc = os.path.join(self.data, "content", "0000000000000000")
        user_dlc = os.path.join(dst, "user", "0000000000000000")
        # Only the DLC folder of user/ is ours: saves and settings next to it are never touched.
        game_files.sync_folder(self.tree(dlc), user_dlc)

    @staticmethod
    def tree(root):
        files = {}
        for dp, _, fn in os.walk(root):
            for n in fn:
                files[os.path.relpath(os.path.join(dp, n), root)] = os.path.join(dp, n)
        return files

    def toml_text(self, path, boot_zombies):
        text = open(path, encoding="utf-8").read()
        return re.sub(r"(?m)^bo2zm_boot_zombies\s*=\s*\w+", f"bo2zm_boot_zombies = {boot_zombies}", text)

    def write(self, path, text, newline="\n"):
        os.makedirs(os.path.dirname(path), exist_ok=True)
        with open(path, "w", encoding="utf-8", newline=newline) as f:
            f.write(text)

    def step_layout_switch(self, run):
        root = os.path.join(self.out, "switch")
        dst = os.path.join(root, "bo2zm")
        os.makedirs(dst, exist_ok=True)
        boot_zombies = "true" if "zm" in self.modes else "false"
        if self.main_program():
            link_or_copy(os.path.join(self.src, "app", "out", "switch", "bo2zm.nro"), os.path.join(dst, "bo2zm.nro"))
            self.write(os.path.join(dst, "bo2zm.toml"),
                       self.toml_text(os.path.join(self.src, "app", "switch", "bo2zm.toml"), boot_zombies))
            self.lay_out_shaders(run, "bo2zm", dst)
        if self.campaign():
            link_or_copy(os.path.join(self.src, "campaign", "out", "switch", "bo2cp.nro"),
                         os.path.join(dst, "bo2cp.nro"))
            text = open(os.path.join(self.src, "campaign", "switch", "bo2cp.toml"), encoding="utf-8").read()
            self.write(os.path.join(dst, "bo2cp.toml"), text)
            self.lay_out_shaders(run, "bo2cp", dst)
        self.copy_bots(dst)
        self.lay_out_game(dst)
        self.remove_unchosen(dst, (".nro", ".toml"))
        start = "bo2zm" if self.main_program() else "bo2cp"
        self.write(os.path.join(root, "README.txt"), SWITCH_README.format(name=NAME, start=start), "\r\n")
        say(f"  {dst}")

    def copy_bots(self, dst):
        target = os.path.join(dst, BOTS_PACK)
        if self.bots_pack and "zm" in self.modes:
            link_or_copy(self.bots_pack, target)
        elif os.path.isfile(target):
            os.remove(target)

    def remove_unchosen(self, dst, extensions):
        """A program and library left by an earlier run with other modes."""
        keep = set()
        if self.main_program():
            keep |= {"bo2zm"}
        if self.campaign():
            keep |= {"bo2cp"}
        for name in os.listdir(dst):
            stem, ext = os.path.splitext(name)
            program = stem.split("_")[0]
            if program in ("bo2zm", "bo2cp") and program not in keep and (ext in extensions or ext == ".spvpack"
                                                                       or ext == ".spvidx"):
                if stem != "bo2zm_bots":
                    os.remove(os.path.join(dst, name))

    def step_layout_windows(self, run):
        root = os.path.join(self.out, "windows")
        dst = os.path.join(root, "bo2zm")
        os.makedirs(dst, exist_ok=True)
        main_out = os.path.join(self.src, "app", "out", "win")
        cp_out = os.path.join(self.src, "campaign", "out", "win")
        if self.main_program():
            link_or_copy(os.path.join(main_out, "bo2zm.exe"), os.path.join(dst, "bo2zm.exe"))
            self.write(os.path.join(dst, "bo2zm.toml"),
                       PC_TOML.format(name=NAME, boot_zombies="true" if "zm" in self.modes else "false"))
            self.lay_out_shaders(run, "bo2zm", dst)
        if self.campaign():
            link_or_copy(os.path.join(cp_out, "bo2cp.exe"), os.path.join(dst, "bo2cp.exe"))
            self.write(os.path.join(dst, "bo2cp.toml"), PC_CAMPAIGN_TOML.format(name=NAME))
            self.lay_out_shaders(run, "bo2cp", dst)
        # Both programs run on the same runtime libraries (the same SDK sources and options); one copy serves both.
        libs = main_out if self.main_program() else cp_out
        for name in PC_RUNTIME_FILES:
            if os.path.isfile(os.path.join(libs, name)):
                link_or_copy(os.path.join(libs, name), os.path.join(dst, name))
        self.copy_bots(dst)
        self.lay_out_game(dst)
        self.remove_unchosen(dst, (".exe", ".toml"))
        exe = "bo2zm.exe" if self.main_program() else "bo2cp.exe"
        icon = os.path.join(self.data, "art", "icon_pc.ico")
        if os.path.isfile(icon):
            shutil.copy2(icon, os.path.join(dst, "bo2zm.ico"))
            icon = os.path.join(dst, "bo2zm.ico")
        else:
            icon = os.path.join(dst, exe)
        self.write(os.path.join(dst, f"Play {NAME}.cmd"), PC_LAUNCHER.format(name=NAME, exe=exe), "\r\n")
        lnk = os.path.join(root, f"{NAME}.lnk")
        made = os.name == "nt" and make_shortcut(lnk, os.path.join(dst, exe),
                                                 f'--user_data_root="{os.path.join(dst, "user")}"', icon,
                                                 f"{NAME}: Black Ops II")
        self.write(os.path.join(root, "README.txt"), WINDOWS_README.format(name=NAME), "\r\n")
        say(f"  {dst}")
        say(f"  shortcut: {lnk if made else 'not made (start Play ' + NAME + '.cmd in the folder instead)'}")

    # --- Driver ------------------------------------------------------------------------------------------------

    def tool_signature(self):
        """The tool's own scripts (their contents) and host tools (their sizes): a newer tool unpacked over an older one,
        or next to its work folder, copies its scripts again, and the steps after run again with them."""
        h = hashlib.sha256()
        for root, by_content in ((self.source, True), (self.host(), False)):
            if not os.path.isdir(root):
                continue
            for dp, dns, fns in os.walk(root):
                dns.sort()
                for n in sorted(fns):
                    path = os.path.join(dp, n)
                    rel = os.path.relpath(path, root).replace(os.sep, "/")
                    if rel.startswith("__pycache__/") or "/__pycache__/" in rel:
                        continue
                    h.update(rel.encode() + b"\0")
                    if by_content:
                        with open(path, "rb") as f:
                            h.update(hashlib.sha256(f.read()).digest())
                    else:
                        h.update(str(os.path.getsize(path)).encode())
        return h.hexdigest()[:16]

    def options(self):
        """What each step's result depends on besides its inputs: a step done with other values runs again."""
        dlc = sorted(p.file_name for p in self.packages) if self.found else None
        return {"source": [self.tool_signature()],
                "game": [sorted(self.modes), dlc],
                "decrypt": [self.main_program(), self.campaign()],
                "shaders": [sorted({"zm", "mp"} & set(self.modes))],
                "build-switch": [self.lto(), bool(self.args.pgo)],
                "build-switch-campaign": [self.lto()],
                "layout-switch": [sorted(self.modes), bool(self.bots_pack)],
                "layout-windows": [sorted(self.modes), bool(self.bots_pack)]}

    def list_steps(self):
        for s in self.steps():
            d = self.state.get("steps", {}).get(s.name)
            say(f"  {s.name:24} {'done in ' + seconds_text(d['seconds']) if d else 'to do':18} {s.what}")

    def dependents(self, name, steps):
        """Every step that needs name, directly or through another step."""
        found, changed = set(), True
        while changed:
            changed = False
            for s in steps:
                if s.name not in found and (name in s.needs or found & set(s.needs)):
                    found.add(s.name)
                    changed = True
        return found

    def run(self):
        steps = self.steps()
        names = [s.name for s in steps]
        done = self.state.setdefault("steps", {})
        if self.args.redo:
            if self.args.redo not in names:
                fail(f"--redo: no step called {self.args.redo} (steps: {', '.join(names)})")
            for n in {self.args.redo} | self.dependents(self.args.redo, steps):
                done.pop(n, None)
        self.check()
        options = self.options()
        for name, opts in options.items():
            if name in done and done[name].get("options") != opts:
                done.pop(name)
                for n in self.dependents(name, steps):
                    done.pop(n, None)
        self.save_state()
        total = time.time()
        for i, s in enumerate(steps, 1):
            if s.name in done:
                say(f"[{i}/{len(steps)}] {s.what}: done earlier")
                continue
            for n in self.dependents(s.name, steps):
                done.pop(n, None)
            say(f"[{i}/{len(steps)}] {s.what}")
            started = time.time()
            s.method(Runner(os.path.join(self.work, "logs", s.name + ".log")))
            done[s.name] = {"seconds": round(time.time() - started, 1), "options": options.get(s.name)}
            self.save_state()
            say(f"    done in {seconds_text(done[s.name]['seconds'])}")
        say()
        say(f"Finished in {seconds_text(time.time() - total)}.")
        self.report()

    def report(self):
        """Where the results are, said once every step is done."""
        if "switch" in self.targets:
            say(f"  Switch:  copy the bo2zm folder from {os.path.join(self.out, 'switch')} into sdmc:/switch/ "
                "on your SD card.")
        if "windows" in self.targets:
            say(f"  Windows: start the game with the {NAME} shortcut in {os.path.join(self.out, 'windows')}.")


SWITCH_README = """{name} for the Nintendo Switch
==============================

Copy the bo2zm folder into the switch folder on your SD card, so that it reads sdmc:/switch/bo2zm/.

Start {start} from the Homebrew Menu opened through a game (hold R while starting one) or from a forwarder, so the
game gets the console's full memory. Each map compiles its pipelines the first time it is played.

The folder holds your own game files: keep it for your own consoles.
"""

WINDOWS_README = """{name} for Windows
==================

Start the game with the {name} shortcut in this folder, or with "Play {name}.cmd" inside bo2zm (that one keeps
working if you move the bo2zm folder; the shortcut points to where the folder is now). Both keep your saves, settings
and DLC in bo2zm\\user. The game needs a GPU with Vulkan 1.3.

Settings on the command line
----------------------------
A few settings have no menu in the game. Add them to the shortcut (right-click it, Properties, and add them at the end
of Target, after a space) or after the .cmd's name in a command prompt. The same lines also work in bo2zm\\bo2zm.toml
without the two dashes (fullscreen = false).

  --fullscreen=false               a window instead of full screen
  --window_width=1600 --window_height=900   the window's size
  --monitor=2                      the monitor to open on (1 = the main one)
  --bo2zm_preset=medium            graphics preset: lowest, very_low, low, medium, high (Options > Graphics too)
  --bo2zm_fps_limit=30             a steady 30 FPS instead of 60
  --bo2zm_show_fps=true            frames per second in the top-left corner
  --vulkan_allow_present_mode_immediate=true --vulkan_allow_present_mode_mailbox=true
                                   lowest latency when you don't stream (bo2zm.toml presents in step with the display,
                                   so Discord or OBS window capture stays live)
  --input_xbox_layout=true         face buttons by position, as on an Xbox controller
  --offline_user_count=2           local profiles for split screen (1 to 4)

Playing with others
-------------------
System Link (same network) and internet play (ZOMBIES MULTIPLAYER, with a join code) work from this folder as it is,
with Switch and Windows players together. Allow the game through the Windows firewall the first time it asks.

The folder holds your own game files: keep it for yourself.
"""


def parse_list(value, allowed, aliases):
    if isinstance(value, list):
        items = value
    else:
        items = []
        for part in re.split(r"[\s,]+", value or ""):
            if part:
                items += aliases.get(part, [part])
    bad = [i for i in items if i not in allowed]
    if bad or not items:
        raise SystemExit(f"expected one or more of {', '.join(allowed)} (got {value!r})")
    return [a for a in allowed if a in items]


def parser():
    p = argparse.ArgumentParser(description=__doc__.split("\n")[0], formatter_class=argparse.RawTextHelpFormatter)
    p.add_argument("--game-files", "--disc", "--title-update", "--dlc", action="append", dest="game_files")
    p.add_argument("--no-dlc", action="store_true")
    p.add_argument("--out", required=True)
    p.add_argument("--work")
    p.add_argument("--target", default="switch")
    p.add_argument("--modes", default="zm")
    p.add_argument("--bots-pack")
    p.add_argument("--pgo")
    p.add_argument("--jobs", type=int)
    p.add_argument("--no-lto", action="store_true")
    p.add_argument("--low-memory", action="store_true")
    p.add_argument("--redo")
    p.add_argument("--list-steps", action="store_true")
    p.add_argument("--kit")
    p.add_argument("--build-id", help=argparse.SUPPRESS)  # a fixed build id, to compare two builds byte for byte
    return p


def main(argv=None):
    p = parser()
    args = p.parse_args(argv)
    inst = Installer(args)
    if args.list_steps:
        inst.list_steps()
        return 0
    if not args.game_files:
        args.game_files = inst.state.get("game_files")
    if not args.game_files:
        p.error("--game-files is required")
    inst.state["game_files"] = [os.path.abspath(f) for f in args.game_files]
    try:
        inst.run()
    except BuildError as e:
        say()
        say("ERROR: " + str(e))
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
