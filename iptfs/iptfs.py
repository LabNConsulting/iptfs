# -*- coding: utf-8 eval: (yapf-mode 1) -*-
#
# January 13 2019, Christian E. Hopps <chopps@gmail.com>
#
from __future__ import absolute_import, division, unicode_literals, print_function, nested_scopes

import binascii
import logging
import sys
import threading
import traceback
from .util import Limit
from .mbuf import MQueue

HDRSPACE = 18
MAXBUF = 9000 + HDRSPACE
MAXQSZ = 32

logger = logging.getLogger(__file__)

DEBUG = False

# Address for sending to UDP.
peeraddr = None


def get32(m):
    return ((m[0] << 24) + (m[1] << 16) + (m[2] << 8) + m[3])


def put32(m, i):
    m[0] = (i >> 24) & 0xFF
    m[1] = (i >> 16) & 0xFF
    m[2] = (i >> 8) & 0xFF
    m[3] = (i) & 0xFF


# -----------------
# Interface Packets
# -----------------


def read_intf_packet(fd, m):
    n = fd.readinto(m.start)
    if n <= 0:
        logger.error("read: bad read %d on interface, dropping", n)
        return 0
    if DEBUG:
        logger.debug("read: %d bytes on interface", n)
    m.end = m.start[n:]
    return n


def write_intf_packet(fd, m):
    mlen = m.len()
    n = fd.write(m.start[:mlen])
    if n != mlen:
        logger.error("write: bad write %d (mlen %d) on interface", n, mlen)
    elif DEBUG:
        logger.debug("write: %d bytes (%s) on %s ", n, binascii.hexlify(m.start[:8]), str(fd))


def write_intf_packets(fd, outq, inq):
    logger.info("write_packets: from %s", outq.name)
    while True:
        m = outq.pop()
        write_intf_packet(fd, m)
        inq.push(m, True)


# ------------------
# TFS Tunnel Packets
# ------------------


def read_tunnel_packet(s, m):
    m.seq = 0

    m.prepend(4)
    (n, addr) = s.recvfrom_into(m.start)
    assert (addr == peeraddr)
    if n <= 4:
        logger.error("read: bad read %d on TFS link, dropping", n)
        return 0

    m.seq = get32(m.start)
    m.start = m.start[4:]
    n -= 4
    m.end = m.start[n:]

    if DEBUG:
        logger.debug("read: %d bytes seq %d on %s", n, m.seq, str(s))

    return n


def recv_ack(inq, m):  # pylint: disable=W0613
    pass


def write_tunnel_packet(s, m):
    # Prepend sequence number on tunnel.
    m.prepend(4)
    put32(m.start, m.seq)
    mlen = m.len()

    n = s.send(m.start[:mlen])
    if n != mlen:
        logger.error("write: bad write %d of %d on TFS link", n, mlen)
    elif DEBUG:
        logger.debug("write: %d bytes (%s) on TFS Link", n, binascii.hexlify(m.start[:8]))


def write_tunnel_packets(fd, outq, inq, rate):  # pylint: disable=W0613
    logger.info("write_packets: from %s", outq.name)

    # Loop writing packets limited by "rate"
    while True:
        m = outq.pop()
        write_tunnel_packet(fd, m)
        inq.push(m, True)


# -------
# Generic
# -------


def read_packets(fd, inq, outq, max_rxrate):
    logger.info("read: start reading on %s", str(fd))
    issock = hasattr(fd, "listen")
    if issock:
        readf = read_tunnel_packet
    else:
        readf = read_intf_packet

    rxlimit = None
    if max_rxrate:
        # IP/UDP + IP/TCP + TCP timestamps
        # overhead = 20 + 8 + 20 + 20 + 12
        overhead = 0
        rxlimit = Limit(max_rxrate, overhead, 10) if max_rxrate else None

    while True:
        m = inq.pop()

        n = 0
        if m.len() > 0:
            recv_ack(inq, m)
        else:
            n = readf(fd, m)
        if n <= 0 or (rxlimit and rxlimit.limit(n)):
            # If we should drop then push back on readq
            inq.push(m, True)
        else:
            # else add to write queue.
            outq.push(m, False)


def thread_catch(func, *args):
    def thread_main():
        try:
            func(*args)
        except Exception as e:  # pylint: disable=W0612  # pylint: disable=W0703
            logger.critical("%s%s: Uncaught exception: \"%s\"", func.__name__, str(args), repr(e))
            traceback.print_exc()
            sys.exit(1)

    return threading.Thread(target=thread_main)


def tunnel_ingress(riffd, s, rate):
    inq = MQueue("TFS Ingress INQ", MAXQSZ, MAXBUF, HDRSPACE, DEBUG)
    outq = MQueue("TFS Ingress OUTQ", MAXQSZ, 0, 0, DEBUG)

    threads = [
        thread_catch(read_packets, riffd, inq, outq, 0),
        thread_catch(write_tunnel_packets, s, outq, inq, rate),
    ]

    for t in threads:
        t.daemon = True
        t.start()

    return threads


def tunnel_egress(s, wiffd, congest_rate):
    inq = MQueue("TFS Egress INQ", MAXQSZ, MAXBUF, HDRSPACE, DEBUG)
    outq = MQueue("TFS Egress OUTQ", MAXQSZ, 0, 0, DEBUG)

    threads = [
        thread_catch(read_packets, s, inq, outq, congest_rate),
        thread_catch(write_intf_packets, wiffd, outq, inq),
    ]

    for t in threads:
        t.daemon = True
        t.start()

    return threads


__author__ = 'Christian E. Hopps'
__date__ = 'January 13 2019'
__version__ = '1.0'
__docformat__ = "restructuredtext en"
