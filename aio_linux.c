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
#include "aio_linux.h"
#include "io_timer.h"
#include <poll.h>
#include <libaio.h>
#include <stdlib.h>
#include <errno.h>


typedef struct aio_linux_handle {
	io_bench_thr_ctx_t *thr_ctx;
	int (*requeue_io)(io_bench_thr_ctx_t *ctx, io_ctx_t *io);
	io_context_t handle;
	struct iocb *iocbs;
	io_bench_timer_t *timer_handle;
	uint32_t qs;
	int *fds;
	unsigned int fd_count;
} aio_linux_handle_t;


int aio_linux_handle_create(aio_linux_params_t *params, aio_linux_handle_t **phandle)
{
	aio_linux_handle_t *handle = calloc(1, sizeof(*handle));

	*phandle = NULL;
	if (!handle) {
		ERROR("Failed to alloc for aio_linux handle");
		return -ENOMEM;
	}
	if (params->target_usec_latency) {
		if (create_io_bench_timer(&handle->timer_handle, &(timer_params_t) {
			.get_io_ctx = params->get_io_ctx,
			.qs = params->queue_size,
			.requeue_io = params->requeue_io,
			.target_usec_latency = params->target_usec_latency,
			.thr_ctx = params->thr_ctx,
		})) {
			ERROR("Failed to create timer handle");
			free(handle);
			return -ENOMEM;
		}
	}
	handle->fds = calloc(params->fd_count, sizeof(*handle->fds));
	if (!handle->fds) {
		ERROR("Failed to alloc for file descriptors");
		if (params->target_usec_latency)
			destroy_io_bench_timer(handle->timer_handle);
		free(handle);
		return -ENOMEM;
	}
	memcpy(handle->fds, params->fds, sizeof(*params->fds) * params->fd_count);
	handle->thr_ctx = params->thr_ctx;
	handle->requeue_io = params->requeue_io;
	handle->fd_count = params->fd_count;
	handle->qs = params->queue_size;
	handle->iocbs = calloc(params->queue_size, sizeof(*handle->iocbs));
	if (!handle->iocbs) {
		ERROR("Failed to alloc iocbs");
		free(handle->fds);
		if (params->target_usec_latency)
			destroy_io_bench_timer(handle->timer_handle);
		free(handle);
		return -ENOMEM;
	}
	if (io_setup(handle->qs, &handle->handle)) {
		ERROR("Failed to init aio handle");
		free(handle->fds);
		free(handle->iocbs);
		if (params->target_usec_latency)
			destroy_io_bench_timer(handle->timer_handle);
		free(handle);
		return -ENOMEM;
	}
	*phandle = handle;
	return 0;
}

void aio_linux_handle_destroy(aio_linux_handle_t *handle)
{
	if (handle) {
		free(handle->iocbs);
		free(handle->fds);
		if (handle->timer_handle)
			destroy_io_bench_timer(handle->timer_handle);
		free(handle);
	}
}

int aio_linux_poll(aio_linux_handle_t *handle, int n)
{
	int rc = 0;
	struct io_event events[n];
	uint64_t next_poll_usec = -1UL;

	if (handle->timer_handle) {
		rc = io_bench_poll_timers(handle->timer_handle, &next_poll_usec);
		if (rc)
			return rc;
	}
	if (next_poll_usec == -1UL) {
		rc = io_getevents(handle->handle, 1, n, events, NULL);
	} else if (next_poll_usec) {
		rc = io_getevents(handle->handle, 1, n, events, &(struct timespec) {
			.tv_sec = next_poll_usec / 1000000,
			.tv_nsec = (next_poll_usec % 1000000) * 1000,
		});
	}
	if (rc > 0) {
		int i;
		for (i = 0; i < rc; i++) {
			io_ctx_t *io = events[i].data;
			io->status = (events[i].res == events[i].obj->u.c.nbytes) ?  events[i].res2 :  events[i].res;
			update_io_stats(handle->thr_ctx, io, get_uptime_us());

			if (!handle->timer_handle)
				handle->requeue_io(handle->thr_ctx, io);
			else
				io_bench_wait_or_requeue_io(handle->timer_handle, io);
		}
		rc = 0;
	}
	return rc;
}

int aio_linux_submit_poll(aio_linux_handle_t *handle, io_ctx_t *ioctx, int fd_idx)
{
	struct iocb *iocb = &handle->iocbs[ioctx->slot_idx];
	struct iocb *io_list[1] = { iocb };
	int rc;
	int fd;


	fd = handle->fds[fd_idx];
	io_prep_poll(iocb, fd, POLLIN);
	iocb->data = ioctx;
	rc = io_submit(handle->handle, 1, io_list);
	return (rc > 0) ? 0 : (rc < 0) ? rc : -1;
}

int aio_linux_submit_io(aio_linux_handle_t *handle, io_ctx_t *ioctx, uint32_t size)
{
	struct iocb *iocb = &handle->iocbs[ioctx->slot_idx];
	struct iocb *io_list[1] = { iocb };
	int rc;
	int fd;


	fd = handle->fds[ioctx->dev_idx];
	if (!ioctx->write)
		io_prep_pread(iocb, fd, ioctx->buf, size, ioctx->offset);
	else
		io_prep_pwrite(iocb, fd, ioctx->buf, size, ioctx->offset);
	iocb->data = ioctx;
	rc = io_submit(handle->handle, 1, io_list);
	return (rc > 0) ? 0 : (rc < 0) ? rc : -1;
}

