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
	if (verbose)                                                           \
	printf(x)

struct ring {
	const char *name;
	struct mbuf *mbuf; /* array of mbufs using up buffer space */
	uint8_t *space;    /* actual buffer space for all mbufs */
	int reading;       /* index of mbuf currently reading into */
	int writing;       /* index of mbuf to write, if == reading then none */
	pthread_mutex_t lock;
	pthread_cond_t rcv, wcv;
	// Immutable.
	uint64_t rxrate;
	int hdrspace;
	int maxbuf;
	int mcount;
};

void
ring_init(struct ring *r, const char *name, int count, int maxbuf, int hdrspace,
	  uint64_t rxrate)
{
	int i, bi;

	memset(r, 0, sizeof(*r));
	r->name = name;
	r->maxbuf = maxbuf;
	r->hdrspace = hdrspace;
	r->rxrate = rxrate;
	r->mcount = count++;
	if ((r->space = malloc(count * maxbuf)) == NULL)
		err(1, "ring_init");
	if ((r->mbuf = malloc(count * sizeof(struct mbuf))) == NULL)
		err(1, "ring_init");
	for (bi = 0, i = 0; i < count; i++, bi += maxbuf) {
		struct mbuf *m = &r->mbuf[i];
		m->space = &r->space[bi];
		m->espace = m->space + maxbuf;
		m->start = &m->space[hdrspace];
		m->end = m->start;
	}
	r->reading = r->writing = 0;

	pthread_mutex_init(&r->lock, NULL);
	pthread_cond_init(&r->rcv, NULL);
	pthread_cond_init(&r->wcv, NULL);
}

/* call when locked */
static bool
ring_isempty(struct ring *r)
{
	return r->reading == r->writing;
}

/* call when locked */
static bool
ring_isfull(struct ring *r)
{
	return ((r->reading + 1) % r->mcount) == r->writing;
}

static void
ring_rdone(struct ring *r)
{
	pthread_mutex_lock(&r->lock);
	{
		r->reading = (r->reading + 1) % r->mcount;
		pthread_cond_signal(&r->wcv);
	}
	pthread_mutex_unlock(&r->lock);
}

static void
ring_wdone(struct ring *r)
{
	pthread_mutex_lock(&r->lock);
	{
		mbuf_reset(&r->mbuf[r->writing], r->hdrspace);
		r->writing = (r->writing + 1) % r->mcount;
		pthread_cond_signal(&r->rcv);
	}
	pthread_mutex_unlock(&r->lock);
}

static ssize_t
read_packet(int fd, struct ring *r, bool udp)
{
	struct sockaddr_in sin;
	socklen_t slen;
	struct mbuf *m = &r->mbuf[r->reading];
	ssize_t n = m->espace - m->start;

	if (!udp) {
		n = read(fd, m->end, n);
		DBG("read_packet: read() returns %ld on %s\n", n, r->name);
	} else {
		slen = sizeof(sin);
		n = recvfrom(fd, m->end, n, 0, (struct sockaddr *)&sin, &slen);
		DBG("read_packet: recvfrom() returns %ld on %s\n", n, r->name);
	}
	if (n <= 0) {
		if (n == 0)
			err(1, "EOF on intf tunnel %d on %s", fd, r->name);
		warn("bad read %ld on %s for packet read (udp %d)", n, r->name,
		     udp);
		if (udp) {
			exit(1);
		}
		return 0;
	} else {
		/* XXX UDP validate the recvfrom addr */
		m->end += n;
		return n;
	}
}

static void
write_packet(int fd, struct ring *r, bool isudp)
{
	struct mbuf *m = &r->mbuf[r->writing];
	ssize_t n, mlen;

	assert(r->writing != r->reading);

	mlen = MBUF_LEN(m);
	if (isudp)
		n = sendto(fd, m->start, mlen, 0, (struct sockaddr *)&peeraddr,
			   sizeof(peeraddr));
	else
		n = write(fd, m->start, mlen);
	if (n < 0) {
		warn("write_packet: bad write on %s %d for write_packet",
		     r->name, fd);
	} else if (n != mlen) {
		warn("write_packet: short write (%ld of %ld) on %s for ", n,
		     mlen, r->name);
	}

	DBG("write_packet: write() returns %ld on %s\n", n, r->name);
	ring_wdone(r);
}

