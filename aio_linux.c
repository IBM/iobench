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
#include <libaio.h>
#include <sys/mman.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <stdlib.h>
#include <unistd.h>
#include <errno.h>


static void aio_linux_destroy_thread_ctx(io_bench_thr_ctx_t *ctx);
static int aio_linux_init_thread_ctx(io_bench_thr_ctx_t **pctx, io_bench_params_t *params, unsigned int dev_idx);
static int aio_linux_poll_completions(io_bench_thr_ctx_t *ctx, int n);
static io_ctx_t *aio_linux_get_io_ctx(io_bench_thr_ctx_t *ctx, uint16_t slot);
static int aio_linux_queue_io(io_bench_thr_ctx_t *ctx, io_ctx_t *io);

io_eng_def_t aio_linux_engine = {
	.init_thread_ctx = &aio_linux_init_thread_ctx,
	.destroy_thread_ctx = &aio_linux_destroy_thread_ctx,
	.poll_completions = &aio_linux_poll_completions,
	.get_io_ctx = &aio_linux_get_io_ctx,
	.queue_io = &aio_linux_queue_io,
};

typedef struct {
	struct iocb iocb;
	io_ctx_t ioctx;
} aio_linux_ioctx_t;

typedef struct {
	io_context_t handle;
	aio_linux_ioctx_t *ioctx;
	struct iocb **iocbs;
	void *buf_head;
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
} aio_linux_thr_ctx_t;

#define U_2_P(_u_h_) \
({ \
	force_type((_u_h_), io_bench_thr_ctx_t *); \
	aio_linux_thr_ctx_t *__res__ = list_parent_struct(_u_h_, aio_linux_thr_ctx_t, iobench_ctx); \
	__res__; \
})

#define UIO_2_PIO(_u_h_) \
({ \
	force_type((_u_h_), io_ctx_t*); \
	aio_linux_ioctx_t *__res__ = list_parent_struct(_u_h_, aio_linux_ioctx_t, ioctx); \
	__res__; \
})

#define UAIO_2_PAIO(_u_h_) \
({ \
	force_type((_u_h_), struct iocb*); \
	aio_linux_ioctx_t *__res__ = list_parent_struct(_u_h_, aio_linux_ioctx_t, iocb); \
	__res__; \
})

static void aio_linux_destroy_thread_ctx(io_bench_thr_ctx_t *ctx)
{
	aio_linux_thr_ctx_t *pctx = U_2_P(ctx);
	if (pctx->buf_head)
		munmap(pctx->buf_head, ((size_t)pctx->bs) * pctx->qs);
	if (pctx->ioctx)
		free(pctx->ioctx);
	if (pctx->iocbs)
		free(pctx->iocbs);
	if (!pctx->rr && pctx->fd != -1)
		close(pctx->fd);
	if (pctx->rr && pctx->fds)
		free((void *)pctx->fds);
}

