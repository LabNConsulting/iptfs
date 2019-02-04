# -*- coding: utf-8 eval: (yapf-mode 1) -*-
#
# January 13 2019, Christian E. Hopps <chopps@gmail.com>
#
from __future__ import absolute_import, division, unicode_literals, print_function, nested_scopes

import binascii

import logging
import io
import socket
import sys
import threading
import time
import traceback
from . import DEBUG
from .mbuf import MBuf, MQueue
from .util import monotonic_ns, Limit, Periodic  # , PeriodicSignal
from . import util

DEBUG = False
TRACE = False

TUNMTU = 1500  # MTU for outer packets on tunnel.
HDRSPACE = 18
MAXBUF = 9000 + HDRSPACE
MAXQSZ = 32

PADBYTES = memoryview(bytearray(MAXBUF))
PADBYTES[0] = 0
logger = logging.getLogger(__file__)

# Address for sending to UDP.
peeraddr = None


def get16(mv: memoryview):
    return ((mv[0] << 8) + mv[1])


def get32(mv: memoryview):
    return ((mv[0] << 24) + (mv[1] << 16) + (mv[2] << 8) + mv[3])


def put16(m, i):
    m[0] = (i >> 8) & 0xFF
    m[1] = (i) & 0xFF


def put32(m, i):
    m[0] = (i >> 24) & 0xFF
    m[1] = (i >> 16) & 0xFF
    m[2] = (i >> 8) & 0xFF
    m[3] = (i) & 0xFF


# XXX not nice An empty mbuf full of padding.
emptym = MBuf(MAXBUF, HDRSPACE)
put32(emptym.start[4:], 0)
emptym.start[8] = 0x0

# =================
# Interface Packets
# =================


def read_intf_packets(fd: io.RawIOBase, inq: MQueue, outq: MQueue):
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


def write_intf_packets(fd: io.RawIOBase, outq: MQueue, freeq: MQueue):
    logger.info("write_packets: from %s", outq.name)
    while True:
        m = outq.pop()
        mlen = m.len()

        n = fd.write(m.start[:mlen])
        if n != mlen:
            logger.error("write: bad write %d (mlen %d) on interface", n, mlen)
        if DEBUG:
            logger.debug("write: %d bytes (%s) on interface", n, binascii.hexlify(m.start[:8]))
        freeq.push(m, True)


# ==================
# TFS Tunnel Packets
# ==================

# -------------------
# Read Tunnel Packets
# -------------------


def tunnel_get_recv_mbuf(freeq: MQueue):
    while True:
        m = freeq.pop()

        # We push acks onto our freeq to process in-band.
        if m.len() == 0:
            m.left = -1
            return m

        recv_ack(m)
        freeq.push(m, True)


