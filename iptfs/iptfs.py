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
from .util import Limit

HDRSPACE = 18
MAXBUF = 9000 + HDRSPACE
RINGSZ = 32

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


class MBuf:
    def __init__(self, mv, hdrspace):
        # self.space = memoryview(bytearray(size))
        self.space = mv
        self.reset(hdrspace)

    def reset(self, hdrspace):
        self.end = self.start = self.space[hdrspace:]
        self.seq = 0

    def len(self):
        return self.start.nbytes - self.end.nbytes


class Ring:
    def __init__(self, name, count, maxbuf, hdrspace):
        self.name = name
        self.mcount = count
        self.maxbuf = maxbuf
        self.hdrspace = hdrspace
        self.lock = threading.Lock()
        self.read_cv = threading.Condition(self.lock)
        self.write_cv = threading.Condition(self.lock)
        self.reading = self.writing = 0
        self.wseq = 1
        count += 1

        self.bufspace = bytearray(count * maxbuf)
        self.mbufs = []
        for i in range(0, count):
            mv = memoryview(self.bufspace[i * maxbuf:(i + 1) * maxbuf])
            self.mbufs.append(MBuf(mv, hdrspace))

    def empty(self):
        return self.reading == self.writing

    def full(self):
        return (self.reading + 1) % self.mcount == self.writing

    def rdone(self):
        if DEBUG:
            logger.debug("rdone on %s", self.name)

        with self.lock:
            self.reading = (self.reading + 1) % self.mcount
            self.write_cv.notify()

        if DEBUG:
            logger.debug("rdone complete on %s", self.name)

    def wdone(self):
        if DEBUG:
            logger.debug("wdone on %s", self.name)

        self.mbufs[self.writing].reset(self.hdrspace)
        with self.lock:
            self.writing = (self.writing + 1) % self.mcount
            self.wseq += 1
            self.read_cv.notify()

        if DEBUG:
            logger.debug("wdone complete on %s", self.name)


def read_packet(fd, ring, isfile, isudp):
    m = ring.mbufs[ring.reading]
    m.seq = 0
    if isfile:
        n = fd.readinto(m.start)
    elif isudp:
        m.start = m.space[ring.hdrspace - 4:]
        (n, addr) = fd.recvfrom_into(m.start)
        assert (addr == peeraddr)
        if n >= 4:
            m.seq = get32(m.start)
            m.start = m.start[4:]
            n -= 4
    else:
        n = fd.recv_into(m.start)

    if n <= 0:
        logger.critical("read_packets: bad read %d on %s isfile %d isudp %d", n, ring.name, isfile,
                        isudp)
        sys.exit(1)
    elif DEBUG:
        logger.debug("read_packets: read %d bytes seq %d on %s", n, m.seq, ring.name)

    m.end = m.start[n:]
    return m.len()


def read_stream(s, ring):
    m = ring.mbufs[ring.reading]

    n = s.recv_into(m.start, 6)
    if n != 6:
        logger.critical("read_stream: short read %d from stream on %s exiting", n, ring.name)
        sys.exit(1)

    # Get length from the IP header.
    iphdr = m.start
    if (iphdr[0] & 0xf0) == 0x40:
        left = (iphdr[2] << 8) + iphdr[3]
    else:
        left = (iphdr[4] << 8) + iphdr[5]
    left -= 6
    m.end = m.start[6:]

    if DEBUG:
        logger.debug("read_stream: pktlen %d on %s", left, ring.name)

    while (left > 0):
        n = s.recv_into(m.end, left)
        if n == 0:
            logger.critical("read_stream: zero read from interface on %s", ring.name)
            sys.exit(1)
        m.end = m.end[n:]
        left -= n

    if DEBUG:
        logger.debug("read_stream: bytes %s on %s", binascii.hexlify(m.start[:8]), ring.name)

    return m.len()


def write_packet(fd, ring, isfile):
    m = ring.mbufs[ring.writing]
    wseq = ring.wseq
    mlen = m.len()
    if not isfile:
        # Prepend sequence number on tunnel.
        m.start = m.space[ring.hdrspace - 4:]
        put32(m.start, wseq)
        mlen += 4
    if DEBUG:
        logger.debug("write_packet: got buf mlen %d seq %d on %s", mlen, wseq, ring.name)

    n = os.write(fd, m.start[:mlen])
    if n != mlen:
        if n < 0:
            logger.error("write_packet: bad write to interface on %s", ring.name)
        else:
            logger.warning("write_packet: short write %d to interface on %s", n, ring.name)
    elif DEBUG:
        logger.debug("write_packet: wrote %d bytes (%s) on %s ", n, binascii.hexlify(m.start[:8]),
                     ring.name)
    ring.wdone()


def read_packets(fd, ring, isfile, isudp, max_rxrate):
    if isfile:
        fd = io.open(fd, "rb", buffering=0)

    # IP/UDP + IP/TCP + TCP timestamps
    overhead = 20 + 8 + 20 + 20 + 12
    rxlimit = Limit(max_rxrate, overhead, 10) if max_rxrate else None

    while True:
        with ring.read_cv:
            while ring.full():
                if DEBUG:
                    logger.debug("read_packets: ring %s is full", ring.name)
                ring.read_cv.wait()
        if DEBUG:
            logger.debug("read_packets: ready on %s", ring.name)

        if isfile or isudp:
            n = read_packet(fd, ring, isfile, isudp)
        else:
            n = read_stream(fd, ring)

        if not rxlimit or not rxlimit.limit(n):
            ring.rdone()


def write_packets(fd, ring, isfile):
    logger.info("write_packets: start from %s", ring.name)
    while True:
        with ring.write_cv:
            while ring.empty():
                if DEBUG:
                    logger.debug("write_packets: ring %s is empty", ring.name)
                ring.write_cv.wait()
        if DEBUG:
            logger.debug("write_packets: ready on %s", ring.name)

        write_packet(fd, ring, isfile)


def thread_catch(func, *args):
    def thread_main():
        try:
            func(*args)
        except Exception as e:  # pylint: disable=W0612  # pylint: disable=W0703
            logger.critical("%s%s: Uncaught exception: \"%s\"", func.__name__, str(args), repr(e))
            sys.exit(1)

    return threading.Thread(target=thread_main)


def tunnel(iffd, s, udp, rxrate):
    red = Ring("RED RECV RING", RINGSZ, MAXBUF, HDRSPACE)
    black = Ring("BLACK RECV RING", RINGSZ, MAXBUF, HDRSPACE)

    threads = [
        thread_catch(write_packets, iffd, black, True),
        thread_catch(write_packets, s.fileno(), red, False),
        thread_catch(read_packets, iffd, red, True, udp, None),
        thread_catch(read_packets, s, black, False, udp, rxrate),
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
