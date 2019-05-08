/*
 * -*- coding: utf-8 -*-*
 *
 * January 12 2019, Christian E. Hopps <chopps@labn.net>
 *
 * Copyright (c) 2019, LabN Consulting, L.L.C.
 * All Rights Reserved.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 * http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include <arpa/inet.h>
#include <assert.h>
#include <err.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/param.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>

#include "iptfs.h"

/* Globals for write_tfs_pkt */
ssize_t g_tfsmtu;
uint g_max_inner_pkt;
struct msghdr g_wtfs_msg;
struct iovec *g_wtfs_iovecs;
struct mbuf **g_wtfs_freem;
uint g_wtfs_niov;
uint8_t g_wtfs_hdr[8];

uint8_t padbytes[MAXBUF];

extern struct sockaddr_in peeraddr; /* XXX remove */
struct pps *g_pps;

static __inline__ void
put32(uint8_t *buf, uint32_t value)
{
	*buf++ = (value >> 24);
	*buf++ = (value >> 16);
	*buf++ = (value >> 8);
	*buf++ = value;
}

static __inline__ void
put16(uint8_t *buf, uint32_t value)
{
	*buf++ = (value >> 8);
	*buf++ = value;
}

static __inline__ uint32_t
get32(uint8_t *buf)
{
	return ((uint32_t)buf[0] << 24) + ((uint32_t)buf[1] << 16) +
	       ((uint32_t)buf[2] << 8) + buf[3];
}

static __inline__ uint16_t
get16(uint8_t *buf)
{
	return ((uint16_t)buf[0] << 8) + buf[1];
}

void send_ack(int s, struct timespec *now, struct ackinfo *ack);
void recv_ack(struct mbuf *m);

/*
 * =================
 * Interface Packets
 * =================
 */

void
read_intf_pkts(int fd, struct mqueue *freeq, struct mqueue *outq)
{
	int zeros;
	LOG("read_intf_pkts: START\n");
	while (true) {
		struct mbuf *m = mqueue_pop(freeq);
		ssize_t n;

		if ((n = read(fd, m->start, MBUF_AVAIL(m))) < 0) {
			warn("read_intf_pkts: bad read %ld\n", n);
			mqueue_push(freeq, m, true);
			continue;
		}

		if (n == 0) {
			zeros++;
			continue;
		}

		// DBG("read_intf_pkts: %ld bytes on interface\n", n);
		m->end = m->start + n;
		int depth = mqueue_push(outq, m, false);
		DBG("read_intf_pkts: pushed %ld bytes outq depth %d zero read "
		    "count %d\n",
		    n, depth, zeros);
		zeros = 0;
	}
}

void
write_intf_pkts(int fd, struct miovq *outq, struct miovq *freeq)
{
	LOG("write_intf_pkts: START\n");
	while (true) {
		struct miov *m = miovq_pop(outq);
		ssize_t n;

		if ((n = writev(fd, m->iov, m->niov)) != m->len)
			warn("write_intf_pkts: bad write %ld/%ld\n", n, m->len);
		else
			DBG("write_intf_pkts: %ld bytes\n", n);
		miovq_push(freeq, m);
	}
}

/*
 * ==================
 * TFS Tunnel Packets
 * ==================
 */

/*
 * ----------------
 * Read TFS Packets
 * ----------------
 */

/* struct mbuf * */
/* tfs_get_recv_mbuf(struct mqueue *freeq) */
/* { */
/* 	while (true) { */
/* 		struct mbuf *m = mqueue_pop(freeq); */
/* 		if (MBUF_LEN(m) == 0) { */
/* 			m->left = -1; */
/* 			return m; */
/* 		} */
/* 		recv_ack(m); */
/* 		mqueue_push(freeq, m, true); */
/* 	} */
/* } */

static ssize_t __inline__ getiplen(struct miov *m, struct mbuf *tbuf)
{
	switch (*(uint8_t *)m->iov[0].iov_base & 0xF0) {
	case 0x4:
		return get16(&tbuf->start[2 - m->len]);
	case 0x6:
		return get16(&tbuf->start[4 - m->len]);
	default:
		assert(0);
	}
}

/*
 * add_to_inner_packet adds data from an outer packet mbuf to an inner one
 */
