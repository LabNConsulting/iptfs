# -*- coding: utf-8 eval: (yapf-mode 1) -*-
#
# January 13 2019, Christian E. Hopps <chopps@gmail.com>
#
from __future__ import absolute_import, division, unicode_literals, print_function, nested_scopes

import binascii
import io
import logging
import os
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


def read_packet(fd, m, issock):
    m.seq = 0

    if not issock:
        n = fd.readinto(m.start)
    else:
        m.prepend(4)
        (n, addr) = fd.recvfrom_into(m.start)
        assert (addr == peeraddr)
        if n >= 4:
            m.seq = get32(m.start)
            m.start = m.start[4:]
            n -= 4

    if n <= 0:
        logger.critical("read: bad read %d on %s", n, str(fd))
        sys.exit(1)
    elif DEBUG:
        logger.debug("read: %d bytes seq %d on %s", n, m.seq, str(fd))

    m.end = m.start[n:]
    return m.len()


def write_packet(fd, m, isfile):
    mlen = m.len()
    if not isfile:
        # Prepend sequence number on tunnel.
        m.prepend(4)
        put32(m.start, m.seq)
        mlen += 4
    if DEBUG:
        logger.debug("write: got mlen %d seq %d on %s", mlen, m.seq, str(fd))

    n = os.write(fd, m.start[:mlen])
    if n != mlen:
        if n < 0:
            logger.error("write: bad write to interface on %s", str(fd))
        else:
            logger.warning("write: short write %d to interface on %s", n, str(fd))
    elif DEBUG:
        logger.debug("write: %d bytes (%s) on %s ", n, binascii.hexlify(m.start[:8]), str(fd))


def read_packets(fd, freeq, q, max_rxrate):
    logger.info("read: start reading on %s", str(fd))
    issock = hasattr(fd, "listen")
    if not issock:
        fd = io.open(fd, "rb", buffering=0)

    # IP/UDP + IP/TCP + TCP timestamps
    overhead = 20 + 8 + 20 + 20 + 12
    rxlimit = Limit(max_rxrate, overhead, 10) if max_rxrate else None

    while True:
        m = freeq.pop()
        n = read_packet(fd, m, issock)
        if n <= 0 or (rxlimit and rxlimit.limit(n)):
            # If we should drop then push back on readq
            freeq.push(m)
        else:
            # else add to write queue.
            q.push(m)


def write_packets(fd, q, freeq):
    logger.info("write_packets: from %s", q.name)
    isfile = not hasattr(fd, "listen")
    try:
        fd = fd.fileno()
    except Exception:
        pass
    while True:
        m = q.pop()
        write_packet(fd, m, isfile)
        freeq.push(m)


def thread_catch(func, *args):
    def thread_main():
        try:
            func(*args)
        except Exception as e:  # pylint: disable=W0612  # pylint: disable=W0703
            logger.critical("%s%s: Uncaught exception: \"%s\"", func.__name__, str(args), repr(e))
            traceback.print_exc()
            sys.exit(1)

    return threading.Thread(target=thread_main)


def tunnel(fromfd, tofd, rxrate):
    freeq = MQueue("RED FREEQ", MAXQSZ, MAXBUF, HDRSPACE, DEBUG)
    q = MQueue("BLACK TXQ", MAXQSZ, 0, 0, DEBUG)

    threads = [
        thread_catch(write_packets, tofd, q, freeq),
        thread_catch(read_packets, fromfd, freeq, q, rxrate),
    ]
    for t in threads:
        logger.debug("Starting thread %s", str(t))
        t.daemon = True
        t.start()

    return threads


__author__ = 'Christian E. Hopps'
__date__ = 'January 13 2019'
__version__ = '1.0'
__docformat__ = "restructuredtext en"
