#!/bin/sh
if [ $# -ne 2 ]; then
    echo "uso: $0 <ip_nodo0> <ip_nodo1>" >&2
    exit 64
fi
ip0=$1
ip1=$2
cd "$(dirname "$0")/.." || exit 1
sed -i "s/^node = 0 .*/node = 0 $ip0 27800/" dsm.conf
sed -i "s/^node = 1 .*/node = 1 $ip1 27801/" dsm.conf
sed -i "s/^node = 0 .*/node = 0 $ip0 27810/" dsm.lab2.conf
sed -i "s/^node = 1 .*/node = 1 $ip1 27811/" dsm.lab2.conf
echo "dsm.conf:      nodo0=$ip0:27800 nodo1=$ip1:27801"
echo "dsm.lab2.conf: nodo0=$ip0:27810 nodo1=$ip1:27811"
