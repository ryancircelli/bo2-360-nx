#!/usr/bin/env bash
# Starts the Linux ARM64 build on an x86_64 Linux machine under QEMU user-mode emulation, with Mesa's software Vulkan
# driver (lavapipe) from the sysroot and a virtual X server, to check that it runs as far as it can without ARM
# hardware: it starts, reads its config, opens the game files and creates its Vulkan device. Emulation is far too slow
# to play; the run stops after a time limit. With QEMU 10 (Debian 13's static build) it reaches the Zombies main menu in
# about a minute. Ubuntu 24.04's QEMU 8.2 crashes on its own after a while ("QEMU internal SIGSEGV"), at a different
# point each run: that is the emulator, not the game; set BO2_QEMU to a newer qemu-aarch64.
#
#   tools/linux/smoke_qemu.sh <package folder> <data folder> [seconds] [extra game arguments...]
#
#   <package folder>  e.g. games/bo2/out/linux-arm64/package/bo2zm-linux-arm64
#   <data folder>     holds game/, update/ and bo2zm_shaders.spvpack/.spvidx (read only; linked, never copied)
#   seconds           time limit (default 600)
#
# Needs qemu-user-static and xvfb (Xvfb, xauth) on the host, and the sysroot (BO2_LINUX_SYSROOT, default
# out/linux-sysroot). BO2_QEMU names another qemu-aarch64 binary (e.g. a newer static build unpacked from its package).
# The run folder is a scratch copy under $BO2_SMOKE_DIR (default /tmp/bo2zm-smoke); its log is bo2zm-smoke.log there.
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
PKG="$(cd "${1:?usage: smoke_qemu.sh <package folder> <data folder> [seconds] [args...]}" && pwd)"
DATA="$(cd "${2:?usage: smoke_qemu.sh <package folder> <data folder> [seconds] [args...]}" && pwd)"
SECS="${3:-600}"
shift 2
[ $# -gt 0 ] && shift
SYSROOT="$(cd "${BO2_LINUX_SYSROOT:-$ROOT/out/linux-sysroot}" && pwd)"
RUN="${BO2_SMOKE_DIR:-/tmp/bo2zm-smoke}"

# The executable is copied, not linked: the game finds its shader library and config next to /proc/self/exe.
rm -rf "$RUN"
mkdir -p "$RUN"
cp "$PKG"/bo2zm "$PKG"/*.so "$RUN/"
for f in game update bo2zm_shaders.spvpack bo2zm_shaders.spvidx; do
  [ -e "$DATA/$f" ] && ln -s "$DATA/$f" "$RUN/$f"
done

# Lavapipe and the Vulkan loader from the sysroot: QEMU's -L resolves the absolute paths in the ICD file and the
# loader's search folders inside the sysroot first.
# QEMU_LD_PREFIX does the same for the relaunch: it runs /proc/self/exe again, which the kernel hands to QEMU's
# binfmt handler without -L.
export QEMU_LD_PREFIX="$SYSROOT"
export VK_DRIVER_FILES="/usr/share/vulkan/icd.d/lvp_icd.aarch64.json"
export VK_ICD_FILENAMES="$VK_DRIVER_FILES"
export SDL_VIDEO_DRIVER=x11
unset WAYLAND_DISPLAY
cd "$RUN"
echo "smoke: $RUN, up to $SECS s"
status=0
# timeout around QEMU itself, inside xvfb-run: it stops the game (SIGINT, then SIGKILL; QEMU hands SIGINT to the game,
# which may not stop on it), and xvfb-run then closes the X server. Around xvfb-run it would kill only the script.
xvfb-run -a -s "-screen 0 1280x720x24" \
  timeout --signal=INT --kill-after=30 "$SECS" \
  "${BO2_QEMU:-qemu-aarch64-static}" -L "$SYSROOT" ./bo2zm \
    --log_file="$RUN/bo2zm-smoke.log" --log_level=info \
    --bo2zm_renderer=native --bo2zm_volume=0 --fullscreen=false "$@" || status=$?
echo "smoke: exit status $status (124 = stopped at the time limit)"
tail -n 40 "$RUN/bo2zm-smoke.log" 2>/dev/null || true
