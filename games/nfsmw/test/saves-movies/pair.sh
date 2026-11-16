#!/bin/bash
# One job, two launches: A = fresh profile, movies + save (Down+A = Yes); B = relaunch on A's saved data, load + continue career.
set -uo pipefail
exe="$1"; out="$2"; H=/tmp/bo2/nfsmw-saves-movies
mkdir -p "$out/A" "$out/B"
NFSMW_EXTRA_ARGS="--gpu_plugin=metal --hid_virtual_pad_script=$H/r2.route --metal_frame_dump=40,100,200,300,400,500,600,700,800,8000,8300,8600,9000,9500,10500,12000 --metal_frame_dump_dir=$out/A/dump --metal_frame_dump_timeout_s=340" \
  bash $H/run.sh "$exe" "$out/A" 330; a=$?
echo "A exit $a"
cp -R "$out/A/support" "$out/B/support"
NFSMW_EXTRA_ARGS="--gpu_plugin=metal --hid_virtual_pad_script=$H/r4.route --metal_frame_dump=3000,4000,5000,6000,7000,8000,9000,10000,11000,12000,13000 --metal_frame_dump_dir=$out/B/dump --metal_frame_dump_timeout_s=340" \
  bash $H/run.sh "$exe" "$out/B" 330; b=$?
echo "B exit $b"
exit $(( a | b ))
