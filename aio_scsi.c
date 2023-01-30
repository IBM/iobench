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
#include "scsi.h"
#include "compiler.h"
#include "aio_linux.h"
#include "uring.h"
#include <pthread.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <stdlib.h>
#include <unistd.h>
#include <errno.h>


static void sg_destroy_thread_ctx(io_bench_thr_ctx_t *ctx);
static int sg_init_thread_ctx(io_bench_thr_ctx_t **pctx, io_bench_params_t *params, void *buf_head, unsigned int dev_idx, unsigned int cpu);
static int sg_aio_linux_poll_completions(io_bench_thr_ctx_t *ctx, int n);
static io_ctx_t *sg_get_io_ctx(io_bench_thr_ctx_t *ctx, uint16_t slot);
static int sg_aio_linux_queue_io(io_bench_thr_ctx_t *ctx, io_ctx_t *io);

static int sg_aio_uring_poll_completions(io_bench_thr_ctx_t *ctx, int n);
static int sg_aio_uring_queue_io(io_bench_thr_ctx_t *ctx, io_ctx_t *io);

io_eng_def_t sg_aio_engine = {
	.init_thread_ctx = &sg_init_thread_ctx,
	.destroy_thread_ctx = &sg_destroy_thread_ctx,
	.poll_completions = &sg_aio_linux_poll_completions,
	.get_io_ctx = &sg_get_io_ctx,
	.queue_io = &sg_aio_linux_queue_io,
	.need_mr_buffers = true,
};

io_eng_def_t sg_uring_engine = {
	.init_thread_ctx = &sg_init_thread_ctx,
	.destroy_thread_ctx = &sg_destroy_thread_ctx,
	.poll_completions = &sg_aio_uring_poll_completions,
	.get_io_ctx = &sg_get_io_ctx,
	.queue_io = &sg_aio_uring_queue_io,
	.need_mr_buffers = true,
};

typedef struct {
	union {
		aio_linux_handle_t *handle_aio;
		uring_handle_t *handle_uring;
	};
	io_ctx_t *ioctx;
	sg_open_params_t sg_handle;
	uint32_t slot;
	io_bench_thr_ctx_t iobench_ctx;
	io_eng_t engine;
} sg_thr_ctx_t;

#define U_2_P(_u_h_) \
({ \
	force_type((_u_h_), io_bench_thr_ctx_t *); \
	sg_thr_ctx_t *__res__ = list_parent_struct(_u_h_, sg_thr_ctx_t, iobench_ctx); \
	__res__; \
})


static void sg_destroy_thread_ctx(io_bench_thr_ctx_t *ctx)
{
	sg_thr_ctx_t *pctx = U_2_P(ctx);
	if (pctx->ioctx)
		free(pctx->ioctx);
	if (pctx->engine == ENGINE_SG_AIO) {
		if (pctx->handle_aio)
			aio_linux_handle_destroy(pctx->handle_aio);
	} else {
		if (pctx->handle_uring)
			uring_handle_destroy(pctx->handle_uring);
	}
	close_sg_io_contexts(&pctx->sg_handle);
}


static int sg_requeue_io(io_bench_thr_ctx_t *ctx, io_ctx_t *io)
{
	sg_thr_ctx_t *pctx =  U_2_P(ctx);
	sg_open_params_t *sg_handle = &pctx->sg_handle;
	sg_ioctx_t *sg_ioctx = &sg_handle->ioctx[io->slot_idx];
	io->status = read(sg_ioctx->fd, &sg_ioctx->sg_resp, sizeof(sg_ioctx->sg_resp));
	io->status = decode_sg_status(io->status, &sg_ioctx->sg_resp);
	io_bench_requeue_io(ctx, io);
	return 0;
}

