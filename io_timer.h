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
