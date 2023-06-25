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