struct miov *
add_to_inner_packet(struct mbuf *tbuf, bool new, struct miov *m,
		    struct miovq *freeq, struct miovq *outq, uint32_t seq)
{
	ssize_t ltlen = MBUF_LEN(tbuf);
	uint16_t tlen, offset = 0;
	uint8_t *start;

	if (ltlen <= 0)
		errx(1, "tlen %ld <= 0 new %d", ltlen, new);

	// XXX why are we failing with tlen
	if (new) {
		/* Starting a new tfs buf */
		assert(ltlen > 8);
		offset = get16(&tbuf->start[6]);
		tbuf->start += 8;
	}

	start = tbuf->start;
	tlen = MBUF_LEN(tbuf);

	/*
	 * Get a new inner packet buf if we have none
	 */
	if (!m)
		m = miovq_pop(freeq);

	if (MBUF_LEN(tbuf) != tlen)
		errx(1, "XXX6 MBUF_LEN(tbuf) %ld tlen %d", MBUF_LEN(tbuf),
		     tlen);

	if (m->len == 0) {
		if (!new)
			DBG("add_to_inner_packet: recurse mlen 0 off %d\n",
			    offset);
		/*
		 * Start new inner packet
		 */

		if (offset >= tlen) {
			/* next data block is past the end of this packet. */
			tbuf->start = tbuf->end;
			return m;
		}

		/* Skip past unknown leading packet part */
		start = tbuf->start = tbuf->start + offset;
		tlen -= offset;

		uint8_t vnib = start[0] & 0xF0;
		uint16_t iplen = 0;
		if (vnib == 0x40) { /* IPv4 */
			if (tlen >= 4)
				iplen = get16(&start[2]);
		} else if (vnib == 0x60) { /* IPv6 */
			if (tlen >= 6)
				iplen = get16(&start[4]);
		} else if (vnib == 0x0) {
			DBG("PAD: new %d len %d\n", new, tlen);
			tbuf->start = tbuf->end;
			return m;
		}
		if (iplen == 0) {
			DBG("STARTSHORT: new %d v %d len %d\n", new, vnib,
			    tlen);
			miov_addmbuf(m, tbuf, start, tlen);
			tbuf->start += tlen;
			assert(MBUF_LEN(tbuf) == 0);
			return m;
		}
		DBG("START: add_to_inner_packet: new %d off %d iplen %d\n", new,
		    offset, iplen);

		m->left = iplen;

		/* FALLTHROUGH*/
		if (MBUF_LEN(tbuf) != tlen)
			errx(1, "XXX7 MBUF_LEN(tbuf) %ld tlen %d",
			     MBUF_LEN(tbuf), tlen);
	} else if (offset > tlen) {
		/* This is us logging here what we didn't in in get outer */
		if (new)
			DBG("got_outer: seq %d tlen %ld\n", seq, ltlen);

		/* Initial mbuf wasn't large enough to include length */
		if (m->left <= 0)
			m->left = getiplen(m, tbuf);

		if (m->left > tlen) {
			// XXX Code Copy A
			DBG("MORE: off>tlen: new %d off %d mleft %ld "
			    "tlen %d\n",
			    new, offset, m->left, tlen);

			miov_addmbuf(m, tbuf, start, tlen);
			m->left -= tlen;
			tbuf->start += tlen;
			assert(MBUF_LEN(tbuf) == 0);
			return m;
		}

		/*
		 * XXX Offset points into subsequent packet, but we had enough
		 * room in this packet. This should never happen, but if it does
		 * we treat the slop at the end as pad.
		 */
		// XXX copy B
		DBG("SLOPPY END: off>tlen: new %d off %d mleft %ld tlen %d\n",
		    new, offset, m->left, tlen);
		miov_addmbuf(m, tbuf, start, m->left);
		tbuf->start += m->left;
		m->left = 0;
		miovq_push(outq, m);

		tbuf->start = tbuf->end; /* Skip slop */
		return (NULL);
	} else {
		/*
		 * Existing inner packet where offset is within the TFS buffer.
		 */
		/* This is us logging here what we didn't in in get outer */
		if (new)
			DBG("got_outer: seq %d tlen %ld\n", seq, ltlen);

		/* Initial mbuf wasn't large enough to include length */
		if (m->left <= 0)
			m->left = getiplen(m, tbuf);

		DBG("CONTINUED: new %d off for next %d mleft %ld tlen %d\n",
		    new, offset, m->left, tlen);
		tlen = offset;
		assert(m->left == tlen);
	}

