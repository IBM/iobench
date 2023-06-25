/* Copyright 2021 IBM Corporation
 *
 * Author: Constantine Gavrilov
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

#define _GNU_SOURCE
#include "logger.h"
DECLARE_BFN
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


static void aio_block_destroy_thread_ctx(io_bench_thr_ctx_t *ctx);
static int aio_block_init_thread_ctx(io_bench_thr_ctx_t **pctx, io_bench_params_t *params, void *buf_head, unsigned int dev_idx, unsigned int cpu);
static int aio_block_aio_linux_poll_completions(io_bench_thr_ctx_t *ctx, int n);
static io_ctx_t *aio_block_get_io_ctx(io_bench_thr_ctx_t *ctx, uint16_t slot);
static int aio_block_aio_linux_queue_io(io_bench_thr_ctx_t *ctx, io_ctx_t *io);

static int aio_block_aio_uring_poll_completions(io_bench_thr_ctx_t *ctx, int n);
static int aio_block_aio_uring_queue_io(io_bench_thr_ctx_t *ctx, io_ctx_t *io);

io_eng_def_t aio_linux_engine = {
	.init_thread_ctx = &aio_block_init_thread_ctx,
	.destroy_thread_ctx = &aio_block_destroy_thread_ctx,
	.poll_completions = &aio_block_aio_linux_poll_completions,
	.get_io_ctx = &aio_block_get_io_ctx,
	.queue_io = &aio_block_aio_linux_queue_io,
};

io_eng_def_t aio_uring_engine = {
	.init_thread_ctx = &aio_block_init_thread_ctx,
	.destroy_thread_ctx = &aio_block_destroy_thread_ctx,
	.poll_completions = &aio_block_aio_uring_poll_completions,
	.get_io_ctx = &aio_block_get_io_ctx,
	.queue_io = &aio_block_aio_uring_queue_io,
};

typedef struct {
	union {
		aio_linux_handle_t *handle_aio;
		uring_handle_t *handle_uring;
	};
	io_ctx_t *ioctx;
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
		};
	};
	io_bench_thr_ctx_t iobench_ctx;
	io_eng_t engine;
} aio_block_thr_ctx_t;

#define U_2_P(_u_h_) \
({ \
	force_type((_u_h_), io_bench_thr_ctx_t *); \
	aio_block_thr_ctx_t *__res__ = list_parent_struct(_u_h_, aio_block_thr_ctx_t, iobench_ctx); \
	__res__; \
})


static void aio_block_destroy_thread_ctx(io_bench_thr_ctx_t *ctx)
{
	aio_block_thr_ctx_t *pctx = U_2_P(ctx);
	if (pctx->ioctx)
		free(pctx->ioctx);
	if (!pctx->rr && pctx->fd != -1)
		close(pctx->fd);
	if (pctx->rr && pctx->fds && !pctx->slot)
		free((void *)pctx->fds);
	if (pctx->engine == ENGINE_AIO_LINUX) {
		if (pctx->handle_aio)
			aio_linux_handle_destroy(pctx->handle_aio);
	} else {
		if (pctx->handle_uring)
			uring_handle_destroy(pctx->handle_uring);
	}
}

static int aio_block_init_thread_ctx(io_bench_thr_ctx_t **pctx, io_bench_params_t *params, void *buf_head, unsigned int dev_idx, unsigned int poll_cpu)
{
	aio_block_thr_ctx_t *aio_block_thr_ctx;
	unsigned int i;
	static int *fds = NULL;

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
	aio_block_thr_ctx = calloc(1, sizeof(*aio_block_thr_ctx));
	if (!aio_block_thr_ctx) {
		ERROR("Failed to alloc");
		return -ENOMEM;
	}

	aio_block_thr_ctx->engine = params->engine;
	aio_block_thr_ctx->qs = params->qs;
	aio_block_thr_ctx->bs = params->bs;
	aio_block_thr_ctx->slot = dev_idx;
	aio_block_thr_ctx->rr = params->rr;
	if (!params->rr)
		aio_block_thr_ctx->fd = -1;

	aio_block_thr_ctx->ioctx = calloc(params->qs, sizeof(*aio_block_thr_ctx->ioctx));
	if (!aio_block_thr_ctx->ioctx) {
		ERROR("Failed to alloc");
		aio_block_destroy_thread_ctx(&aio_block_thr_ctx->iobench_ctx);
		return -ENOMEM;
	}
	if (!params->rr) {
		aio_block_thr_ctx->fd = open(params->devices[dev_idx], O_RDWR|O_DIRECT);
		if (aio_block_thr_ctx->fd < 0) {
			ERROR("Failed to open %s", params->devices[dev_idx]);
			aio_block_destroy_thread_ctx(&aio_block_thr_ctx->iobench_ctx);
			return -ENOMEM;
		}
	} else {
		aio_block_thr_ctx->fds = fds;
	}
	if ((params->engine == ENGINE_AIO_LINUX)) {
		if (aio_linux_handle_create(
			&(aio_linux_params_t) {
				.thr_ctx = &aio_block_thr_ctx->iobench_ctx,
				.target_usec_latency = 1000000.0 / ((params->kiops * 1000)/(params->threads *params->qs)),
				.requeue_io = io_bench_requeue_io,
				.get_io_ctx = aio_block_get_io_ctx,
				.queue_size = params->qs,
				.fds = (params->rr) ? aio_block_thr_ctx->fds : &aio_block_thr_ctx->fd,
				.fd_count = (params->rr) ? params->ndevs : 1,
			},
			&aio_block_thr_ctx->handle_aio)) {
			ERROR("Failed to init aio handle");
			aio_block_destroy_thread_ctx(&aio_block_thr_ctx->iobench_ctx);
			return -ENOMEM;
		}
	} else if (uring_handle_create(
		&(uring_params_t) {
				.thr_ctx = &aio_block_thr_ctx->iobench_ctx,
				.requeue_io = io_bench_requeue_io,
				.get_io_ctx = aio_block_get_io_ctx,
				.target_usec_latency = 1000000.0 / ((params->kiops * 1000)/(params->threads *params->qs)),
				.mem = buf_head,
				.mem_size = params->bs * params->qs,
				.queue_size = params->qs,
				.fds = (params->rr) ? aio_block_thr_ctx->fds : &aio_block_thr_ctx->fd,
				.fd_count = (params->rr) ? params->ndevs : 1,
				.poll_idle_kernel_ms = params->poll_idle_kernel_ms,
				.poll_idle_user_ms = params->poll_idle_user_ms,
				.poll_cpu = poll_cpu,
			},
			&aio_block_thr_ctx->handle_uring)) {
		ERROR("Failed to init uring handle");
		aio_block_destroy_thread_ctx(&aio_block_thr_ctx->iobench_ctx);
		return -ENOMEM;
	}
	*pctx = &aio_block_thr_ctx->iobench_ctx;
	return 0;
}

static int aio_block_aio_linux_poll_completions(io_bench_thr_ctx_t *ctx, int n)
{
	return aio_linux_poll(U_2_P(ctx)->handle_aio, n);
}

static io_ctx_t *aio_block_get_io_ctx(io_bench_thr_ctx_t *ctx, uint16_t slot)
{
	aio_block_thr_ctx_t *pctx = U_2_P(ctx);
	if (pctx->qs <= slot)
		return NULL;
	return &pctx->ioctx[slot];
}

static int aio_block_aio_linux_queue_io(io_bench_thr_ctx_t *ctx, io_ctx_t *io)
{
	if (!U_2_P(ctx)->rr)
		io->dev_idx = 0;
	return aio_linux_submit_io(U_2_P(ctx)->handle_aio, io, U_2_P(ctx)->bs);
}

static int aio_block_aio_uring_poll_completions(io_bench_thr_ctx_t *ctx, int n)
{
	poll_uring(U_2_P(ctx)->handle_uring);
	return 0;
}

static int aio_block_aio_uring_queue_io(io_bench_thr_ctx_t *ctx, io_ctx_t *io)
{
	if (!U_2_P(ctx)->rr)
		io->dev_idx = 0;
	return uring_submit_io(U_2_P(ctx)->handle_uring, io, U_2_P(ctx)->bs);
}
