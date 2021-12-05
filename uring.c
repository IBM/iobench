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
#include "uring.h"
#include <stdlib.h>
#include <unistd.h>
#include <sys/uio.h>
#include <linux/io_uring.h>
#include <sys/mman.h>
#include <sys/eventfd.h>
#include <sys/syscall.h>
#include <poll.h>
#include <errno.h>
#include <string.h>
#include <poll.h>

typedef struct {
	uint32_t *head;
	uint32_t *tail;
	uint32_t *ring_mask;
	uint32_t *flags;
} queue_ptrs_t;

typedef struct uring_handle
{
	int *fds;
	int (*requeue_io)(io_bench_thr_ctx_t *ctx, io_ctx_t *io);
	int fds_count;
	int uring_fd;
	int event_fd;
	bool memreg_done;
	io_bench_thr_ctx_t *thr_ctx;
	uint32_t poll_idle_user_ms;
	uint32_t *sq_indices;
	struct io_uring_sqe *sqes;
	struct io_uring_cqe *cqes;
	queue_ptrs_t sq_ptrs;
	queue_ptrs_t cq_ptrs;
	struct io_uring_params params;
} uring_handle_t;


static int io_uring_setup(uint32_t entries, struct io_uring_params *p)
{
	return syscall(__NR_io_uring_setup, entries, p);
}

static int io_uring_register(unsigned int fd, unsigned int opcode, void *arg, unsigned int nr_args)
{
	return syscall(__NR_io_uring_register, fd, opcode, arg, nr_args);
}

static int io_uring_enter(unsigned int fd, unsigned int to_submit,
				unsigned int min_complete, unsigned int flags, sigset_t *sig)
{
	int rc;
	rc = syscall(__NR_io_uring_enter, fd, to_submit, min_complete, flags, sig);
	return (rc > 0) ? 0 : rc;
}