	if (m->left > tlen) {
		// XXX copy A
		DBG("MORELEFT: new %d off %d mleft %ld tlen %d\n", new, offset,
		    m->left, tlen);
		miov_addmbuf(m, tbuf, start, tlen);
		m->left -= tlen;
		tbuf->start += tlen;
		assert(MBUF_LEN(tbuf) == 0);
		return m;
	}

	// XXX copy B
	DBG("COMPLETE: new %d off %d mleft %ld tlen %d\n", new, offset, m->left,
	    tlen);
	miov_addmbuf(m, tbuf, start, m->left);
	tbuf->start += m->left;
	tlen -= m->left;
	m->left = 0;
	miovq_push(outq, m);

	// XXX This is needed if we are fetching the first part of a split
	// packet.
	tlen = MBUF_LEN(tbuf);

	if (tlen == 0)
		return NULL;

	if (MBUF_LEN(tbuf) != tlen)
		errx(1, "MBUF_LEN(tbuf) %ld tlen %d", MBUF_LEN(tbuf), tlen);

	// Recurse!
	DBG("recurse: new %d\n", new);
	return add_to_inner_packet(tbuf, false, NULL, freeq, outq, seq);
}

void
read_tfs_pkts(int s, struct mqueue *freeq, struct miovq *iovfreeq,
	      struct miovq *outq, uint64_t ack_rate, uint64_t congest_rate)
{
	stimer_t acktimer;

	struct ratelimit *rl = NULL;
	if (congest_rate > 0) {
		// uint overhead = 20 + 8 + 20 + 20 + 12;
		rl = new_ratelimit(congest_rate, 0, 10);
	}

	/* convert ack rate from ms to ns */
	ack_rate *= 1000000;
	st_reset(&acktimer, ack_rate);

	struct ackinfo ack;
	struct sockaddr_storage sender;
	socklen_t addrlen;
	ssize_t offset, n;
	uint32_t seq = 0;

	struct miov *m = NULL;

	memset(&ack, 0, sizeof(ack));

	struct mbuf *tbuf = mqueue_pop(freeq);
	atomic_fetch_add(&tbuf->refcnt, 1);

	while (true) {
		/* Check to see if we should send ack info */
		if (st_check(&acktimer))
			send_ack(s, &acktimer.ts, &ack);

		/* If no one referenced the mbuf reset and re-use it */
		if (atomic_fetch_sub(&tbuf->refcnt, 1) == 1) {
			mbuf_reset(tbuf, HDRSPACE);
		} else {
			/*
			 * tbuf will get freed when all references to it
			 * drop get a new one
			 */
			tbuf = mqueue_pop(freeq);
		}
		atomic_store(&tbuf->refcnt, 1);

		addrlen = sizeof(sender);
		if ((n = recvfrom(s, tbuf->start, MBUF_AVAIL(tbuf), 0,
				  (struct sockaddr *)&sender, &addrlen)) < 0) {
			warn("read_tfs_get_outer: %ld\n", n);
			continue;
		}

		/* Check that sender == peeraddr */

		if (n == 0) {
			warnx("read_tfs_get_outer: zero-length read\n");
			continue;
		}
		if (n < 8) {
			warnx("read_tfs_get_outer: bad read len %ld\n", n);
			ack.ndrop++;
			continue;
		}
		if (rl && limit(rl, n)) {
			DBG("read_tfs_get_outer: congestion "
			    "creation\n");
			ack.ndrop++;
			continue;
		}

		tbuf->end = &tbuf->start[n];
		offset = get32(&tbuf->start[4]);

		if ((offset & 0xC0000000) == 0x40000000) {
			recv_ack(tbuf);
			continue;
		}
		if ((offset & 0x80000000) != 0) {
			warn("read_tfs_get_outer: Invalid version "
			     "dropping");
			ack.ndrop++;
			continue;
		}

		seq = get32(tbuf->start);
		if (ack.start == 0)
			ack.start = seq;
		if (seq <= ack.last) {
			warn("read_tfs_get_outer: prev/dup seq %d "
			     "detected\n",
			     seq);
			continue;
		}
		if (seq != ack.last + 1 && ack.last != 0) {
			uint32_t ndrop = seq - (ack.last + 1);
			ack.ndrop += ndrop;
			DBG("read_tfs_get_outer: packet loss %d total %d\n",
			    ndrop, ack.ndrop);

			/* drop any in-progress inner packet reconstruction */
			if (m)
				miov_reset(m, iovfreeq);
		}
		ack.last = seq;
		m = add_to_inner_packet(tbuf, true, m, iovfreeq, outq, seq);
	}
}

