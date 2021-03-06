/*
 * OpenSSH Multi-threaded AES-CTR Cipher
 *
 * Author: Benjamin Bennett <ben@psc.edu>
 * Author: Mike Tasota <tasota@gmail.com>
 * Author: Chris Rapier <rapier@psc.edu>
 * Copyright (c) 2008-2013 Pittsburgh Supercomputing Center. All rights reserved.
 *
 * Based on original OpenSSH AES-CTR cipher. Small portions remain unchanged,
 * Copyright (c) 2003 Markus Friedl <markus@openbsd.org>
 *
 * Permission to use, copy, modify, and distribute this software for any
 * purpose with or without fee is hereby granted, provided that the above
 * copyright notice and this permission notice appear in all copies.
 *
 * THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES
 * WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF
 * MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR
 * ANY SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
 * WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN AN
 * ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF
 * OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
 */
#include "includes.h"

#if defined(WITH_OPENSSL)
#include <sys/types.h>

#include <stdarg.h>
#include <string.h>

#include <openssl/evp.h>

#include "xmalloc.h"
#include "log.h"

/* compatibility with old or broken OpenSSL versions */
#include "openbsd-compat/openssl-compat.h"

#ifndef USE_BUILTIN_RIJNDAEL
#include <openssl/aes.h>
#endif

#include <pthread.h>

/*-------------------- TUNABLES --------------------*/
/* Number of pregen threads to use */
#define CIPHER_THREADS	2

/* Number of keystream queues */
#define NUMKQ		(CIPHER_THREADS + 2)

/* Length of a keystream queue */
#define KQLEN		4096

/* Processor cacheline length */
#define CACHELINE_LEN	64

/* Collect thread stats and print at cancellation when in debug mode */
/* #define CIPHER_THREAD_STATS */

/* Can the system do unaligned loads natively? */
#if defined(__aarch64__) || \
    defined(__i386__)    || \
    defined(__powerpc__) || \
    defined(__x86_64__)
# define CIPHER_UNALIGNED_OK
#endif
#if defined(__SIZEOF_INT128__)
# define CIPHER_INT128_OK
#endif
/*-------------------- END TUNABLES --------------------*/


const EVP_CIPHER *evp_aes_ctr_mt(void);

#ifdef CIPHER_THREAD_STATS
/*
 * Struct to collect thread stats
 */
struct thread_stats {
	u_int	fills;
	u_int	skips;
	u_int	waits;
	u_int	drains;
};

/*
 * Debug print the thread stats
 * Use with pthread_cleanup_push for displaying at thread cancellation
 */
static void
thread_loop_stats(void *x)
{
	struct thread_stats *s = x;

	debug("tid %lu - %u fills, %u skips, %u waits", pthread_self(),
			s->fills, s->skips, s->waits);
}

# define STATS_STRUCT(s)	struct thread_stats s
# define STATS_INIT(s)		{ memset(&s, 0, sizeof(s)); }
# define STATS_FILL(s)		{ s.fills++; }
# define STATS_SKIP(s)		{ s.skips++; }
# define STATS_WAIT(s)		{ s.waits++; }
# define STATS_DRAIN(s)		{ s.drains++; }
#else
# define STATS_STRUCT(s)
# define STATS_INIT(s)
# define STATS_FILL(s)
# define STATS_SKIP(s)
# define STATS_WAIT(s)
# define STATS_DRAIN(s)
#endif

/* Keystream Queue state */
enum {
	KQINIT,
	KQEMPTY,
	KQFILLING,
	KQFULL,
	KQDRAINING
};

/* Keystream Queue struct */
struct kq {
	u_char		keys[KQLEN][AES_BLOCK_SIZE];
	u_char		ctr[AES_BLOCK_SIZE];
	u_char		pad0[CACHELINE_LEN];
	volatile int	qstate;
	pthread_mutex_t	lock;
	pthread_cond_t	cond;
	u_char		pad1[CACHELINE_LEN];
};

/* Context struct */
struct ssh_aes_ctr_ctx
{
	struct kq	q[NUMKQ];
	AES_KEY		aes_ctx;
	STATS_STRUCT(stats);
	u_char		aes_counter[AES_BLOCK_SIZE];
	pthread_t	tid[CIPHER_THREADS];
	int		state;
	int		qidx;
	int		ridx;
};

/* <friedl>
 * increment counter 'ctr',
 * the counter is of size 'len' bytes and stored in network-byte-order.
 * (LSB at ctr[len-1], MSB at ctr[0])
 */
