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