static int aio_linux_init_thread_ctx(io_bench_thr_ctx_t **pctx, io_bench_params_t *params, unsigned int dev_idx)
{
	aio_linux_thr_ctx_t *aio_linux_thr_ctx;
	size_t map_size;
	unsigned int i;
	static int *fds = NULL;
	int fd;
	static pthread_once_t once_control = PTHREAD_ONCE_INIT;
	void init_dio(void)
	{
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
	aio_linux_thr_ctx = calloc(1, sizeof(*aio_linux_thr_ctx));
	if (!aio_linux_thr_ctx) {
		ERROR("Failed to alloc");
		return -ENOMEM;
	}

	aio_linux_thr_ctx->qs = params->qs;
	aio_linux_thr_ctx->bs = params->bs;
	aio_linux_thr_ctx->slot = dev_idx;
	aio_linux_thr_ctx->rr = params->rr;
	if (!params->rr)
		aio_linux_thr_ctx->fd = -1;

	if (io_setup(aio_linux_thr_ctx->qs, &aio_linux_thr_ctx->handle)) {
		ERROR("Failed to init aio handle");
		free(aio_linux_thr_ctx);
		return -ENOMEM;
	}

	aio_linux_thr_ctx->ioctx = calloc(params->qs, sizeof(*aio_linux_thr_ctx->ioctx));
	if (!aio_linux_thr_ctx->ioctx) {
		ERROR("Failed to alloc");
		aio_linux_destroy_thread_ctx(&aio_linux_thr_ctx->iobench_ctx);
		return -ENOMEM;
	}
	aio_linux_thr_ctx->iocbs = calloc(params->qs, sizeof(*aio_linux_thr_ctx->iocbs));
	if (!aio_linux_thr_ctx->iocbs) {
		ERROR("Failed to alloc");
		aio_linux_destroy_thread_ctx(&aio_linux_thr_ctx->iobench_ctx);
		return -ENOMEM;
	}
	map_size = params->bs;
	map_size *= params->qs;
	aio_linux_thr_ctx->buf_head = mmap(NULL, map_size, PROT_READ|PROT_WRITE, MAP_SHARED|MAP_ANONYMOUS, -1, 0);
	if (aio_linux_thr_ctx->buf_head == MAP_FAILED) {
		ERROR("Failed to map IO buffers");
		aio_linux_destroy_thread_ctx(&aio_linux_thr_ctx->iobench_ctx);
		return -ENOMEM;
	}
	if (!params->rr) {
		aio_linux_thr_ctx->fd = open(params->devices[dev_idx], O_RDWR|O_DIRECT);
		if (aio_linux_thr_ctx->fd < 0) {
			ERROR("Failed to open %s", params->devices[dev_idx]);
			aio_linux_destroy_thread_ctx(&aio_linux_thr_ctx->iobench_ctx);
			return -ENOMEM;
		}
		fd = aio_linux_thr_ctx->fd;
	} else {
		aio_linux_thr_ctx->fds = fds;
		aio_linux_thr_ctx->dev_names = params->devices;
		fd = fds[dev_idx];
	}
	aio_linux_thr_ctx->iobench_ctx.capacity = lseek(fd, 0, SEEK_END);
	if (aio_linux_thr_ctx->iobench_ctx.capacity == -1ULL) {
		ERROR("Failed to determine capacity for %s", params->devices[dev_idx]);
		aio_linux_destroy_thread_ctx(&aio_linux_thr_ctx->iobench_ctx);
		return -ENOMEM;
	}

	for (i = 0; i < aio_linux_thr_ctx->qs; i++) {
		aio_linux_thr_ctx->iocbs[i] = &aio_linux_thr_ctx->ioctx[i].iocb;
		aio_linux_thr_ctx->ioctx[i].ioctx.buf = aio_linux_thr_ctx->buf_head + ((size_t)aio_linux_thr_ctx->bs) * i;
		io_prep_pread(&aio_linux_thr_ctx->ioctx[i].iocb, fd, aio_linux_thr_ctx->ioctx[i].ioctx.buf,  aio_linux_thr_ctx->bs, 0);
	}
	*pctx = &aio_linux_thr_ctx->iobench_ctx;
	return 0;
}

static int aio_linux_poll_completions(io_bench_thr_ctx_t *ctx, int n)
{
	aio_linux_thr_ctx_t *pctx = U_2_P(ctx);
	int rc;
	struct io_event events[n];

	rc = io_getevents(pctx->handle, 1, n, events, NULL);
	if (rc > 0) {
		int i;
		for (i = 0; i < rc; i++) {
			aio_linux_ioctx_t *pio = UAIO_2_PAIO(events[i].obj);
			pio->ioctx.status = (events[i].res == pctx->bs) ?  events[i].res2 :  events[i].res;
			io_bench_requeue_io(ctx, &pio->ioctx);
		}
		rc = 0;
	}
	return rc;
}

static io_ctx_t *aio_linux_get_io_ctx(io_bench_thr_ctx_t *ctx, uint16_t slot)
{
	aio_linux_thr_ctx_t *pctx = U_2_P(ctx);
	if (pctx->qs <= slot)
		return NULL;
	return &pctx->ioctx[slot].ioctx;
}

static int aio_linux_queue_io(io_bench_thr_ctx_t *ctx, io_ctx_t *io)
{
	aio_linux_ioctx_t *pio = UIO_2_PIO(io);
	aio_linux_thr_ctx_t *pctx = U_2_P(ctx);
	struct iocb *io_list[1] = { &pio->iocb };
	int rc;
	int fd;


	fd = (pctx->rr) ?  pctx->fds[io->dev_idx] : pctx->fd;
	if (!io->write)
		io_prep_pread(&pio->iocb, fd, io->buf, pctx->bs, io->offset);
	else
		io_prep_pwrite(&pio->iocb, fd, io->buf, pctx->bs, io->offset);

	rc = io_submit(pctx->handle, 1, io_list);
	return (rc > 0) ? 0 : (rc < 0) ? rc : -1;
}
