# -*- coding: utf-8 eval: (yapf-mode 1) -*-
#
# January 13 2019, Christian E. Hopps <chopps@gmail.com>
#
from __future__ import absolute_import, division, unicode_literals, print_function, nested_scopes

import binascii

import logging
import sys
import threading
import time
import traceback
from .util import Limit
from .mbuf import MBuf, MQueue

HDRSPACE = 18
MAXBUF = 9000 + HDRSPACE
MAXQSZ = 32

PADBYTES = memoryview(bytearray(MAXBUF))
PADBYTES[0] = 0
logger = logging.getLogger(__file__)

DEBUG = False

# Address for sending to UDP.
peeraddr = None


def get16(m):
    return ((m[0] << 8) + m[1])


def get32(m):
    return ((m[0] << 24) + (m[1] << 16) + (m[2] << 8) + m[3])


def put32(m, i):
    m[0] = (i >> 24) & 0xFF
    m[1] = (i >> 16) & 0xFF
    m[2] = (i >> 8) & 0xFF
    m[3] = (i) & 0xFF


# -----------------
# Interface Packets
# -----------------


def read_intf_packets(fd, inq, outq):
    logger.info("read: start reading from interface")
    while True:
        m = inq.pop()

        n = fd.readinto(m.start)
        if n <= 0:
            logger.error("read: bad read %d on interface, dropping", n)
            inq.push(m, True)
        else:
            if DEBUG:
                logger.debug("read: %d bytes on interface", n)
            m.end = m.start[n:]
            outq.push(m, False)


def write_intf_packets(fd, outq, freeq):
    logger.info("write_packets: from %s", outq.name)
    while True:
        m = outq.pop()
        mlen = m.len()

        n = fd.write(m.start[:mlen])
        if n != mlen:
            logger.error("write: bad write %d (mlen %d) on interface", n, mlen)
        if DEBUG:
            logger.debug("write: %d bytes (%s) on %s ", n, binascii.hexlify(m.start[:8]), str(fd))
        freeq.push(m, True)


# ------------------
# TFS Tunnel Packets
# ------------------


class Periodic:
    def __init__(self, packet_rate):
        self.prate = packet_rate
        # self.timestamp = time.time_ns()
        self.timestamp = time.time()
        self.ival = 1.0 / packet_rate

    def wait(self):
        now = time.time()
        delta = now - self.timestamp
        waittime = self.ival - delta
        if waittime < 0:
            self.timestamp = now
            if waittime != 0:
                logging.info("Overran periodic timer by %f seconds", -waittime)
        else:
            time.sleep(self.ival - delta)
            self.timestamp = time.time()


def recv_ack(inq, m):  # pylint: disable=W0613
    pass


def tunnel_get_recv_mbuf(freeq):
    while True:
        m = freeq.pop()

        # We push acks onto our freeq to process in-band.
        if m.len() == 0:
            m.left = -1
            return m

        recv_ack(freeq, m)
        freeq.push(m, True)


