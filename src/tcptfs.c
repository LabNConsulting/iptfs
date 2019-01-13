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
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
//#include <sys/ioctl.h>
#include <stdbool.h>
#include <sys/select.h>
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
	int left;	/* left to read, or sent based on reading/writing */
};

struct mbuf mdrop = {dropbuf, &dropbuf[MAXBUF], &dropbuf[HDRSPACE],
		     &dropbuf[HDRSPACE], -1};

struct ring {
	const char *name;
	uint8_t *space;
	struct mbuf *mbuf;
	struct mbuf *dropping;
	int reading; /* index of mbuf currently reading into */
	int writing; /* index of mbuf to write, if == reading then none */
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
		m->left = -1;
	}
	r->reading = r->writing = 0;
}

static bool
ring_isempty(struct ring *r)
{
	return r->reading == r->writing;
}

static bool
ring_isfull(struct ring *r)
{
	return ((r->reading + 1) % r->mcount) == r->writing;
}

static struct mbuf *
ring_nextrbuf(struct ring *r)
{
	struct mbuf *m, *om;
	int old = r->reading;
	int next = (old + 1) % r->mcount;

	om = &r->mbuf[old];
	if (om->left == -1) {
		m = om;
	} else if (next == r->writing) {
		return NULL;
	} else {
		r->reading = next;
		m = &r->mbuf[next];
	}

	// We should only need to initialize after the buffer is written;
	assert(r->mbuf[old].left == 0 || r->mbuf[old].left == -1);
	assert(m->start == &m->space[r->hdrspace]);
	assert(m->start == m->end);
	assert(m->left < 0);
	return m;
}

static void
ring_resetread(struct ring *r)
{
	if (--r->reading < 0)
		r->reading = r->mcount - 1;
}

static bool
ring_nextwbuf(struct ring *r, struct mbuf *m)
{
	struct mbuf *mi;
	if (r->writing == r->reading)
		return false;
	mi = &r->mbuf[r->writing];
	r->writing = (r->writing + 1) % r->mcount;

	/* copy to user buffer and reset */
	if (m != NULL)
		*m = *mi;
	mi->start = &mi->space[r->hdrspace];
	mi->end = mi->start;
	mi->left = -1;

	return true;
}

static void
read_packet(int fd, struct ring *r)
{
	ssize_t n = r->maxbuf;
	struct mbuf *m;
	if ((m = ring_nextrbuf(r)) == NULL) {
		warn("no buffers on %s for packet read", r->name);
		m = &r->mbuf[r->mcount];
	}
	n = read(fd, m->start, r->maxbuf - r->hdrspace);
	DBG("read_packet: read() returns %ld on %s\n", n, r->name);
	if (n <= 0) {
		if (n < 0)
			warn("bad read on %s for packet read", r->name);
		ring_resetread(r);
	} else {
		m->end = m->start + n;
		m->left = 0;
		/* We finished reading this try and advance ring */
		(void)ring_nextrbuf(r);
	}
}

static void
write_packet(int fd, struct ring *r)
{
	struct mbuf m;
	if (!ring_nextwbuf(r, &m)) {
		warn("no write buffers on %s for packet write", r->name);
		return;
	}
	ssize_t len = m.end - m.start;
	if (len == 0) {
		warn("zero len buffer on %s for packet write", r->name);
		return;
	}
	ssize_t n = write(fd, m.start, len);
	DBG("write_packet: write() returns %ld on %s\n", n, r->name);
	if (n != len) {
		warn("short write (%ld of %ld) on %s for packet write", n, len,
		     r->name);
	}
}

