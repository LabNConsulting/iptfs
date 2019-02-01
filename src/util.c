/*
 * -*- coding: utf-8 -*-*
 * January 21 2019, Christian E. Hopps <chopps@gmail.com>
 *
 */
#include <err.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <time.h>
#ifndef __STDC_NO_ATOMICS__
#include <stdatomic.h>
#else
#include <asm-x86_64/atomic.h>
#endif

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
new_ratelimit(uint32_t rate, uint overhead, uint count)
{
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
clock_delta(struct timespec *recent, struct timespec *past)
{
	uint64_t nsecs;

	uint sdiff = recent->tv_sec - past->tv_sec;
	nsecs = recent->tv_nsec + sdiff * NSECS_IN_SEC;
	return nsecs - past->tv_nsec;
}
/* void */
/* clock_normalize(struct timespec *ts) */
/* { */
/* 	while (ts->tv_nsec > NSECS_IN_SEC) { */
/* 		ts->tv_sec++; */
/* 		ts->tv_nsec -= NSECS_IN_SEC; */
/* 	} */
/* } */

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

struct pps {
	struct timespec timestamp;
	atomic_uint_least64_t ival;
};

struct pps *
pps_new(int pps)
{
	struct pps *pp;

	if ((pp = malloc(sizeof(*pp))) == NULL)
		err(1, "pps_init");
	clock_gettime(CLOCK_MONOTONIC, &pp->timestamp);
	pp->ival = ATOMIC_VAR_INIT(NSECS_IN_SEC / pps);
	return pp;
}

void
pps_change_rate(struct pps *pp, int pps)
{
	uint64_t nval = NSECS_IN_SEC / pps;
	uint64_t oval = atomic_exchange(&pp->ival, nval);
	if (nval == oval)
		return;
}

bool
pps_is_expired(struct timespec *now, struct timespec *expire)
{
	if (now->tv_sec > expire->tv_sec)
		return true;
	if (now->tv_sec == expire->tv_sec)
		return now->tv_nsec > expire->tv_nsec;
	return false;
}

void
pps_wait(struct pps *pp)
{
	uint64_t ival = atomic_load(&pp->ival);
	struct timespec expire, now;

	expire = pp->timestamp;
	if ((expire.tv_nsec += ival) > NSECS_IN_SEC) {
		expire.tv_sec++;
		expire.tv_nsec -= NSECS_IN_SEC;
	}

	clock_gettime(CLOCK_MONOTONIC, &now);
	if (!pps_is_expired(&now, &expire)) {
		struct timespec ns;
		ns.tv_sec = expire.tv_sec - now.tv_sec;
		if (expire.tv_nsec >= now.tv_nsec) {
			ns.tv_nsec = expire.tv_nsec - now.tv_nsec;
		} else {
			ns.tv_sec--;
			ns.tv_nsec =
			    NSECS_IN_SEC + expire.tv_nsec - now.tv_nsec;
		}
		while (nanosleep(&ns, &ns) < 0)
			;
		clock_gettime(CLOCK_MONOTONIC, &now);
	}
	pp->timestamp = now;
}

/* Local Variables: */
/* c-file-style: "bsd" */
/* c-c++-enable-clang-format-on-save: t */
/* End: */