enum fdtype { FDT_FILE = 0, FDT_UDP };
struct thread_args {
	struct ring *r;
	int fd;
	enum fdtype type;
};

void *
write_packets(void *_arg)
{
	struct thread_args *args = (struct thread_args *)_arg;
	struct ring *r = args->r;
	enum fdtype fdtype = args->type;
	int fd = args->fd;

	while (true) {
		pthread_mutex_lock(&r->lock);
		while (ring_isempty(r)) {
			DBG("write_packets: ring %s is empty\n", r->name);
			pthread_cond_wait(&r->wcv, &r->lock);
		}
		pthread_mutex_unlock(&r->lock);
		DBG("write_packets: ready on %s\n", r->name);

		write_packet(fd, r, fdtype == FDT_UDP);
	}

	return NULL;
}

void *
read_packets(void *_arg)
{
	struct ratelimit *rl = NULL;
	struct thread_args *args = (struct thread_args *)_arg;
	struct ring *r = args->r;
	enum fdtype fdtype = args->type;
	int fd = args->fd;
	size_t n;

	if (r->rxrate > 0) {
		// uint overhead = 20 + 8 + 20 + 20 + 12;
		rl = new_ratelimit(r->rxrate, 0, 10);
	}

	while (true) {
		pthread_mutex_lock(&r->lock);
		while (ring_isfull(r)) {
			DBG("read_packets: ring %s is full\n", r->name);
			pthread_cond_wait(&r->rcv, &r->lock);
		}
		pthread_mutex_unlock(&r->lock);
		DBG("read_packets: ready on ring %s\n", r->name);

		switch (fdtype) {
		case FDT_UDP:
			n = read_packet(fd, r, true);
			break;
		case FDT_FILE:
			n = read_packet(fd, r, false);
			break;
		}
		if (rl == NULL || (n > 0 && !limit(rl, n)))
			ring_rdone(r);
	}

	return NULL;
}

/*
 * tunnel - tunnel from tun intf over a TCP connection.
 */
void
tfs_tunnel_ingress(int fd, int s, uint64_t txrate, phread_t *threads)
{
	static struct thread_args args[4];
	static struct thread_args *ap = args;
	static struct ring red, black;
	void *rv;

	DBG("tfs_tunnel: fd: %d s: %d\n", fd, s);
	ring_init(&red, "RED RECV RING", RINGSZ, MAXBUF, HDRSPACE, 0);
	ring_init(&black, "BLACK RECV RING", RINGSZ, MAXBUF, HDRSPACE, rxrate);

	ap->r = &black;
	ap->fd = fd;
	ap->type = FDT_FILE;
	pthread_create(&threads[0], NULL, write_packets, ap++);

	ap->r = &red;
	ap->fd = s;
	ap->type = FDT_UDP;
	pthread_create(&threads[1], NULL, write_packets, ap++);

	ap->r = &red;
	ap->fd = fd;
	ap->type = FDT_FILE;
	pthread_create(&threads[2], NULL, read_packets, ap++);

	ap->r = &black;
	ap->fd = s;
	ap->type = FDT_UDP;
	pthread_create(&threads[3], NULL, read_packets, ap++);
}

void
tfs_tunnel_egress(int fd, int s, uint64_t congest, pthread_t *threads)
{
	static struct thread_args args[4];
	static struct thread_args *ap = args;
	static struct mqueue *freeq, *outq;
	void *rv;

	DBG("tfs_tunnel_egress: fd: %d s: %d\n", fd, s);
	freeq = mqueue_new_freeq("TFS Egress FreeqQ", MAXQSZ, MAXBUF, HDRSPACE);
	outq = mqueue_new("TFS Egress OutQ", MAXQSZ);

	ap->r = &black;
	ap->fd = fd;
	ap->type = FDT_FILE;
	pthread_create(&threads[0], NULL, write_packets, ap++);

	ap->r = &red;
	ap->fd = s;
	ap->type = FDT_UDP;
	pthread_create(&threads[1], NULL, write_packets, ap++);

	ap->r = &red;
	ap->fd = fd;
	ap->type = FDT_FILE;
	pthread_create(&threads[2], NULL, read_packets, ap++);

	ap->r = &black;
	ap->fd = s;
	ap->type = FDT_UDP;
	pthread_create(&threads[3], NULL, read_packets, ap++);
}

/* Local Variables: */
/* c-file-style: "bsd" */
/* c-c++-enable-clang-format-on-save: t */
/* End: */
