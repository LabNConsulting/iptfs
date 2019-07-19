#!/bin/bash
#
# Copyright (c) 2019, LabN Consulting, L.L.C.
# All Rights Reserved.
#
# Licensed under the Apache License, Version 2.0 (the "License");
# you may not use this file except in compliance with the License.
# You may obtain a copy of the License at
#
# http://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS,
# WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
# See the License for the specific language governing permissions and
# limitations under the License.
#
# vtun 192.168.32.68/32 via tcp 192.168.10.67
#
# [ dpdk2 ] vtun <.....TCP.....> vtun [ dpdk3 ]
#
# vtun 192.168.32.66/32 via via 192.168.10.66
#


usage() {
    echo "Usage: $0 [-o] [-p tunpfx] [-u usepfx] [peerip]"
    echo "o -- default peers start odd (e.g., .1 and .2 vs .2 and .3)"
    echo "tunpfx -- 3 octet prefix for tfs interface (default: 192.168.60)."
    echo "usepfx -- 3 octet prefix to use for outer tunnel (default is same as route default)"
    echo "peerip -- to connect to if default doesn't work."
    exit 1;
}

TUNPFX=192.168.60
autoset=0
even=0
while getopts "ap:" o; do
    case "${o}" in
        e)
            even=1
            ;;
        p)
            TUNPFX=${OPTARG}
            ;;
        u)
            USEPFX=${OPTARG}
            ;;
        *)
            usage
            ;;
    esac
done
shift $((OPTIND-1))
PEERIP=$1

DEFIP=$(ip -4 route show default | sed -Ee 's/.*via (.*) dev.*/\1/')
DEFPFX=${DEFIP%.*}
if [[ -z $PEERIP ]]; then
    # XXX this really only works for things like .66 / .67 but not for .1 / .2
    IPID=${DEFIP##*.}
    ODD=$((IPID % 2))
    if (( ODD )); then
        OIPID=$((IPID + 1))
    else
        OIPID=$((IPID - 1))
    fi
fi

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

# FRAMESZ=1400
#TXRATE=1000 # (Mbps)
TXRATE=10 # (Mbps)
# CONGESTRATE=1000

# COMMON=" --congest-rate=1500 -v --dev tfs0 --port 8001"
#COMMON="--trace -v --rate=$TXRATEMb --dev tfs0 --port 8001"
#COMMON="--rate=$TXRATE --congest=$CONGESTRATE --dev tfs0 --port 8001"

#COMMON="--rate=$TXRATE --dev tfs0 --port 8001"
COMMON="--verbose --rate=$TXRATE --dev tfs0 --port 8001"
#COMMON="--debug --rate=$TXRATE --dev tfs0 --port 8001"
if (( IPID == 66 )); then
    OVMID=3
    sleep 3
    if [[ -d venv ]]; then
        . venv/bin/activate
    fi
    build/iptfs $COMMON --connect 192.168.10.$((OVMID + 64)) &
    #venv/bin/iptfs $COMMON --connect 192.168.10.$((OVMID + 64)) &
    tfspid=$!
    sleep 1
    sysctl -w net.ipv6.conf.tfs0.disable_ipv6=1
    ip addr add 192.168.60.$IPID/24 dev tfs0
    ip link set tfs0 mtu 1470
    #ip link set tfs0 mtu 8970
    ip link set tfs0 up
else
    if [[ -d venv ]]; then
        . venv/bin/activate
    fi
    build/iptfs $COMMON --listen 192.168.10.$IPID &
    #iptfs $COMMON --listen 192.168.10.$IPID &
    tfspid=$!
    sleep 1
    sysctl -w net.ipv6.conf.tfs0.disable_ipv6=1
    ip addr add 192.168.60.$IPID/24 dev tfs0
    ip link set tfs0 mtu 1470
    #ip link set tfs0 mtu 8970
    ip link set tfs0 up
fi

#tcpdump -vvv -i tfs0
wait $tfspid
