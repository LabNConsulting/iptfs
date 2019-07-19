/*
 * -*- coding: utf-8 -*-*
 *
 * February 1 2019, Christian E. Hopps <chopps@labn.net>
 *
 * Copyright (c) 2019, LabN Consulting, L.L.C.
 * All Rights Reserved.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 * http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */
#include "iptfs.h"
#include <err.h>
#include <pthread.h>
#include <string.h>
#include <sys/uio.h>

/*
 * mbuFs
 */

struct mbuf *
mbuf_new(size_t maxbuf, size_t hdrspace)
{
	struct mbuf *m = xzmalloc(maxbuf + sizeof(*m));
	m->space = (void *)&m[1];
	m->espace = m->space + maxbuf;
	m->end = m->start = &m->space[hdrspace];
	return m;
}

struct mqueue {
	const char *name;
	struct mbuf **queue;
	int size;
	int depth;
	ssize_t hdrspace;
	pthread_mutex_t lock;
	pthread_cond_t pushcv;
	pthread_cond_t popcv;
	struct ackinfo ackinfo;
};

struct miovq {
	const char *name;
	struct mqueue *freeq;
	struct miov **queue;
	int size;
	int depth;
	pthread_mutex_t lock;
	pthread_cond_t pushcv;
	pthread_cond_t popcv;
};

/*
 * mqueues - mbuf queues
 */

#define MQ_FULL(mq) ((mq)->depth == (mq)->size)
#define MQ_EMPTY(mq) ((mq)->depth == 0)

struct mqueue *
mqueue_new(const char *name, int size)
{
	struct mqueue *mq;
	if ((mq = malloc(sizeof(struct mqueue))) == NULL)
		err(1, "mqueue_new: malloc mqueue");
	if ((mq->queue = malloc(sizeof(struct mbuf *) * size)) == NULL)
		err(1, "mqueue_new: malloc queue");

	memset(&mq->ackinfo, 0, sizeof(mq->ackinfo));
	mq->name = name;
	mq->size = size;
	mq->depth = 0;

	pthread_mutex_init(&mq->lock, NULL);
	pthread_cond_init(&mq->pushcv, NULL);
	pthread_cond_init(&mq->popcv, NULL);

	return mq;
}

struct mqueue *
mqueue_new_freeq(const char *name, int size, ssize_t maxbuf, ssize_t hdrspace)
{
	struct mqueue *mq = mqueue_new(name, size);
	struct mbuf *mbufs;
	uint8_t *space;
	int i, bi;

	mq->hdrspace = hdrspace;

	if ((space = malloc(size * maxbuf)) == NULL)
		err(1, "mqueue_new_freeq: malloc space");
	if ((mbufs = malloc(size * sizeof(struct mbuf))) == NULL)
		err(1, "mqueue_new_freeq: malloc mbufs");

	for (bi = 0, i = 0; i < size; i++, bi += maxbuf) {
		struct mbuf *m = &mbufs[i];
		m->space = &space[bi];
		m->espace = m->space + maxbuf;
		m->start = &m->space[hdrspace];
		m->end = m->start;
		mq->queue[i] = m;
		mq->depth++;
	}

	return mq;
}

struct mbuf *
mqueue_pop(struct mqueue *mq)
{
	struct mbuf *m;

	pthread_mutex_lock(&mq->lock);
	while (MQ_EMPTY(mq)) {
		DBG("mqueue_pop: %s empty\n", mq->name);
		pthread_cond_wait(&mq->popcv, &mq->lock);
	}
	pthread_cond_signal(&mq->pushcv);
	m = mq->queue[--mq->depth];
	pthread_mutex_unlock(&mq->lock);

	return m;
}

void
mqueue_wait(struct mqueue *mq)
{
	struct mbuf *m;

	pthread_mutex_lock(&mq->lock);
	while (MQ_EMPTY(mq)) {
		pthread_cond_wait(&mq->popcv, &mq->lock);
	}
	pthread_mutex_unlock(&mq->lock);
	return;
}
struct mbuf *
mqueue_trypop(struct mqueue *mq, int maxsize)
{
	struct mbuf *m;

	pthread_mutex_lock(&mq->lock);
	if (MQ_EMPTY(mq)) {
		m = NULL;
		goto out;
	}
	pthread_cond_signal(&mq->pushcv);
	m = mq->queue[--mq->depth];
	if (maxsize && MBUF_LEN(m) > maxsize) {
		m = NULL;
		goto out;
	}
	--mq->depth;
out:
	pthread_mutex_unlock(&mq->lock);
	return m;
}

