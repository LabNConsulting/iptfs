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

/*
 * mbuf.h
 */

struct mbuf {
    uint8_t *space;  /* The buffer. */
    uint8_t *espace; /* The end of the buffer. */
    uint8_t *start;  /* The start of the packet */
    uint8_t *end;    /* The end (one past) of the packet */
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

/*
 * util.h
 */

struct ratelimit *new_ratelimit(uint32_t, uint, uint);
bool limit(struct ratelimit *, uint);
struct pps *pps_new(int pps);
void pps_change_rate(struct pps *pp, int pps);
bool pps_is_expired(struct timespec *now, struct timespec *expire);
void pps_wait(struct pps *pp);

#endif /* IPTFS_H */