/*
 * -----------------
 * Write TFS Packets
 * -----------------
 */

uint32_t
write_empty_tfs_pkt(int s, uint32_t seq, uint32_t mtu)
{
	static uint8_t ebytes[MAXBUF];
	ssize_t n;

	put32(ebytes, seq);
	if ((n = send(s, ebytes, mtu, 0)) != mtu)
		warn("write_empty_tfs_pkt: short write %ld\n", n);

	return seq + 1;
}

#ifndef NDEBUG
ssize_t
iovlen(struct iovec *iov, int count)
{
	ssize_t n = 0;
	for (int i = 0; i < count; i++)
		n += iov[i].iov_len;
	return n;
}
#endif

static stimer_t sectimer;

uint32_t
write_tfs_pkt(int s, struct mqueue *inq, struct mqueue *freeq, uint32_t seq,
	      struct mbuf **leftover)
{
	static uint tcount;
	static uint ecount;

	/* XXX check for use of all iov */
	struct iovec *iov = &g_wtfs_iovecs[1];
	struct mbuf **efreem = g_wtfs_freem;
	struct mbuf *m, **freem;
	ssize_t mlen, mtu, n;
	uint16_t offset;

	mtu = g_tfsmtu;
	if (*leftover) {
		DBG("write_tfs_pkt: seq %d mtu %ld leftover %p\n", seq, mtu,
		    *leftover);
		m = *leftover;
		*leftover = NULL;
		offset = MBUF_LEN(m);
	} else {
		m = mqueue_trypop(inq);
		offset = 0;
		if (m) {
			DBG("write_tfs_pkt: trypop: seq %d mtu %ld m %p\n", seq,
			    mtu, m);
		}
	}
	tcount++;

	if (!st_check(&sectimer)) {
		if (m == NULL)
			ecount++;
	} else {
		LOG("write_tfs_pkt: empty %d of %d (used %d)\n", ecount, tcount,
		    tcount - ecount);
		ecount = 0;
		tcount = 0;
	}

	if (m == NULL) {
		*leftover = NULL;
		return write_empty_tfs_pkt(s, seq, mtu);
	}

	/* Set the header */
	put32(g_wtfs_hdr, seq);
	put16(&g_wtfs_hdr[6], offset);
	mtu -= sizeof(g_wtfs_hdr);

	while (mtu > 0) {
		if (mtu <= 6 || m == NULL) {
			/* No room for dbhdr or no more data -- pad */
			DBG("write_tfs_pkt: seq %d pad %ld enter %ld\n", seq,
			    mtu, g_tfsmtu);
			(*iov).iov_base = padbytes;
			(*iov++).iov_len = mtu;
			mtu = 0;
			break;
		}

		mlen = MBUF_LEN(m);
		if (mlen > mtu) {
			/* Room for partial MBUF */
			(*iov).iov_base = m->start;
			(*iov++).iov_len = mtu;
			m->start += mtu;
			*leftover = m;
			DBG("write_tfs_pkt: seq %d add partial mtu %ld "
			    "of %ld "
			    "enter %ld\n",
			    seq, mtu, mlen, g_tfsmtu);
			mtu = 0;
			break;
		}

		/* Room for full MBUF */
		(*iov).iov_base = m->start;
		(*iov++).iov_len = mlen;
		*efreem++ = m;
		m = NULL;
		DBG("write_tfs_pkt: seq %d add mbuf %ld of mtu %ld enter %ld\n",
		    seq, mlen, mtu, g_tfsmtu);
		mtu -= mlen;

		/* Get next MBUF if we have space */
		if (mtu > 6)
			m = mqueue_trypop(inq);
	}

	/* XXX assert check on length adding up */
	uint niov = iov - g_wtfs_iovecs;
	ssize_t iovl = iovlen(g_wtfs_iovecs, niov);
	if (iovl != g_tfsmtu) {
		errx(1, "iovlen(g_wtfs_iovecs, %d) => %ld, g_tfsmtu %ld", niov,
		     iovl, g_tfsmtu);
	}
	assert(iovl == g_tfsmtu);

