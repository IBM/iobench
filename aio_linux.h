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

#ifndef _IO_AIO_LINUX_H_
#define _IO_AIO_LINUX_H_

#include <stdbool.h>
#include "iobench.h"

typedef struct {
	io_bench_thr_ctx_t *thr_ctx;
	int (*requeue_io)(io_bench_thr_ctx_t *ctx, io_ctx_t *io);
	io_ctx_t *(*get_io_ctx)(io_bench_thr_ctx_t *ctx, uint16_t slot);
	uint64_t target_usec_latency;
	int queue_size;
	const int *fds;
	unsigned int fd_count;
} aio_linux_params_t;

typedef struct aio_linux_handle aio_linux_handle_t;

int aio_linux_handle_create(aio_linux_params_t *params, aio_linux_handle_t **phandle);
void aio_linux_handle_destroy(aio_linux_handle_t *handle);
int aio_linux_poll(aio_linux_handle_t *handle, int n);
int aio_linux_submit_io(aio_linux_handle_t *handle, io_ctx_t *ioctx, uint32_t size);
int aio_linux_submit_poll(aio_linux_handle_t *handle, io_ctx_t *ioctx, int fd_idx);

#endif

