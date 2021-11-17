/*
 * <copyright-info>
 * IBM Confidential
 * OCO Source Materials
 * 2810
 * Author: Constantine Gavrilov <constg@il.ibm.com>
 * (C) Copyright IBM Corp. 2021
 * The source code for this program is not published or otherwise
 * divested of its trade secrets, irrespective of what has
 * been deposited with the U.S. Copyright Office
 * </copyright-info>
 */

#define _GNU_SOURCE
#include "logger.h"
DECLARE_BFN
#include "compiler.h"
#include "iobench.h"
#include <pthread.h>
#include <sys/mman.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <stdlib.h>
#include <unistd.h>
#include <signal.h>
#include <errno.h>


static void dio_destroy_thread_ctx(io_bench_thr_ctx_t *ctx);
static int dio_init_thread_ctx(io_bench_thr_ctx_t **pctx, io_bench_params_t *params, unsigned int dev_idx);
static int dio_poll_completions(io_bench_thr_ctx_t *ctx, int n);
static io_ctx_t *dio_get_io_ctx(io_bench_thr_ctx_t *ctx, uint16_t slot);
static int dio_queue_io(io_bench_thr_ctx_t *ctx, io_ctx_t *io);

io_eng_def_t dio_engine = {
	.init_thread_ctx = &dio_init_thread_ctx,
	.stop_thread_ctx = &dio_destroy_thread_ctx,
	.poll_completions = &dio_poll_completions,
	.get_io_ctx = &dio_get_io_ctx,
	.queue_io = &dio_queue_io,
	.seed_per_io = true,
};

typedef struct {
	int fd;
	io_ctx_t ioctx;
	io_bench_thr_ctx_t *parent;
	pthread_t tid;
	pthread_cond_t run_cond;
	pthread_mutex_t run_mutex;
	bool may_run;
} dio_ioctx_t;

typedef struct {
	dio_ioctx_t *ioctx;
	pthread_t self;
	void *buf_head;
	uint32_t bs;
	uint32_t qs;
	int fd;
	io_bench_thr_ctx_t iobench_ctx;
} dio_thr_ctx_t;

#define U_2_P(_u_h_) \
({ \
	force_type((_u_h_), io_bench_thr_ctx_t *); \
	dio_thr_ctx_t *__res__ = list_parent_struct(_u_h_, dio_thr_ctx_t, iobench_ctx); \
	__res__; \
})

#define UIO_2_PIO(_u_h_) \
({ \
	force_type((_u_h_), io_ctx_t*); \
	dio_ioctx_t *__res__ = list_parent_struct(_u_h_, dio_ioctx_t, ioctx); \
	__res__; \
})

static void kill_all_dio_threads(io_bench_thr_ctx_t *ctx)
{
	dio_thr_ctx_t *pctx = U_2_P(ctx);
	unsigned int i;

	if (!pctx->ioctx)
		return;
	for(i = 0; i < pctx->qs; i++) {
		if (pctx->ioctx[i].tid) {
			pthread_kill(pctx->ioctx[i].tid, SIGUSR1);
		}
	}
	for(i = 0; i < pctx->qs; i++) {
		if (pctx->ioctx[i].tid) {
			pthread_join(pctx->ioctx[i].tid, NULL);
		}
	}

	for (i = 0; i < pctx->qs; i++) {
		if (pctx->ioctx[i].fd >= 0)
			close(pctx->ioctx[i].fd);
	}
}

static void dio_destroy_thread_ctx(io_bench_thr_ctx_t *ctx)
{
	dio_thr_ctx_t *pctx = U_2_P(ctx);

	kill_all_dio_threads(ctx);
	if (pctx->buf_head)
		munmap(pctx->buf_head, ((size_t)pctx->bs) * pctx->qs);

	if (pctx->ioctx)
		free(pctx->ioctx);
}

static void term_handler(int signo)
{
	pthread_exit(NULL);
}

#define SET_ERR(__rc__) ((__rc__ < 0) ? (errno ? -errno : -1) : -ENODATA)

static void *thread_func(void *arg)
{
	dio_ioctx_t *ctx = arg;
	io_ctx_t *ioctx = &ctx->ioctx;
	io_bench_thr_ctx_t *thr_ctx = ctx->parent;
	int fd = ctx->fd;
	uint32_t bs = U_2_P(thr_ctx)->bs;
	int rc;

	pthread_mutex_lock(&ctx->run_mutex);
	while (!ctx->may_run)
		pthread_cond_wait(&ctx->run_cond, &ctx->run_mutex);
	pthread_mutex_unlock(&ctx->run_mutex);

	while (1) {
		rc = (!ioctx->write) ? pread(fd, ioctx->buf, bs, ioctx->offset) : pwrite(fd, ioctx->buf, bs, ioctx->offset);
		ioctx->status = (rc == bs) ? 0 : SET_ERR(rc);
		io_bench_complete_and_prep_io(thr_ctx, ioctx);
	}
	return NULL;
}

