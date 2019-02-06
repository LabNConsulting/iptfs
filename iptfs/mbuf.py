# -*- coding: utf-8 eval: (yapf-mode 1) -*-
#
# January 22 2019, Christian Hopps <chopps@gmail.com>
#
# Copyright (c) 2019, LabN Consulting, L.L.C.
# All Rights Reserved.
#
# Licensed under the Apache License, Version 2.0 (the "License");
# you may not use this file except in compliance with the License.
# You may obtain a copy of the License at
#
# http://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS,
# WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
# See the License for the specific language governing permissions and
# limitations under the License.
#
from __future__ import absolute_import, division, unicode_literals, print_function, nested_scopes

import logging
import threading

logger = logging.getLogger(__file__)


class MBuf:
    def __init__(self, size, hdrspace, refcnt=False):
        self.space = memoryview(bytearray(size))
        self.reset(hdrspace)
        self.end = self.start = self.space[hdrspace:]
        self.seq = self.flags = 0
        self.reflock = None
        if refcnt:
            self.reflock = threading.Lock()
            self.refcnt = 0

    def reset(self, hdrspace):
        self.end = self.start = self.space[hdrspace:]
        self.flags = self.seq = 0

    def addref(self):
        with self.reflock:
            self.refcnt += 1
            return self.refcnt

    def deref(self, freeq):
        with self.reflock:
            self.refcnt -= 1
            if self.refcnt != 0:
                return False
        freeq.push(self, True)
        return True

    def after(self):
        return self.end.nbytes

    def len(self):
        return self.start.nbytes - self.end.nbytes


class MQueue:
    def __init__(self, name, count, maxbuf, hdrspace, refcnt, debug):  # pylint: disable=R0913
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
                self.mbufs.append(MBuf(maxbuf, hdrspace, refcnt))

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
            # if self.full():
            #     self.push_cv.notify()
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
            # if self.empty():
            #     self.pop_cv.notify()
            self.pop_cv.notify()
            self.mbufs.append(m)


class MIOVBuf:
    def __init__(self):
        self.mbufs = []
        self.iov = []
        self.mlen = 0

    def addmbuf(self, m, start):
        m.addref()
        self.mbufs.append(m)
        self.iov.append(start)
        self.mlen += len(start)

    def reset(self, freeq):
        for m in self.mbufs:
            m.deref(freeq)
        self.mbufs = []
        self.iov = []
        self.mlen = 0

    def len(self):
        return self.mlen


class MIOVQ:
    def __init__(self, name, size, freeq=None, debug=False):
        """MIOVQ is a queue for MIOVBufs.

        :Parameters:
            - `name` (`src`) - descriptive name for the queue.
            - `size` (`int`) - max depth of the queue.
            - `freeq` (`MQueue`) - queue to free refered mbufs into.
            - `debug` (`bool`) - enable debug logging.

        If freeq is not None then the queue will allocate and push count empty
        miovbufs on creation.
        """

        self.name = name
        self.mcount = size
        self.freeq = freeq
        self.debug = debug
        self.manage = freeq is not None

        self.lock = threading.Lock()
        self.push_cv = threading.Condition(self.lock)
        self.pop_cv = threading.Condition(self.lock)
        self.queue = []
        if self.manage:
            for _ in range(0, size):
                self.queue.append(MIOVBuf())

    def empty(self):
        return len(self.queue) == 0

    def full(self):
        return len(self.queue) >= self.mcount

    def pop(self):
        with self.pop_cv:
            while self.empty():
                if self.debug:
                    logger.debug("pop: mqueue %s is empty", self.name)
                self.pop_cv.wait()
            self.push_cv.notify()
            return self.queue.pop()

    def trypop(self):
        with self.pop_cv:
            if self.empty():
                return None
            self.push_cv.notify()
            return self.queue.pop()

    def push(self, m):
        """push an MIOVBuf on the queue.

        If this is a free-ing queue then reset the MIOVBuf.
        """
        if self.freeq is not None:
            m.reset(self.freeq)

        with self.push_cv:
            while self.full():
                if self.debug:
                    logger.debug("push: queue %s is full", self.name)
                self.push_cv.wait()

            self.pop_cv.notify()
            self.queue.append(m)


__author__ = 'Christian Hopps'
__date__ = 'January 22 2019'
__version__ = '1.0'
__docformat__ = "restructuredtext en"