int uring_handle_create(uring_params_t *params, uring_handle_t **phandle)
{
	uring_handle_t *handle;
	int rc = -ENOMEM;
	void *p;
	struct iovec iov;

	*phandle = NULL;
	handle = calloc(1, sizeof(uring_handle_t));
	if (!handle) {
		ERROR("Cannot alloc for handle");
		return -ENOMEM;
	}
	handle->requeue_io = params->requeue_io;
	handle->uring_fd = handle->event_fd = -1U;
	handle->fds = calloc(params->fd_count, sizeof(int));
	if (!handle->fds) {
		ERROR("Cannot alloc for file descriptors");
		goto cleanup;
	}
	handle->thr_ctx = params->thr_ctx;
	handle->poll_idle_user_ms = params->poll_idle_user_ms;
	memcpy(handle->fds, params->fds, params->fd_count * sizeof(int));
	handle->fds_count = params->fd_count;
	if (params->poll_idle_kernel_ms) {
		handle->params.sq_thread_idle = params->poll_idle_kernel_ms;
		handle->params.flags = IORING_SETUP_SQPOLL;
		if (params->poll_cpu != -1) {
			handle->params.sq_thread_cpu = params->poll_cpu;
			handle->params.flags |= IORING_SETUP_SQ_AFF;
		}
	}
	handle->event_fd = eventfd(0, EFD_NONBLOCK);
	if (handle->event_fd < 0) {
		rc = errno ? -errno : -EPERM;
		ERROR("Failed to create event FD");
		goto cleanup;
	}
	handle->uring_fd = io_uring_setup(params->queue_size, &handle->params);
	if (handle->uring_fd < 0) {
		rc = errno ? -errno : -EPERM;
		ERROR("Failed to create uring");
		goto cleanup;
	}

	iov.iov_base = params->mem;
	iov.iov_len = params->mem_size;

	if (io_uring_register(handle->uring_fd, IORING_REGISTER_BUFFERS, &iov, 1)) {
		static bool log_once = false;
		if (!log_once) {
			ERROR("Failed to register buffers");
			log_once = true;
		}
	} else {
		handle->memreg_done = true;
	}
	if (io_uring_register(handle->uring_fd, IORING_REGISTER_FILES, handle->fds, handle->fds_count)) {
		rc = errno ? -errno : -EPERM;
		ERROR("Failed to register files");
		goto cleanup;
	}
	if (io_uring_register(handle->uring_fd, IORING_REGISTER_EVENTFD, &handle->event_fd, 1)) {
		rc = errno ? -errno : -EPERM;
		ERROR("Failed to register evend FD");
		goto cleanup;
	}

	p =  mmap(NULL, handle->params.sq_off.array + handle->params.sq_entries * sizeof(uint32_t),
			  PROT_READ|PROT_WRITE, MAP_SHARED|MAP_POPULATE, handle->uring_fd, IORING_OFF_SQ_RING);
	if (p == MAP_FAILED) {
		rc = errno ? -errno : -EPERM;
		ERROR("Failed to map SQ indices");
		goto cleanup;
	}
	handle->sq_ptrs.head = p + handle->params.sq_off.head;
	handle->sq_ptrs.tail = p + handle->params.sq_off.tail;
	handle->sq_ptrs.ring_mask = p + handle->params.sq_off.ring_mask;
	handle->sq_ptrs.flags = p + handle->params.sq_off.flags;
	handle->sq_indices = p + handle->params.sq_off.array;

	p = mmap(NULL, handle->params.sq_entries * sizeof(struct io_uring_sqe), PROT_READ|PROT_WRITE,
			 MAP_SHARED|MAP_POPULATE, handle->uring_fd, IORING_OFF_SQES);
	if (p == MAP_FAILED) {
		rc = errno ? -errno : -EPERM;
		ERROR("Failed to map SQ ring");
		goto cleanup;
	}
	handle->sqes = p;

	p =  mmap(NULL, handle->params.cq_off.cqes + handle->params.cq_entries * sizeof(struct io_uring_cqe),
			  PROT_READ|PROT_WRITE, MAP_SHARED|MAP_POPULATE, handle->uring_fd, IORING_OFF_CQ_RING);
	if (p == MAP_FAILED) {
		rc = errno ? -errno : -EPERM;
		ERROR("Failed to map CQ ring");
		goto cleanup;
	}
	handle->cq_ptrs.head = p + handle->params.cq_off.head;
	handle->cq_ptrs.tail = p + handle->params.cq_off.tail;
	handle->cq_ptrs.ring_mask = p + handle->params.cq_off.ring_mask;
	handle->cq_ptrs.flags = NULL; //older kernels do not have it, we do not use it
	handle->cqes = p + handle->params.cq_off.cqes;

	*phandle = handle;
	return 0;
cleanup:
	uring_handle_destroy(handle);
	return rc;
}

void uring_handle_destroy(uring_handle_t *handle)
{
	if (!handle)
		return;
	if (handle->uring_fd >= 0)
		close(handle->uring_fd);
	if (handle->sq_indices)
		munmap(((void *)handle->sq_indices) - handle->params.sq_off.array,  handle->params.sq_off.array +  handle->params.sq_entries * sizeof(uint32_t));
	if (handle->sqes)
		munmap(handle->sqes, handle->params.sq_entries *  sizeof(struct io_uring_sqe));
	if (handle->cqes)
		munmap(((void *)handle->cqes) - handle->params.cq_off.cqes,  handle->params.cq_off.cqes + handle->params.cq_entries * sizeof(struct io_uring_cqe));
	if (handle->fds)
		free(handle->fds);
	if (handle->event_fd >= 0)
		close(handle->event_fd);
	free(handle);
}

static void inline handle_one_cqe(uring_handle_t *handle, uint32_t idx)
{
	io_ctx_t *io_ctx = (void *)handle->cqes[idx].user_data;
	io_ctx->status = handle->cqes[idx].res;
	if (io_ctx->status > 0)
		io_ctx->status = 0;
	handle->requeue_io(handle->thr_ctx, io_ctx);
	__sync_synchronize();
	(*handle->cq_ptrs.head)++;
}

