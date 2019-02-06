# -*- coding: utf-8 eval: (yapf-mode 1) -*-
#
# January 21 2019, Christian E. Hopps <chopps@gmail.com>
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

import time
import logging
import threading

logger = logging.getLogger(__file__)

SEC_NANOSECS = 1000000000

try:
    clock_gettime_ns = time.clock_gettime_ns
except AttributeError:

    def clock_gettime_ns(clock):
        fp = time.clock_gettime(clock)
        ns = int(fp * SEC_NANOSECS)
        return ns


def monotonic_ns():
    return clock_gettime_ns(time.CLOCK_MONOTONIC)


def monotonic():
    return time.clock_gettime(time.CLOCK_MONOTONIC)


class Timestamp:
    """A way to track the lifetime left of an object"""

    def __init__(self):
        self.timestamp = monotonic()

    def reset(self):
        self.timestamp = monotonic()

    def elapsed(self):
        return monotonic() - self.timestamp


class RunningAverage:
    def __init__(self, runlen, defval=int(0), avgf=None):
        self.runlen = runlen
        self.values = [defval] * runlen
        self.index = 0
        self.ticks = 0

        if avgf is None:
            self.avgf = lambda l: sum(l) / len(l)
        else:
            self.avgf = avgf

        self.average = self.avgf(self.values)

    def add_value(self, value):
        """add_value adds a new value to the running average.

        Returns True if a full run has occurred.
        """
        self.values[self.index] = value
        self.index += 1
        rv = False
        if self.index == self.runlen:
            self.ticks += 1
            self.index = 0
            rv = True
        self.average = self.avgf(self.values)
        return rv


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
    def __init__(self, rate: float):
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


class PeriodicPPS:
    def __init__(self, pps: int):
        # self.timestamp = time.time_ns()
        self.ival_lock = threading.Lock()
        self.timestamp = time.time()
        self.pps = pps
        self.ival = 1.0 / pps

    def change_rate(self, pps: int):
        with self.ival_lock:
            if pps != self.pps:
                self.pps = pps
                self.ival = 1.0 / pps
                return True
        return False

    def wait(self):
        with self.ival_lock:
            ival = self.ival
        now = time.time()
        delta = now - self.timestamp
        waittime = ival - delta
        if waittime < 0:
            self.timestamp = now
            if waittime != 0:
                logging.info("Overran periodic timer by %f seconds", -waittime)
        else:
            # logging.debug("Waiting: %s", str(self.ival - delta))
            time.sleep(ival - delta)
            # logging.debug("Waking up!")
            self.timestamp = time.time()
        return True

    def waitspin(self):
        with self.ival_lock:
            expire = self.timestamp + self.ival
        now = time.time()
        if now > expire:
            logging.info("Overran periodic timer by %f seconds", now - expire)
        else:
            while time.sleep(0):
                now = time.time()
                if now > expire:
                    break
        self.timestamp = now
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
