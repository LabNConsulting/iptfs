/*
 * -*- coding: utf-8 -*-*
 * January 12 2019, Christian E. Hopps <chopps@gmail.com>
 *
 */

//#include <arpa/inet.h>
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

#define HDRSPACE 18
#define MAXBUF 9000 + HDRSPACE
#define RINGSZ 4
uint8_t dropbuf[MAXBUF];

//#define DBG(x...) printf(x)
#define DBG(x...)

#define MBUF_AVAIL(m) ((m)->espace - (m)->end)
#define MBUF_LEN(m) ((m)->end - (m)->start)

struct mbuf {
	uint8_t *space;  /* The buffer. */
	uint8_t *espace; /* The end of the buffer. */
	uint8_t *start;  /* The start of the packet */
	uint8_t *end;    /* The end (one past) of the packet */
};

static void
mbuf_reset(struct mbuf *m, int hdrspace)
{
	m->end = m->start = &m->space[hdrspace];
}

struct ring {
	const char *name;
	struct mbuf *mbuf; /* array of mbufs using up buffer space */
	uint8_t *space;    /* actual buffer space for all mbufs */
	int reading;       /* index of mbuf currently reading into */
	int writing;       /* index of mbuf to write, if == reading then none */
	pthread_mutex_t lock;
	pthread_cond_t rcv, wcv;
	// Immutable.
	int hdrspace;
	int maxbuf;
	int mcount;
};

void
ring_init(struct ring *r, const char *name, int count, int maxbuf, int hdrspace)
{
	int i, bi;

	memset(r, 0, sizeof(*r));
	r->name = name;
	r->maxbuf = maxbuf;
	r->hdrspace = hdrspace;
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

static void
read_packet(int fd, struct ring *r)
{
	struct mbuf *m = &r->mbuf[r->reading];
	ssize_t n = m->espace - m->start;
	n = read(fd, m->end, n);
	DBG("read_packet: read() returns %ld on %s\n", n, r->name);
	if (n <= 0) {
		if (n == 0)
			err(1, "EOF on intf tunnel %d on %s", fd, r->name);
		warn("bad read on %s for packet read", r->name);
	} else {
		m->end += n;
		ring_rdone(r);
	}
}

static void
read_stream(int s, struct ring *r)
{
	struct mbuf *m = &r->mbuf[r->reading];
	ssize_t n, pktlen;
	uint8_t *lenbuf = m->start - 2;

	n = recv(s, lenbuf, 2, 0);
	if (n != 2) {
		err(1, "read_stream: short read of len %ld on %s", n, r->name);
	}

	pktlen = (lenbuf[0] << 8) + lenbuf[1];
	DBG("read_stream: pktlen %ld on %s\n", pktlen, r->name);

	while (pktlen > 0) {
		if ((n = recv(s, m->end, pktlen, 0)) <= 0) {
			err(1, "read_stream: bad read %ld from socket on %s", n,
			    r->name);
		}
		m->end += n;
		pktlen -= n;
	}

	ring_rdone(r);
}

static void
write_packet(int fd, struct ring *r, bool issock)
{
	const char *dname = issock ? "write_stream" : "write_packet";
	struct mbuf *m = &r->mbuf[r->writing];
	ssize_t n, mlen;
	uint8_t *span;

	assert(r->writing != r->reading);

	mlen = MBUF_LEN(m);
	if (!issock) {
		span = m->start;
	} else {
		span = m->start - 2;
		span[0] = (uint8_t)(mlen >> 8);
		span[1] = (uint8_t)mlen;
		mlen += 2;
	}
	if ((n = write(fd, span, mlen)) < 0) {
		warn("%s: bad write on %s for write_packet", dname, r->name);
	} else if (n != mlen) {
		if (issock)
			err(1,
			    "%s: short write (%ld of %ld) on "
			    "%s for stream "
			    "write",
			    dname, n, mlen, r->name);
		warn("%s: short write (%ld of %ld) on %s for "
		     "stream write",
		     dname, n, mlen, r->name);
	}

	DBG("%s: write() returns %ld on %s\n", dname, n, r->name);
	ring_wdone(r);
}

struct thread_args {
	struct ring *r;
	int fd;
	int s;
};

void *
write_packets(void *_arg)
{
	struct thread_args *args = (struct thread_args *)_arg;
	struct ring *r = args->r;
	bool issock = (args->fd == -1);
	int fd = (args->fd == -1 ? args->s : args->fd);

	while (true) {
		pthread_mutex_lock(&r->lock);
		while (ring_isempty(r)) {
			pthread_cond_wait(&r->rcv, &r->lock);
		}
		pthread_mutex_unlock(&r->lock);

		if (issock) {
			write_packet(fd, r, true);
		} else {
			write_packet(fd, r, false);
		}
	}

	return NULL;
}

void *
read_packets(void *_arg)
{
	struct thread_args *args = (struct thread_args *)_arg;
	struct ring *r = args->r;
	bool issock = (args->fd == -1);
	int fd = (args->fd == -1 ? args->s : args->fd);

	while (true) {
		pthread_mutex_lock(&r->lock);
		while (ring_isfull(r)) {
			pthread_cond_wait(&r->rcv, &r->lock);
		}
		pthread_mutex_unlock(&r->lock);

		if (issock) {
			read_stream(fd, r);
		} else {
			read_packet(fd, r);
		}
	}

	return NULL;
}

/*
 * tunnel - tunnel from tun intf over a TCP connection.
 */
void
tfs_tunnel(int fd, int s)
{
	static struct thread_args args[4];
	static struct ring red, black;
	pthread_t threads[4];
	void *rv;

	ring_init(&red, "RED RECV RING", RINGSZ, MAXBUF, HDRSPACE);
	ring_init(&black, "BLACK RECV RING", RINGSZ, MAXBUF, HDRSPACE);

	args[0].r = &black;
	args[0].fd = fd;
	args[0].s = -1;
	pthread_create(&threads[0], NULL, write_packets, &args[0]);

	args[1].r = &red;
	args[3].fd = -1;
	args[1].s = s;
	pthread_create(&threads[1], NULL, write_packets, &args[1]);

	args[2].r = &red;
	args[2].fd = fd;
	args[0].s = -1;
	pthread_create(&threads[2], NULL, read_packets, &args[2]);

	args[3].r = &black;
	args[3].fd = -1;
	args[3].s = s;
	pthread_create(&threads[3], NULL, read_packets, &args[3]);

	pthread_join(threads[3], &rv);
	pthread_join(threads[2], &rv);
	pthread_join(threads[1], &rv);
	pthread_join(threads[0], &rv);
}

/* Local Variables: */
/* c-file-style: "bsd" */
/* c-c++-enable-clang-format-on-save: t */
/* End: */