static void
ssh_ctr_inc(u_char *ctr, size_t len)
{
	int i;

	for (i = len - 1; i >= 0; i--)
		if (++ctr[i])	/* continue on overflow */
			return;
}

/*
 * Add num to counter 'ctr'
 */
static void
ssh_ctr_add(u_char *ctr, uint32_t num, u_int len)
{
	int i;
	uint16_t n;

	for (n = 0, i = len - 1; i >= 0 && (num || n); i--) {
		n = ctr[i] + (num & 0xff) + n;
		num >>= 8;
		ctr[i] = n & 0xff;
		n >>= 8;
	}
}

/*
 * Threads may be cancelled in a pthread_cond_wait, we must free the mutex
 */
static void
thread_loop_cleanup(void *x)
{
	pthread_mutex_unlock((pthread_mutex_t *)x);
}

/*
 * The life of a pregen thread:
 *    Find empty keystream queues and fill them using their counter.
 *    When done, update counter for the next fill.
 */
static void *
thread_loop(void *x)
{
	AES_KEY key;
	STATS_STRUCT(stats);
	struct ssh_aes_ctr_ctx *c = x;
	struct kq *q;
	int i;
	int qidx;

	/* Threads stats on cancellation */
	STATS_INIT(stats);
#ifdef CIPHER_THREAD_STATS
	pthread_cleanup_push(thread_loop_stats, &stats);
#endif

	/* Thread local copy of AES key */
	memcpy(&key, &c->aes_ctx, sizeof(key));

	/*
	 * Handle the special case of startup, one thread must fill
	 * the first KQ then mark it as draining. Lock held throughout.
	 */
	if (pthread_equal(pthread_self(), c->tid[0])) {
		q = &c->q[0];
		pthread_mutex_lock(&q->lock);
		if (q->qstate == KQINIT) {
			for (i = 0; i < KQLEN; i++) {
				AES_encrypt(q->ctr, q->keys[i], &key);
				ssh_ctr_inc(q->ctr, AES_BLOCK_SIZE);
			}
			ssh_ctr_add(q->ctr, KQLEN * (NUMKQ - 1), AES_BLOCK_SIZE);
			q->qstate = KQDRAINING;
			STATS_FILL(stats);
			pthread_cond_broadcast(&q->cond);
		}
		pthread_mutex_unlock(&q->lock);
	} else
		STATS_SKIP(stats);

	/*
	 * Normal case is to find empty queues and fill them, skipping over
	 * queues already filled by other threads and stopping to wait for
	 * a draining queue to become empty.
	 *
	 * Multiple threads may be waiting on a draining queue and awoken
	 * when empty.  The first thread to wake will mark it as filling,
	 * others will move on to fill, skip, or wait on the next queue.
	 */
	for (qidx = 1;; qidx = (qidx + 1) % NUMKQ) {
		/* Check if I was cancelled, also checked in cond_wait */
		pthread_testcancel();

		/* Lock queue and block if its draining */
		q = &c->q[qidx];
		pthread_mutex_lock(&q->lock);
		pthread_cleanup_push(thread_loop_cleanup, &q->lock);
		while (q->qstate == KQDRAINING || q->qstate == KQINIT) {
			STATS_WAIT(stats);
			pthread_cond_wait(&q->cond, &q->lock);
		}
		pthread_cleanup_pop(0);

		/* If filling or full, somebody else got it, skip */
		if (q->qstate != KQEMPTY) {
			pthread_mutex_unlock(&q->lock);
			STATS_SKIP(stats);
			continue;
		}

		/*
		 * Empty, let's fill it.
		 * Queue lock is relinquished while we do this so others
		 * can see that it's being filled.
		 */
		q->qstate = KQFILLING;
		pthread_mutex_unlock(&q->lock);
		for (i = 0; i < KQLEN; i++) {
			AES_encrypt(q->ctr, q->keys[i], &key);
			ssh_ctr_inc(q->ctr, AES_BLOCK_SIZE);
		}

		/* Re-lock, mark full and signal consumer */
		pthread_mutex_lock(&q->lock);
		ssh_ctr_add(q->ctr, KQLEN * (NUMKQ - 1), AES_BLOCK_SIZE);
		q->qstate = KQFULL;
		STATS_FILL(stats);
		pthread_cond_signal(&q->cond);
		pthread_mutex_unlock(&q->lock);
	}

#ifdef CIPHER_THREAD_STATS
	/* Stats */
	pthread_cleanup_pop(1);
#endif

	return NULL;
}

