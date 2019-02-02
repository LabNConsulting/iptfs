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

#define HDRSPACE 18
#define MAXBUF 9000 + HDRSPACE
#define MAXQSZ 32

extern struct sockaddr_in peeraddr; /* XXX remove */
extern bool verbose;

#define DBG(x...)
#define _DBG(x...)                                                             \
	do {                                                                   \
		if (verbose)                                                   \
			printf(x)                                              \
	} while (0)

static inline void
put32(uint8_t *buf, uint32_t value)
{
	*buf++ = (value >> 24);
	*buf++ = (value >> 16);
	*buf++ = (value >> 8);
	*buf++ = value;
}

static inline void
put16(uint8_t *buf, uint32_t value)
{
	*buf++ = (value >> 8);
	*buf++ = value;
}

static inline uint32_t
get32(uint8_t *buf)
{
	return ((uint32_t)buf[0] << 24) + ((uint32_t)buf[1] << 16) +
	       ((uint32_t)buf[2] << 8) + buf[3];
}

static inline uint16_t
get16(uint8_t *buf)
{
	return ((uint16_t)buf[0] << 8) + buf[1];
}

/*
 * =================
 * Interface Packets
 * =================
 */

void
read_intf_pkts(int fd, struct mqueue *freeq, struct mqueue *outq)
{
	DBG("read_intf_pkts: start\n");
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
	DBG("write_intf_pkts: start\n");
	while (true) {
		struct mbuf *m = mqueue_pop(outq);
		ssize_t n, mlen = MBUF_LEN(m);
		if ((n = write(fd, m->start, mlen)) != mlen)
			warn("write_intf_pkts: bad write %ld/%ld\n", n, mlen);
		else
			DBG("write_intf_pkts: %d bytes\n", n);
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
		// XXX recv_ack(m);
		mqueue_push(freeq, m, true);
	}
}

/*
 * ====
 * ACKs
 * ====
 */

void
send_acks(int s, int permsec, struct mqueue *outq)
{
	uint8_t buffer[24], *bp;
	struct timespec ts;
	uint32_t ndrop, start, end;
	uint64_t ticks;
	ssize_t n;

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
		ticks = ts.tv_sec * 1000000 + ts.tv_nsec / 1000;
		put32(bp, 0x40000000 | ndrop);
		put32(&bp[4], ticks >> 32);
		put32(&bp[8], ticks & 0xFFFFFFFF);
		put32(&bp[12], start);
		put32(&bp[16], end);

		if ((n = send(s, buffer, sizeof(buffer))) != sizeof(buffer))
			warn("send_acks: short write: %ld\n", n);
		else
			DBG("write ack: %ld bytes\n", n);
	}
}

/* static ssize_t */
/* read_packet(int fd, struct ring *r, bool udp) */
/* { */
/* 	struct sockaddr_in sin; */
/* 	socklen_t slen; */
/* 	struct mbuf *m = &r->mbuf[r->reading]; */
/* 	ssize_t n = m->espace - m->start; */

/* 	if (!udp) { */
/* 		n = read(fd, m->end, n); */
/* 		DBG("read_packet: read() returns %ld on %s\n", n, r->name); */
/* 	} else { */
/* 		slen = sizeof(sin); */
/* 		n = recvfrom(fd, m->end, n, 0, (struct sockaddr *)&sin, &slen);
 */
/* 		DBG("read_packet: recvfrom() returns %ld on " */
/* 		    "%s\n", */
/* 		    n, r->name); */
/* 	} */
/* 	if (n <= 0) { */
/* 		if (n == 0) */
/* 			err(1, "EOF on intf tunnel %d on %s", fd, r->name); */
/* 		warn("bad read %ld on %s for packet read (udp " */
/* 		     "%d)", */
/* 		     n, r->name, udp); */
/* 		if (udp) { */
/* 			exit(1); */
/* 		} */
/* 		return 0; */
/* 	} else { */
/* 		/\* XXX UDP validate the recvfrom addr *\/ */
/* 		m->end += n; */
/* 		return n; */
/* 	} */
/* } */

/* static void */
/* write_packet(int fd, struct ring *r, bool isudp) */
/* { */
/* 	struct mbuf *m = &r->mbuf[r->writing]; */
/* 	ssize_t n, mlen; */

/* 	assert(r->writing != r->reading); */

/* 	mlen = MBUF_LEN(m); */
/* 	if (isudp) */
/* 		n = sendto(fd, m->start, mlen, 0, (struct sockaddr *)&peeraddr,
 */
/* 			   sizeof(peeraddr)); */
/* 	else */
/* 		n = write(fd, m->start, mlen); */
/* 	if (n < 0) { */
/* 		warn("write_packet: bad write on %s %d for " */
/* 		     "write_packet", */
/* 		     r->name, fd); */
/* 	} else if (n != mlen) { */
/* 		warn("write_packet: short write (%ld of %ld) " */
/* 		     "on %s for ", */
/* 		     n, mlen, r->name); */
/* 	} */

/* 	DBG("write_packet: write() returns %ld on %s\n", n, r->name); */
/* 	ring_wdone(r); */
/* } */