def add_to_inner_packet(tmbuf, new, m, freeq, outq):  # pylint: disable=R0911,R0912
    if not new:
        offset = 0
    else:
        offset = get16(tmbuf.start[6:8])
        # move start past frame to where offset will refer.
        tmbuf.start = tmbuf.start[8:]

    start = tmbuf.start
    tmlen = tmbuf.len()

    if not m:
        m = tunnel_get_recv_mbuf(freeq)

    if m.len() == 0:
        # -----------
        # New packet.
        # -----------
        if offset > tmlen:
            # Here we have an old packet filling the entire outer buffer
            # but no existing inner, thrown it away.
            return m

        # skip past the existing packet we don't know about.
        if True:
            start = tmbuf.start = tmbuf.start[offset:]
            tmlen = tmbuf.len()
            assert (tmlen <= m.after())

            # Check to see if the rest is padding.
            if tmlen < 6 or (start[0] & 0xF0) not in (0x40, 0x60):
                return m

        # Get the IP length
        vnibble = start[0] & 0xF0
        if vnibble == 0x40:
            iplen = get16(start[2:4])
        else:
            iplen = get16(start[4:6])

        if iplen > m.after():
            logger.error("IP length %d larger than MRU %d", iplen, m.after())
            return m

        m.left = iplen
        # Fall through
    elif offset > tmlen:
        # We are continuing to fill an existing inner packet, and this entire buffer is for it.
        if m.left > tmlen:
            m.end[:tmlen] = start[:tmlen]
            m.end = m.end[tmlen:]
            m.left -= tmlen
            return m
        m.end[:m.left] = start[:m.left]
        m.end = m.end[m.left:]
        m.left = 0
        outq.push(m)
        return None
    else:
        start = tmbuf.start = tmbuf.start[offset:]
        tmlen = tmbuf.len()
        assert (tmlen <= m.after())

    if m.left > tmlen:
        m.end[:tmlen] = start[:tmlen]
        m.end = m.end[tmlen:]
        m.left -= tmlen
        return m

    # We have enough to finish this inner packet.
    m.end[:m.left] = start[:m.left]
    m.end = m.end[m.left:]
    tmbuf.start = start[m.left:]
    m.left = 0
    outq.push(m)

    # Recurse!
    return add_to_inner_packet(tmbuf, False, None, freeq, outq)


def tunnel_get_outer_packet(s, tmbuf, outq, rxlimit):
    tmbuf.reset()
    while True:
        (n, addr) = s.recvfrom_into(tmbuf.start)
        assert (addr == peeraddr)
        if n <= 8:
            logger.error("read: bad read %d on TFS link, dropping", n)
            break

        # Check if we are forcing congestion
        if rxlimit and rxlimit.limit(n):
            logger.debug("read: Congestion Creation, dropping")
            outq.dropcnt += 1
            continue

        offset = get32(tmbuf.start[4:8])
        if (offset & 0x80000000) != 0:
            logger.error("read: bad version on TFS link, dropping")
            outq.dropcnt += 1
            continue

        seq = get32(tmbuf.start[:4])
        if outq.startseq == 0:
            outq.startseq = seq

        if seq == outq.lastseq + 1 or outq.lastseq == 0:
            # Make this valid.
            tmbuf.end = tmbuf.start[:n]
            outq.lastseq = seq
            return seq, True

        # Drops or duplicates

        if seq <= outq.lastseq:
            if seq < outq.lastseq:
                logger.error("Previous seq number packet detected seq: %d len %d", seq, n)
            else:
                logger.warning("Duplicate packet detected seq: %d len %d", seq, n)
            # Ignore this packet it's old.
            continue

        # record missing packets.
        outq.dropcnt += seq - (outq.lastseq + 1)
        logger.error("Detected packet loss (totl count: %d lasseq %d seq %d)", outq.dropcnt,
                     outq.lastseq, seq)

        # abandon any in progress packet.
        return seq, False


def read_tunnel_into_packet(s, tmbuf, freeq, outq, rxlimit):
    m = None
    seq = 0
    while True:
        # If we don't have a current outer packet get one.
        if seq == 0:
            seq, reset = tunnel_get_outer_packet(s, tmbuf, outq, rxlimit)

        if m and reset:
            m.reset()

        # Consume the outer packet.
        m = add_to_inner_packet(tmbuf, True, m, freeq, outq)


# We really want MHeaders with MBuf chains here.
def read_tunnel_packets(s, freeq, outq, max_rxrate):
    logger.info("read: start reading on TFS link")

    outq.startseq = 0
    outq.lastseq = 0
    outq.dropcnt = 0

    rxlimit = None
    if max_rxrate:
        # IP/UDP + IP/TCP + TCP timestamps
        # overhead = 20 + 8 + 20 + 20 + 12
        overhead = 0
        rxlimit = Limit(max_rxrate, overhead, 10) if max_rxrate else None

    # Loop reconstructing inner packets
    tmbuf = MBuf(MAXBUF, 0)
    while True:
        read_tunnel_into_packet(s, tmbuf, freeq, outq, rxlimit)


