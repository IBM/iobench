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

#ifndef _IO_BENCH_URING_H_
#define _IO_BENCH_URING_H_

#include <stdbool.h>
#include "iobench.h"

typedef struct {
	io_bench_thr_ctx_t *thr_ctx;
	int (*requeue_io)(io_bench_thr_ctx_t *ctx, io_ctx_t *io);
	io_ctx_t *(*get_io_ctx)(io_bench_thr_ctx_t *ctx, uint16_t slot);
	uint64_t target_usec_latency;
	void *mem;
	size_t mem_size;
	int queue_size;
	const int *fds;
	unsigned int fd_count;
	uint32_t poll_idle_kernel_ms;
	uint32_t poll_idle_user_ms;
	int poll_cpu;
} uring_params_t;

typedef struct uring_handle uring_handle_t;

int uring_handle_create(uring_params_t *params, uring_handle_t **phandle);
void uring_handle_destroy(uring_handle_t *handle);
void poll_uring(uring_handle_t *handle);
int uring_submit_io(uring_handle_t *handle, io_ctx_t *ioctx, uint32_t size);
int uring_submit_poll(uring_handle_t *handle, io_ctx_t *ioctx, uint32_t fd_idx);

#endif
