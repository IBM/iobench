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
#include <aio.h>
#include <sys/mman.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <stdlib.h>
#include <unistd.h>
#include <errno.h>

#ifndef linux
#define O_DIRECT 0
#endif

static void aio_destroy_thread_ctx(io_bench_thr_ctx_t *ctx);
static int aio_init_thread_ctx(io_bench_thr_ctx_t **pctx, io_bench_params_t *params, unsigned int dev_idx);
static int aio_poll_completions(io_bench_thr_ctx_t *ctx, int n);
static io_ctx_t *aio_get_io_ctx(io_bench_thr_ctx_t *ctx, uint16_t slot);
static int aio_queue_io(io_bench_thr_ctx_t *ctx, io_ctx_t *io);

io_eng_def_t aio_engine = {
	.init_thread_ctx = &aio_init_thread_ctx,
	.destroy_thread_ctx = &aio_destroy_thread_ctx,
	.poll_completions = &aio_poll_completions,
	.get_io_ctx = &aio_get_io_ctx,
	.queue_io = &aio_queue_io,
};

typedef struct {
	struct aiocb aiocb;
	io_ctx_t ioctx;
} aio_ioctx_t;

typedef struct {
	aio_ioctx_t *ioctx;
	const struct aiocb **aiocb;
	uint32_t bs;
	uint32_t qs;
	uint32_t slot;
	bool rr;
	union {
		struct {
			int fd;
		};
		struct {
			const int *fds;
			char **dev_names;
		};
	};
	io_bench_thr_ctx_t iobench_ctx;
} aio_thr_ctx_t;

#define U_2_P(_u_h_) \
({ \
	force_type((_u_h_), io_bench_thr_ctx_t *); \
	aio_thr_ctx_t *__res__ = list_parent_struct(_u_h_, aio_thr_ctx_t, iobench_ctx); \
	__res__; \
})

#define UIO_2_PIO(_u_h_) \
({ \
	force_type((_u_h_), io_ctx_t*); \
	aio_ioctx_t *__res__ = list_parent_struct(_u_h_, aio_ioctx_t, ioctx); \
	__res__; \
})

static void aio_destroy_thread_ctx(io_bench_thr_ctx_t *ctx)
{
	aio_thr_ctx_t *pctx = U_2_P(ctx);
	if (pctx->ioctx)
		free(pctx->ioctx);
	if (pctx->aiocb)
		free(pctx->aiocb);
	if (!pctx->rr && pctx->fd != -1)
		close(pctx->fd);
	if (pctx->rr && !pctx->slot && pctx->fds)
		free((void *)pctx->fds);
}