static int sg_init_thread_ctx(io_bench_thr_ctx_t **pctx, io_bench_params_t *params, void *buf_head, unsigned int dev_idx, unsigned int poll_cpu)
{
	sg_thr_ctx_t *sg_thr_ctx;
	unsigned int i;
	int fds[params->qs];
	int rc;

	*pctx = NULL;
	if (params->rr) {
		ERROR("SG engines do not support RR mode");
		return -EINVAL;
	}
	sg_thr_ctx = calloc(1, sizeof(*sg_thr_ctx));
	if (!sg_thr_ctx) {
		ERROR("Failed to alloc");
		return -ENOMEM;
	}

	sg_thr_ctx->sg_handle.name = params->devices[dev_idx];
	sg_thr_ctx->engine = params->engine;
	sg_thr_ctx->sg_handle.qs = params->qs;
	sg_thr_ctx->sg_handle.bs = params->bs;
	sg_thr_ctx->slot = dev_idx;
	sg_thr_ctx->ioctx = calloc(params->qs, sizeof(*sg_thr_ctx->ioctx));
	if (!sg_thr_ctx->ioctx) {
		ERROR("Failed to alloc");
		sg_destroy_thread_ctx(&sg_thr_ctx->iobench_ctx);
		return -ENOMEM;
	}
	rc = open_sg_io_contexts(&sg_thr_ctx->sg_handle);
	if (rc) {
		ERROR("Failed to create SCSI handle");
		sg_destroy_thread_ctx(&sg_thr_ctx->iobench_ctx);
		return rc;
	}

	for (i = 0; i < params->qs; i++) {
		fds[i] = sg_thr_ctx->sg_handle.ioctx[i].fd;
		sg_thr_ctx->ioctx[i].buf = sg_thr_ctx->sg_handle.ioctx[i].buf;
	}

	if ((params->engine == ENGINE_SG_AIO)) {
		if (aio_linux_handle_create(
			&(aio_linux_params_t) {
				.thr_ctx = &sg_thr_ctx->iobench_ctx,
				.requeue_io = sg_requeue_io,
				.queue_size = params->qs,
				.get_io_ctx = sg_get_io_ctx,
				.target_usec_latency = 1000000.0 / ((params->kiops * 1000)/(params->threads *params->qs)),
				.fds = fds,
				.fd_count = params->qs,
			},
			&sg_thr_ctx->handle_aio)) {
			ERROR("Failed to init aio handle");
			sg_destroy_thread_ctx(&sg_thr_ctx->iobench_ctx);
			return -ENOMEM;
		}
	} else if (uring_handle_create(
		&(uring_params_t) {
				.thr_ctx = &sg_thr_ctx->iobench_ctx,
				.requeue_io = sg_requeue_io,
				.get_io_ctx = sg_get_io_ctx,
				.target_usec_latency = 1000000.0 / ((params->kiops * 1000)/(params->threads *params->qs)),
				.mem = sg_thr_ctx->sg_handle.ioctx,
				.mem_size = sizeof(sg_thr_ctx->sg_handle.ioctx[0]) * params->qs,
				.queue_size = params->qs,
				.fds = fds,
				.fd_count = params->qs,
				.poll_idle_kernel_ms = params->poll_idle_kernel_ms,
				.poll_idle_user_ms = params->poll_idle_user_ms,
				.poll_cpu = poll_cpu,
			},
			&sg_thr_ctx->handle_uring)) {
		ERROR("Failed to init uring handle");
		sg_destroy_thread_ctx(&sg_thr_ctx->iobench_ctx);
		return -ENOMEM;
	}
	*pctx = &sg_thr_ctx->iobench_ctx;
	return 0;
}

static int sg_aio_linux_poll_completions(io_bench_thr_ctx_t *ctx, int n)
{
	return aio_linux_poll(U_2_P(ctx)->handle_aio, n);
}

static io_ctx_t *sg_get_io_ctx(io_bench_thr_ctx_t *ctx, uint16_t slot)
{
	sg_thr_ctx_t *pctx = U_2_P(ctx);
	if (pctx->sg_handle.qs <= slot)
		return NULL;
	return &pctx->ioctx[slot];
}

static int sg_sumbit_io(io_bench_thr_ctx_t *ctx, io_ctx_t *io)
{
	sg_thr_ctx_t *pctx =  U_2_P(ctx);
	sg_open_params_t *sg_handle = &pctx->sg_handle;
	sg_ioctx_t *sg_ioctx = &sg_handle->ioctx[io->slot_idx];

	int rc = !io->write ? frame_sg_read_io(sg_ioctx, io->offset / sg_handle->lba_size, sg_handle->bs_lbas, sg_handle->bs) :
						frame_sg_write_io(sg_ioctx, io->offset / sg_handle->lba_size, sg_handle->bs_lbas, sg_handle->bs);
	if (rc < 0)
		ERROR("Failed to submit SG IO");
	return rc;
}

static int sg_aio_linux_queue_io(io_bench_thr_ctx_t *ctx, io_ctx_t *io)
{
	sg_thr_ctx_t *pctx =  U_2_P(ctx);

	int rc = sg_sumbit_io(ctx, io);
	if (!rc)
		return aio_linux_submit_poll(pctx->handle_aio, io, io->slot_idx);
	return rc;
}

static int sg_aio_uring_poll_completions(io_bench_thr_ctx_t *ctx, int n)
{
	poll_uring(U_2_P(ctx)->handle_uring);
	return 0;
}

static int sg_aio_uring_queue_io(io_bench_thr_ctx_t *ctx, io_ctx_t *io)
{
	sg_thr_ctx_t *pctx =  U_2_P(ctx);

	int rc = sg_sumbit_io(ctx, io);
	if (!rc)
		return uring_submit_poll(pctx->handle_uring, io, io->slot_idx);
	return rc;
}
