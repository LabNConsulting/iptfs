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
import traceback
from . import DEBUG
from .mbuf import MBuf, MQueue
from .util import Limit, Periodic

DEBUG = False

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

# -----------------
# Interface Packets
# -----------------


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


# ------------------
# TFS Tunnel Packets
# ------------------


def recv_ack(inq: MQueue, m: MBuf):  # pylint: disable=W0613
    pass


def tunnel_get_recv_mbuf(freeq: MQueue):
    while True:
        m = freeq.pop()

        # We push acks onto our freeq to process in-band.
        if m.len() == 0:
            m.left = -1
            return m

        recv_ack(freeq, m)
        freeq.push(m, True)


def add_to_inner_packet(tmbuf: MBuf, new: bool, m: MBuf, freeq: MQueue, outq: MQueue, recurse=False):  # pylint: disable=R0911,R0912

    if not new:
        offset = 0
    else:
        offset = get16(tmbuf.start[6:8])
        # move start past frame to where offset will refer.
        tmbuf.start = tmbuf.start[8:]

    start = tmbuf.start
    tmlen = tmbuf.len()

    # if TRACE:
    #     logger.debug("add_to_inner_packet tmbuf len %d new %d mbuf %s", tmlen, new, str(m))

    if not m:
        if DEBUG:
            logger.debug("add_to_inner_packet getting new mbuf")
        m = tunnel_get_recv_mbuf(freeq)

    if m.len() == 0:
        if DEBUG and recurse:
            logger.debug("add_to_inner_packet mbuf len == 0, offset %d", offset)
        # if TRACE:
        #     logger.debug("add_to_inner_packet mbuf len == 0, offset %d", offset)

        # -----------
        # New packet.
        # -----------
        if offset > tmlen:
            # Here we have an old packet filling the entire outer buffer
            # but no existing inner, thrown it away.
            return m, 0

        # skip past the existing packet we don't know about.
        if True:  # pylint: disable=W0125
            start = tmbuf.start = tmbuf.start[offset:]
            tmlen = tmbuf.len()
            assert (tmlen <= m.after())

            # Check to see if the rest is padding.
            if tmlen < 6:
                if DEBUG and recurse:
                    logger.debug("add_to_inner_packet mbuf len == 0, tmlen < 6")
                # if TRACE:
                #     logger.debug("add_to_inner_packet mbuf len == 0, tmlen < 6")
                tmbuf.start = tmbuf.end
                return m

            if (start[0] & 0xF0) not in (0x40, 0x60):
                if DEBUG and recurse:
                    logger.debug("add_to_inner_packet mbuf len == 0, padding (verbyte %d)",
                                 start[0])
                # if TRACE:
                #     logger.debug("add_to_inner_packet mbuf len == 0, padding (verbyte %d)",
                #                  start[0])
                tmbuf.start = tmbuf.end
                return m

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

        m.left = iplen
        # Fall through
    elif offset > tmlen:
        # We are continuing to fill an existing inner packet, and this entire buffer is for it.
        if m.left > tmlen:
            m.end[:tmlen] = start[:tmlen]
            m.end = m.end[tmlen:]
            m.left -= tmlen
            tmbuf.start = tmbuf.start[tmlen:]
            return m
        m.end[:m.left] = start[:m.left]
        m.end = m.end[m.left:]
        m.left = 0
        tmbuf.start = tmbuf.start[m.left:]
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
        tmbuf.start = tmbuf.start[tmlen:]
        return m

    # We have enough to finish this inner packet.
    m.end[:m.left] = start[:m.left]
    m.end = m.end[m.left:]
    tmbuf.start = tmbuf.start[m.left:]
    m.left = 0
    outq.push(m)

    # Recurse!
    if tmbuf.len() == 0:
        return None
    logger.debug("recurse: recurse %d", recurse)
    return add_to_inner_packet(tmbuf, False, None, freeq, outq, True)


def tunnel_get_outer_packet(s: socket.socket, tmbuf: MBuf, outq: MQueue, rxlimit: Limit):
    while True:
        tmbuf.reset(HDRSPACE)
        (n, addr) = s.recvfrom_into(tmbuf.start)
        assert (addr == peeraddr)
        if n <= 8:
            logger.error("read: bad read len %d on TFS link, dropping", n)
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
            tmbuf.end = tmbuf.start[n:]
            outq.lastseq = seq
            # if TRACE:
            #     logger.debug("Got outer packet seq: %d len: %d tmbuf.len: %d", seq, n, tmbuf.len())
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


