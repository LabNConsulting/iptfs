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
        self.reset(hdrspace)
        self.end = self.start = self.space[hdrspace:]
        self.seq = self.flags = 0

    def reset(self, hdrspace):
        self.end = self.start = self.space[hdrspace:]
        self.flags = self.seq = 0

    # this is broken as start may have shrunk from bothe ends
    # def prepend(self, space):
    #     newstart = self.hdrspace() - space
    #     assert (newstart >= 0)
    #     self.start = self.space[newstart:]
    #     return self.start
    # def hdrspace(self):
    #     # XXX this is broken if start has been shrunk from the top end.
    #     # return self.space.nbytes - self.start.nbytes

    def after(self):
        return self.end.nbytes

    def len(self):
        return self.start.nbytes - self.end.nbytes


class MQueue:
    def __init__(self, name, count, maxbuf, hdrspace, debug):  # pylint: disable=R0913
        """MQueue is a queue for MBUfs.

        If maxbuf is non-0 then the queue will allocate and push count empty
        mbufs on creation.
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

            # If we were full then notify there will be push space.
            if self.full():
                self.push_cv.notify()

            return self.mbufs.pop()

    def trypop(self):
        with self.pop_cv:
            if self.empty():
                return None

            # If we were full then notify there will be push space.
            if self.full():
                self.push_cv.notify()

            return self.mbufs.pop()

    def push(self, m, reset=False):
        """push an mbuf on the queue.

        If reset is true then the mbuf is reset to an initial state.
        """
        if reset:
            m.seq = 0
            m.reset(self.hdrspace)

        with self.push_cv:
            while self.full():
                if self.debug:
                    logger.debug("push: queue %s is full", self.name)
                self.push_cv.wait()

            # If we were empty then notify there will be something to pop.
            if self.empty():
                self.pop_cv.notify()

            self.mbufs.append(m)


__author__ = 'Christian Hopps'
__date__ = 'January 22 2019'
__version__ = '1.0'
__docformat__ = "restructuredtext en"