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
#include "aio_linux.h"
#include <poll.h>
#include <libaio.h>
#include <stdlib.h>
#include <errno.h>


typedef struct aio_linux_handle {
	io_bench_thr_ctx_t *thr_ctx;
	int (*requeue_io)(io_bench_thr_ctx_t *ctx, io_ctx_t *io);
	io_context_t handle;
	struct iocb *iocbs;
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
	handle->fds = calloc(params->fd_count, sizeof(*handle->fds));
	if (!handle->fds) {
		ERROR("Failed to alloc for file descriptors");
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
		free(handle);
		return -ENOMEM;
	}
	if (io_setup(handle->qs, &handle->handle)) {
		ERROR("Failed to init aio handle");
		free(handle->fds);
		free(handle->iocbs);
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
		free(handle);
	}
}

int aio_linux_poll(aio_linux_handle_t *handle, int n)
{
	int rc;
	struct io_event events[n];

	rc = io_getevents(handle->handle, 1, n, events, NULL);
	if (rc > 0) {
		int i;
		for (i = 0; i < rc; i++) {
			io_ctx_t *io = events[i].data;
			io->status = (events[i].res == events[i].obj->u.c.nbytes) ?  events[i].res2 :  events[i].res;
			handle->requeue_io(handle->thr_ctx, io);
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

