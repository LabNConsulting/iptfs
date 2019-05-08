# -*- coding: utf-8 eval: (yapf-mode 1) -*-
#
# January 13 2019, Christian E. Hopps <chopps@labn.net>
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
from __future__ import absolute_import, division, unicode_literals, print_function, nested_scopes

import argparse
import fcntl
import logging
import io
import os
import socket
import struct
import sys
import threading
from . import iptfs

TUNSETIFF = 0x400454ca
IFF_TUN = 0x0001
IFF_TAP = 0x0002
IFF_NO_PI = 0x1000

logger = logging.getLogger(__file__)


def usage():
    print("usage: {} [-c|--connect server] [-p|--port service]\n", sys.argv[0])
    sys.exit(1)


def tun_alloc(devname):
    fd = os.open("/dev/net/tun", os.O_RDWR)
    rfd = io.open(fd, "rb", buffering=0)
    wfd = io.open(fd, "wb", buffering=0)
    # ff = io.open(fd, "rb")
    # f = io.open("/dev/net/tun", "rb", buffering=0)
    ifs = fcntl.ioctl(fd, TUNSETIFF, struct.pack("16sH", devname.encode(), IFF_TUN | IFF_NO_PI))
    devname = ifs[:16]
    devname = devname.strip(b"\x00")
    return rfd, wfd, devname


def connect(sname, service, isudp):
    # stype = socket.SOCK_DGRAM if isudp else socket.SOCK_STREAM
    proto = socket.IPPROTO_UDP if isudp else socket.IPPROTO_TCP
    for hent in socket.getaddrinfo(sname, service, 0, 0, proto):
        try:
            s = socket.socket(*hent[0:3])
            if isudp:
                # Save the peer address
                iptfs.peeraddr = hent[4]
            s.connect(hent[4])
            return s
        except socket.error:
            continue
    return None


def accept(sname, service, isudp):
    # stype = socket.SOCK_DGRAM if isudp else socket.SOCK_STREAM
    proto = socket.IPPROTO_UDP if isudp else socket.IPPROTO_TCP
    for hent in socket.getaddrinfo(sname, service, 0, 0, proto):
        try:
            logger.info("Get socket")
            s = socket.socket(*hent[0:3])
            logger.info("Set socketopt")
            s.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
            logger.info("Try to bind to: %s", str(hent[4]))
            s.bind(hent[4])
            break
        except socket.error as e:
            logger.info("Got exception for %s: %s", str(hent), str(e))
            continue
    else:
        logger.info("Can't bind to %s:%s", sname, service)
        return None

    if isudp:
        # Do PEEK to get first UDP address from client.
        logger.info("Server: waiting on initial UDP packet %s:%s:%s", sname, service, str(hent))  # pylint: disable=W0631
        b = bytearray(9170)
        (n, iptfs.peeraddr) = s.recvfrom_into(b, 0, socket.MSG_PEEK)
        logger.info("Server: Got UDP packet from %s of len %d", iptfs.peeraddr, n)
        s.connect(iptfs.peeraddr)
        return (s, iptfs.peeraddr)

    logger.info("Listen 5 on %s", str(iptfs.peeraddr))
    s.listen(5)
    logger.info("Doing accept.")
    return s.accept()


def checked_main(*margs):
    parser = argparse.ArgumentParser()
    parser.add_argument(
        "-a", "--ack-rate", type=float, default=1.0, help="Rate in float seconds to send ACK info")
    parser.add_argument("-c", "--connect", help="Connect to server")
    parser.add_argument(
        "--congest-rate", type=float, default=0, help="Forced maximum egress rate in Kilobits")
    parser.add_argument("-d", "--dev", default="vtun%d", help="Name of tun interface.")
    parser.add_argument("--debug", action="store_true", help="Debug logging and checks.")
    parser.add_argument(
        "--no-egress", action="store_true", help="Do not create tunnel egress endpoint")
    parser.add_argument(
        "--no-ingress", action="store_true", help="Do not create tunnel ingress endpoint")
    parser.add_argument("-l", "--listen", default="::", help="Server listen on this address")
    parser.add_argument("-p", "--port", default="8001", help="TCP port to use.")
    # parser.add_argument("-u", "--udp", action="store_true", help="Use UDP instead of TCP")
    parser.add_argument("-r", "--rate", type=float, default=0, help="Tunnel rate in Kilobits")
    parser.add_argument("--trace", action="store_true", help="Trace logging.")
    parser.add_argument("-v", "--verbose", action="store_true", help="Verbose logging.")
    args = parser.parse_args(*margs)

    FORMAT = '%(asctime)-15s %(threadName)s %(message)s'
    if args.trace:
        iptfs.TRACE = True
        iptfs.DEBUG = True
        logging.basicConfig(format=FORMAT, level=logging.DEBUG)
    elif args.debug:
        iptfs.DEBUG = True
        logging.basicConfig(format=FORMAT, level=logging.DEBUG)
    elif args.verbose:
        logging.basicConfig(format=FORMAT, level=logging.DEBUG)
    else:
        logging.basicConfig(format=FORMAT, level=logging.INFO)

    riffd, wiffd, devname = tun_alloc(args.dev)
    logger.info("Opened tun device: %s", devname)

    if not args.connect:
        s, _ = accept(args.listen, args.port, True)
        logger.info("Accepted from client: %s", str(s))
    else:
        s = connect(args.connect, args.port, True)
        logger.info("Connected to server: %s", str(s))

    send_lock = threading.Lock()

    threads = []
    if not args.no_ingress:
        threads.extend(iptfs.tunnel_ingress(riffd, s, send_lock, int(args.rate * 1000)))
    if not args.no_egress:
        threads.extend(
            iptfs.tunnel_egress(s, send_lock, wiffd, args.ack_rate, int(args.congest_rate * 1000)))
    for thread in threads:
        thread.join()

    return 0


def main(*margs):
    try:
        return checked_main(*margs)
    except Exception as e:  # pylint: disable=W0703
        logger.critical("Unexpected exception: %s", str(e))
        sys.exit(1)


__author__ = "Christian E. Hopps"
__date__ = "January 13 2019"
__version__ = "1.0"
__docformat__ = "restructuredtext en"
