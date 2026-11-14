#!/usr/bin/env bash
# Builds the AArch64 Linux sysroot that games/bo2/build_linux.sh compiles and links against.
#
#   tools/linux/make_sysroot.sh [<output folder>]     (default: out/linux-sysroot, under the checkout)
#
# The sysroot is a plain folder of Ubuntu 24.04 (noble) arm64 packages: glibc, libstdc++ 14 and the development
# headers SDL needs for X11, Wayland, ALSA, PulseAudio and PipeWire. Packages are only unpacked (mmdebstrap's extract
# variant), so no ARM code runs and no emulator is needed to make it.
#
# Reproducibility: only noble's release pocket is used, never noble-updates or noble-security. The release pocket has
# not changed since Ubuntu 24.04 shipped, so every run picks the same package versions. The list of what went in is
# written to <output>/packages.txt; tools/linux/sysroot-packages.txt is the list a known-good build used, and the
# script warns if the two differ.
#
# The game itself links glibc dynamically (so the player's own system libraries, Vulkan driver and audio server are
# used) and libstdc++ statically. The binary therefore needs glibc 2.38 or newer: Ubuntu 24.04, Debian 13, Fedora 39,
# Raspberry Pi OS (Debian 13 based) and newer.
#
# For the QEMU smoke test (tools/linux/smoke_qemu.sh) the sysroot also carries the runtime side: the Vulkan loader
# and Mesa's software driver (lavapipe). The build itself never uses them.
#
# Needs: mmdebstrap (apt install mmdebstrap) and network access to ports.ubuntu.com. Run as root (Docker, WSL) or
# with unprivileged user namespaces (mmdebstrap picks the mode).
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
OUT="${1:-$ROOT/out/linux-sysroot}"
MIRROR="${BO2ZM_UBUNTU_PORTS:-http://ports.ubuntu.com/ubuntu-ports}"

PACKAGES=(
  # C and C++ runtime
  libc6-dev libstdc++-14-dev libgcc-14-dev linux-libc-dev
  # SDL3's video drivers (both are built; SDL picks one at runtime) and the SDK's surface code
  libx11-dev libx11-xcb-dev libxcb1-dev libxext-dev libxrandr-dev libxcursor-dev libxi-dev libxfixes-dev libxss-dev
  libxtst-dev libxkbcommon-dev libwayland-dev wayland-protocols libdecor-0-dev libegl-dev libdrm-dev libgbm-dev
  # SDL3's audio drivers (loaded at runtime) and input
  libasound2-dev libpulse-dev libpipewire-0.3-dev libudev-dev libdbus-1-dev
  # Runtime only, for the QEMU smoke test: the Vulkan loader and lavapipe
  libvulkan1 mesa-vulkan-drivers
)

if [ -e "$OUT/usr/include/stdio.h" ]; then
  echo "sysroot already present: $OUT (delete it to rebuild)"
  exit 0
fi
mkdir -p "$(dirname "$OUT")"
TMP="$OUT.tmp"
rm -rf "$TMP"

IFS=,
mmdebstrap --variant=extract --arch=arm64 --components=main,universe --include="${PACKAGES[*]}" \
  --skip=cleanup/apt/cache noble "$TMP" "deb $MIRROR noble main universe"
unset IFS

# What went in, from the downloaded packages' names (name_version_arch.deb); the extract variant writes no dpkg
# status. Then the packages themselves go.
for f in "$TMP"/var/cache/apt/archives/*.deb; do
  b="$(basename "$f" .deb)"
  echo "${b%_*}"
done | sort > "$TMP/packages.txt"
rm -f "$TMP"/var/cache/apt/archives/*.deb

# Ubuntu's merged /usr: the extract variant unpacks into /usr only, but glibc's linker scripts and the program
# interpreter (/lib/ld-linux-aarch64.so.1) name /lib.
for d in bin sbin lib; do
  [ -e "$TMP/$d" ] || ln -s "usr/$d" "$TMP/$d"
done

# libstdc++ 14's first release (noble's release pocket has 14-20240412) has a typo in bits/unicode.h, which <format>
# includes: "++this;" in _Grapheme_cluster_view::_Iterator::operator++(int). Clang 19 and newer reject it in every
# file that includes <format>. GCC 14.2 fixed it to "++*this;"; apply the same fix here.
UNICODE_H="$TMP/usr/include/c++/14/bits/unicode.h"
if grep -q '^[[:space:]]*++this;$' "$UNICODE_H"; then
  sed -i 's/^\([[:space:]]*\)++this;$/\1++*this;/' "$UNICODE_H"
  echo "patched $UNICODE_H (++this -> ++*this)"
fi

# Absolute symbolic links inside the sysroot (Ubuntu's dev packages have a few, e.g. some libfoo.so) point at the
# build machine's own /usr. Make them relative to the sysroot.
find "$TMP" -type l -lname '/*' | while read -r link; do
  target="$(readlink "$link")"
  rel="$(realpath -m --relative-to="$(dirname "$link")" "$TMP$target")"
  ln -sfn "$rel" "$link"
done

mv "$TMP" "$OUT"
echo "sysroot: $OUT ($(wc -l < "$OUT/packages.txt") packages)"
if [ -f "$ROOT/tools/linux/sysroot-packages.txt" ] && ! diff -q "$ROOT/tools/linux/sysroot-packages.txt" "$OUT/packages.txt" > /dev/null; then
  echo "warning: the package list differs from tools/linux/sysroot-packages.txt:" >&2
  diff "$ROOT/tools/linux/sysroot-packages.txt" "$OUT/packages.txt" >&2 || true
fi
