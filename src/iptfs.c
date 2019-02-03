/*
 * -*- coding: utf-8 -*-*
 *
 * January 12 2019, Christian E. Hopps <chopps@gmail.com>
 *
 */

#include <arpa/inet.h>
#include <assert.h>
#include <err.h>
#include <sys/param.h>
//#include <fcntl.h>
//#include <getopt.h>
//#include <linux/if.h>
//#include <linux/if_tun.h>
//#include <netdb.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>

#include "iptfs.h"

#define TFSMTU 1500
#define HDRSPACE 18
#define MAXBUF 9000 + HDRSPACE
#define MAXQSZ 32

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

void recv_ack(struct mbuf *m);

/*
 * =================
 * Interface Packets
 * =================
 */

void
read_intf_pkts(int fd, struct mqueue *freeq, struct mqueue *outq)
{
	LOG("read_intf_pkts: START\n");
	while (true) {
		struct mbuf *m = mqueue_pop(freeq);
		ssize_t n;

		if ((n = read(fd, m->start, MBUF_AVAIL(m))) < 0) {
			warn("read_intf_pkts: bad read %ld\n", n);
			mqueue_push(freeq, m, true);
			continue;
		}
		DBG("read_intf_pkts: %ld bytes on interface\n", n);
		m->end = m->start + n;
		mqueue_push(outq, m, false);
	}
}

