#!/usr/bin/env bash
# Phase 4b: translate BO2 shader blobs (tools/inspect/ff_shaders.py --dump) to SPIR-V and count what fails.
#   tools/xenosrecomp/run.sh <blob folder> <output folder>
# Steps: build bo2zm_hlsl (our XenosRecomp fork in translator/, BO2ZM_RECOMP on) -> HLSL -> dxc -> spirv-val.
# The output is game-derived: keep it outside the repo.
set -euo pipefail
HERE="$(cd "$(dirname "$0")" && pwd)"
ROOT="$HERE/../.."
IN="${1:?blob folder}"
OUT="${2:?output folder}"
TR="$HERE/translator"            # XenosRecomp (MIT, hedge-dev) with our changes
TP="$ROOT/sdk/thirdparty"             # fmt + xxHash (fetched by tools/fetch_thirdparty.py)
. "$ROOT/tools/toolchain.sh"  # LLVM and the Vulkan SDK (VULKAN_SDK)
VK="$VULKAN_SDK"
# Windows SDK layout (Bin/*.exe), else the macOS/Linux one (bin/), else PATH.
DXC="$VK/Bin/dxc.exe"; [ -x "$DXC" ] || DXC="$VK/bin/dxc"; [ -x "$DXC" ] || DXC="$(command -v dxc)"
VAL="$VK/Bin/spirv-val.exe"; [ -x "$VAL" ] || VAL="$VK/bin/spirv-val"; [ -x "$VAL" ] || VAL="$(command -v spirv-val)"
JOBS="${JOBS:-12}"

case "$(cd "$OUT" 2>/dev/null && pwd || echo "$OUT")" in
  "$(cd "$ROOT" && pwd)"*) echo "refusing to write game-derived output inside the repo"; exit 1;;
esac
[ -e "$OUT" ] && { echo "output folder exists: $OUT"; exit 1; }
mkdir -p "$OUT/spirv" "$ROOT/work/xenosrecomp"

EXE="$ROOT/work/xenosrecomp/bo2zm_hlsl.exe"
clang++ -std=c++23 -O1 -w \
  "-I$TR" "-I$TP/fmt/include" "-I$TP/xxHash" \
  -include "$TR/pch_min.h" -include "$HERE/assert_throw.h" \
  -DFMT_HEADER_ONLY -DXXH_INLINE_ALL -DBO2ZM_RECOMP -D_CRT_SECURE_NO_WARNINGS \
  "$HERE/bo2zm_hlsl.cpp" "$TR/shader_recompiler.cpp" -o "$EXE" \
  > "$OUT/build.log" 2>&1 || { echo "translator build failed: $OUT/build.log"; tail -20 "$OUT/build.log"; exit 1; }

"$EXE" "$IN" "$OUT/hlsl" "$TR/shader_common.h" | tee "$OUT/translate.log"

# dxc + spirv-val per shader, in parallel. One line per shader in compile.tsv.
compile_one() {
  f="$1"; base=$(basename "$f" .hlsl)
  if [[ "$base" == p_* ]]; then t=ps_6_6; extra=(); else t=vs_6_6; extra=(-fvk-invert-y); fi
  if ! "$DXC" -spirv -T "$t" -E main -HV 2021 -fspv-target-env=vulkan1.2 -fvk-use-dx-layout \
        "${extra[@]}" -Fo "$OUT/spirv/$base.spv" "$f" > "$OUT/spirv/$base.dxc.txt" 2>&1; then
    printf '%s\tdxc\t%s\n' "$base" "$(grep -m1 -E 'error' "$OUT/spirv/$base.dxc.txt" | tr '\t' ' ' | cut -c1-200)"
    return
  fi
  if ! "$VAL" --target-env vulkan1.2 --scalar-block-layout "$OUT/spirv/$base.spv" > "$OUT/spirv/$base.val.txt" 2>&1; then
    printf '%s\tspirv-val\t%s\n' "$base" "$(head -1 "$OUT/spirv/$base.val.txt" | tr '\t' ' ' | cut -c1-200)"
    return
  fi
  rm -f "$OUT/spirv/$base.dxc.txt" "$OUT/spirv/$base.val.txt"
  printf '%s\tok\t\n' "$base"
}
export -f compile_one; export OUT DXC VAL
ls "$OUT"/hlsl/*.hlsl | xargs -P "$JOBS" -I{} bash -c 'compile_one "$@"' _ {} > "$OUT/compile.tsv"

echo "translate: $(grep -c $'\tok\t' "$OUT/hlsl/translate.tsv") ok, $(grep -c $'\tfailed\t' "$OUT/hlsl/translate.tsv") failed, $(grep -c $'\tskipped\t' "$OUT/hlsl/translate.tsv") skipped"
echo "compile:   $(grep -c $'\tok\t' "$OUT/compile.tsv") ok, $(grep -c $'\tdxc\t' "$OUT/compile.tsv") dxc errors, $(grep -c $'\tspirv-val\t' "$OUT/compile.tsv") invalid SPIR-V"
# Name every shader left without SPIR-V, with the reason, so a short pack shows which ones and why.
awk -F'\t' '$3 == "failed" { printf "  translate failed: %s (%s): %s\n", $1, $2, $4 }' "$OUT/hlsl/translate.tsv"
awk -F'\t' '$2 != "ok" { printf "  %s failed: %s: %s\n", $2, $1, $3 }' "$OUT/compile.tsv"
