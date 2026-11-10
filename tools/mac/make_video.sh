#!/bin/bash
# Turns a capture into an mp4 plus a contact sheet. Input is either a raw rgba file with a .txt beside it
# ("<w> <h> <fps> <count>", the format peggle1-nx records) or a folder of PNGs (bo2zm_capture_every_s).
#   make_video.sh <capture.rgba | captures/> <out.mp4> [fps]
# Adapted from peggle1-nx tools/make_video.sh. Needs ffmpeg (FFMPEG=/path to override).
set -euo pipefail
in="${1:?capture.rgba or a PNG folder}"; out="${2:?out.mp4}"; fps="${3:-}"
FFMPEG="${FFMPEG:-$(command -v ffmpeg || true)}"; [ -x "${FFMPEG:-}" ] || { echo "ffmpeg not found (brew install ffmpeg)" >&2; exit 2; }
if [ -d "$in" ]; then
  [ -n "$fps" ] || fps=1
  "$FFMPEG" -y -loglevel error -framerate "$fps" -pattern_type glob -i "$in/*.png" -c:v libx264 -pix_fmt yuv420p -crf 18 "$out"
else
  read -r w h f n < "${in%.rgba}.txt"; [ -n "$fps" ] || fps="$f"
  "$FFMPEG" -y -loglevel error -f rawvideo -pix_fmt rgb0 -s "${w}x${h}" -framerate "$fps" -i "$in" \
    -c:v libx264 -pix_fmt yuv420p -crf 18 "$out"
fi
# contact sheet: 24 evenly spaced frames, 6 x 4, each 320 wide
sheet="${out%.mp4}-sheet.jpg"
n=$("$FFMPEG" -i "$out" -map 0:v:0 -c copy -f null - 2>&1 | grep -o 'frame= *[0-9]*' | tail -1 | grep -o '[0-9]*' || echo 24)
step=$(( (${n:-24} / 24) > 0 ? ${n:-24} / 24 : 1 ))
"$FFMPEG" -y -loglevel error -i "$out" -vf "select='not(mod(n,$step))',scale=320:-1,tile=6x4" -frames:v 1 "$sheet" || true
echo "video: $out"; [ -f "$sheet" ] && echo "sheet: $sheet"
