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


class Limit:
    def __init__(self, rate, overhead, count):
        self.rate = rate / 8
        self.overhead = overhead
        self.count = count
        self.pkttimes = [(0, 0) for x in range(0, count)]
        self.totb = 0
        self.pktidx = 0
        self.dropcnt = 0

    def limit(self, n):
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

__author__ = 'Christian E. Hopps'
__date__ = 'January 21 2019'
__version__ = '1.0'
__docformat__ = "restructuredtext en"
