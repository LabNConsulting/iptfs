/*
 * -*- coding: utf-8 -*-*
 *
 * February 1 2019, Christian E. Hopps <chopps@gmail.com>
 *
 */

#ifndef IPTFS_H
#define IPTFS_H

#include <sys/uio.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <stdbool.h>

/* Constants */
#define OUTERQSZ 256
#define INNERQSZ 256
#define MAXBUF (8192 + 1024)
#define HDRSPACE 24

/* Globals */
extern ssize_t g_tfsmtu;
extern uint g_max_inner_pkt;
extern bool g_debug, g_verbose;

#define DBG(x...) do { if (g_debug) printf(x); } while (0)
#define LOG(x...) do { if (g_verbose) printf(x); } while (0)

int tfs_tunnel_ingress(int, int, uint64_t, pthread_t *);
int tfs_tunnel_egress(int, int, uint64_t, pthread_t *);

/*
 * mbuf.h
 */

struct ackinfo {
    uint32_t start;
    uint32_t last;
    uint32_t ndrop;
};

struct mbuf {
    uint8_t *space;  /* The buffer. */
    uint8_t *espace; /* The end of the buffer. */
    uint8_t *start;  /* The start of the packet */
    uint8_t *end;    /* The end (one past) of the packet */
    ssize_t left;    /* used to track what's left to read */
    uint refcnt;     /* if this has mbufrefs the count of references */
};

struct miov {
    struct iovec *iov;          /* iovec space. */
    struct mbuf **mbufs;        /* mbuf we point into */
    ssize_t len;                /* total length */
    ssize_t left;               /* used to track what's left to read */
    uint niov;                  /* currently used iov */
    uint maxiov;                /* Max number of iov/mbuf */
};

static void __inline__
mbuf_reset(struct mbuf *m, int hdrspace)
{
    m->end = m->start = &m->space[hdrspace];
}

static void __inline__
miov_addmbuf(struct miov *mi, struct mbuf *m, uint8_t *start, ssize_t len)
{
    int i = mi->niov++;
    mi->iov[i].iov_base = start;
    mi->iov[i].iov_len = len;
    mi->mbufs[i] = m;
    mi->len += len;
}

struct mbuf *mbuf_new(size_t max, size_t hdrspace);

#define MBUF_AVAIL(m) ((m)->espace - (m)->end)
#define MBUF_LEN(m) ((m)->end - (m)->start)

struct mqueue *mqueue_new(const char *name, int depth);
struct mqueue *mqueue_new_freeq(const char *name, int depth, ssize_t maxbuf, ssize_t hdrspace);
struct mbuf *mqueue_pop(struct mqueue *mq);
struct mbuf *mqueue_trypop(struct mqueue *mq);
int mqueue_push(struct mqueue *mq, struct mbuf *m, bool reset);
void mqueue_get_ackinfo(struct mqueue *outq, uint32_t *drops, uint32_t *start, uint32_t *end);
struct ackinfo *mqueue_get_ackinfop(struct mqueue *outq);

static void __inline__
mbuf_deref(struct mqueue *freeq, struct mbuf *m)
{
    if (--m->refcnt == 0)
        mqueue_push(freeq, m, true);
}

struct miovq;
struct miovq *miovq_new(const char *name, int size);
struct miovq *miovq_new_freeq(const char *name, int size, int maxiov, struct mqueue *freeq);
struct miov *miovq_pop(struct miovq *mq);
int miovq_push(struct miovq *mq, struct miov *m);
void miov_reset(struct miov *m, struct miovq *freeq);

/*
 * util.h
 */

#define NSECS_IN_SEC 1000000000

struct runavg {
    uint runlen;  /* length of the running average */
    uint *values; /* runlen worth of values */
    uint ticks;   /* number of wraps */
    uint index;   /* index into values of next value */
    uint average; /* running average */
    uint total;   /* sum of values */
    uint min;     /* minimum average value */
};

void *xmalloc(size_t sz);
void *xzmalloc(size_t sz);

struct ratelimit *new_ratelimit(uint32_t, uint, uint);
bool limit(struct ratelimit *, uint);

struct runavg *runavg_new(uint runlen, uint min);
bool runavg_add(struct runavg *avg, uint value);

uint64_t clock_delta(struct timespec *recent, struct timespec *past);

struct periodic *periodic_new(uint64_t nsecs);
void periodic_change_rate(struct periodic *pp, uint64_t nsecs);
void periodic_wait(struct periodic *pp);

struct pps *pps_new(int pps);
uint pps_incrate(struct pps *pp, int inc);
uint pps_decrate(struct pps *pp, int pct);
uint pps_change_pps(struct pps *pp, int pps);
void pps_wait(struct pps *pp);

typedef struct stimer {
    struct timespec ts;
    uint64_t nsecs;
} stimer_t;

void st_reset(stimer_t *t, uint64_t nsec);
bool st_check(stimer_t *t);

#endif /* IPTFS_H */