void
write_intf_pkts(int fd, struct mqueue *outq, struct mqueue *freeq)
{
	LOG("write_intf_pkts: START\n");
	while (true) {
		struct mbuf *m = mqueue_pop(outq);
		ssize_t n, mlen = MBUF_LEN(m);
		if ((n = write(fd, m->start, mlen)) != mlen)
			warn("write_intf_pkts: bad write %ld/%ld\n", n, mlen);
		else
			DBG("write_intf_pkts: %ld bytes\n", n);
		mqueue_push(freeq, m, true);
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

struct mbuf *
tfs_get_recv_mbuf(struct mqueue *freeq)
{
	while (true) {
		struct mbuf *m = mqueue_pop(freeq);
		if (MBUF_LEN(m) == 0) {
			m->left = -1;
			return m;
		}
		recv_ack(m);
		mqueue_push(freeq, m, true);
	}
}

/*
 * add_to_inner_packet adds data from an outer packet mbuf to an inner one
 */
struct mbuf *
add_to_inner_packet(struct mbuf *tbuf, bool new, struct mbuf *m,
		    struct mqueue *freeq, struct mqueue *outq, uint32_t seq)
{
	ssize_t ltlen = MBUF_LEN(tbuf);
	uint16_t tlen, offset = 0;
	uint8_t *start;

	if (ltlen <= 0)
		err(1, "tlen %ld <= 0", ltlen);

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
		m = tfs_get_recv_mbuf(freeq);

	if (MBUF_LEN(m) == 0) {
		if (!new)
			DBG("add_to_inner_packet: recurse mlen 0 off %d\n",
			    offset);
		/*
		 * Start new inner packet
		 */

		if (offset > tlen) {
			/* next data block is past the end of this packet. */
			tbuf->start = tbuf->end;
			return m;
		}
		/* Skip past unknown leading packet part */
		start = tbuf->start = tbuf->start + offset;
		tlen -= offset;

		uint8_t vnib = start[0] & 0xF0;
		uint16_t iplen;
		if (vnib == 0x40) /* IPv4 */
			iplen = get16(&start[2]);
		else if (vnib == 0x60) /* IPv6 */
			iplen = get16(&start[4]);
		else { /* Pad or wrong. */
			// DBG("PAD: add_to_inner_packet: new %d v %d len %d\n",
			//    new, vnib, tlen);
			tbuf->start = tbuf->end;
			return m;
		}
		DBG("START: add_to_inner_packet: new %d off %d iplen %d\n", new,
		    offset, iplen);
		/* XXX we can always reconstruct this too */
		m->left = iplen;
		/* FALLTHROUGH*/
	} else if (offset > tlen) {
		/* This is us logging here what we didn't in in get outer */
		if (new)
			DBG("got_outer: seq %d tlen %ld\n", seq, ltlen);

		if (m->left > tlen) {
			// XXX copy A
			DBG("MORE: off>tlen: new %d off %d mleft %ld tlen "
			    "%ld\n",
			    new, offset, m->left, tlen);
			memcpy(m->end, start, tlen); // XXX remove copy
			m->end += tlen;
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
		DBG("SLOPPY END: off>tlen: new %d off %d mleft %ld tlen %ld\n",
		    new, offset, m->left, tlen);
		memcpy(m->end, start, m->left); /* XXX remove copy */
		m->end += m->left;
		tbuf->start += m->left;
		m->left = 0;
		mqueue_push(outq, m, false);

		tbuf->start = tbuf->end; /* Skip slop */
		return (NULL);
	} else {
		/*
		 * Existing inner packet where offset is within the TFS buffer.
		 */
		/* This is us logging here what we didn't in in get outer */
		if (new)
			DBG("got_outer: seq %d tlen %ld\n", seq, ltlen);
		DBG("CONTINUED: new %d off for next %d mleft %ld tlen %ld\n",
		    new, offset, m->left, tlen);
		tlen = offset;
		assert(m->left == tlen);
	}

	if (m->left > tlen) {
		// XXX copy A
		DBG("MORELEFT: new %d off %d mleft %d tlen %ld\n", new, offset,
		    m->left, tlen);
		memcpy(m->end, start, tlen); // XXX remove copy
		m->end += tlen;
		m->left -= tlen;
		tbuf->start += tlen;
		assert(MBUF_LEN(tbuf) == 0);
		return m;
	}

	// XXX copy B
	DBG("COMPLETE: new %d off %d mleft %ld tlen %ld\n", new, offset,
	    m->left, tlen);
	memcpy(m->end, start, m->left); /* XXX remove copy */
	m->end += m->left;
	tbuf->start += m->left;
	tlen -= m->left;
	m->left = 0;
	mqueue_push(outq, m, false);

	assert(tlen >= 0);
	if (tlen == 0)
		return NULL;

	// Recurse!
	DBG("recurse: new %d\n", new);
	return add_to_inner_packet(tbuf, false, NULL, freeq, outq, seq);
}

uint32_t
read_tfs_get_outer(int s, struct mbuf *tbuf, struct mqueue *outq,
		   struct ratelimit *rl, bool *reset)
{
	struct ackinfo *ack = mqueue_get_ackinfop(outq);
	struct sockaddr_storage sender;
	socklen_t addrlen;
	ssize_t offset, n;
	uint32_t seq;

	/* XXX write me */
	while (true) {
		mbuf_reset(tbuf, HDRSPACE);
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
			ack->ndrop++;
			continue;
		}
		if (rl && limit(rl, n)) {
			DBG("read_tfs_get_outer: congestion "
			    "creation\n");
			ack->ndrop++;
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
			ack->ndrop++;
			continue;
		}

		seq = get32(tbuf->start);
		if (ack->start == 0)
			ack->start = seq;
		if (seq == ack->last + 1 || ack->last == 0) {
			/* Mark valid and return */
			ack->last = seq;
			return seq;
		}
		if (seq <= ack->last) {
			warn("read_tfs_get_outer: prev/dup seq %d "
			     "detected\n",
			     seq);
			continue;
		}

		uint32_t ndrop = seq - (ack->last + 1);
		ack->ndrop += ndrop;
		DBG("read_tfs_get_outer: packet loss %d total %d\n", ndrop,
		    ack->ndrop);

		*reset = true; /* Lost packets so drop any in-progress */
		ack->last = seq;
		return seq;
	}
}

void
read_tfs_pkts(int s, struct mqueue *freeq, struct mqueue *outq,
	      uint64_t congest_rate)
{
	struct mbuf *tbuf = mbuf_new(MAXBUF, HDRSPACE);
	struct ratelimit *rl = NULL;

	if (congest_rate > 0) {
		// uint overhead = 20 + 8 + 20 + 20 + 12;
		rl = new_ratelimit(congest_rate, 0, 10);
	}

	struct mbuf *m = NULL;
	uint32_t seq = 0;
	while (true) {
		bool reset = false;
		seq = read_tfs_get_outer(s, tbuf, outq, rl, &reset);
		if (m && reset) {
			mbuf_reset(m, HDRSPACE);
		}
		m = add_to_inner_packet(tbuf, true, m, freeq, outq, seq);
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

	put32(ebytes, seq++);
	if ((n = send(s, ebytes, mtu, 0)) != mtu)
		warn("write_empty_tfs_pkt: short write %ld\n", n);

	return seq;
}

#ifndef NDEBUG
ssize_t
iovlen(struct iovec *iov, int count)
{
	ssize_t n = 0;
	for (int i = 0; i < count; i++)
		n += iov->iov_len;
	return n;
}
#endif

#define MAXPKT (TFSMTU / 20) /* smallest packet is 20b IP. */

uint32_t
write_tfs_pkt(int s, struct mqueue *inq, struct mqueue *freeq, uint32_t seq,
	      ssize_t mtu, struct mbuf **leftover)
{
	static uint8_t tfshdr[8];
	static struct iovec iovecs[MAXPKT] = {
	    {.iov_base = tfshdr, .iov_len = sizeof(tfshdr)}};
	static struct msghdr msg = {.msg_iov = iovecs};
	static struct mbuf *freembufs[MAXPKT];

	struct mbuf **efreem = freembufs;
	struct mbuf **freem;
	struct iovec *iov = &iovecs[1];
	struct mbuf *m;
	ssize_t mtuenter = mtu;
	ssize_t mlen, n;
	uint16_t offset;

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
			DBG("write_tfs_pkt: seq %d mtu %ld m %p\n", seq, mtu,
			    m);
		}
	}
	if (m == NULL) {
		*leftover = NULL;
		return write_empty_tfs_pkt(s, seq, mtu);
	}

	/* Set the header */
	put32(tfshdr, seq++);
	put16(&tfshdr[6], offset);
	mtu -= sizeof(tfshdr);

	while (mtu > 0) {
		if (mtu <= 6 || m == NULL) {
			/* No room for dbhdr or no more data -- pad */
			mlen = mtu;
			DBG("write_tfs_pkt: seq %d pad %d enter %ld\n", seq,
			    mtu, mtuenter);
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
			    seq, mtu, mlen, mtuenter);
			mtu = 0;
			break;
		}

		/* Room for full MBUF */
		(*iov).iov_base = m->start;
		(*iov++).iov_len = mlen;
		*efreem++ = m;
		m = NULL;
		DBG("write_tfs_pkt: seq %d add mbuf %ld of mtu %ld enter %ld\n",
		    seq, mlen, mtu, mtuenter);
		mtu -= mlen;

		/* Get next MBUF if we have space */
		if (mtu > 6)
			m = mqueue_trypop(inq);
	}

	/* Send the TFS packet */
	assert(iovlen(iovecs, iov - iovecs) == mtuenter);
	msg.msg_iovlen = iov - iovecs;
	n = sendmsg(s, &msg, 0);
	if (n != mtuenter) {
		warn("write_tfs_pkt: short write %ld of %ld on "
		     "TFSLINK\n",
		     n, mtuenter);
		if (*leftover)
			*efreem++ = *leftover;
		*leftover = NULL;
	}

	/* Pushed used MBUFS onto freeq XXX make this a single call. */
	for (freem = freembufs; freem < efreem; freem++)
		mqueue_push(freeq, *freem, true);

	return seq;
}