def add_to_inner_packet(tmbuf: MBuf, new: bool, m: MBuf, freeq: MQueue, outq: MQueue, seq: int):  # pylint: disable=R0911,R0912,R0913,R0915

    logtmlen = tmbuf.len()
    if logtmlen <= 0:
        logger.error("ERRORX1: logtmlen %d new %d ", logtmlen, new)
    assert (logtmlen > 0)

    if not new:
        recurse = True
        offset = 0
    else:
        recurse = False
        if logtmlen < 8:
            logger.error("ERRORX2: tmlen < 8 %d recurse %d ", logtmlen, recurse)
            assert False

        offset = get16(tmbuf.start[6:8])
        # move start past frame to where offset will refer.
        tmbuf.start = tmbuf.start[8:]

    start = tmbuf.start
    tmlen = tmbuf.len()

    # if TRACE:
    #     logger.debug("add_to_inner_packet tmbuf len %d new %d mbuf %s", tmlen, new, str(m))

    if not m:
        # if DEBUG:
        #     logger.debug("add_to_inner_packet getting new mbuf")
        m = tunnel_get_recv_mbuf(freeq)

    if m.len() == 0:
        if (DEBUG and recurse):  # or TRACE:
            logger.debug("add_to_inner_packet(recures) mbuf len == 0, offset %d", offset)

        # -----------------
        # New Inner packet.
        # -----------------
        if offset > tmlen:
            if new:
                logger.debug("XXX1 Got outer packet seq: %d tmbuf.len: %d", seq, logtmlen)

            # Here we have an old packet filling the entire outer buffer
            # but no existing inner, thrown it away.
            tmbuf.start = tmbuf.end
            return m

        # skip past the existing packet we don't know about.
        start = tmbuf.start = tmbuf.start[offset:]
        tmlen = tmbuf.len()
        assert (tmlen <= m.after())

        # Check to see if the rest is padding.
        if tmlen < 6:
            if (DEBUG and recurse):  # or TRACE:
                logger.debug("add_to_inner_packet(recurse) mbuf len == 0, tmlen < 6")
            tmbuf.start = tmbuf.end
            return m

        if (start[0] & 0xF0) not in (0x40, 0x60):
            if (DEBUG and recurse):  # or TRACE:
                logger.debug("add_to_inner_packet(recurse) mbuf len == 0, padding (verbyte %d)",
                             start[0])
            tmbuf.start = tmbuf.end
            return m

        # This is logging we moved from get_outer_tunnel_packet so we can skip
        # the empties
        if DEBUG and new:
            logger.debug("Got outer packet seq: %d tmbuf.len: %d", seq, logtmlen)

        # Get the IP length
        vnibble = start[0] & 0xF0
        if vnibble == 0x40:
            iplen = get16(start[2:4])
        else:
            iplen = get16(start[4:6])

        if iplen > m.after():
            logger.error("IP length %d larger than MRU %d", iplen, m.after())
            tmbuf.start = tmbuf.end
            return m

        if DEBUG:
            logger.debug(
                "START: add_to_inner_packet(recurse: %d) mbuf len == 0, offset %d iplen %d",
                recurse, offset, iplen)

        m.left = iplen

        # Fall through
    elif offset > tmlen:
        # -------------------------------------------------------
        # Existing inner packet where all of the outer is for it.
        # -------------------------------------------------------

        # This is logging we moved from get_outer_tunnel_packet so we can skip
        # the empties
        if DEBUG and new:
            logger.debug("Got outer packet seq: %d tmbuf.len: %d", seq, logtmlen)

        # We are continuing to fill an existing inner packet, and this entire buffer is for it.
        if m.left > tmlen:
            if DEBUG:
                logger.debug("MORELEFT: recurse: %d offset>tmlen, offset %d m.left %d tmlen %d",
                             recurse, offset, m.left, tmlen)
            # XXX remove copy
            m.end[:tmlen] = start[:tmlen]
            m.end = m.end[tmlen:]
            m.left -= tmlen
            tmbuf.start = tmbuf.start[tmlen:]
            assert tmbuf.len() == 0
            return m

        if DEBUG:
            logger.debug("COMPLETE: (recurse: %d) offset>tmlen, offset: %d m.left %d tmlen %d",
                         recurse, offset, m.left, tmlen)

        # XXX remove copy
        m.end[:m.left] = start[:m.left]
        m.end = m.end[m.left:]
        tmbuf.start = tmbuf.start[m.left:]
        m.left = 0
        outq.push(m)

        # So m.left is not > than tmlen, but the offset points past the tmbuf, so this must be
        # padding at the end
        assert (m.left == tmlen)
        tmbuf.start = tmbuf.end

        # XXX What's this anyway, we have offset to next in next tmbuf, but we have used less than
        # this one?
        return None
    else:
        # ---------------------------------------------------------------------
        # Existing inner packet where the offset part of the outer is for next.
        # ---------------------------------------------------------------------

        # This is logging we moved from get_outer_tunnel_packet so we can skip
        # the empties
        if DEBUG and new:
            logger.debug("Got outer packet seq: %d tmbuf.len: %d", seq, logtmlen)

        if DEBUG:
            logger.debug(
                "CONTINUED: add_to_inner_packet(recurse: %d) mbuf len == %d, offset (for next) %d",
                recurse, m.len(), offset)

        # skip past the existing packet we don't know about.
        tmlen = offset
        assert m.left == tmlen

    if m.left > tmlen:
        # This can't be true for case 3.
        if DEBUG:
            logger.debug("MORELEFT: add_to_inner_packet(recurse: %d) offset: %d m.left %d tmlen %d",
                         recurse, offset, m.left, tmlen)
        # XXX remove copy
        m.end[:tmlen] = start[:tmlen]
        m.end = m.end[tmlen:]
        m.left -= tmlen
        tmbuf.start = tmbuf.start[tmlen:]
        assert tmbuf.len() == 0
        return m

    # We have enough to finish this inner packet.
    if DEBUG:
        logger.debug("COMPLET: add_to_inner_packet(recurse: %d) offset %d m.left %d tmlen %d",
                     recurse, offset, m.left, tmlen)
    # XXX remove copy
    m.end[:m.left] = start[:m.left]
    m.end = m.end[m.left:]
    tmbuf.start = tmbuf.start[m.left:]
    m.left = 0
    outq.push(m)

    tmlen = tmbuf.len()
    if tmlen < 0:
        logger.error("ERROR: tmlen < 0: %d recurse %d ", tmlen, recurse)
    assert (tmlen >= 0)

    if tmlen == 0:
        return None

    # Recurse!
    if DEBUG:
        logger.debug("recurse: recursed %d", recurse)
    return add_to_inner_packet(tmbuf, False, None, freeq, outq, seq)


