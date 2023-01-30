/*
 * <copyright-info>
 * IBM Confidential
 * OCO Source Materials
 * 2810
 * Author: Constantine Gavrilov <constg@il.ibm.com>
 * (C) Copyright IBM Corp. 2023
 * The source code for this program is not published or otherwise
 * divested of its trade secrets, irrespective of what has
 * been deposited with the U.S. Copyright Office
 * </copyright-info>
 */

#ifndef _IO_TIMER_H_
#define _IO_TIMER_H_

#include "iobench.h"
#include <stdint.h>
#include <stdbool.h>

typedef struct {
	io_bench_thr_ctx_t *thr_ctx;
	int (*requeue_io)(io_bench_thr_ctx_t *ctx, io_ctx_t *io);
	io_ctx_t *(*get_io_ctx)(io_bench_thr_ctx_t *ctx, uint16_t slot);
	uint64_t target_usec_latency;
	uint16_t qs;
} timer_params_t;

typedef struct io_bench_timer io_bench_timer_t;

int create_io_bench_timer(io_bench_timer_t **phandle, timer_params_t *params);
void destroy_io_bench_timer(io_bench_timer_t *timer);
int io_bench_wait_or_requeue_io(io_bench_timer_t *handle, io_ctx_t *io_ctx);
int io_bench_poll_timers(io_bench_timer_t *handle, uint64_t *next_poll_to);

#endif
