# -*- coding: utf-8 eval: (yapf-mode 1) -*-
#
# January 13 2019, Christian E. Hopps <chopps@gmail.com>
#
from __future__ import absolute_import, division, unicode_literals, print_function, nested_scopes

import binascii
import logging
import os
import io
import sys
import threading
# from .bstr import read, recv, memspan, writev  # pylint: disable=E0611
# from .bstr import memspan, read, write
from .bstr import memspan, read

HDRSPACE = 18
MAXBUF = 9000 + HDRSPACE
RINGSZ = 32

logger = logging.getLogger(__file__)

DEBUG = False

# Address for sending to UDP.
peeraddr = None


class MBuf:
    def __init__(self, size, hdrspace):
        self.space = memoryview(bytearray(size))
        self.reset(hdrspace)

    def reset(self, hdrspace):
        self.end = self.start = self.space[hdrspace:]

    def len(self):
        return memspan(self.start, self.end)


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
        count += 1

        self.mbufs = []
        for _ in range(0, count):
            self.mbufs.append(MBuf(maxbuf, hdrspace))

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
            self.read_cv.notify()
        if DEBUG:
            logger.debug("wdone complete on %s", self.name)


def read_packet(fd, ring, isfile, isudp):
    m = ring.mbufs[ring.reading]
    # n = read(fd, m.end, len(m.end))
    if isfile:
        # n = read(fd.fileno(), m.end, len(m.end))
        # n = fd.readinto(m.end, len(m.end))
        n = fd.readinto(m.end)
    # elif isudp:
    #     # n = recv(fd, m.end, len(m.end))

    #     (n, addr) = fd.recvfrom_into(m.end)
    #     assert (addr == peeraddr)

    #     # # We have connected the UDP socket so we can do this?
    #     # n = fd.recv_into(m.end)
    else:
        n = fd.recv_into(m.end)

    if n <= 0:
        logger.critical("read_packets: bad read %d on %s isfile %d isudp %d", n, ring.name, isfile,
                        isudp)
        sys.exit(1)
    elif DEBUG:
        logger.debug("read_packets: read %d bytes on %s", n, ring.name)

    m.end = m.end[n:]
    ring.rdone()


def read_stream(s, ring):
    # logger.info("read_stream start into %s", ring.name)
    m = ring.mbufs[ring.reading]

    lenbuf = m.space[ring.hdrspace - 2:]
    n = s.recv_into(lenbuf, 2)
    if n != 2:
        logger.critical("read_stream: short read %d from stream on %s exiting", n, ring.name)
        sys.exit(1)
    left = (lenbuf[0] << 8) + lenbuf[1]

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

    ring.rdone()


def _read_packets(fd, ring, isfile, isudp):
    with ring.read_cv:
        while ring.full():
            if DEBUG:
                logger.debug("read_packets: ring %s is full", ring.name)
            ring.read_cv.wait()
    if DEBUG:
        logger.debug("read_packets: ready on %s", ring.name)

    if isfile or isudp:
        read_packet(fd, ring, isfile, isudp)
    else:
        read_stream(fd, ring)


def read_packets(fd, ring, isfile, isudp):
    logger.info("read_packets start into %s", ring.name)
    if isfile:
        fd = io.open(fd, "rb", buffering=0)
    try:
        while True:
            _read_packets(fd, ring, isfile, isudp)
    except Exception as e:  # pylint: disable=W0612  # pylint: disable=W0703
        logger.critical("read_packets: uncaught exception on %s: \"%s\"", ring.name, repr(e))
        sys.exit(1)


def write_packet(fd, ring, isfile, isudp):
    if DEBUG:
        dname = "write_stream" if not isfile else "write_packets"

    m = ring.mbufs[ring.writing]
    mlen = m.len()

    if DEBUG:
        logger.debug("%s: got buf mlen %d on %s", dname, mlen, ring.name)

    if isfile or isudp:
        span = m.start[:mlen]
    else:
        span = m.space[ring.hdrspace - 2:]
        span[0] = (mlen >> 8) & 0xFF
        span[1] = mlen & 0xFF
        mlen += 2
        span = span[:mlen]

    # if not isfile and isudp:
    #     # n = socket.sendto(fd, span, 0, peeraddr)
    #     n = fd.sendto(span, peeraddr)
    #     # XXX need a custom function for this, why convert addrs each time??.
    # else:
    # n = writev(fd, [span])
    if isfile:
        n = os.write(fd, span)
    else:
        n = os.write(fd.fileno(), span)
    # n = write(fd, span)
    if n != mlen:
        if n < 0:
            logger.error("write_packet: bad write to interface on %s", ring.name)
        else:
            logger.warning("write_packet: short write %d to interface on %s", n, ring.name)
    elif DEBUG:
        logger.debug("%s: wrote %d bytes (%s) on %s ", dname, n, binascii.hexlify(span[:8]),
                     ring.name)

    ring.wdone()


def write_packets(fd, ring, isfile, isudp):
    dname = "write_stream" if not isfile else "write_packets"
    logger.info("%s: start from %s isfile %d", dname, ring.name, isfile)
    try:
        while True:
            with ring.write_cv:
                while ring.empty():
                    if DEBUG:
                        logger.debug("%s: ring %s is empty", dname, ring.name)
                    ring.write_cv.wait()
            if DEBUG:
                logger.debug("%s: ready on %s", dname, ring.name)
            write_packet(fd, ring, isfile, isudp)
    except Exception as e:  # pylint: disable=W0612  # pylint: disable=W0703
        logger.critical("%s: uncaught exception on %s: \"%s\"", dname, ring.name, repr(e))
        sys.exit(1)


def tunnel(iffd, s, udp):
    red = Ring("RED RECV RING", RINGSZ, MAXBUF, HDRSPACE)
    black = Ring("BLACK RECV RING", RINGSZ, MAXBUF, HDRSPACE)

    threads = [
        threading.Thread(target=write_packets, args=(iffd, black, True, udp)),
        threading.Thread(target=write_packets, args=(s, red, False, udp)),
        threading.Thread(target=read_packets, args=(iffd, red, True, udp)),
        threading.Thread(target=read_packets, args=(s, black, False, udp)),
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
