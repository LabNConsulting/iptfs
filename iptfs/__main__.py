# -*- coding: utf-8 eval: (yapf-mode 1) -*-
#
# January 13 2019, Christian E. Hopps <chopps@gmail.com>
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
    # ff = io.open(fd, "rb")
    # f = io.open("/dev/net/tun", "rb", buffering=0)
    ifs = fcntl.ioctl(fd, TUNSETIFF, struct.pack("16sH", devname.encode(), IFF_TUN | IFF_NO_PI))
    devname = ifs[:16]
    devname = devname.strip(b"\x00")
    return fd, devname


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
        except Exception as e:
            logger.info("Got unexpected exception for %s: %s", str(hent), str(e))
            continue
    else:
        logger.info("Can't bind to %s:%s", sname, service)
        return None

    if isudp:
        # Do PEEK to get first UDP address from client.
        logger.info("Server: waiting on initial UDP packet %s:%s:%s", sname, service, str(hent))
        b = bytearray(9170)
        (n, iptfs.peeraddr) = s.recvfrom_into(b, 0, socket.MSG_PEEK)
        logger.info("Server: Got UDP packet from %s of len %d", iptfs.peeraddr, n)
        s.connect(iptfs.peeraddr)
        return (s, iptfs.peeraddr)

    logger.info("Listen 5 on %s", str(iptfs.peeraddr))
    s.listen(5)
    logger.info("Doing accept.")
    return s.accept()


def main(*margs):
    parser = argparse.ArgumentParser()
    parser.add_argument("-c", "--connect", help="Connect to server")
    parser.add_argument("-d", "--dev", default="vtun%d", help="Name of tun interface.")
    parser.add_argument("-l", "--listen", default="::", help="Server listen on this address")
    parser.add_argument("-p", "--port", default="8001", help="TCP port to use.")
    # parser.add_argument("-u", "--udp", action="store_true", help="Use UDP instead of TCP")
    parser.add_argument("-r", "--rx-rate", type=int, default=0, help="Maximum RX rate in Megabits")
    parser.add_argument("-v", "--verbose", action="store_true", help="Name of tun interface.")
    args = parser.parse_args(*margs)

    if args.verbose:
        logging.basicConfig(level=logging.DEBUG)
    else:
        logging.basicConfig(level=logging.INFO)

    tunfd, devname = tun_alloc(args.dev)
    print("opened tun device: {} {} ".format(devname, tunfd))

    try:
        if not args.connect:
            s, _ = accept(args.listen, args.port, True)
            print("accepted from client: %s", str(s))
        else:
            s = connect(args.connect, args.port, True)
            print("connected to server: %s", str(s))
    except Exception as e:
        print("Unexpected exception: %s", str(e))
        sys.exit(1)

    threads = iptfs.tunnel(tunfd, s, args.rx_rate * 1000000)
    threads.extend(iptfs.tunnel(s, tunfd, args.rx_rate * 1000000))
    for thread in threads:
        thread.join()

    return 0


__author__ = "Christian E. Hopps"
__date__ = "January 13 2019"
__version__ = "1.0"
__docformat__ = "restructuredtext en"
