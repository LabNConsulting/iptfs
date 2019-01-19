# -*- coding: utf-8 eval: (yapf-mode 1) -*-
#
# January 13 2019, Christian E. Hopps <chopps@gmail.com>
#
from __future__ import absolute_import, division, unicode_literals, print_function, nested_scopes

import argparse
import fcntl
import logging
import os
import socket
import struct
import sys
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
    f = os.open("/dev/net/tun", os.O_RDWR)
    ifs = fcntl.ioctl(f, TUNSETIFF, struct.pack("16sH", devname.encode(), IFF_TUN | IFF_NO_PI))
    devname = ifs[:16]
    devname = devname.strip(b"\x00")
    return f, devname


def connect(sname, service, isudp):
    for hent in socket.getaddrinfo(sname, service):
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
    for hent in socket.getaddrinfo(sname, service):
        try:
            s = socket.socket(*hent[0:3])
            s.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
            s.bind(hent[4])
            if isudp:
                # Do PEEK to get first UDP address from client.
                logger.info("Server: waiting on initial UDP packet %s:%s", sname, service)
                (_, iptfs.peeraddr) = s.recvfrom_into(None, 0, socket.MSG_PEEK)
            s.listen(5)
            return s.accept()
        except socket.error:
            continue
    return None


def main(*margs):
    parser = argparse.ArgumentParser()
    parser.add_argument("-c", "--connect", help="Connect to server")
    parser.add_argument("-d", "--dev", default="vtun%d", help="Name of tun interface.")
    parser.add_argument("-l", "--listen", default="::", help="Server listen on this address")
    parser.add_argument("-p", "--port", default="8001", help="TCP port to use.")
    parser.add_argument("-u", "--udp", action="store_true", help="Use UDP instead of TCP")
    parser.add_argument("-v", "--verbose", action="store_true", help="Name of tun interface.")
    args = parser.parse_args(*margs)

    if args.verbose:
        logging.basicConfig(level=logging.DEBUG)
    else:
        logging.basicConfig(level=logging.INFO)

    tunfd, devname = tun_alloc(args.dev)
    print("opened tun device: {} {} ", devname, tunfd)

    if not args.connect:
        s, _ = accept(args.listen, args.port, args.udp)
        print("accepted from client: {}", s)
    else:
        s = connect(args.connect, args.port, args.udp)
        print("connected to server: {}", s)

    threads = iptfs.tunnel(tunfd, s, args.udp)
    for thread in threads:
        thread.join()

    return 0


__author__ = "Christian E. Hopps"
__date__ = "January 13 2019"
__version__ = "1.0"
__docformat__ = "restructuredtext en"
