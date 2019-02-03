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
#include "iptfs.h"

void *
xmalloc(size_t sz)
{
	void *m = malloc(sz);
	if (m == NULL)
		err(1, "xmalloc: %ld\n", sz);
	return m;
}

void *
xzmalloc(size_t sz)
{
	void *m = xmalloc(sz);
	memset(m, 0, sz);
	return m;
}

struct runavg *
runavg_new(uint runlen, uint min)
{
	struct runavg *avg = xzmalloc(sizeof(*avg) + sizeof(uint) * runlen);
	avg->values = (uint *)&avg[1];
	avg->runlen = runlen;
	avg->min = min;
	return avg;
}

bool
runavg_add(struct runavg *avg, uint value)
{
	if (avg->ticks) {
		// remove oldest value from total.
		uint i = (avg->index + avg->runlen - 1) % avg->runlen;
		avg->total -= avg->values[i];
	}
	avg->total += value;
	avg->values[avg->index++] = value;
	if (avg->ticks)
		avg->average = avg->total / avg->runlen;
	else
		avg->average = avg->total / avg->index;
	if (avg->total && avg->average < avg->min)
		avg->average = avg->min;
	if (avg->index != avg->runlen)
		return false;
	avg->ticks++;
	avg->index = 0;
	return true;
}

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

struct periodic {
	struct timespec timestamp;
	atomic_uint_least64_t ival;
};

static struct periodic *
periodic_init(struct periodic *pp, uint64_t nsecs)
{
	clock_gettime(CLOCK_MONOTONIC, &pp->timestamp);
	pp->ival = ATOMIC_VAR_INIT(nsecs);
	return pp;
}

struct periodic *
periodic_new(uint64_t nsecs)
{
	return periodic_init(xmalloc(sizeof(struct periodic)), nsecs);
}

void
periodic_change_rate(struct periodic *pp, uint64_t nsecs)
{
	uint64_t oval = atomic_exchange(&pp->ival, nsecs);
	if (nsecs == oval)
		return;
}

static bool
periodic_is_expired(struct timespec *now, struct timespec *expire)
{
	if (now->tv_sec > expire->tv_sec)
		return true;
	if (now->tv_sec == expire->tv_sec)
		return now->tv_nsec > expire->tv_nsec;
	return false;
}

void
periodic_wait(struct periodic *pp)
{
	uint64_t ival = atomic_load(&pp->ival);
	struct timespec expire, now;

	expire = pp->timestamp;
	if ((expire.tv_nsec += ival) > NSECS_IN_SEC) {
		expire.tv_sec++;
		expire.tv_nsec -= NSECS_IN_SEC;
	}

	clock_gettime(CLOCK_MONOTONIC, &now);
	if (!periodic_is_expired(&now, &expire)) {
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

struct pps {
	struct periodic periodic;
	atomic_uint_least32_t pps;
	uint target_pps; /* immutable */
};

struct pps *
pps_new(int target_pps)
{
	struct pps *pp = xmalloc(sizeof(*pp));
	pp->target_pps = target_pps;
	pp->pps = ATOMIC_VAR_INIT(target_pps);
	periodic_init(&pp->periodic, NSECS_IN_SEC / target_pps);
	return pp;
}

void
pps_incrate(struct pps *pp, int inc)
{
	uint32_t oval = atomic_load(&pp->pps);
	uint32_t nval = oval + inc;
	if (nval > pp->target_pps)
		nval = pp->target_pps;
	if (nval != oval) {
		atomic_store(&pp->pps, nval);
		periodic_change_rate(&pp->periodic, NSECS_IN_SEC / nval);
	}
}

void
pps_decrate(struct pps *pp, int pct)
{
	uint32_t oval = atomic_load(&pp->pps);
	uint32_t nval = oval * pct / 100;
	if (nval > pp->target_pps)
		nval = pp->target_pps;
	if (nval == 0)
		nval = 1;
	if (nval != oval) {
		atomic_store(&pp->pps, nval);
		periodic_change_rate(&pp->periodic, NSECS_IN_SEC / nval);
	}
}

void
pps_wait(struct pps *pp)
{
	periodic_wait(&pp->periodic);
}

/* Local Variables: */
/* c-file-style: "bsd" */
/* c-c++-enable-clang-format-on-save: t */
/* End: */
