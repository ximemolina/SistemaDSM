#!/bin/sh
cd "$(dirname "$0")/.." || exit 1
PR_N=${PR_N:-131072}
PR_DEG=${PR_DEG:-16}
PR_ITER=${PR_ITER:-12}
HEAT_NZ=${HEAT_NZ:-2048}
HEAT_STEPS=${HEAT_STEPS:-8}

DSM_NODE_ID=1 ./build/app_pagerank dsm.local.conf "$PR_N" "$PR_DEG" "$PR_ITER" > build/pagerank_n1.log 2>&1 &
P1=$!
DSM_NODE_ID=0 ./build/app_pagerank dsm.local.conf "$PR_N" "$PR_DEG" "$PR_ITER" > build/pagerank_n0.log 2>&1 &
P0=$!
DSM_NODE_ID=1 ./build/app_heat3d dsm.local2.conf "$HEAT_NZ" "$HEAT_STEPS" > build/heat3d_n1.log 2>&1 &
H1=$!
DSM_NODE_ID=0 ./build/app_heat3d dsm.local2.conf "$HEAT_NZ" "$HEAT_STEPS" > build/heat3d_n0.log 2>&1 &
H0=$!

wait $P0; R0=$?
wait $P1; R1=$?
wait $H0; S0=$?
wait $H1; S1=$?
echo "--- pagerank n0 ---"; cat build/pagerank_n0.log
echo "--- pagerank n1 ---"; cat build/pagerank_n1.log
echo "--- heat3d n0 ---"; cat build/heat3d_n0.log
echo "--- heat3d n1 ---"; cat build/heat3d_n1.log
echo "codigos: pagerank n0=$R0 n1=$R1 | heat3d n0=$S0 n1=$S1"
[ $R0 -eq 0 ] && [ $R1 -eq 0 ] && [ $S0 -eq 0 ] && [ $S1 -eq 0 ]
