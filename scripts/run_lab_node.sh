#!/usr/bin/env bash
if [ $# -lt 3 ]; then
    echo "uso: $0 <app_pagerank|app_heat3d> <config> <0|1> [args...]" >&2
    exit 64
fi
app=$1
conf=$2
id=$3
shift 3
cd "$(dirname "$0")/.." || exit 1
mkdir -p build
if [ -n "$DSM_VERBOSE" ]; then export DSM_VERBOSE=1; fi
log="build/lab_${app}_n${id}.log"
DSM_NODE_ID=$id "./build/$app" "$conf" "$@" 2>&1 | tee "$log"
rc=${PIPESTATUS[0]}
echo "codigo de salida n$id: $rc | log: $log"
exit "$rc"
