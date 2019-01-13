# -*- coding: utf-8 eval: (yapf-mode 1) -*-
#
# January 13 2019, Christian E. Hopps <chopps@gmail.com>
#
from __future__ import absolute_import, division, unicode_literals, print_function, nested_scopes

import binascii
import logging
import sys
import threading
from .bstr import read, recv, memspan, writev  # pylint: disable=E0611

HDRSPACE = 18
MAXBUF = 9000 + HDRSPACE
RINGSZ = 32

logger = logging.getLogger(__file__)


class MBuf(object):
    def __init__(self, size, hdrspace):
        self.space = memoryview(bytearray(size))
        self.reset(hdrspace)

    def reset(self, hdrspace):
        self.end = self.start = self.space[hdrspace:]

    def len(self):
        return memspan(self.start, self.end)


class Ring(object):
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
        logger.debug("rdone on %s", self.name)
        with self.lock:
            self.reading = (self.reading + 1) % self.mcount
            self.write_cv.notify()
        logger.debug("rdone complete on %s", self.name)

    def wdone(self):
        logger.debug("wdone on %s", self.name)
        self.mbufs[self.writing].reset(self.hdrspace)
        with self.lock:
            self.writing = (self.writing + 1) % self.mcount
            self.read_cv.notify()
        logger.debug("wdone complete on %s", self.name)


def read_packets(fd, ring):
    try:
        _read_packets(fd, ring)
    except Exception as e:  # pylint: disable=W0612  # pylint: disable=W0703
        logger.critical("read_packets: uncaught exception on %s: %s", ring.name, str(e))
        sys.exit(1)


def read_stream(s, ring):
    try:
        _read_stream(s, ring)
    except Exception as e:  # pylint: disable=W0612  # pylint: disable=W0703
        logger.critical("read_stream: uncaught exception on %s: %s", ring.name, str(e))
        sys.exit(1)


def _read_packets(fd, ring):
    # logger.debug("read_packets start into %s", ring.name)
    while True:
        with ring.read_cv:
            while ring.full():
                logger.debug("read_packets: ring %s is full", ring.name)
                ring.read_cv.wait()
        logger.debug("read_packets: ready on %s", ring.name)

        m = ring.mbufs[ring.reading]
        n = read(fd, m.end, len(m.end))
        if n == 0:
            logger.critical("read_packets: zero read from interface on %s", ring.name)
            sys.exit(1)
        logger.debug("read_packets: read %d bytes from interface on %s", n, ring.name)
        m.end = m.end[n:]
        ring.rdone()


def _read_stream(s, ring):
    # logger.info("read_stream start into %s", ring.name)
    while True:
        with ring.read_cv:
            while ring.full():
                logger.debug("read_stream: ring %s is full", ring.name)
                ring.read_cv.wait()

        logger.debug("read_stream: ready on %s", ring.name)

        m = ring.mbufs[ring.reading]

        lenbuf = m.space[ring.hdrspace - 2:]
        n = recv(s, lenbuf, 2)
        if n != 2:
            logger.critical("read_stream: short read %d from stream on %s exiting", n, ring.name)
            sys.exit(1)
        left = (lenbuf[0] << 8) + lenbuf[1]

        logger.debug("read_stream: pktlen %d on %s", left, ring.name)

        while (left > 0):
            n = recv(s, m.end, left)
            if n == 0:
                logger.critical("read_stream: zero read from interface on %s", ring.name)
                sys.exit(1)
            m.end = m.end[n:]
            left -= n

        logger.debug("read_stream: bytes %s on %s", binascii.hexlify(m.start[:8]), ring.name)

        ring.rdone()


def _write_packets(fd, ring, inc_len):
    dname = "write_stream" if inc_len else "write_packets"
    while True:
        with ring.write_cv:
            while ring.empty():
                logger.debug("%s: ring %s is empty", dname, ring.name)
                ring.write_cv.wait()
        logger.debug("%s: ready on %s", dname, ring.name)

        m = ring.mbufs[ring.writing]

        logger.debug("%s: got buf on %s", dname, ring.name)

        mlen = m.len()

        logger.debug("%s: mlen %d on %s", dname, mlen, ring.name)

        if not inc_len:
            span = m.start[:mlen]
        else:
            span = m.space[ring.hdrspace - 2:]
            span[0] = (mlen >> 8) & 0xFF
            span[1] = mlen & 0xFF
            mlen += 2
            span = span[:mlen]

        n = writev(fd, [span])
        if n != mlen:
            logger.warning("%s: short write to interface on %s", dname, ring.name)

        logger.debug("%s: wrote %d bytes (%s) on %s ", dname, n, binascii.hexlify(span[:8]),
                     ring.name)

        ring.wdone()


def write_packets(fd, ring, inc_len):
    dname = "write_stream" if inc_len else "write_packets"
    logger.info("%s: start from %s inc_len %d", dname, ring.name, inc_len)
    try:
        _write_packets(fd, ring, inc_len)
    except Exception as e:  # pylint: disable=W0612  # pylint: disable=W0703
        logger.critical("%s: uncaught exception on %s: %s", dname, ring.name, str(e))
        sys.exit(1)


def tunnel(iffd, s):
    red = Ring("RED RECV RING", RINGSZ, MAXBUF, HDRSPACE)
    black = Ring("BLACK RECV RING", RINGSZ, MAXBUF, HDRSPACE)

    threads = [
        threading.Thread(target=write_packets, args=(iffd, black, False)),
        threading.Thread(target=write_packets, args=(s, red, True)),
        threading.Thread(target=read_packets, args=(iffd, red)),
        threading.Thread(target=read_stream, args=(s, black)),
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
