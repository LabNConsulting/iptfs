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
    echo "Usage: $0 [-dlv] [-r mbpsrate] [-p port] [-l listenip] [-t tunpfx] ip"
    echo
    echo "d -- debug"
    echo "l -- listen"
    echo "v -- verbose"
    echo
    echo "tunpfx -- 3 octet prefix for tfs interface prefix (default: 192.168.30)."
    echo "ip -- listen on or connect to this iP"
    echo "port -- use port (default: 8001)"
    exit 1;
}

TUNPFX=192.168.30
CONIP=
LISTIP=
port=8001
mtu=1470
rate=10
listen=
debug=
while getopts "Cdlm:p:r:t:v" o; do
    case "${o}" in
        C)
            runc=1
            ;;
        d)
            debug="--debug"
            ;;
        l)
            listen=-l
            ;;
        m)
            mtu=${OPTARG}
            ;;
        p)
            port=${OPTARG}
            ;;
        r)
            rate=${OPTARG}
            ;;
        t)
            TUNPFX=${OPTARG}
            ;;
        v)
            verbose="--verbose"
            ;;
        *)
            usage
            ;;
    esac
done
shift $((OPTIND-1))
IP=$1

if [[ -z $IP ]]; then
    usage
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

COMMON="${debug} ${verbose} --rate=${rate} --dev tfs0 --port ${port}"

sleep 1
if [[ -d venv ]]; then
    . venv/bin/activate
fi


if [[ -z $listen ]]; then
    PFX=${IP%.*}
    PFX=${PFX//\./\\.}
    IPID=$(ip addr | sed -e '/'"$PFX"'/!d;s,.*'"$PFX"'\.\([0-9]*\)/.*,\1,')
    COMMON="$COMMON --connect $IP"
else
    PFX=${IP%.*}
    IPID=${IP##*.}
    COMMON="$COMMON --listen $IP"
fi

set -x

if (( runc )); then
    build/iptfs $COMMON &
else
    iptfs $COMMON &
fi
tfspid=$!

sleep 1
if [ -e /proc/sys/net/ipv6/conf/tfs0/disable_ipv6 ]; then
    sysctl -w net.ipv6.conf.tfs0.disable_ipv6=1
fi
ip addr add $TUNPFX.$IPID/24 dev tfs0
ip link set tfs0 mtu ${mtu}
ip link set tfs0 up

if [[ -n "$listen" ]]; then
    iperf -s &
fi
#tcpdump -vvv -i tfs0
wait $tfspid