void
write_tfs_pkts(int s, struct mqueue *outq, struct mqueue *freeq, uint mtu,
	       uint64_t txrate)
{
	struct mbuf *leftover = NULL;
	uint32_t seq = 1;
	uint mtub = (mtu - 32) * 8;
	uint pps = txrate / mtub;

	g_pps = pps_new(pps);
	LOG("Writing TFS %d pps for %d bps\n", pps, pps * mtub);

	while (true) {
		pps_wait(g_pps);
		seq = write_tfs_pkt(s, outq, freeq, seq, mtu, &leftover);
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
	uint8_t *mstart = &m->start[4];
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
		return;
	}
	if (g_avgdrops->average == 0) {
		// Not degraded, increase rate
		pps_incrate(g_pps, 1);
	} else {
		// Degraded, slow rate byte 50% of the droprate.
		int droppct = (g_avgdrops->average * 50) / g_avgpps->average;
		if (!droppct)
			droppct = 1;
		pps_decrate(g_pps, droppct);
	}
	DBG("recv_ack: ndrop %d start %d end %d\n", ndrop, start, end);
}

void
send_acks(int s, int permsec, struct mqueue *outq)
{
	uint8_t buffer[20], *bp;
	struct timespec ts;
	uint32_t ndrop, start, end;
	ssize_t n;

	LOG("send_acks: START: permsec %d\n", permsec);

	/* No sequence number */
	put32(buffer, 0xFFFFFFFF);
	bp = &buffer[4];

	int ival = permsec / 1000;
	while (true) {
		sleep(ival);

		mqueue_get_ackinfo(outq, &ndrop, &start, &end);

		if (start == 0) {
			/* nothing to talk about */
			continue;
		}
		if (ndrop > 0xFFFFFF)
			ndrop = 0xFFFFFF;
		clock_gettime(CLOCK_MONOTONIC, &ts);
		put32(bp, 0x40000000 | ndrop);
		put32(&bp[4], ts.tv_sec * 1000 + ts.tv_nsec / 1000000);
		put32(&bp[8], start);
		put32(&bp[12], end);

		if ((n = send(s, buffer, sizeof(buffer), 0)) != sizeof(buffer))
			warn("send_acks: short write: %ld\n", n);
		else
			DBG("write ack: %ld bytes\n", n);
	}
}