	/* Send the TFS packet */
	g_wtfs_msg.msg_iovlen = iov - g_wtfs_iovecs;
	n = sendmsg(s, &g_wtfs_msg, 0);
	seq++;
	if (n != g_tfsmtu) {
		warn("write_tfs_pkt: short write %ld of %ld\n", n, g_tfsmtu);
		if (*leftover)
			*efreem++ = *leftover;
		*leftover = NULL;
	}

	/* Pushed used MBUFS onto freeq XXX make this a single call. */
	for (freem = g_wtfs_freem; freem < efreem; freem++)
		mqueue_push(freeq, *freem, true);

	return seq;
}

void
write_tfs_pkts(int s, struct mqueue *outq, struct mqueue *freeq,
	       uint64_t txrate)
{
	struct mbuf *leftover = NULL;
	uint32_t seq = 1;
	uint64_t mtub = (g_tfsmtu - 32) * 8;
	uint64_t pps = txrate / mtub;

	st_reset(&sectimer, NSECS_IN_SEC);

	g_pps = pps_new(pps);
	LOG("Writing TFS %ld pps for %ld Mbps\n", pps, pps * mtub / 1000000);

	/* Allocate globals for writing to TFS */
	g_max_inner_pkt = MAXBUF / 20;
	g_wtfs_iovecs = xmalloc(sizeof(struct iovec) * g_max_inner_pkt);
	g_wtfs_freem = xmalloc(sizeof(struct mbuf *) * g_max_inner_pkt);
	g_wtfs_iovecs[0].iov_base = g_wtfs_hdr;
	g_wtfs_iovecs[0].iov_len = sizeof(g_wtfs_hdr);
	g_wtfs_msg.msg_iov = g_wtfs_iovecs;

	while (true) {
		pps_wait(g_pps);
		seq = write_tfs_pkt(s, outq, freeq, seq, &leftover);
	}
}

/*
 * ====
 * ACKs
 * ====
 */

static struct runavg *g_avgpps;
static struct runavg *g_avgdrops;

void
recv_ack(struct mbuf *m)
{
	if (MBUF_LEN(m) != 20) {
		warn("recv_ack: bad length %ld\n", MBUF_LEN(m));
		return;
	}
	uint8_t *mstart = m->start;
	assert(get32(mstart) == 0xFFFFFFFF);
	mstart += 4;
	// uint32_t seq = get32(mstart);
	uint32_t ndrop = get32(mstart) & 0xFFFFFF;
	uint32_t start, end, runlen;

	/* Unpack congestion information */
	start = get32(&mstart[8]);
	end = get32(&mstart[12]);
	runlen = end - start;

	if (end < start) {
		DBG("recv_ack: bad sequence range %d, %d\n", start, end);
		return;
	}

	// XXX Lou thinks thinks its pointless to try and deal
	// with lost ACk info b/c outbound congestion has no
	// direct bearing on inbound ACK info. Probably right.

	runavg_add(g_avgpps, runlen);
	if (!runavg_add(g_avgdrops, ndrop)) {
		// Wait for enough data to start.
		LOG("recv_ack: ndrop %d pcount %d\n", ndrop, end - start);
		return;
	}
	uint64_t mtub = (g_tfsmtu - 32) * 8;
	if (g_avgdrops->average == 0) {
		// Not degraded, increase rate
		uint64_t pps = pps_change_pps(g_pps, 1);
		LOG("recv_ack: upgrading ndrop %d pcount %d: inc %d to %ld pps "
		    "%ldMbps\n",
		    ndrop, end - start, 1, pps, pps * mtub / 1000000);
	} else {
		// Degraded, slow rate byte 1/4 of the droppct.
		int droppct = (g_avgdrops->average * 100) / g_avgpps->average;
		if (droppct < 1)
			droppct = 1;
		uint64_t pps = pps_change_pps(g_pps, -g_avgdrops->average);
		LOG("recv_ack: ndrop %d avg %d pcount %d: reduce to %ld pps "
		    "%ld "
		    "Mbps\n",
		    ndrop, g_avgdrops->average, end - start, pps,
		    (pps * mtub) / 1000000);
		// uint64_t pps = pps_decrate(g_pps, (100 - droppct));
		/* LOG("recv_ack: ndrop %d avg %d pcount %d: reduce %d%% to " */
		/*     "%d pps %d Mbps\n", */
		/*     ndrop, g_avgdrops->average, end - start, (100 - droppct),
		 */
		/*     pps, (pps * mtub) / 1000000); */
	}
}

