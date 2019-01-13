#!/bin/bash
#
# vtun 192.168.32.68/32 via tcp 192.168.10.67
#
# [ dpdk2 ] vtun <.....TCP.....> vtun [ dpdk3 ]
#
# vtun 192.168.32.66/32 via via 192.168.10.66
#

catch () {
    echo "SIGNAL"
    exit 1;
}
trap catch SIGINT SIGQUIT SIGTERM SIGHUP

cleanup () {
    echo "EXIT killing $tfspid"
    if [[ -n $tfspid ]]; then
        kill -9 $tfspid
        wait $tfspid
        unset tfspid
    fi
}
trap cleanup EXIT ERR

IPID=$(ip addr | sed -e '/192\.168\.2\./!d;s,.*192\.168\.2\.\([0-9]*\)/.*,\1,')
VMID=$((IPID - 64))

FRAMESZ=1400
TXRATEKb=$((10 * 1000 * 1000))
MAXAGG=$((1000000 / 1000))

if (( VMID == 2 )); then
    OVMID=3
    sleep 1
    build/tcptfs --dev tfs0 --connect 192.168.10.$((OVMID + 64)) --port 8001 &
    tfspid=$!
    sleep 1
    sysctl -w net.ipv6.conf.tfs0.disable_ipv6=1
    ip addr add 192.168.30.$IPID/24 dev tfs0
    ip link set tfs0 up
else
    build/tcptfs --dev tfs0 --listen 192.168.10.$IPID --port 8001 &
    tfspid=$!
    sleep 1
    sysctl -w net.ipv6.conf.tfs0.disable_ipv6=1
    ip addr add 192.168.30.$IPID/24 dev tfs0
    ip link set tfs0 up
fi

tcpdump -vvv -i tfs0
