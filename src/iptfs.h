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

int tfs_tunnel_ingress(int, int, uint64_t, pthread_t *);
int tfs_tunnel_egress(int, int, uint64_t, pthread_t *);

/*
 * mbuf.h
 */

struct mbuf {
    uint8_t *space;  /* The buffer. */
    uint8_t *espace; /* The end of the buffer. */
    uint8_t *start;  /* The start of the packet */
    uint8_t *end;    /* The end (one past) of the packet */
    ssize_t left;    /* used to track what's left to read */
};

void inline
mbuf_reset(struct mbuf *m, int hdrspace)
{
    m->end = m->start = &m->space[hdrspace];
}

#define MBUF_AVAIL(m) ((m)->espace - (m)->end)
#define MBUF_LEN(m) ((m)->end - (m)->start)

struct mqueue *mqueue_new(const char *name, int depth);
struct mqueue *mqueue_new_freeq(const char *name, int depth, ssize_t maxbuf, ssize_t hdrspace);
struct mbuf *mqueue_pop(struct mqueue *mq);
struct mbuf *mqueue_trypop(struct mqueue *mq);
void mqueue_push(struct mqueue *mq, struct mbuf *m, bool reset);
void mqueue_get_ackinfo(struct mqueue *outq, uint32_t *drops, uint32_t *start, uint32_t *end);

/*
 * util.h
 */

void *xmalloc(size_t sz);
void *xzmalloc(size_t sz);

struct ratelimit *new_ratelimit(uint32_t, uint, uint);
bool limit(struct ratelimit *, uint);

struct periodic *periodic_new(uint64_t nsecs);
void periodic_change_rate(struct periodic *pp, uint64_t nsecs);
void periodic_wait(struct periodic *pp);

struct pps *pps_new(int pps);
void pps_incrate(struct pps *pp, int inc);
void pps_decrate(struct pps *pp, int pct);
void pps_wait(struct pps *pp);

#endif /* IPTFS_H */
