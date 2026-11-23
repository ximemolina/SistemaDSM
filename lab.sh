#!/usr/bin/env bash
# Uso: bash lab.sh info | udp-recv | udp-send | tasks | rep [N]
cd "$(dirname "$0")" || exit 1
mkdir -p build
CONF=${CONF:-dsm.conf}
[ -f "$CONF" ] || { echo "No existe $CONF"; exit 1; }

ACTIVE=$(grep -vE '^[[:space:]]*(#|$)' "$CONF" | sed -E 's/[[:space:]]*#.*$//')
OFFSET=$(echo "$ACTIVE" | awk -F= '/^[[:space:]]*heartbeat_port_offset/ {gsub(/[[:space:]]/,"",$2); print $2}')
OFFSET=${OFFSET:-1000}
NODES=$(echo "$ACTIVE" | awk '/^[[:space:]]*node[[:space:]]*=/ {sub(/^[^=]*=/,""); print $1, $2, $3}')
MYIPS=$(ip -4 -o addr show | awk '{print $4}' | cut -d/ -f1)

ID=""; MYPORT=""
while read -r nid nip nport; do
  for ip in $MYIPS; do
    if [ "$ip" = "$nip" ]; then ID=$nid; MYPORT=$nport; fi
  done
done <<< "$NODES"

dur() { awk -v a="$1" -v b="$(date +%s.%N)" 'BEGIN{printf "%.1f", b-a}'; }
need() { [ -x "build/$1" ] || { echo "FALTA build/$1 ejecutable: hacer 'make build/$1' en este equipo"; exit 1; }; }
hdr() { echo "[lab] $(date +%T) nodo=$ID conf=$CONF huella_config=$(echo "$ACTIVE" | md5sum | cut -c1-12) UDP_propio=$((MYPORT+OFFSET))"; }

cmd=${1:-info}
if [ "$cmd" != info ] && [ -z "$ID" ]; then
  echo "Ninguna IP de este equipo coincide con las lineas node= de $CONF"
  echo "IPs de este equipo: $MYIPS"; echo "$NODES"; exit 1
fi

case "$cmd" in
info)
  echo "=== INFO $(date)"; hostname; uname -m; uname -r; nproc
  git log --oneline 2>/dev/null | head -1 || true
  echo "huella_codigo: $(cat src/*.c src/*.h apps/*.c Makefile 2>/dev/null | md5sum | cut -c1-12)"
  echo "huella_config: $(echo "$ACTIVE" | md5sum | cut -c1-12)"
  echo "--- configuracion activa:"; echo "$ACTIVE"
  echo "--- IPs de este equipo: $MYIPS"
  if [ -n "$ID" ]; then echo "--- ESTE EQUIPO ES EL NODO $ID (TCP $MYPORT, UDP $((MYPORT+OFFSET)))"
  else echo "--- ATENCION: ninguna IP local coincide con las lineas node= (revisar $CONF)"; fi
  ls -l build/test_basic build/app_tasks 2>&1
  command -v ufw >/dev/null && { echo "--- ufw:"; ufw status 2>&1 | head -3; }
  while read -r nid nip nport; do
    [ "$nid" = "$ID" ] && continue
    echo "--- ping al nodo $nid ($nip):"; ping -c 2 -W 2 "$nip" </dev/null 2>&1 | tail -3
  done <<< "$NODES"
  ;;
udp-recv)
  hdr
  python3 - "$((MYPORT+OFFSET))" <<'PY'
import socket,sys,time
p=int(sys.argv[1])
s=socket.socket(socket.AF_INET,socket.SOCK_DGRAM)
s.bind(('0.0.0.0',p)); s.settimeout(1.0)
print(f"escuchando UDP {p} durante 30 s ...",flush=True)
t0=time.time(); n=0
while time.time()-t0<30:
    try:
        d,a=s.recvfrom(1024); n+=1
        print("RECIBIDO de",a[0],":",d.decode(errors='replace'),flush=True)
    except socket.timeout:
        pass
print("RESULTADO UDP-RECV:", "OK (%d datagramas)"%n if n else "NADA RECIBIDO")
PY
  ;;
udp-send)
  hdr
  while read -r nid nip nport; do
    [ "$nid" = "$ID" ] && continue
    python3 - "$nip" "$((nport+OFFSET))" "$ID" <<'PY'
import socket,sys,time
ip,p,me=sys.argv[1],int(sys.argv[2]),sys.argv[3]
s=socket.socket(socket.AF_INET,socket.SOCK_DGRAM)
for i in range(1,6):
    s.sendto(f"hola desde nodo {me} #{i}".encode(),(ip,p))
    print("enviado",i,"a",ip,p,flush=True); time.sleep(1)
PY
  done <<< "$NODES"
  ;;
tasks)
  need app_tasks; hdr
  pgrep -x app_tasks >/dev/null && echo "AVISO: ya hay un app_tasks corriendo en este equipo"
  t0=$(date +%s.%N)
  DSM_VERBOSE=1 DSM_NODE_ID=$ID timeout 180 ./build/app_tasks "$CONF" 96 2>&1 | tee build/lab_tasks_n$ID.log
  rc=${PIPESTATUS[0]}
  echo "[lab] app_tasks nodo $ID: exit=$rc duracion=$(dur $t0)s"
  ;;
rep)
  need test_basic; hdr
  pgrep -x test_basic >/dev/null && echo "AVISO: ya hay un test_basic corriendo en este equipo"
  N=${2:-10}; SUM=build/rep_resumen_n$ID.txt; : > "$SUM"
  for i in $(seq 1 "$N"); do
    L=build/rep_n${ID}_$i.log
    t0=$(date +%s.%N)
    DSM_VERBOSE=1 DSM_NODE_ID=$ID timeout 120 ./build/test_basic "$CONF" > "$L" 2>&1
    rc=$?
    res=$(grep -h "RESULTADO" "$L" | tail -1)
    rt=$(grep -ho "reintentos=[0-9]*" "$L" | tail -1)
    echo "iter $i exit=$rc dur=$(dur $t0)s | $res | $rt" | tee -a "$SUM"
    if [ $rc -ne 0 ]; then echo "   ultimas lineas de $L:" | tee -a "$SUM"; tail -5 "$L" | sed 's/^/      /' | tee -a "$SUM"; fi
    sleep 5
  done
  echo "[lab] nodo $ID: $(grep -c '^iter .* exit=0 ' "$SUM") de $N iteraciones con exit=0" | tee -a "$SUM"
  ;;
*) echo "Uso: bash lab.sh info | udp-recv | udp-send | tasks | rep [N]";;
esac
