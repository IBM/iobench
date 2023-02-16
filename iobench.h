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
	ENGINE_AIO_URING,
	ENGINE_DIO,
	ENGINE_SG_AIO,
	ENGINE_SG_URING,
	ENGINE_NVME,
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
	uint16_t poll_idle_kernel_ms;
	uint16_t poll_idle_user_ms;
	uint16_t poll_kcpu_offset;
	uint32_t delay_sec;
	bool fail_on_err;
	bool seq;
	bool rr;
	bool mlock;
	bool pass_once;
	bool poll;
	uint8_t wp;
	io_eng_t engine;
	char *pf_name;
	char **devices;
	unsigned int threads_per_dev;
	unsigned int ndevs;
	unsigned int threads;
	unsigned int max_dev_lease_usec;
	bool use_numa;
	double kiops;
	char *cpuset;
	char *remap_numa;
} io_bench_params_t;

typedef struct {
	uint64_t capacity;
	uint64_t base_offset;
	uint64_t offset;
} io_bench_dev_ctx_t;

typedef struct {
	void *buf_head;
	uint64_t pf_offset;
	io_bench_stats_t write_stats;
	io_bench_stats_t read_stats;
	unsigned int seed;
	uint16_t thr_idx;
	uint16_t rr_dev_sel;
	uint64_t rr_dev_sel_stamp;
	uint16_t rr_dev_off;
	uint16_t max_rr_devs;
} io_bench_thr_ctx_t;

typedef struct {
	char *buf;
	uint64_t start_stamp;
	uint64_t slack;
	uint64_t offset;
	uint16_t dev_idx;
	uint16_t slot_idx;
	int status;
	unsigned int seed;
	bool write;
} io_ctx_t;

typedef struct {
	int (*init_thread_ctx)(io_bench_thr_ctx_t **pctx, io_bench_params_t *params, void *buf_head, unsigned int dev_idx, unsigned int poll_cpu);
	void (*stop_thread_ctx)(io_bench_thr_ctx_t *ctx);
	void (*destroy_thread_ctx)(io_bench_thr_ctx_t *ctx);
	int (*poll_completions)(io_bench_thr_ctx_t *ctx, int n);
	io_ctx_t *(*get_io_ctx)(io_bench_thr_ctx_t *ctx, uint16_t slot);
	int (*queue_io)(io_bench_thr_ctx_t *ctx, io_ctx_t *io);
	bool seed_per_io;
	bool need_mr_buffers;
} io_eng_def_t;

int io_bench_parse_args(int argc, char **argv, io_bench_params_t *params);
int io_bench_requeue_io(io_bench_thr_ctx_t *ctx, io_ctx_t *io);
void io_bench_complete_and_prep_io(io_bench_thr_ctx_t *ctx, io_ctx_t *io);

void update_io_stats(io_bench_thr_ctx_t *ctx, io_ctx_t *io, uint64_t stamp);

extern io_eng_def_t aio_engine;
extern io_eng_def_t aio_linux_engine;
extern io_eng_def_t aio_uring_engine;
extern io_eng_def_t dio_engine;
extern io_eng_def_t sg_aio_engine;
extern io_eng_def_t sg_uring_engine;
extern io_eng_def_t nvme_engine;

#endif

