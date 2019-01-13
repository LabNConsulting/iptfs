# -*- coding: utf-8 eval: (yapf-mode 1) -*-
#
# January 13 2019, Christian E. Hopps <chopps@gmail.com>
#
from __future__ import absolute_import, division, unicode_literals, print_function, nested_scopes

import argparse
import fcntl
import os
import socket
import struct
import sys
import tcptfs

TUNSETIFF = 0x400454ca
IFF_TUN = 0x0001
IFF_TAP = 0x0002
IFF_NO_PI = 0x0004


def usage():
    print("usage: {} [-c|--connect server] [-p|--port service]\n", sys.argv[0])
    sys.exit(1)


def tun_alloc(devname):
    f = os.open("/dev/net/tun", os.O_RDWR)
    ifs = fcntl.ioctl(f, TUNSETIFF, struct.pack("16sH", devname, IFF_TUN | IFF_NO_PI))
    devname = ifs[:16].strip("\x00")
    return f, devname


def connect(sname, service):
    for hent in socket.getaddrinfo(sname, service):
        try:
            s = socket.socket(*hent[0:3])
            s.connect(hent[4])
            return s
        except socket.error:
            continue
    return None


def accept(sname, service):
    for hent in socket.getaddrinfo(sname, service):
        try:
            s = socket.socket(*hent[0:3])
            s.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
            s.bind(hent[4])
            return s
        except socket.error:
            continue
    return None


def main(*margs):
    parser = argparse.ArgumentParser()
    parser.add_argument("-c", "--connect", help="Connect to server")
    parser.add_argument("-d", "--dev", default="vtun%d", help="Name of tun interface.")
    parser.add_argument("-l", "--listen", default="::", help="Server listen on this address")
    parser.add_argument("-p", "--port", default="8001", help="TCP port to use.")
    args = parser.parse_args(*margs)

    tunfd = tun_alloc(args.dev)
    print("opened tun device: {}", tunfd)

    if not args.connect:
        s = tcptfs.accept(args.listen, args.port)
        print("accepted from client: {}", s)
    else:
        s = tcptfs.connect(args.listen, args.port)
        print("connected to server: {}", s)

    tcptfs.tunnel(tunfd, s)
    return 0


__author__ = "Christian E. Hopps"
__date__ = "January 13 2019"
__version__ = "1.0"
__docformat__ = "restructuredtext en"
