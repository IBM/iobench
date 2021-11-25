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
#include "aio_linux.h"
DECLARE_BFN
#include <libaio.h>
#include <stdlib.h>
#include <errno.h>


typedef struct aio_linux_handle {
	io_bench_thr_ctx_t *thr_ctx;
	io_context_t handle;
	struct iocb *iocbs;
	uint32_t qs;
	const int *fds;
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
	handle->thr_ctx = params->thr_ctx;
	handle->fds = params->fds;
	handle->fd_count = params->fd_count;
	handle->qs = params->queue_size;
	handle->iocbs = calloc(params->queue_size, sizeof(*handle->iocbs));
	if (!handle->iocbs) {
		ERROR("Failed to alloc iocbs");
		free(handle);
		return -ENOMEM;
	}
	if (io_setup(handle->qs, &handle->handle)) {
		ERROR("Failed to init aio handle");
		free(handle->iocbs);
		free(handle);
		return -ENOMEM;
	}
	*phandle = handle;
	return 0;
}

void aio_linux_handle_destroy(aio_linux_handle_t *handle)
{
	free(handle->iocbs);
	free(handle);
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
			io_bench_requeue_io(handle->thr_ctx, io);
		}
		rc = 0;
	}
	return rc;
}

int aio_linux_submit_io(aio_linux_handle_t *handle, io_ctx_t *io, uint32_t size)
{
	struct iocb *iocb = &handle->iocbs[io->slot_idx];
	struct iocb *io_list[1] = { iocb };
	int rc;
	int fd;


	fd = handle->fds[io->dev_idx];
	if (!io->write)
		io_prep_pread(iocb, fd, io->buf, size, io->offset);
	else
		io_prep_pwrite(iocb, fd, io->buf, size, io->offset);

	rc = io_submit(handle->handle, 1, io_list);
	return (rc > 0) ? 0 : (rc < 0) ? rc : -1;
}

