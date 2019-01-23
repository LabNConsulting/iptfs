# -*- coding: utf-8 eval: (yapf-mode 1) -*-
#
# January 22 2019, Christian Hopps <chopps@gmail.com>
#
from __future__ import absolute_import, division, unicode_literals, print_function, nested_scopes

import logging
import threading

logger = logging.getLogger(__file__)

class MBuf:
    def __init__(self, size, hdrspace):
        self.space = memoryview(bytearray(size))
        self.end = self.start = self.space[hdrspace:]
        self.reset(hdrspace)

    def reset(self, hdrspace):
        self.end = self.start = self.space[hdrspace:]
        self.seq = 0

    def prepend(self, space):
        newstart = self.hdrspace() - space
        assert(newstart >= 0)
        self.start = self.space[newstart:]
        return self.start

    def hdrspace(self):
        return self.space.nbytes - self.start.nbytes

    def len(self):
        return self.start.nbytes - self.end.nbytes



class MQueue:
    def __init__(self, name, count, maxbuf, hdrspace, debug):  # pylint: disable=R0913
        """MQueue is a queue for MBUfs.

        If maxbuf is non-0 then this is the managing queue. A managing queue
        will create count MBufs and will reset them on push and set there
        sequence number on pop.
        """

        self.name = name
        self.mcount = count
        self.maxbuf = maxbuf
        self.manage = self.maxbuf != 0
        self.hdrspace = hdrspace
        self.debug = debug

        self.lock = threading.Lock()
        self.push_cv = threading.Condition(self.lock)
        self.pop_cv = threading.Condition(self.lock)
        self.seq = 0
        self.mbufs = []
        if self.manage:
            for _ in range(0, count):
                self.mbufs.append(MBuf(maxbuf, hdrspace))

    def empty(self):
        return len(self.mbufs) == 0

    def full(self):
        return len(self.mbufs) >= self.mcount

    def pop(self):
        with self.pop_cv:
            while self.empty():
                if self.debug:
                    logger.debug("pop: mqueue %s is empty", self.name)
                self.pop_cv.wait()

            m = self.mbufs.pop()

            if self.manage:
                self.seq += 1
                m.seq = self.seq

            if self.full():
                self.push_cv.notify()

            return m

    def push(self, m):
        if self.manage:
            m.seq = 0
            m.reset(self.hdrspace)

        with self.push_cv:
            while self.full():
                if self.debug:
                    logger.debug("push: queue %s is full", self.name)
                self.push_cv.wait()

            if self.empty():
                self.pop_cv.notify()
            self.mbufs.append(m)

__author__ = 'Christian Hopps'
__date__ = 'January 22 2019'
__version__ = '1.0'
__docformat__ = "restructuredtext en"
