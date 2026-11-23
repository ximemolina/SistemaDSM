#!/bin/sh
cd "$(dirname "$0")/.." || exit 1
PR_N=${PR_N:-98304}
PR_DEG=${PR_DEG:-16}
PR_ITER=${PR_ITER:-12}
HEAT_NZ=${HEAT_NZ:-2049}
HEAT_STEPS=${HEAT_STEPS:-8}

for n in 0 1 2; do
    DSM_NODE_ID=$n ./build/app_pagerank dsm.local3.conf "$PR_N" "$PR_DEG" "$PR_ITER" > "build/pr3_n$n.log" 2>&1 &
done
wait
for n in 0 1 2; do
    DSM_NODE_ID=$n ./build/app_heat3d dsm.local3.conf "$HEAT_NZ" "$HEAT_STEPS" > "build/heat3_n$n.log" 2>&1 &
done
wait
for n in 0 1 2; do echo "--- pagerank n$n ---"; cat "build/pr3_n$n.log"; done
for n in 0 1 2; do echo "--- heat3d n$n ---"; cat "build/heat3_n$n.log"; done
