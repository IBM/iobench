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

#ifndef _IO_BENCH_
#define _IO_BENCH_

#include <stdint.h>
#include <stdbool.h>

typedef enum {
	ENGINE_AIO,
	ENGINE_AIO_LINUX,
	ENGINE_DIO,
	ENGINE_SCSI,
	ENGINE_NVNE,
	ENGINE_INVALID,
} io_eng_t;

typedef struct {
	uint64_t min_lat;
	uint64_t max_lat;
	uint64_t lat;
	uint64_t iops;
} io_bench_stats_t;

typedef struct {
	uint64_t hit_size;
	uint32_t run_time;
	uint32_t bs;
	uint16_t qs;
	bool fail_on_err;
	bool seq;
	bool rr;
	uint8_t wp;
	io_eng_t engine;
	char **devices;
	unsigned int ndevs;
	bool use_numa;
	char *cpuset;
} io_bench_params_t;

typedef struct {
	uint64_t capacity;
	uint64_t offset;
	io_bench_stats_t write_stats;
	io_bench_stats_t read_stats;
	uint16_t thr_idx;
} io_bench_thr_ctx_t;

typedef struct {
	char *buf;
	uint64_t start_stamp;
	uint64_t offset;
	uint16_t dev_idx;
	uint16_t slot_idx;
	int status;
	bool write;
} io_ctx_t;

typedef struct {
	int (*init_thread_ctx)(io_bench_thr_ctx_t **pctx, io_bench_params_t *params, unsigned int dev_idx);
	void (*destroy_thread_ctx)(io_bench_thr_ctx_t *ctx);
	int (*poll_completions)(io_bench_thr_ctx_t *ctx, int n);
	io_ctx_t *(*get_io_ctx)(io_bench_thr_ctx_t *ctx, uint16_t slot);
	int (*queue_io)(io_bench_thr_ctx_t *ctx, io_ctx_t *io);
} io_eng_def_t;

int io_bench_parse_args(int argc, char **argv, io_bench_params_t *params);
int io_bench_requeue_io(io_bench_thr_ctx_t *ctx, io_ctx_t *io);
void io_bench_complete_and_prep_io(io_bench_thr_ctx_t *ctx, io_ctx_t *io);

extern io_eng_def_t aio_engine;
extern io_eng_def_t aio_linux_engine;
extern io_eng_def_t dio_engine;

#endif

