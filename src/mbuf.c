/*
 * -*- coding: utf-8 -*-*
 *
 * February 1 2019, Christian E. Hopps <chopps@gmail.com>
 *
 */
#include "iptfs.h"
#include <err.h>
#include <pthread.h>

struct mqueue {
	const char *name;
	struct mbuf **queue;
	int size;
	int depth;
	ssize_t hdrspace;
	pthread_mutex_t lock;
	pthread_cond_t pushcv;
	pthread_cond_t popcv;
};

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
	}

	return mq;
}

struct mbuf *
mqueue_pop(struct mqueue *mq)
{
	struct mbuf *m;

	pthread_mutex_lock(&mq->lock);
	while (MQ_EMPTY(mq))
		pthread_cond_wait(&mq->popcv, &mq->lock);

	if (MQ_FULL(mq)) {
		pthread_cond_signal(&mq->pushcv);
	}

	m = mq->queue[--mq->depth];
	pthread_mutex_unlock(&mq->lock);

	return m;
}

struct mbuf *
mqueue_trypop(struct mqueue *mq)
{
	struct mbuf *m;

	pthread_mutex_lock(&mq->lock);
	if (MQ_EMPTY(mq)) {
		m = NULL;
		goto out;
	}

	if (MQ_FULL(mq)) {
		pthread_cond_signal(&mq->pushcv);
	}

	m = mq->queue[--mq->depth];
out:
	pthread_mutex_unlock(&mq->lock);
	return m;
}

void
mqueue_push(struct mqueue *mq, struct mbuf *m, bool reset)
{
	if (reset)
		mbuf_reset(m, mq->hdrspace);

	pthread_mutex_lock(&mq->lock);
	while (MQ_FULL(mq))
		pthread_cond_wait(&mq->pushcv, &mq->lock);

	if (MQ_EMPTY(mq)) {
		pthread_cond_signal(&mq->popcv);
	}

	mq->queue[mq->depth++] = m;
	pthread_mutex_unlock(&mq->lock);
}

/* Local Variables: */
/* c-file-style: "bsd" */
/* c-c++-enable-clang-format-on-save: t */
/* End: */
