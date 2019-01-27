# -*- coding: utf-8 eval: (yapf-mode 1) -*-
#
# January 21 2019, Christian E. Hopps <chopps@gmail.com>
#
# Copyright (c) 2019 by Christian E. Hopps.
# All rights reserved.
#
# REDISTRIBUTION IN ANY FORM PROHIBITED WITHOUT PRIOR WRITTEN
# CONSENT OF THE AUTHOR.
#
from __future__ import absolute_import, division, unicode_literals, print_function, nested_scopes

import time
import logging
import threading

logger = logging.getLogger(__file__)


class Limit:
    def __init__(self, rate: int, overhead: int, count: int):
        self.rate = rate / 8
        self.overhead = overhead
        self.count = count
        self.pkttimes = [(0, 0) for x in range(0, count)]
        self.totb = 0
        self.pktidx = 0
        self.dropcnt = 0

    def limit(self, n: int):
        # Size of all packets currently in the RXQ
        n -= self.overhead
        otime = self.pkttimes[self.pktidx][1]
        ntotb = self.totb + n - self.pkttimes[self.pktidx][0]
        ntime = time.perf_counter()

        if otime:
            delta = ntime - otime
            rate = ntotb / delta
        else:
            rate = 0

        if rate > self.rate:
            self.dropcnt += 1
            return True

        # if otime and int(ntime) != int(otime):
        #     logger.info("read_packets: RXRate: %f dropcnt %d on %s",
        #          rxrate, dropcnt, ring.name)

        self.totb = ntotb
        self.pkttimes[self.pktidx] = (n, ntime)
        self.pktidx = (self.pktidx + 1) % self.count
        return False


class Periodic:
    def __init__(self, rate: int):
        # self.timestamp = time.time_ns()
        self.timestamp = time.time()
        self.ival = rate

    def wait(self):
        now = time.time()
        delta = now - self.timestamp
        waittime = self.ival - delta
        if waittime < 0:
            self.timestamp = now
            if waittime != 0:
                logging.info("Overran periodic timer by %f seconds", -waittime)
        else:
            # logging.debug("Waiting: %s", str(self.ival - delta))
            time.sleep(self.ival - delta)
            # logging.debug("Waking up!")
            self.timestamp = time.time()
        return True


class PeriodicSignal:
    def __init__(self, name: str, rate: int):
        self.cv = threading.Condition()
        self._periodic = Periodic(rate)
        self._thread = threading.Thread(name=name, target=self._periodic_signal)
        self._thread.daemon = True

    def start(self):
        self._thread.start()

    def _periodic_signal(self):
        while self._periodic.wait():
            with self.cv:
                self.cv.notifyAll()


__author__ = 'Christian E. Hopps'
__date__ = 'January 21 2019'
__version__ = '1.0'
__docformat__ = "restructuredtext en"