void
send_ack(int s, struct timespec *now, struct ackinfo *ack)
{
	/* first 4 bytes are sequence which acks have none */
	static uint8_t buffer[20] = {
	    0xFF,
	    0xFF,
	    0xFF,
	    0xFF,
	};
	static uint8_t *bp = &buffer[4];

	if (ack->start == 0) {
		/* nothing to talk about */
		return;
	}

	uint32_t ndrop = ack->ndrop;
	if (ndrop > 0xFFFFFF)
		ndrop = 0xFFFFFF;
	put32(bp, 0x40000000 | ndrop);
	put32(&bp[4], now->tv_sec * 1000 + now->tv_nsec / 1000000);
	put32(&bp[8], ack->start);
	put32(&bp[12], ack->last);
	memset(ack, 0, sizeof(*ack));

	ssize_t n;
	if ((n = send(s, buffer, sizeof(buffer), 0)) != sizeof(buffer))
		warn("send_acks: short write: %ld\n", n);
	else
		DBG("write ack: %ld bytes\n", n);
}

struct thread_args {
	struct mqueue *freeq, *outq;
	struct miovq *iovfreeq, *iovoutq;
	uint64_t rate;
	int fd;
	int s;
};

static void *
_read_intf_pkts(void *_arg)
{
	struct thread_args *args = _arg;
	read_intf_pkts(args->fd, args->freeq, args->outq);
	return NULL;
}

static void *
_write_intf_pkts(void *_arg)
{
	struct thread_args *args = _arg;
	write_intf_pkts(args->fd, args->iovoutq, args->iovfreeq);
	return NULL;
}

static void *
_read_tfs_pkts(void *_arg)
{
	struct thread_args *args = _arg;
	read_tfs_pkts(args->s, args->freeq, args->iovfreeq, args->iovoutq, 1000,
		      args->rate);
	return NULL;
}

static void *
_write_tfs_pkts(void *_arg)
{
	struct thread_args *args = _arg;
	write_tfs_pkts(args->s, args->outq, args->freeq, args->rate);
	return NULL;
}

/*
 * tunnel - tunnel from tun intf over a TCP connection.
 */
int
tfs_tunnel_ingress(int fd, int s, uint64_t txrate, pthread_t *threads)
{
	static struct thread_args args;

	DBG("tfs_tunnel_ingress: fd: %d s: %d\n", fd, s);
	args.freeq =
	    mqueue_new_freeq("TFS Ingress FreeqQ", INNERQSZ, MAXBUF, HDRSPACE);
	args.outq = mqueue_new("TFS Ingress OutQ", INNERQSZ);
	args.rate = txrate;
	args.fd = fd;
	args.s = s;

	g_max_inner_pkt = MAXBUF / 20;

	pthread_create(&threads[0], NULL, _read_intf_pkts, &args);
	pthread_create(&threads[1], NULL, _write_tfs_pkts, &args);
	return 0;
}

int
tfs_tunnel_egress(int fd, int s, uint64_t congest, pthread_t *threads)
{
	static struct thread_args args;

	DBG("tfs_tunnel_egress: fd: %d s: %d\n", fd, s);
	args.freeq =
	    mqueue_new_freeq("TFS Egress FreeqQ", OUTERQSZ, MAXBUF, HDRSPACE);

	uint maxiov = (MAXBUF / g_tfsmtu) + 2;
	args.iovfreeq = miovq_new_freeq("TFS IOV Egress FreeqQ", OUTERQSZ,
					maxiov, args.freeq);
	args.iovoutq = miovq_new("TFS Egress OutQ", OUTERQSZ);

	args.rate = congest;
	args.fd = fd;
	args.s = s;

	pthread_create(&threads[0], NULL, _read_tfs_pkts, &args);
	pthread_create(&threads[1], NULL, _write_intf_pkts, &args);

	g_avgpps = runavg_new(5, 1);
	g_avgdrops = runavg_new(5, 1);
	return 0;
}

/* Local Variables: */
/* c-file-style: "bsd" */
/* c-c++-enable-clang-format-on-save: t */
/* End: */