# We really want MHeaders with MBuf chains here.
def read_tfs_packets(s, freeq: MQueue, outq: MQueue, send_ack_cv: threading.Condition,
                     max_rxrate: int):
    del send_ack_cv  # quiet the warning.
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
    m = None
    seq = 0
    tmbuf = MBuf(MAXBUF, HDRSPACE)
    while True:
        tmbuf.reset(HDRSPACE)
        (n, addr) = s.recvfrom_into(tmbuf.start)
        assert (addr == peeraddr)
        if n <= 8:
            logger.error("read: bad read len %d on TFS link, dropping", n)
            outq.dropcnt += 1
            continue

        # Check if we are forcing congestion
        if rxlimit and rxlimit.limit(n):
            logger.debug("read: Congestion Creation, dropping")
            outq.dropcnt += 1
            continue

        tmbuf.end = tmbuf.start[n:]

        offset = get32(tmbuf.start[4:8])
        # This is our hack to in-band send ACK info since we have no IKEv2.
        if (offset & 0xC0000000) == 0x40000000:
            recv_ack(tmbuf)
            continue

        if (offset & 0x80000000) != 0:
            logger.error("read: bad version on TFS link, dropping, dump: %s",
                         binascii.hexlify(tmbuf.start[:16]))
            outq.dropcnt += 1
            continue

        seq = get32(tmbuf.start[:4])
        if outq.startseq == 0:
            outq.startseq = seq

        # Drops or duplicates
        if seq <= outq.lastseq:
            if seq < outq.lastseq:
                logger.error("Previous seq number packet detected seq: %d len %d", seq, n)
            else:
                logger.warning("Duplicate packet detected seq: %d len %d", seq, n)
            # Ignore this packet it's old.
            continue

        if seq != outq.lastseq + 1 and outq.lastseq != 0:
            # record missing packets.
            outq.dropcnt += seq - (outq.lastseq + 1)
            if DEBUG:
                logger.debug("Detected packet loss (totl count: %d lasseq %d seq %d)", outq.dropcnt,
                             outq.lastseq, seq)
            # with send_ack_cv:
            #     send_ack_cv.notify()

            # abandon any in progress packet.
            if m:
                if DEBUG:
                    logger.debug("reset current inner mbuf")
                m.reset(freeq.hdrspace)

        # Consume the outer packet.
        outq.lastseq = seq
        m = add_to_inner_packet(tmbuf, True, m, freeq, outq, seq)


