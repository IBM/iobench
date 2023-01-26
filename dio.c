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
#include "core_affinity.h"
#include <pthread.h>
#include <sys/mman.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <stdlib.h>
#include <unistd.h>
#include <signal.h>
#include <errno.h>
#include <sys/ioctl.h>
#include <linux/nvme_ioctl.h>
#include <limits.h>


static void dio_destroy_thread_ctx(io_bench_thr_ctx_t *ctx);
static int dio_init_thread_ctx(io_bench_thr_ctx_t **pctx, io_bench_params_t *params, void *buf_head, unsigned int dev_idx, unsigned int cpu);
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

io_eng_def_t nvme_engine = {
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
	int cpu;
	bool may_run;
} dio_ioctx_t;

typedef struct {
	dio_ioctx_t *ioctx;
	pthread_t self;
	int *fds;
	int lba_size_bits;
	uint32_t bs;
	uint32_t qs;
	uint32_t ndevs;
	bool rr;
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
		if (pctx->ioctx[i].fd >= 0 && !pctx->rr)
			close(pctx->ioctx[i].fd);
	}
}

static void dio_destroy_thread_ctx(io_bench_thr_ctx_t *ctx)
{
	dio_thr_ctx_t *pctx = U_2_P(ctx);

	kill_all_dio_threads(ctx);

	if (pctx->ioctx)
		free(pctx->ioctx);
	if (pctx->rr && pctx->fds) {
		uint32_t i;
		for (i = 0; i < pctx->ndevs; i++) {
			if (pctx->fds[i] >= 0)
				close(pctx->fds[i]);
		}
	}
	free(pctx->fds);
	pctx->fds = NULL;
}

static void term_handler(int signo)
{
	pthread_exit(NULL);
}

#define SET_ERR(__rc__) ((__rc__ < 0) ? (errno ? -errno : -1) : -ENODATA)

static void *thread_func_dio(void *arg)
{
	dio_ioctx_t *ctx = arg;
	io_ctx_t *ioctx = &ctx->ioctx;
	io_bench_thr_ctx_t *thr_ctx = ctx->parent;
	dio_thr_ctx_t *pctx = U_2_P(thr_ctx);
	uint32_t bs = U_2_P(thr_ctx)->bs;
	int rc;

	if (ctx->cpu != -1)
		set_thread_affinity(ctx->cpu);
	pthread_mutex_lock(&ctx->run_mutex);
	while (!ctx->may_run)
		pthread_cond_wait(&ctx->run_cond, &ctx->run_mutex);
	pthread_mutex_unlock(&ctx->run_mutex);

	while (1) {
		int fd = (pctx->rr) ? pctx->fds[ioctx->dev_idx] : ctx->fd;
		rc = (!ioctx->write) ? pread(fd, ioctx->buf, bs, ioctx->offset) : pwrite(fd, ioctx->buf, bs, ioctx->offset);
		ioctx->status = (rc == bs) ? 0 : SET_ERR(rc);
		io_bench_complete_and_prep_io(thr_ctx, ioctx);
	}
	return NULL;
}

#define NVME_CMD_READ (2)
#define NVME_CMD_WRITE (1)

static void *thread_func_nvme(void *arg)
{
	dio_ioctx_t *ctx = arg;
	io_ctx_t *ioctx = &ctx->ioctx;
	io_bench_thr_ctx_t *thr_ctx = ctx->parent;
	dio_thr_ctx_t *pctx = U_2_P(thr_ctx);
	uint32_t bs =(U_2_P(thr_ctx)->bs >> U_2_P(thr_ctx)->lba_size_bits) - 1;
	int rc;

	if (ctx->cpu != -1)
		set_thread_affinity(ctx->cpu);
	pthread_mutex_lock(&ctx->run_mutex);
	while (!ctx->may_run)
		pthread_cond_wait(&ctx->run_cond, &ctx->run_mutex);
	pthread_mutex_unlock(&ctx->run_mutex);

	while (1) {
		int fd = (pctx->rr) ? pctx->fds[ioctx->dev_idx] : ctx->fd;
		struct nvme_user_io nvme_io = {
			.opcode = !ioctx->write ? NVME_CMD_READ : NVME_CMD_WRITE,
			.nblocks = bs,
			.addr = (uint64_t)ioctx->buf,
			.slba = ioctx->offset >> U_2_P(thr_ctx)->lba_size_bits,
		};
		rc = ioctl(fd, NVME_IOCTL_SUBMIT_IO, &nvme_io);
		ioctx->status = (rc == 0) ? 0 : SET_ERR(rc);
		io_bench_complete_and_prep_io(thr_ctx, ioctx);
	}
	return NULL;
}