static int dio_init_thread_ctx(io_bench_thr_ctx_t **pctx, io_bench_params_t *params, unsigned int dev_idx)
{
	dio_thr_ctx_t *dio_thr_ctx;
	size_t map_size;
	unsigned int i;
	unsigned int seed;
	struct sigaction act = {{0}};

	*pctx = NULL;
	dio_thr_ctx = calloc(1, sizeof(*dio_thr_ctx));
	if (!dio_thr_ctx) {
		ERROR("Failed to alloc");
		return -ENOMEM;
	}

	dio_thr_ctx->self = pthread_self();
	dio_thr_ctx->qs = params->qs;
	dio_thr_ctx->bs = params->bs;

	map_size = params->bs;
	map_size *= params->qs;
	dio_thr_ctx->buf_head = mmap(NULL, map_size, PROT_READ|PROT_WRITE, MAP_SHARED|MAP_ANONYMOUS, -1, 0);
	if (dio_thr_ctx->buf_head == MAP_FAILED) {
		ERROR("Failed to map IO buffers");
		dio_destroy_thread_ctx(&dio_thr_ctx->iobench_ctx);
		return -ENOMEM;
	}
	dio_thr_ctx->ioctx = calloc(params->qs, sizeof(*dio_thr_ctx->ioctx));
	if (!dio_thr_ctx->ioctx) {
		ERROR("Failed to map IO buffers");
		dio_destroy_thread_ctx(&dio_thr_ctx->iobench_ctx);
		return -ENOMEM;
	}
	seed = get_uptime_us();
	for (i = 0; i < dio_thr_ctx->qs; i++) {
		dio_thr_ctx->ioctx[i].ioctx.seed = seed++;
		dio_thr_ctx->ioctx[i].ioctx.buf = dio_thr_ctx->buf_head + ((size_t)dio_thr_ctx->bs) * i;
		dio_thr_ctx->ioctx[i].fd = -1;
		dio_thr_ctx->ioctx[i].run_mutex = (pthread_mutex_t)PTHREAD_MUTEX_INITIALIZER;
		dio_thr_ctx->ioctx[i].run_cond = (pthread_cond_t)PTHREAD_COND_INITIALIZER;
		dio_thr_ctx->ioctx[i].parent = &dio_thr_ctx->iobench_ctx;
	}
	for (i = 0; i < dio_thr_ctx->qs; i++) {
		dio_thr_ctx->ioctx[i].fd = open(params->devices[dev_idx], O_RDWR|O_DIRECT);
		if (dio_thr_ctx->ioctx[i].fd < 0) {
			ERROR("Failed to open %s", params->devices[dev_idx]);
			dio_destroy_thread_ctx(&dio_thr_ctx->iobench_ctx);
			return -ENOMEM;
		}
	}

	dio_thr_ctx->iobench_ctx.capacity = lseek(dio_thr_ctx->ioctx[0].fd, 0, SEEK_END);
	if (dio_thr_ctx->iobench_ctx.capacity == -1ULL) {
		ERROR("Failed to determine capacity for %s", params->devices[dev_idx]);
		dio_destroy_thread_ctx(&dio_thr_ctx->iobench_ctx);
		return -ENOMEM;
	}

	act.sa_handler = term_handler;
	if (sigaction(SIGUSR1, &act, NULL)) {
		ERROR("Failed to setup SIGTERM handler");
		dio_destroy_thread_ctx(&dio_thr_ctx->iobench_ctx);
		return -EPERM;
	}

	for (i = 0; i < dio_thr_ctx->qs; i++) {
		int rc = pthread_create(&dio_thr_ctx->ioctx[i].tid, NULL, thread_func, &dio_thr_ctx->ioctx[i]);
		if (rc ) {
			dio_thr_ctx->ioctx[i].tid = 0;
			if (rc > 0)
				rc = -rc;
			ERROR("Failed to create DIO thread");
			dio_destroy_thread_ctx(&dio_thr_ctx->iobench_ctx);
			return rc;
		}
	}
	*pctx = &dio_thr_ctx->iobench_ctx;
	return 0;
}

static int dio_poll_completions(io_bench_thr_ctx_t *ctx, int n)
{
	usleep(100000);
	return 0;
}

static io_ctx_t *dio_get_io_ctx(io_bench_thr_ctx_t *ctx, uint16_t slot)
{
	dio_thr_ctx_t *pctx = U_2_P(ctx);
	if (pctx->qs <= slot)
		return NULL;
	return &pctx->ioctx[slot].ioctx;
}

static int dio_queue_io(io_bench_thr_ctx_t *ctx, io_ctx_t *io)
{
	dio_ioctx_t *pio = UIO_2_PIO(io);
	dio_thr_ctx_t *pctx = U_2_P(ctx);

	ASSERT(pctx->self == pthread_self());
	pthread_mutex_lock(&pio->run_mutex);
	pio->may_run = true;
	pthread_cond_signal(&pio->run_cond);
	pthread_mutex_unlock(&pio->run_mutex);
	return 0;
}