# ------------
# Write Tunnel
# ------------


def write_empty_tunnel_packet(s: socket.socket, send_lock: threading.Lock, seq: int, mtu: int):
    m = emptym
    mlen = mtu

    put32(m.start, seq)
    seq += 1

    with send_lock:
        n = s.sendmsg([m.start[:mlen]])
    if n != mlen:
        logger.error("write: bad empty write %d of %d on TFS link", n, mlen)
    # elif TRACE:
    #     logger.debug("write: %d bytes (%s) on TFS Link", n, binascii.hexlify(m.start[:8]))

    return None, seq


def iovlen(iov):
    iovl = 0
    for x in iov:
        iovl += len(x)
    return iovl


def write_tfs_packet(  # pylint: disable=R0912,R0913,R0914,R0915
        s: socket.socket, send_lock: threading.Lock, seq: int, mtu: int, leftover: MBuf,
        inq: MQueue, freeq: MQueue):

    # if TRACE:
    #     logger.debug("write_tfs_packet seq: %d, mtu %d", seq, mtu)

    mtuenter = mtu
    iov = []
    freem = []

    if leftover:
        if DEBUG:
            logger.debug("write_tfs_packet seq: %d, mtu %d leftover %d", seq, mtu, id(leftover))
        # This is an mbuf we didn't finish sending last time.
        m = leftover
        leftover = None
        # Set the offset to after this mbuf data.
        offset = m.len()
    else:
        # Try and get a new mbuf to embed
        m = inq.trypop()
        offset = 0
        if (DEBUG and m):  # or TRACE:
            logger.debug("write_tfs_packet: seq: %d, mtu %d m %d", seq, mtu, id(m))

    if not m:
        return write_empty_tunnel_packet(s, send_lock, seq, mtu)

    # Prepend our framing to first mbuf.
    # Would be nice but is broken. put back when we fix mbuf
    # m.prepend(8)

    assert (mtu <= mtuenter)

    # XXX create first IOV of header
    hdr = memoryview(bytearray(8))
    iov.append(hdr)
    mtu -= len(hdr)
    assert (mtu <= mtuenter)

    put32(hdr, seq)
    put16(hdr[4:], 0)
    put16(hdr[6:], offset)

    while mtu > 0:
        # We need a minimum of 6 bytes to include IPv6 length field.
        if mtu <= 6 or m is None:
            if DEBUG:
                logger.debug("write_tfs_packet: seq %d mtu %d < 6 ", seq, mtu)
            iov.append(PADBYTES[8:8 + mtu])
            assert (mtu <= mtuenter)
            mtu = 0
            break

        mlen = m.len()
        assert mlen >= 0

        if mlen > mtu:
            # Room for partial MBUF
            iov.append(m.start[:mtu])
            m.start = m.start[mtu:]
            leftover = m
            if DEBUG:
                logger.debug(
                    "write_tfs_packet: seq %d Add partial(1) MBUF mtu %d of mlen %d mtuenter %d",
                    seq, mtu, mlen, mtuenter)
            mtu = 0
            break

        # Room for full MBUF
        iov.append(m.start[:mlen])
        freem.append(m)
        m = None
        if DEBUG:
            logger.debug("write_tfs_packet: seq %d Add initial MBUF mlen %d of mtu %d mtuenter %d",
                         seq, mlen, mtu, mtuenter)
        mtu -= mlen
        if mtu > 6:
            m = inq.trypop()

    iovl = iovlen(iov)
    if iovl != mtuenter:
        logger.error("write: bad length %d of mtu %d on TFS link", iovl, mtuenter)

    with send_lock:
        n = s.sendmsg(iov)
    seq += 1  # Update sequence number now that we've written it out.

    if n != iovl:
        logger.error("write: bad write %d of %d on TFS link", n, mlen)
        if leftover:
            freem.append(leftover)
            leftover = None
    elif DEBUG:
        logger.debug("write: wrote %d bytes on TFS Link", n)

    # Free any MBufs we are done with.
    for m in freem:
        freeq.push(m, True)

    if leftover:
        assert (leftover.len() > 0)

    return leftover, seq


