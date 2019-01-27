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
TXRATE=10 # (Mbps)
CONGESTRATE=50

# COMMON=" --congest-rate=1500 -v --dev tfs0 --port 8001"
#COMMON="--trace -v --rate=$TXRATEMb --dev tfs0 --port 8001"
COMMON="--rate=$TXRATE --congest=$CONGESTRATE --dev tfs0 --port 8001"
#COMMON="--debug --rate=$TXRATE --dev tfs0 --port 8001"
if (( VMID == 2 )); then
    OVMID=3
    sleep 1
    . venv/bin/activate
    # build/iptfs $COMMON --connect 192.168.10.$((OVMID + 64)) &
    venv/bin/iptfs $COMMON --connect 192.168.10.$((OVMID + 64)) &
    tfspid=$!
    sleep 1
    sysctl -w net.ipv6.conf.tfs0.disable_ipv6=1
    ip addr add 192.168.30.$IPID/24 dev tfs0
    ip link set tfs0 mtu 1470
    #ip link set tfs0 mtu 8970
    ip link set tfs0 up
else
    . venv/bin/activate
    # build/iptfs $COMMON --listen 192.168.10.$IPID &
    venv/bin/iptfs $COMMON --listen 192.168.10.$IPID &
    tfspid=$!
    sleep 1
    sysctl -w net.ipv6.conf.tfs0.disable_ipv6=1
    ip addr add 192.168.30.$IPID/24 dev tfs0
    ip link set tfs0 mtu 1470
    #ip link set tfs0 mtu 8970
    ip link set tfs0 up
fi

#tcpdump -vvv -i tfs0
wait $tfspid