static int
ssh_aes_ctr(EVP_CIPHER_CTX *ctx, u_char *dest, const u_char *src,
    LIBCRYPTO_EVP_INL_TYPE len)
{
	typedef union {
#ifdef CIPHER_INT128_OK
		__uint128_t *u128;
#endif
		uint64_t *u64;
		uint32_t *u32;
		uint8_t *u8;
		const uint8_t *cu8;
		uintptr_t u;
	} ptrs_t;
	ptrs_t destp, srcp, bufp;
	uintptr_t align;
	struct ssh_aes_ctr_ctx *c;
	struct kq *q, *oldq;
	int ridx;
	u_char *buf;

	if (len == 0)
		return 1;
	if ((c = EVP_CIPHER_CTX_get_app_data(ctx)) == NULL)
		return 0;

	q = &c->q[c->qidx];
	ridx = c->ridx;

	/* src already padded to block multiple */
	srcp.cu8 = src;
	destp.u8 = dest;
	while (len > 0) {
		buf = q->keys[ridx];
		bufp.u8 = buf;

		/* figure out the alignment on the fly */
#ifdef CIPHER_UNALIGNED_OK
		align = 0;
#else
		align = destp.u | srcp.u | bufp.u;
#endif

#ifdef CIPHER_INT128_OK
		if ((align & 0xf) == 0) {
			destp.u128[0] = srcp.u128[0] ^ bufp.u128[0];
		} else
#endif
		if ((align & 0x7) == 0) {
			destp.u64[0] = srcp.u64[0] ^ bufp.u64[0];
			destp.u64[1] = srcp.u64[1] ^ bufp.u64[1];
		} else if ((align & 0x3) == 0) {
			destp.u32[0] = srcp.u32[0] ^ bufp.u32[0];
			destp.u32[1] = srcp.u32[1] ^ bufp.u32[1];
			destp.u32[2] = srcp.u32[2] ^ bufp.u32[2];
			destp.u32[3] = srcp.u32[3] ^ bufp.u32[3];
		} else {
			size_t i;
			for (i = 0; i < AES_BLOCK_SIZE; ++i)
				dest[i] = src[i] ^ buf[i];
		}

		destp.u += AES_BLOCK_SIZE;
		srcp.u += AES_BLOCK_SIZE;
		len -= AES_BLOCK_SIZE;
		ssh_ctr_inc(ctx->iv, AES_BLOCK_SIZE);

		/* Increment read index, switch queues on rollover */
		if ((ridx = (ridx + 1) % KQLEN) == 0) {
			oldq = q;

			/* Mark next queue draining, may need to wait */
			c->qidx = (c->qidx + 1) % NUMKQ;
			q = &c->q[c->qidx];
			pthread_mutex_lock(&q->lock);
			while (q->qstate != KQFULL) {
				STATS_WAIT(c->stats);
				pthread_cond_wait(&q->cond, &q->lock);
			}
			q->qstate = KQDRAINING;
			pthread_mutex_unlock(&q->lock);

			/* Mark consumed queue empty and signal producers */
			pthread_mutex_lock(&oldq->lock);
			oldq->qstate = KQEMPTY;
			STATS_DRAIN(c->stats);
			pthread_cond_broadcast(&oldq->cond);
			pthread_mutex_unlock(&oldq->lock);
		}
	}
	c->ridx = ridx;
	return 1;
}