tunnel_target_pps = 0
tunnel_periodic = util.PeriodicPPS(1)


def write_tfs_packets(  # pylint: disable=W0613,R0913
        s: socket.socket, send_lock: threading.Lock, mtu: int, inq: MQueue, freeq: MQueue,
        rate: int):
    logger.info("write_packets: from %s", inq.name)

    # Loop writing packets limited by "rate"
    # Overhead is IP(20)+UDP(8)+Framing(4)=32
    mtub = (mtu - 32) * 8
    prate = rate / mtub
    nrate = prate * mtub
    logger.info("Writing TFS packets at rate of %d pps for %d bps", prate, nrate)

    global tunnel_target_pps  # pylint: disable=W0603
    global tunnel_periodic  # pylint: disable=W0603

    tunnel_target_pps = prate
    tunnel_periodic = util.PeriodicPPS(prate)

    leftover = None
    seq = 1
    while tunnel_periodic.wait():
        leftover, seq = write_tfs_packet(s, send_lock, seq, mtu, leftover, inq, freeq)


# ========
# ACK Info
# ========


def summin1(l):
    mval = 0
    s = 0
    for x in l:
        if x:
            mval = 1
        s += x
    return max(mval, s // len(l))


# Use integers averages.
ppsavg = util.RunningAverage(5, 0, summin1)
dropavg = util.RunningAverage(5, 0, summin1)
lastack = 0


def recv_ack(m: MBuf):  # pylint: disable=W0613,R0912
    global lastack  # pylint: disable=W0603

    if m.len() != 24:
        logger.info("Received Bad Length ACK: len: %d", m.len())
        return

    start = memoryview(m.start[4:])
    dropcnt = get32(start) & 0xFFFFFF
    ns1 = get32(start[4:])
    ns2 = get32(start[8:])
    ns = (ns1 << 32) + ns2
    ackstart = get32(start[12:])
    ackend = get32(start[16:])
    runlen = ackend - ackstart

    # XXX this all needs to be safer (check for 0 etc).
    ppsavg.add_value(runlen)
    ticked = dropavg.add_value(dropcnt)
    if lastack == 0:
        count = 1
    else:
        # Add 100ms to time to account for a bit of drift.
        count = (ns + 100000000 - lastack) // 1000000000
    lastack = ns

    if count > 1:
        logger.info("Lost ACK count: %d", count - 1)
        pps = ppsavg.average
        for _ in range(1, count):
            # Count missed ACKs as dropping 25%
            ppsavg.add_value(pps)
            if dropavg.add_value(pps // 4):
                ticked = True

    if ticked:
        # We've gone a full run so let's act.
        if dropavg.average == 0:
            # Increase rate as we have zero drops.
            if tunnel_periodic.pps < tunnel_target_pps:
                target = tunnel_periodic.pps + 1
                if target > tunnel_target_pps:
                    target = tunnel_target_pps
                logger.info("Increasing send rate to %d pps", target)
                tunnel_periodic.change_rate(target)
        else:
            # decrease by 1/4 droppct
            droppct = dropavg.average * 25 / ppsavg.average
            if not droppct:
                droppct = 1
            target = max(tunnel_periodic.pps * (100 - droppct) // 100, 1)
            if target < 0:
                target = tunnel_target_pps * 1 // 100
            logger.info("Decreasing send rate to %d pps due to dropavg: %d (%d pct)", target,
                        dropavg.average, 2 * droppct)
            tunnel_periodic.change_rate(target)

    if dropcnt:
        pct = 100 * dropcnt / (ackend - ackstart)
        logger.info("Received ACK: drop %d/%d%% start %d end %d timestamp %d:%d", dropcnt, pct,
                    ackstart, ackend, ns1, ns2)
    elif DEBUG:
        logger.debug("Received ACK: drop %d start %d end %d timestamp %d:%d", dropcnt, ackstart,
                     ackend, ns1, ns2)


# def send_ack_infos(s: socket.socket, cv: threading.Condition, outq: MQueue):
def send_ack_infos(s: socket.socket, send_lock: threading.Lock, rate: float, outq: MQueue):
    m = MBuf(MAXBUF, HDRSPACE)
    start = m.start[4:]

    periodic = Periodic(rate)
    # No sequence number
    put32(m.start, 0xFFFFFFFF)

    time.sleep(3)

    m.end = m.start[24:]
    while periodic.wait():
        # cv.acquire()
        # cv.wait()
        # cv.release()

        with outq.lock:
            # If we haven't seen any sequence (since last reset):
            if outq.startseq == 0:
                continue
            dropcnt = outq.dropcnt
            outq.dropcnt = 0
            ackstart = outq.startseq
            outq.startseq = 0
            ackend = outq.lastseq

        if dropcnt > 0xFFFFFF:
            dropcnt = 0xFFFFFF
        ns = monotonic_ns()

        # We use the 2nd bit to indicate this is an ACK this normally goes in IKEv2
        put32(start, (0x40000000 | dropcnt))
        put32(start[4:], (ns >> 32) & 0xFFFFFFFF)
        put32(start[8:], (ns & 0xFFFFFFFF))
        put32(start[12:], ackstart)
        put32(start[16:], ackend)

        with send_lock:
            n = s.sendmsg([m.start[:24]])
        if n != 24:
            logger.error("write: bad ack write %d of %d on TFS link", n, 24)
        if DEBUG:
            logger.debug("write ack: %d bytes (%s) on TFS Link", n, binascii.hexlify(m.start[4:24]))


# =======
# Generic
# =======


def thread_catch(func, name, *args):
    def thread_main():
        try:
            func(*args)
        except Exception as e:  # pylint: disable=W0612  # pylint: disable=W0703
            logger.critical("%s%s: Uncaught exception: \"%s\"", func.__name__, str(args), repr(e))
            traceback.print_exc()
            sys.exit(1)

    return threading.Thread(name=name, target=thread_main)


def tunnel_ingress(riffd: io.RawIOBase, s: socket.socket, send_lock: threading.Lock, rate: int):
    freeq = MQueue("TFS Ingress FREEQ", MAXQSZ, MAXBUF, HDRSPACE, DEBUG)
    outq = MQueue("TFS Ingress OUTQ", MAXQSZ, 0, 0, DEBUG)

    threads = [
        thread_catch(read_intf_packets, "IFREAD", riffd, freeq, outq),
        thread_catch(write_tfs_packets, "TFSLINKWRITE", s, send_lock, TUNMTU, outq, freeq, rate),
    ]

    for t in threads:
        t.daemon = True
        t.start()

    return threads


def tunnel_egress(s: socket.socket, send_lock: threading.Lock, wiffd: io.RawIOBase, ack_rate: float,
                  congest_rate: int):
    freeq = MQueue("TFS Egress FREEQ", MAXQSZ, MAXBUF, HDRSPACE, DEBUG)
    outq = MQueue("TFS Egress OUTQ", MAXQSZ, 0, 0, DEBUG)

    #send_ack_periodic = PeriodicSignal("ACK Signal", ack_rate)

    threads = [
        thread_catch(read_tfs_packets, "TFSLINKREAD", s, freeq, outq, None, congest_rate),
        thread_catch(write_intf_packets, "IFWRITE", wiffd, outq, freeq),
        thread_catch(send_ack_infos, "ACKINFO", s, send_lock, ack_rate, outq),
    ]

    for t in threads:
        t.daemon = True
        t.start()

    #send_ack_periodic.start()

    return threads


__author__ = 'Christian E. Hopps'
__date__ = 'January 13 2019'
__version__ = '1.0'
__docformat__ = "restructuredtext en"