static void clean_completions(uring_handle_t *handle)
{
	while (*handle->cq_ptrs.head < *handle->cq_ptrs.tail) {
		uint32_t head = (*handle->cq_ptrs.head) & (*handle->cq_ptrs.ring_mask);
		uint32_t tail = (*handle->cq_ptrs.tail) & (*handle->cq_ptrs.ring_mask);
		uint32_t i;
		if (head < tail) {
			for (i = head; i < tail; i++)
				handle_one_cqe(handle, i);
		} else {
			for (i = head; i < handle->params.cq_entries; i++)
				handle_one_cqe(handle, i);
			for (i = 0; i < tail; i++)
				handle_one_cqe(handle, i);
		}
	}
}

void poll_uring(uring_handle_t *handle)
{
	struct pollfd pfd = { .fd = handle->event_fd, .events = POLLIN | POLLPRI| POLLRDNORM | POLLRDBAND | POLLRDHUP};
	eventfd_t val;

	if (handle->poll_idle_user_ms) {
		uint64_t stop;
		uint64_t stamp;

		while (1) {
			stop = get_uptime_us() + handle->poll_idle_user_ms * 1000;
			stamp = 0;
			/* do not poll for more then poll_idle_user_ms */
			while ((*handle->cq_ptrs.head == *handle->cq_ptrs.tail) && stamp < stop)
				stamp = get_uptime_us();
			/* polling done, do we still have no completions? */
			if (unlikely(*handle->cq_ptrs.head == *handle->cq_ptrs.tail)) {
				/* clear the previous unpolled counter, if any */
				eventfd_read(handle->event_fd, &val);
				/* now sleep for next event, if needed */
				if (*handle->cq_ptrs.head == *handle->cq_ptrs.tail)
					poll(&pfd, 1, -1);
			}
			clean_completions(handle);
		}
	}
	poll(&pfd, 1, -1);
	eventfd_read(handle->event_fd, &val);
	clean_completions(handle);
}

static int submit_sqe(uring_handle_t *handle, uint32_t idx)
{
	uint32_t sq_tail;

	sq_tail = (*handle->sq_ptrs.tail) & (*handle->sq_ptrs.ring_mask);
	handle->sq_indices[sq_tail] = idx;

	__sync_synchronize();
	(*handle->sq_ptrs.tail)++;

	if (!handle->params.sq_thread_idle)
		return io_uring_enter(handle->uring_fd, 1, 0, 0, NULL);
	if (*handle->sq_ptrs.flags & IORING_SQ_NEED_WAKEUP)
		return io_uring_enter(handle->uring_fd, 0, 0, IORING_ENTER_SQ_WAKEUP, NULL);
	return 0;
}

int uring_submit_io(uring_handle_t *handle, io_ctx_t *ioctx, uint32_t size)
{
	uint32_t idx = ioctx->slot_idx;
	struct io_uring_sqe *sqe = handle->sqes + idx;
	struct iovec iov;

	memset(sqe, 0, sizeof(*sqe));
	if (unlikely(!handle->memreg_done)) {
		sqe->opcode = (!ioctx->write) ?  IORING_OP_READV : IORING_OP_WRITEV;
		iov.iov_base = ioctx->buf;
		iov.iov_len = size;
		sqe->addr = (uint64_t)&iov;
		sqe->len = 1;
	} else {
		sqe->opcode = (!ioctx->write) ?  IORING_OP_READ_FIXED : IORING_OP_WRITE_FIXED;
		sqe->addr = (uint64_t)ioctx->buf;
		sqe->len = size;
	}
	sqe->flags = IOSQE_FIXED_FILE;
	sqe->fd = ioctx->dev_idx;
	sqe->off = ioctx->offset;
	sqe->user_data = (uint64_t)ioctx;
	return submit_sqe(handle, idx);
}

int uring_submit_poll(uring_handle_t *handle, io_ctx_t *ioctx, uint32_t fd_idx)
{
	uint32_t idx = ioctx->slot_idx;
	struct io_uring_sqe *sqe = handle->sqes + idx;

	memset(sqe, 0, sizeof(*sqe));
	sqe->opcode = IORING_OP_POLL_ADD;
	sqe->poll_events = POLLIN;
	sqe->flags = IOSQE_FIXED_FILE;
	sqe->fd = fd_idx;
	sqe->off = ioctx->offset;
	sqe->user_data = (uint64_t)ioctx;
	return submit_sqe(handle, idx);
}