def write_tunnel_packet(s, mtu, leftover, inq, freeq):
    iov = []
    freem = []
    if leftover:
        m = leftover
        offset = m.len()
    else:
        m = inq.pop()
        offset = 0

    # Prepend our framing to first mbuf.
    m.prepend(8)
    put32(m.start, m.seq)
    put32(m.start[4:], offset)
    mlen = m.len()

    if mlen > mtu:
        remaining = mlen - mtu
        iov = [m.start[:mtu]]
        m.start = m.start[mtu:mtu + remaining]
    else:
        iov = [m.start[:mlen]]
        freem.append(m)
        mtu -= mlen

        leftover = None
        while True:
            # We need a minimum of 6 bytes to include IPv6 length field.
            if mtu <= 6:
                iov.append(PADBYTES[:mtu])
                break
            m = inq.trypop()
            if not m:
                iov.append(PADBYTES[:mtu])
                break
            mlen = m.len()
            if mlen > mtu:
                remaining = mlen - mtu
                iov = [m.start[:mtu]]
                m.start = m.start[mtu:mtu + remaining]
                leftover = m
                break
            freem.append(m)
            mtu -= mlen

    n = s.sendmsg(iov)
    if n != mlen:
        logger.error("write: bad write %d of %d on TFS link", n, mlen)
        if leftover:
            freem.append(leftover)
            leftover = None
    elif DEBUG:
        logger.debug("write: %d bytes (%s) on TFS Link", n, binascii.hexlify(m.start[:8]))

    # Free any MBufs we are done with.
    for m in freem:
        freeq.push(m, True)

    return leftover


def write_tunnel_packets(fd, mtu, inq, freeq, rate):  # pylint: disable=W0613
    logger.info("write_packets: from %s", inq.name)

    # Loop writing packets limited by "rate"
    # Overhead is IP(20)+UDP(8)+Framing(4)=32
    mtu = 1500 - 32
    mtub = mtu * 8
    prate = rate / mtub
    nrate = prate * mtub
    logging.info("Writing TFS packets at rate of %d pps for %d bps", prate, nrate)

    periodic = Periodic(prate)
    leftover = None
    while periodic.wait():
        leftover = write_tunnel_packet(fd, mtu, leftover, inq, freeq)


# -------
# Generic
# -------


def thread_catch(func, *args):
    def thread_main():
        try:
            func(*args)
        except Exception as e:  # pylint: disable=W0612  # pylint: disable=W0703
            logger.critical("%s%s: Uncaught exception: \"%s\"", func.__name__, str(args), repr(e))
            traceback.print_exc()
            sys.exit(1)

    return threading.Thread(target=thread_main)


def tunnel_ingress(riffd, s, rate):
    freeq = MQueue("TFS Ingress FREEQ", MAXQSZ, MAXBUF, HDRSPACE, DEBUG)
    outq = MQueue("TFS Ingress OUTQ", MAXQSZ, 0, 0, DEBUG)

    threads = [
        thread_catch(read_intf_packets, riffd, freeq, outq),
        thread_catch(write_tunnel_packets, s, outq, freeq, rate),
    ]

    for t in threads:
        t.daemon = True
        t.start()

    return threads


def tunnel_egress(s, wiffd, congest_rate):
    freeq = MQueue("TFS Egress FREEQ", MAXQSZ, MAXBUF, HDRSPACE, DEBUG)
    outq = MQueue("TFS Egress OUTQ", MAXQSZ, 0, 0, DEBUG)

    threads = [
        thread_catch(read_tunnel_packets, s, freeq, outq, congest_rate),
        thread_catch(write_intf_packets, wiffd, outq, freeq),
    ]

    for t in threads:
        t.daemon = True
        t.start()

    return threads


__author__ = 'Christian E. Hopps'
__date__ = 'January 13 2019'
__version__ = '1.0'
__docformat__ = "restructuredtext en"
