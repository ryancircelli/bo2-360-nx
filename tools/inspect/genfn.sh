#!/usr/bin/env bash
# Prints one generated function: tools/inspect/genfn.sh sub_82428ED8 [generated folder]
GEN="${2:-$(cd "$(dirname "$0")/../.." && pwd)/games/bo2/generated/default_mp}"
awk -v f="DEFINE_REX_FUNC($1)" 'index($0,f)==1{p=1} p{print} p&&/^}/{exit}' "$GEN"/bo2zm_recomp.*.cpp
