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

#include "logger.h"
DECLARE_BFN
#include "io_timer.h"
#include <stdlib.h>
#include <errno.h>
#include <assert.h>

typedef struct io_bench_timer {
	io_bench_thr_ctx_t *thr_ctx;
	int (*requeue_io)(io_bench_thr_ctx_t *ctx, io_ctx_t *io);
	io_ctx_t *(*get_io_ctx)(io_bench_thr_ctx_t *ctx, uint16_t slot);
	io_ctx_t **io_ctx;
	uint16_t qs;
	uint32_t next_poll_index;
	uint64_t target_usec_latency;
	uint64_t next_poll_usec_stamp;
	uint64_t *poll_stamps;
} io_bench_timer_t;

int create_io_bench_timer(io_bench_timer_t **phandle, timer_params_t *params)
{
	io_bench_timer_t *handle;
	uint16_t i;

	*phandle = NULL;
	handle = calloc(1, sizeof(*handle));
	if (handle) {
		handle->poll_stamps = calloc(params->qs, sizeof(handle->poll_stamps[0]));
		handle->io_ctx = calloc(params->qs, sizeof(handle->io_ctx[0]));
	}
	if (!handle || !handle->poll_stamps || !handle->io_ctx) {
		ERROR("Failed to allocate timer handle");
		if (handle->io_ctx)
			free(handle->io_ctx);
		if (handle->poll_stamps)
			free(handle->poll_stamps);
		if (handle)
			free(handle);
		errno = ENOMEM;
		return -1;
	}

	handle->thr_ctx = params->thr_ctx;
	handle->requeue_io = params->requeue_io;
	handle->get_io_ctx = params->get_io_ctx;
	for (i = 0; i < params->qs; i++)
		handle->io_ctx[i] = params->get_io_ctx(params->thr_ctx, i);
	handle->qs = params->qs;
	handle->target_usec_latency = params->target_usec_latency;
	handle->next_poll_usec_stamp = -1LU;

	*phandle = handle;
	return 0;
}

void destroy_io_bench_timer(io_bench_timer_t *timer)
{
	free(timer->io_ctx);
	free(timer->poll_stamps);
	free(timer);
}

int io_bench_wait_or_requeue_io(io_bench_timer_t *handle, io_ctx_t *io_ctx)
{
	assert(io_ctx->slot_idx < handle->qs && io_ctx == handle->io_ctx[io_ctx->slot_idx]);
	uint64_t now = get_uptime_us();
	uint64_t delta = now - io_ctx->start_stamp;

	if (delta >= handle->target_usec_latency) {
		io_ctx->slack += (delta - handle->target_usec_latency);
		handle->poll_stamps[io_ctx->slot_idx] = 0;
		return handle->requeue_io(handle->thr_ctx, io_ctx);
	}
	delta = handle->target_usec_latency - delta;
	if (delta >= io_ctx->slack) {
		delta -= io_ctx->slack;
		delta += now;
		io_ctx->slack = 0;
		handle->poll_stamps[io_ctx->slot_idx] = delta;
		if (delta < handle->next_poll_usec_stamp) {
			handle->next_poll_usec_stamp = delta;
			handle->next_poll_index = io_ctx->slot_idx;
		}
	} else {
		io_ctx->slack -= delta;
		handle->poll_stamps[io_ctx->slot_idx] = 0;
		return handle->requeue_io(handle->thr_ctx, io_ctx);
	}
	return 0;
}

int io_bench_poll_timers(io_bench_timer_t *handle, uint64_t *next_poll_to)
{
	uint64_t now = get_uptime_us();
	uint32_t i;
	int rc = 0;

	if (handle->next_poll_usec_stamp > now) {
		*next_poll_to = (handle->next_poll_usec_stamp != -1LU) ?  handle->next_poll_usec_stamp - now : -1LU;
		return 0;
	}

	handle->next_poll_usec_stamp = -1LU;
	for (i = 0; i < handle->qs; i++) {
		if (handle->poll_stamps[i]) {
			now = get_uptime_us();
			if (now >= handle->poll_stamps[i]) {
				handle->io_ctx[i]->slack += (now - handle->poll_stamps[i]);
				handle->poll_stamps[i] = 0;
				int rc1 = handle->requeue_io(handle->thr_ctx, handle->io_ctx[i]);
				if (!rc)
					rc = rc1;
			} else {
				 if (handle->next_poll_usec_stamp > handle->poll_stamps[i]) {
					 handle->next_poll_usec_stamp = handle->poll_stamps[i];
					 handle->next_poll_index = i;
				 }
			}
		}
	}

	now = get_uptime_us();
	*next_poll_to = (handle->next_poll_usec_stamp != -1LU) ?  ((now < handle->next_poll_usec_stamp) ? handle->next_poll_usec_stamp - now : 0) : -1LU;
	return rc;
}