struct thread_args {
	struct mqueue *freeq, *outq;
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
	write_intf_pkts(args->fd, args->outq, args->freeq);
	return NULL;
}

static void *
_read_tfs_pkts(void *_arg)
{
	struct thread_args *args = _arg;
	read_tfs_pkts(args->s, args->freeq, args->outq, args->rate);
	return NULL;
}

static void *
_write_tfs_pkts(void *_arg)
{
	struct thread_args *args = _arg;
	write_tfs_pkts(args->s, args->outq, args->freeq, TFSMTU, args->rate);
	return NULL;
}

static void *
_send_acks(void *_arg)
{
	struct thread_args *args = _arg;
	send_acks(args->s, 1000, args->outq);
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
	    mqueue_new_freeq("TFS Ingress FreeqQ", MAXQSZ, MAXBUF, HDRSPACE);
	args.outq = mqueue_new("TFS Ingress OutQ", MAXQSZ);
	args.rate = txrate;
	args.fd = fd;
	args.s = s;

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
	    mqueue_new_freeq("TFS Egress FreeqQ", MAXQSZ, MAXBUF, HDRSPACE);
	args.outq = mqueue_new("TFS Egress OutQ", MAXQSZ);
	args.rate = congest;
	args.fd = fd;
	args.s = s;

	pthread_create(&threads[0], NULL, _read_tfs_pkts, &args);
	pthread_create(&threads[1], NULL, _write_intf_pkts, &args);

	g_avgpps = runavg_new(5, 1);
	g_avgdrops = runavg_new(5, 1);
	pthread_create(&threads[2], NULL, _send_acks, &args);
	return 0;
}

/* Local Variables: */
/* c-file-style: "bsd" */
/* c-c++-enable-clang-format-on-save: t */
/* End: */