def read_tunnel_into_packet(s: socket.socket, tmbuf: MBuf, freeq: MQueue, outq: MQueue,
                            rxlimit: Limit):
    m = None
    seq = 0
    while True:
        # If we don't have a current outer packet get one.
        if seq == 0 or tmbuf.len() == 0:
            seq, reset = tunnel_get_outer_packet(s, tmbuf, outq, rxlimit)

        if m and reset:
            m.reset(freeq.hdrspace)

        # Consume the outer packet.
        m = add_to_inner_packet(tmbuf, True, m, freeq, outq)


# We really want MHeaders with MBuf chains here.
def read_tunnel_packets(s, freeq: MQueue, outq: MQueue, max_rxrate: int):
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
    tmbuf = MBuf(MAXBUF, HDRSPACE)
    while True:
        read_tunnel_into_packet(s, tmbuf, freeq, outq, rxlimit)


def write_empty_tunnel_packet(s: socket.socket, seq: int, mtu: int):
    m = emptym
    mlen = mtu

    put32(m.start, seq)
    seq += 1

    n = s.sendmsg([m.start[:mlen]])
    if n != mlen:
        logger.error("write: bad empty write %d of %d on TFS link", n, mlen)
    elif DEBUG:
        # logger.debug("write: %d bytes (%s) on TFS Link", n, binascii.hexlify(m.start[:8]))
        pass

    return None, seq


def write_tunnel_packet(  # pylint: disable=R0912,R0913,R0915
        s: socket.socket, seq: int, mtu: int, leftover: MBuf, inq: MQueue, freeq: MQueue):

    # if DEBUG:
    #     logging.debug("write_tunnel_packet seq: %d, mtu %d", seq, mtu)

    iov = []
    freem = []

    if leftover:
        if DEBUG:
            logging.debug("write_tunnel_packet seq: %d, mtu %d leftover %d", seq, mtu, id(leftover))
        # This is an mbuf we didn't finish sending last time.
        m = leftover
        # Set the offset to after this mbuf data.
        offset = m.len()
    else:
        # Try and get a new mbuf to embed
        m = inq.trypop()
        offset = 0
        # if TRACE:
        #     logging.debug("write_tunnel_packet seq: %d, mtu %d m %d", seq, mtu, id(m))
        if DEBUG and m:
            logging.debug("write_tunnel_packet seq: %d, mtu %d m %d", seq, mtu, id(m))

    if not m:
        return write_empty_tunnel_packet(s, seq, mtu)

    # Prepend our framing to first mbuf.
    m.prepend(8)
    put32(m.start, seq)
    seq += 1
    put16(m.start[4:], 0)
    put16(m.start[6:], offset)

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
                iov.append(PADBYTES[8:mtu+8])
                break
            m = inq.trypop()
            if not m:
                iov.append(PADBYTES[8:mtu+8])
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

    iovlen = 0
    for x in iov:
        iovlen += len(x)
    n = s.sendmsg(iov)
    if n != iovlen:
        logger.error("write: bad write %d of %d on TFS link", n, mlen)
        if leftover:
            freem.append(leftover)
            leftover = None
    elif DEBUG:
        logger.debug("write: %d bytes on TFS Link", n)

    # Free any MBufs we are done with.
    for m in freem:
        freeq.push(m, True)

    return leftover, seq


def write_tunnel_packets(s: socket.socket, mtu: int, inq: MQueue, freeq: MQueue, rate: int):  # pylint: disable=W0613
    logger.info("write_packets: from %s", inq.name)

    # Loop writing packets limited by "rate"
    # Overhead is IP(20)+UDP(8)+Framing(4)=32
    mtub = (mtu - 32) * 8
    prate = rate / mtub
    nrate = prate * mtub
    logging.info("Writing TFS packets at rate of %d pps for %d bps", prate, nrate)

    periodic = Periodic(prate)
    leftover = None
    extram = MBuf(MAXBUF, HDRSPACE)
    seq = 0
    while periodic.wait():
        leftover, seq = write_tunnel_packet(s, seq, mtu, leftover, inq, freeq)


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


def tunnel_ingress(riffd: io.RawIOBase, s: socket.socket, rate: int):
    freeq = MQueue("TFS Ingress FREEQ", MAXQSZ, MAXBUF, HDRSPACE, DEBUG)
    outq = MQueue("TFS Ingress OUTQ", MAXQSZ, 0, 0, DEBUG)

    threads = [
        thread_catch(read_intf_packets, riffd, freeq, outq),
        thread_catch(write_tunnel_packets, s, TUNMTU, outq, freeq, rate),
    ]

    for t in threads:
        t.daemon = True
        t.start()

    return threads


def tunnel_egress(s: socket.socket, wiffd: io.RawIOBase, congest_rate: int):
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
