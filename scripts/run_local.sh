#!/bin/sh
# Lanza 2 nodos en la misma maquina y verifica que ambos terminen en PASS.
cd "$(dirname "$0")/.." || exit 1
CONF=${1:-dsm.conf}
DSM_NODE_ID=1 ./build/test_basic "$CONF" > build/n1.log 2>&1 &
P1=$!
DSM_NODE_ID=0 ./build/test_basic "$CONF" > build/n0.log 2>&1 &
P0=$!
wait $P0; R0=$?
wait $P1; R1=$?
echo "--- nodo 0 ---"; cat build/n0.log
echo "--- nodo 1 ---"; cat build/n1.log
echo "codigos de salida: n0=$R0 n1=$R1"
[ $R0 -eq 0 ] && [ $R1 -eq 0 ]
