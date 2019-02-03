/*
 * -*- coding: utf-8 -*-*
 *
 * February 1 2019, Christian E. Hopps <chopps@gmail.com>
 *
 */

#ifndef IPTFS_H
#define IPTFS_H

#include <stdlib.h>
#include <stdint.h>
#include <stdbool.h>

extern bool debug, verbose;
#define DBG(x...)                               \
    do { if (debug) printf(x); } while (0)
#define LOG(x...)                               \
    do { if (verbose) printf(x); } while (0)

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
};

static void __inline__
mbuf_reset(struct mbuf *m, int hdrspace)
{
    m->end = m->start = &m->space[hdrspace];
}

struct mbuf *mbuf_new(size_t max, size_t hdrspace);

#define MBUF_AVAIL(m) ((m)->espace - (m)->end)
#define MBUF_LEN(m) ((m)->end - (m)->start)

struct mqueue *mqueue_new(const char *name, int depth);
struct mqueue *mqueue_new_freeq(const char *name, int depth, ssize_t maxbuf, ssize_t hdrspace);
struct mbuf *mqueue_pop(struct mqueue *mq);
struct mbuf *mqueue_trypop(struct mqueue *mq);
void mqueue_push(struct mqueue *mq, struct mbuf *m, bool reset);
void mqueue_get_ackinfo(struct mqueue *outq, uint32_t *drops, uint32_t *start, uint32_t *end);
struct ackinfo *mqueue_get_ackinfop(struct mqueue *outq);

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
void pps_incrate(struct pps *pp, int inc);
void pps_decrate(struct pps *pp, int pct);
void pps_wait(struct pps *pp);

#endif /* IPTFS_H */