/* void * */
/* write_packets(void *_arg) */
/* { */
/* 	struct thread_args *args = (struct thread_args *)_arg; */
/* 	struct ring *r = args->r; */
/* 	enum fdtype fdtype = args->type; */
/* 	int fd = args->fd; */

/* 	while (true) { */
/* 		pthread_mutex_lock(&r->lock); */
/* 		while (ring_isempty(r)) { */
/* 			DBG("write_packets: ring %s is empty\n", r->name); */
/* 			pthread_cond_wait(&r->wcv, &r->lock); */
/* 		} */
/* 		pthread_mutex_unlock(&r->lock); */
/* 		DBG("write_packets: ready on %s\n", r->name); */

/* 		write_packet(fd, r, fdtype == FDT_UDP); */
/* 	} */

/* 	return NULL; */
/* } */

/* void * */
/* read_packets(void *_arg) */
/* { */
/* 	struct ratelimit *rl = NULL; */
/* 	struct thread_args *args = (struct thread_args *)_arg; */
/* 	struct ring *r = args->r; */
/* 	enum fdtype fdtype = args->type; */
/* 	int fd = args->fd; */
/* 	size_t n; */

/* 	if (r->rxrate > 0) { */
/* 		// uint overhead = 20 + 8 + 20 + 20 + 12; */
/* 		rl = new_ratelimit(r->rxrate, 0, 10); */
/* 	} */

/* 	while (true) { */
/* 		pthread_mutex_lock(&r->lock); */
/* 		while (ring_isfull(r)) { */
/* 			DBG("read_packets: ring %s is full\n", r->name); */
/* 			pthread_cond_wait(&r->rcv, &r->lock); */
/* 		} */
/* 		pthread_mutex_unlock(&r->lock); */
/* 		DBG("read_packets: ready on ring %s\n", r->name); */

/* 		switch (fdtype) { */
/* 		case FDT_UDP: */
/* 			n = read_packet(fd, r, true); */
/* 			break; */
/* 		case FDT_FILE: */
/* 			n = read_packet(fd, r, false); */
/* 			break; */
/* 		} */
/* 		if (rl == NULL || (n > 0 && !limit(rl, n))) */
/* 			ring_rdone(r); */
/* 	} */

/* 	return NULL; */
/* } */

struct thread_args {
	struct mqueue *freeq, *outq;
	uint64_t rate;
	int fd;
	int s;
};

static void *
_read_intf_packets(void *_arg)
{
	struct thread_args *args = _arg;
	read_intf_packets(args->fd, &args->freeq, &args->outq);
	return NULL;
}

static void *
_write_intf_packets(void *_arg)
{
	struct thread_args *args = _arg;
	write_intf_packets(args->fd, &args->outq, &args->freeq);
	return NULL;
}

static void *
_read_tunnel_packets(void *_arg)
{
	struct thread_args *args = _arg;
	read_intf_packets(args->s, &args->freeq, &args->outq, args->rate);
	return NULL;
}

static void *
_write_tunnel_packets(void *_arg)
{
	struct thread_args *args = _arg;
	write_intf_packets(args->s, &args->outq, &args->freeq, args->rate);
	return NULL;
}

static void *
_send_acks(void *_arg)
{
	struct thread_args *args = _arg;
	send_acks(args->s, 1000, &args->outq);
	return NULL;
}

/*
 * tunnel - tunnel from tun intf over a TCP connection.
 */
int
tfs_tunnel_ingress(int fd, int s, uint64_t txrate, pthread_t(*threads[2]))
{
	static struct thread_args args;
	static struct mqueue *freeq, *outq;
	void *rv;

	DBG("tfs_tunnel_ingress: fd: %d s: %d\n", fd, s);
	args.freeq =
	    mqueue_new_freeq("TFS Ingress FreeqQ", MAXQSZ, MAXBUF, HDRSPACE);
	args.outq = mqueue_new("TFS Ingress OutQ", MAXQSZ);
	args.rate = txrate;
	args.fd = fd;
	args.s = s;

	pthread_create(&threads[0], NULL, _read_intf_pkts, &args);
	pthread_create(&threads[1], NULL, _write_tunnel_pkts, &args);
	return 0;
}

int
tfs_tunnel_egress(int fd, int s, uint64_t congest, pthread_t(*threads[3]))
{
	static struct thread_args args;
	static struct mqueue *freeq, *outq;
	void *rv;

	DBG("tfs_tunnel_egress: fd: %d s: %d\n", fd, s);
	args.freeq =
	    mqueue_new_freeq("TFS Egress FreeqQ", MAXQSZ, MAXBUF, HDRSPACE);
	args.outq = mqueue_new("TFS Egress OutQ", MAXQSZ);
	args.rate = congest;
	args.fd = fd;
	args.s = s;

	pthread_create(&threads[0], NULL, _read_tunnel_pkts, &args);
	pthread_create(&threads[1], NULL, _write_intf_pkts, &args);
	pthread_create(&threads[2], NULL, _send_acks, &args);
	return 0;
}

/* Local Variables: */
/* c-file-style: "bsd" */
/* c-c++-enable-clang-format-on-save: t */
/* End: */