#define HAVE_NONE       0
#define HAVE_KEY        1
#define HAVE_IV         2
static int
ssh_aes_ctr_init(EVP_CIPHER_CTX *ctx, const u_char *key, const u_char *iv,
    int enc)
{
	struct ssh_aes_ctr_ctx *c;
	int i;

	if ((c = EVP_CIPHER_CTX_get_app_data(ctx)) == NULL) {
		c = xmalloc(sizeof(*c));

		c->state = HAVE_NONE;
		for (i = 0; i < NUMKQ; i++) {
			pthread_mutex_init(&c->q[i].lock, NULL);
			pthread_cond_init(&c->q[i].cond, NULL);
		}

		STATS_INIT(c->stats);
		EVP_CIPHER_CTX_set_app_data(ctx, c);
	}

	if (c->state == (HAVE_KEY | HAVE_IV)) {
		/* Cancel pregen threads */
		for (i = 0; i < CIPHER_THREADS; i++)
			pthread_cancel(c->tid[i]);
		for (i = 0; i < CIPHER_THREADS; i++)
			pthread_join(c->tid[i], NULL);
		/* Start over getting key & iv */
		c->state = HAVE_NONE;
	}

	if (key != NULL) {
		AES_set_encrypt_key(key, EVP_CIPHER_CTX_key_length(ctx) * 8,
		    &c->aes_ctx);
		c->state |= HAVE_KEY;
	}

	if (iv != NULL) {
		memcpy(ctx->iv, iv, AES_BLOCK_SIZE);
		c->state |= HAVE_IV;
	}

	if (c->state == (HAVE_KEY | HAVE_IV)) {
		/* Clear queues */
		memcpy(c->q[0].ctr, ctx->iv, AES_BLOCK_SIZE);
		c->q[0].qstate = KQINIT;
		for (i = 1; i < NUMKQ; i++) {
			memcpy(c->q[i].ctr, ctx->iv, AES_BLOCK_SIZE);
			ssh_ctr_add(c->q[i].ctr, i * KQLEN, AES_BLOCK_SIZE);
			c->q[i].qstate = KQEMPTY;
		}
		c->qidx = 0;
		c->ridx = 0;

		/* Start threads */
		for (i = 0; i < CIPHER_THREADS; i++) {
			debug("spawned a thread");
			pthread_create(&c->tid[i], NULL, thread_loop, c);
		}
		pthread_mutex_lock(&c->q[0].lock);
		while (c->q[0].qstate != KQDRAINING)
			pthread_cond_wait(&c->q[0].cond, &c->q[0].lock);
		pthread_mutex_unlock(&c->q[0].lock);
	}
	return 1;
}

/* this function is no longer used but might prove handy in the future
 * this comment also applies to ssh_aes_ctr_thread_reconstruction
 */
void
ssh_aes_ctr_thread_destroy(EVP_CIPHER_CTX *ctx)
{
	struct ssh_aes_ctr_ctx *c;
	int i;
	c = EVP_CIPHER_CTX_get_app_data(ctx);
	/* destroy threads */
	for (i = 0; i < CIPHER_THREADS; i++) {
		pthread_cancel(c->tid[i]);
	}
	for (i = 0; i < CIPHER_THREADS; i++) {
		pthread_join(c->tid[i], NULL);
	}
}

void
ssh_aes_ctr_thread_reconstruction(EVP_CIPHER_CTX *ctx)
{
	struct ssh_aes_ctr_ctx *c;
	int i;
	c = EVP_CIPHER_CTX_get_app_data(ctx);
	/* reconstruct threads */
	for (i = 0; i < CIPHER_THREADS; i++) {
		debug("spawned a thread");
		pthread_create(&c->tid[i], NULL, thread_loop, c);
	}
}

static int
ssh_aes_ctr_cleanup(EVP_CIPHER_CTX *ctx)
{
	struct ssh_aes_ctr_ctx *c;
	int i;

	if ((c = EVP_CIPHER_CTX_get_app_data(ctx)) != NULL) {
#ifdef CIPHER_THREAD_STATS
		debug("main thread: %u drains, %u waits", c->stats.drains,
				c->stats.waits);
#endif
		/* Cancel pregen threads */
		for (i = 0; i < CIPHER_THREADS; i++)
			pthread_cancel(c->tid[i]);
		for (i = 0; i < CIPHER_THREADS; i++)
			pthread_join(c->tid[i], NULL);

		memset(c, 0, sizeof(*c));
		free(c);
		EVP_CIPHER_CTX_set_app_data(ctx, NULL);
	}
	return 1;
}

/* <friedl> */
const EVP_CIPHER *
evp_aes_ctr_mt(void)
{
	static EVP_CIPHER aes_ctr;

	memset(&aes_ctr, 0, sizeof(EVP_CIPHER));
	aes_ctr.nid = NID_undef;
	aes_ctr.block_size = AES_BLOCK_SIZE;
	aes_ctr.iv_len = AES_BLOCK_SIZE;
	aes_ctr.key_len = 16;
	aes_ctr.init = ssh_aes_ctr_init;
	aes_ctr.cleanup = ssh_aes_ctr_cleanup;
	aes_ctr.do_cipher = ssh_aes_ctr;
#ifndef SSH_OLD_EVP
	aes_ctr.flags = EVP_CIPH_CBC_MODE | EVP_CIPH_VARIABLE_LENGTH |
	    EVP_CIPH_ALWAYS_CALL_INIT | EVP_CIPH_CUSTOM_IV;
#endif
	return &aes_ctr;
}

#endif /* defined(WITH_OPENSSL) */