int
mqueue_push(struct mqueue *mq, struct mbuf *m, bool reset)
{
	int depth;
	if (reset)
		mbuf_reset(m, mq->hdrspace);

	pthread_mutex_lock(&mq->lock);
	while (MQ_FULL(mq)) {
		DBG("mqueue_push: %s full\n", mq->name);
		pthread_cond_wait(&mq->pushcv, &mq->lock);
	}
	pthread_cond_signal(&mq->popcv);
	mq->queue[mq->depth++] = m;
	depth = mq->depth;
	pthread_mutex_unlock(&mq->lock);
	return depth;
}

void
mqueue_get_ackinfo(struct mqueue *mq, uint32_t *drops, uint32_t *start,
		   uint32_t *end)
{
	pthread_mutex_lock(&mq->lock);
	*drops = mq->ackinfo.ndrop;
	*start = mq->ackinfo.start;
	*end = mq->ackinfo.last;
	memset(&mq->ackinfo, 0, sizeof(mq->ackinfo));
	pthread_mutex_unlock(&mq->lock);
}

struct ackinfo *
mqueue_get_ackinfop(struct mqueue *mq)
{
	return &mq->ackinfo;
}

struct miovq *
miovq_new(const char *name, int size)
{
	struct miovq *mq;
	if ((mq = xzmalloc(sizeof(struct miovq))) == NULL)
		err(1, "miovq_new: malloc miovq");
	if ((mq->queue = malloc(sizeof(struct miov *) * size)) == NULL)
		err(1, "miovq_new: malloc queue");
	mq->name = name;
	mq->size = size;

	pthread_mutex_init(&mq->lock, NULL);
	pthread_cond_init(&mq->pushcv, NULL);
	pthread_cond_init(&mq->popcv, NULL);

	return mq;
}

struct miovq *
miovq_new_freeq(const char *name, int size, int maxiov, struct mqueue *freeq)
{
	struct miovq *mq = miovq_new(name, size);
	uint8_t *space;
	int i, sz;

	mq->freeq = freeq;
	sz = sizeof(struct miov) +
	     (sizeof(struct iovec) + sizeof(struct mbuf *)) * maxiov;
	if ((space = malloc(size * sz)) == NULL)
		err(1, "miovq_new_freeq: malloc miov");

	for (i = 0; i < size; i++) {
		struct miov *m = (struct miov *)(space + i * sz);
		m->iov = (struct iovec *)(m + 1);
		m->mbufs = (struct mbuf **)(m->iov + maxiov);
		m->maxiov = maxiov;
		mq->queue[i] = m;
		mq->depth++;
	}

	return mq;
}

struct miov *
miovq_pop(struct miovq *mq)
{
	/* XXX an exact copy of mqueue_pop except for the types.. sigh, C */
	struct miov *m;

	pthread_mutex_lock(&mq->lock);
	while (MQ_EMPTY(mq)) {
		DBG("miovq_pop: %s empty\n", mq->name);
		pthread_cond_wait(&mq->popcv, &mq->lock);
	}
	pthread_cond_signal(&mq->pushcv);
	m = mq->queue[--mq->depth];
	pthread_mutex_unlock(&mq->lock);

	return m;
}

void
miov_reset(struct miov *m, struct miovq *mq)
{
	int niov = m->niov;
	for (int i = 0; i < niov; i++)
		mbuf_deref(mq->freeq, m->mbufs[i]);
	m->niov = 0;
	m->len = 0;
	m->left = 0;
}

int
miovq_push(struct miovq *mq, struct miov *m)
{
	int depth;
	if (mq->freeq)
		miov_reset(m, mq);

	pthread_mutex_lock(&mq->lock);
	while (MQ_FULL(mq)) {
		DBG("miovq_push: %s full\n", mq->name);
		pthread_cond_wait(&mq->pushcv, &mq->lock);
	}

	pthread_cond_signal(&mq->popcv);
	mq->queue[mq->depth++] = m;
	depth = mq->depth;
	pthread_mutex_unlock(&mq->lock);

	return depth;
}

/* Local Variables: */
/* c-file-style: "bsd" */
/* c-c++-enable-clang-format-on-save: t */
/* End: */