static int get_lba_size_bits(const char *name)
{
	const char *p;
	int rc;
	int fd;
	char path[PATH_MAX];
	char buf[16];
	int i;

	p = strrchr(name, '/');
	p = (p) ? p+1 : name;

	snprintf(path, sizeof(path), "/sys/block/%s/queue/minimum_io_size", p);
	fd = open(path, O_RDONLY);
	if (fd < 0) {
		rc = errno ? -errno : -1;
		ERROR("Failed to open %s", path);
		return rc;
	}

	rc = read(fd, buf, sizeof(buf)-1);
	if (rc <= 0) {
		rc = rc ? (errno ? -errno : -1) : -EINVAL;
		ERROR("Failed to read %s", path);
	}
	close(fd);
	if (rc < 0)
		return rc;

	buf[rc] = '\0';
	if (sscanf(buf, "%d", &rc) != 1) {
		ERROR("Failed to parse %s", path);
	}

	for (i = 1; i < 32; i++) {
		if ((1 << i) == rc)
			return i;
	}
	ERROR("Unsupported LBA size of %d", rc);
	return -EINVAL;
}

static int dio_init_thread_ctx(io_bench_thr_ctx_t **pctx, io_bench_params_t *params, void *buf_head, unsigned int dev_idx, unsigned int cpu)
{
	dio_thr_ctx_t *dio_thr_ctx;
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
	dio_thr_ctx->rr = params->rr;
	dio_thr_ctx->ndevs = params->ndevs;

	if (params->rr) {
		dio_thr_ctx->fds = calloc(sizeof(int), params->ndevs);
		if (dio_thr_ctx->fds) {
			memset(dio_thr_ctx->fds, 0xff, sizeof(int) * params->ndevs);
		} else {
			ERROR("Failed to map device FD array");
			dio_destroy_thread_ctx(&dio_thr_ctx->iobench_ctx);
			return -ENOMEM;
		}
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
		dio_thr_ctx->ioctx[i].fd = -1;
		dio_thr_ctx->ioctx[i].run_mutex = (pthread_mutex_t)PTHREAD_MUTEX_INITIALIZER;
		dio_thr_ctx->ioctx[i].run_cond = (pthread_cond_t)PTHREAD_COND_INITIALIZER;
		dio_thr_ctx->ioctx[i].parent = &dio_thr_ctx->iobench_ctx;
		dio_thr_ctx->ioctx[i].cpu = -1U;
		if (params->cpuset) {
			dio_thr_ctx->ioctx[i].cpu = get_next_cpu_from_set(params->cpuset);
		} else if (params->remap_numa || params->use_numa) {
			unsigned int numa = get_numa_id_of_block_device(params->devices[dev_idx]);
			if (params->remap_numa && numa != -1U)
				dio_thr_ctx->ioctx[i].cpu = get_next_remapped_numa_cpu(params->remap_numa, numa);
			else
				dio_thr_ctx->ioctx[i].cpu = get_next_numa_rr_cpu(numa);
		}
	}
	if (!params->rr) {
		for (i = 0; i < dio_thr_ctx->qs; i++) {
			dio_thr_ctx->ioctx[i].fd = open(params->devices[dev_idx], O_RDWR|O_DIRECT);
			if (dio_thr_ctx->ioctx[i].fd < 0) {
				ERROR("Failed to open %s", params->devices[dev_idx]);
				dio_destroy_thread_ctx(&dio_thr_ctx->iobench_ctx);
				return -ENOMEM;
			}
		}
	} else {
		for (i = 0; i < params->ndevs; i++) {
			dio_thr_ctx->fds[i] = open(params->devices[i], O_RDWR|O_DIRECT);
			if (dio_thr_ctx->fds[i] < 0) {
				ERROR("Failed to open %s", params->devices[i]);
				dio_destroy_thread_ctx(&dio_thr_ctx->iobench_ctx);
				return -ENOMEM;
			}
		}
		dio_thr_ctx->ioctx[0].fd = dio_thr_ctx->fds[dev_idx];
	}

	dio_thr_ctx->lba_size_bits = get_lba_size_bits(params->devices[dev_idx]);
	if (dio_thr_ctx->lba_size_bits < 0) {
		int rc = dio_thr_ctx->lba_size_bits;
		dio_destroy_thread_ctx(&dio_thr_ctx->iobench_ctx);
		return rc;
	}
	if ((params->engine != ENGINE_DIO) && (params->bs % (1 << dio_thr_ctx->lba_size_bits))) {
		ERROR("Block size must be a multiple of sector size %d", 1 << dio_thr_ctx->lba_size_bits);
		dio_destroy_thread_ctx(&dio_thr_ctx->iobench_ctx);
		return -EINVAL;
	}

	act.sa_handler = term_handler;
	if (sigaction(SIGUSR1, &act, NULL)) {
		ERROR("Failed to setup SIGTERM handler");
		dio_destroy_thread_ctx(&dio_thr_ctx->iobench_ctx);
		return -EPERM;
	}

	for (i = 0; i < dio_thr_ctx->qs; i++) {
		int rc = pthread_create(&dio_thr_ctx->ioctx[i].tid, NULL, params->engine == ENGINE_DIO ? thread_func_dio : thread_func_nvme, &dio_thr_ctx->ioctx[i]);
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