static void
read_stream(int s, struct ring *r)
{
	struct mbuf *m = &r->mbuf[r->reading];
	ssize_t n;
	int mlen;

	if (m->left == 0 && (m = ring_nextrbuf(r)) == NULL) {
		warn("no buffers available on %s for stream read", r->name);
		return;
	}
	n = MBUF_AVAIL(m);

	assert(n > 0);
	assert(m->left != 0);

	/* If we are continuing only read what we need to for this packet. */
	if (m->left > 0 && m->left < n) {
		n = m->left;
	}

	/* Get the packet length */
	if (m->left == -1) {
		int ll = MBUF_LEN(m);
		if ((n = read(s, m->end, 2 - ll)) <= 0) {
			if (n < 0)
				err(1, "bad read on %s", r->name);
			else
				err(1, "TCP closed on %s\n", r->name);
		}
		m->end += n;
		mlen = MBUF_LEN(m);
		if (mlen < 2)
			return;

		/* we've now got our 2 bytes -- grab pkt len */
		m->left = ((int)m->start[0] << 8) + m->start[1];
		assert(m->left < MAXBUF);
		m->end = m->start;
		DBG("read_stream: got m->left %d on %s\n", m->left, r->name);
	}

	if ((n = read(s, m->end, n)) <= 0) {
		if (n < 0)
			err(1, "bad read on %s", r->name);
		else
			err(1, "TCP closed on %s\n", r->name);
	}

	DBG("read_stream: read() returns %ld on %s\n", n, r->name);
	m->end += n;
	m->left -= n;
	assert(m->left >= 0);
	/* If we finished reading this packet then try and advance ring */
	if (m->left == 0)
		(void)ring_nextrbuf(r);
}

static void
write_stream(int s, struct ring *r)
{
	struct mbuf *m = &r->mbuf[r->writing];
	int mlen, sent, togo;
	ssize_t n;

	assert(r->writing != r->reading);
	assert(m->left != -1);

	sent = m->left;
	mlen = MBUF_LEN(m);
	if (sent == 0) {
		/* start with the length which we stored earlier */
		m->start -= 2;
		m->start[0] = (uint8_t)(mlen >> 8);
		m->start[1] = (uint8_t)mlen;
		mlen += 2;
	}
	togo = mlen - sent;
	if ((n = write(s, m->start + sent, togo)) < 0) {
		warn("bad write on %s for stream write", r->name);
		return;
	} else if (n != togo) {
		warn("short write (%ld of %d) on %s for stream write", n,
		     m->left, r->name);
	}
	DBG("write_stream: write() returns %ld on %s\n", n, r->name);

	m->left += n;

	if (m->left == mlen) {
		(void)ring_nextwbuf(r, m);
	}
}

/*
 * tunnel - tunnel from tun intf over a TCP connection.
 */
void
tfs_tunnel(int fd, int s)
{
	struct ring rred;   // packets from red for black
	struct ring rblack; // packets from black for red
	fd_set rfds, wfds;

	ring_init(&rred, "RED RECV RING", RINGSZ, MAXBUF, HDRSPACE);
	ring_init(&rblack, "BLACK RECV RING", RINGSZ, MAXBUF, HDRSPACE);

	while (1) {
		FD_ZERO(&wfds);
		if (!ring_isempty(&rred)) {
			/* have packet from red (vtun) send to black (tcp) */
			DBG("%s: not empty get write ready\n", rred.name);
			FD_SET(s, &wfds);
		} else {
			DBG("%s: empty\n", rred.name);
		}
		if (!ring_isempty(&rblack)) {
			/* have packet from black (tcp) send to red (vtun) */
			DBG("%s: not empty get write ready\n", rblack.name);
			FD_SET(fd, &wfds);
		} else {
			DBG("%s: empty\n", rblack.name);
		}

		FD_ZERO(&rfds);
		if (!ring_isfull(&rred)) {
			/* have room in red get packet (vtun) */
			DBG("%s: not full get read ready\n", rred.name);
			FD_SET(fd, &rfds);
		} else {
			DBG("%s: full\n", rred.name);
		}
		if (!ring_isfull(&rblack)) {
			/* have room in black get packet (tcp) */
			DBG("%s: not full get read ready\n", rblack.name);
			FD_SET(s, &rfds);
		} else {
			DBG("%s: full\n", rblack.name);
		}

		if (select(FD_SETSIZE, &rfds, &wfds, NULL, NULL) < 0)
			err(1, "tunnel select");

		/* write first to free up mbufs */
		if (FD_ISSET(s, &wfds))
			write_stream(s, &rred);
		if (FD_ISSET(fd, &wfds))
			write_packet(fd, &rblack);

		if (FD_ISSET(s, &rfds))
			read_stream(s, &rblack);
		if (FD_ISSET(fd, &rfds))
			read_packet(fd, &rred);
	}
}

/* Local Variables: */
/* c-file-style: "bsd" */
/* c-c++-enable-clang-format-on-save: t */
/* End: */
