#!/usr/bin/env bash
# Corre una bateria de tests lanzando los nodos LOCALES de esta maquina.
# Ejecutar el mismo comando, casi a la vez, en cada maquina (cada una con sus ids).
#
# Uso:  ./run_suite.sh <conf> "<ids locales>" <test> [<test> ...]
# Ej.:  ./run_suite.sh dsm4.conf "0 1" test_msi test_locks     # maquina A
#       ./run_suite.sh dsm4.conf "2 3" test_msi test_locks     # maquina B
# Variables: TMO=segundos de timeout por test (180), GAP=pausa entre tests (4)
conf="$1"; ids="$2"; shift 2
[ -z "$conf" ] || [ -z "$ids" ] || [ $# -eq 0 ] && { sed -n '2,8p' "$0"; exit 1; }
tmo="${TMO:-180}"; gap="${GAP:-4}"
mkdir -p build/suite
total_fail=0

for t in "$@"; do
  if [ ! -x "build/$t" ]; then echo "== $t: build/$t no existe o no es ejecutable"; total_fail=$((total_fail+1)); continue; fi
  echo "== $t (nodos locales: $ids)"
  declare -A pid_of=()
  for id in $ids; do
    DSM_VERBOSE="${DSM_VERBOSE:-0}" DSM_NODE_ID="$id" timeout "$tmo" "./build/$t" "$conf" > "build/suite/${t}_n${id}.log" 2>&1 &
    pid_of[$id]=$!
  done
  t_fail=0
  for id in $ids; do
    wait "${pid_of[$id]}"; rc=$?
    if [ $rc -eq 0 ]; then echo "   n$id: PASS"; else echo "   n$id: FALLO (rc=$rc$( [ $rc -eq 124 ] && echo ', TIMEOUT'))  -> build/suite/${t}_n${id}.log"; t_fail=$((t_fail+1)); fi
  done
  total_fail=$((total_fail+t_fail))
  unset pid_of
  sleep "$gap"
done

if [ $total_fail -eq 0 ]; then echo "SUITE: TODO PASS"; else echo "SUITE: $total_fail fallo(s)"; fi
[ $total_fail -eq 0 ]
