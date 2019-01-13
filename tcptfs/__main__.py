# -*- coding: utf-8 eval: (yapf-mode 1) -*-
#
# January 13 2019, Christian E. Hopps <chopps@gmail.com>
#
from __future__ import absolute_import, division, unicode_literals, print_function, nested_scopes

import argparse
import sys
import tcptfs


def usage():
    print("usage: {} [-c|--connect server] [-p|--port service]\n", sys.argv[0])
    sys.exit(1)


def main():
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
