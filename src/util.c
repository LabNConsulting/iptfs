/*
 * -*- coding: utf-8 -*-*
 * January 21 2019, Christian E. Hopps <chopps@gmail.com>
 *
 */

#include <sys/types.h>
#include <stdint.h>
#include <string.h>
#include <stdlib.h>
#include <stdbool.h>
#include <time.h>

#define NSECS_IN_SEC 1000000000

struct ratelimit {
    uint64_t rate;
    uint overhead;
    uint count;
    uint64_t totb;
    uint64_t ndrops;
    uint pktidx;
    struct timespec *times;
    uint16_t *sizes;
};


struct ratelimit *
new_ratelimit(uint32_t rate, uint overhead, uint count) {
    struct ratelimit *rl = malloc(sizeof(*rl));
    ssize_t sz;
    memset(rl, 0, sizeof(*rl));
    rl->rate = rate / 8;
    rl->overhead = overhead;
    rl->count = count;
    sz = sizeof(*rl->times) * count;
    rl->times = malloc(sz);
    memset(rl->times, 0, sz);
    sz = sizeof(*rl->sizes) * count;
    rl->sizes = malloc(sz);
    memset(rl->sizes, 0, sz);

    return rl;
}

uint64_t
clock_delta(struct timespec *recent, struct timespec *past) {
    uint64_t nsecs;

    uint sdiff = recent->tv_sec - past->tv_sec;
    nsecs = recent->tv_nsec + sdiff * NSECS_IN_SEC;
    return nsecs - past->tv_nsec;
}

bool
limit(struct ratelimit *rl, uint n)
{
    uint i = rl->pktidx;
    struct timespec otime = rl->times[i];
    struct timespec ntime;
    uint64_t ntotb = rl->totb + n - rl->sizes[i];
    uint64_t rate = 0;
    uint64_t delta;

    if (n > rl->overhead) {
        n -= rl->overhead;
    }

    clock_gettime(CLOCK_MONOTONIC, &ntime);
    if (otime.tv_sec != 0 || otime.tv_nsec != 0) {
        delta = clock_delta(&ntime, &otime);
        rate = ntotb * NSECS_IN_SEC / delta;
    }
    if (rate > rl->rate) {
        rl->ndrops++;
        return true;
    }
    rl->totb = ntotb;
    rl->times[i] = ntime;
    rl->sizes[i] = n;
    rl->pktidx = (rl->pktidx + 1) % rl->count;
    return false;
}