static int aio_init_thread_ctx(io_bench_thr_ctx_t **pctx, io_bench_params_t *params, unsigned int dev_idx)
{
	aio_thr_ctx_t *aio_thr_ctx;
	unsigned int i;
	static int *fds = NULL;
	int fd;
	static pthread_once_t once_control = PTHREAD_ONCE_INIT;
	void init_dio(void)
	{
#ifdef linux
		aio_init(&(struct aioinit) {
			.aio_threads = params->ndevs,
			.aio_num = params->ndevs * params->qs,
		});
#endif
		if (!params->rr)
			return;
		fds = calloc(params->ndevs, sizeof(*fds));
		ASSERT(fds);
		memset(fds, 0xff, sizeof(int) * params->ndevs);
		for (i = 0; i < params->ndevs; i++) {
			fds[i] = open(params->devices[i], O_RDWR|O_DIRECT);
			ASSERT(fds[i] >= 0);
		}
	}
	pthread_once(&once_control, &init_dio);
	*pctx = NULL;
	aio_thr_ctx = calloc(1, sizeof(*aio_thr_ctx));
	if (!aio_thr_ctx) {
		ERROR("Failed to alloc");
		return -ENOMEM;
	}

	aio_thr_ctx->qs = params->qs;
	aio_thr_ctx->bs = params->bs;
	aio_thr_ctx->slot = dev_idx;
	aio_thr_ctx->rr = params->rr;
	if (!params->rr)
		aio_thr_ctx->fd = -1;

	aio_thr_ctx->ioctx = calloc(params->qs, sizeof(*aio_thr_ctx->ioctx));
	if (!aio_thr_ctx->ioctx) {
		ERROR("Failed to alloc");
		aio_destroy_thread_ctx(&aio_thr_ctx->iobench_ctx);
		return -ENOMEM;
	}
	aio_thr_ctx->aiocb = calloc(params->qs, sizeof(*aio_thr_ctx->aiocb));
	if (!aio_thr_ctx->aiocb) {
		ERROR("Failed to alloc");
		aio_destroy_thread_ctx(&aio_thr_ctx->iobench_ctx);
		return -ENOMEM;
	}
	if (!params->rr) {
		aio_thr_ctx->fd = open(params->devices[dev_idx], O_RDWR|O_DIRECT);
		if (aio_thr_ctx->fd < 0) {
			ERROR("Failed to open %s", params->devices[dev_idx]);
			aio_destroy_thread_ctx(&aio_thr_ctx->iobench_ctx);
			return -ENOMEM;
		}
		fd = aio_thr_ctx->fd;
	} else {
		aio_thr_ctx->fds = fds;
		aio_thr_ctx->dev_names = params->devices;
		fd = fds[dev_idx];
	}
	aio_thr_ctx->iobench_ctx.capacity = lseek(fd, 0, SEEK_END);
	if (aio_thr_ctx->iobench_ctx.capacity == -1ULL) {
		ERROR("Failed to determine capacity for %s", params->devices[dev_idx]);
		aio_destroy_thread_ctx(&aio_thr_ctx->iobench_ctx);
		return -ENOMEM;
	}

	for (i = 0; i < aio_thr_ctx->qs; i++) {
		aio_thr_ctx->aiocb[i] = &aio_thr_ctx->ioctx[i].aiocb;
		aio_thr_ctx->ioctx[i].aiocb.aio_buf = aio_thr_ctx->ioctx[i].ioctx.buf;
		aio_thr_ctx->ioctx[i].aiocb.aio_fildes = fd;
		aio_thr_ctx->ioctx[i].aiocb.aio_nbytes = aio_thr_ctx->bs;
		aio_thr_ctx->ioctx[i].aiocb.aio_sigevent.sigev_notify = SIGEV_NONE;
	}
	*pctx = &aio_thr_ctx->iobench_ctx;
	return 0;
}

static int aio_poll_completions(io_bench_thr_ctx_t *ctx, int n)
{
	aio_thr_ctx_t *pctx = U_2_P(ctx);
	int rc;

	if (!(rc = aio_suspend(pctx->aiocb, pctx->qs, NULL))) {
		uint16_t i;
		for (i = 0; i < pctx->qs && n; i++) {
			rc = aio_error(pctx->aiocb[i]);
			if (rc != EINPROGRESS) {
				n--;
				pctx->ioctx[i].ioctx.status = rc;
				io_bench_requeue_io(ctx, &pctx->ioctx[i].ioctx);
			}
		}
	}
	return 0;
}

static io_ctx_t *aio_get_io_ctx(io_bench_thr_ctx_t *ctx, uint16_t slot)
{
	aio_thr_ctx_t *pctx = U_2_P(ctx);
	if (pctx->qs <= slot)
		return NULL;
	return &pctx->ioctx[slot].ioctx;
}

static int aio_queue_io(io_bench_thr_ctx_t *ctx, io_ctx_t *io)
{
	aio_ioctx_t *pio = UIO_2_PIO(io);
	aio_thr_ctx_t *pctx = U_2_P(ctx);

	pio->aiocb.aio_offset = io->offset;
	if (pctx->rr)
		pio->aiocb.aio_fildes = pctx->fds[io->dev_idx];

	return !io->write ? aio_read(&pio->aiocb) : aio_write(&pio->aiocb);
}
