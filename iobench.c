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

#include "logger.h"
DECLARE_BFN
#include "iobench.h"
#include "compiler.h"
#include <stdlib.h>
#include <unistd.h>
#include <pthread.h>
#include <signal.h>
#include <time.h>
#include <errno.h>

#define SLEEP_INT_MS (2500)

struct {
	io_bench_stats_t read_stats;
	io_bench_stats_t write_stats;
	uint64_t int_start;
	uint64_t start;
	io_bench_thr_ctx_t **ctx_array;
	pthread_t *threads;
	pthread_t main_thread;
	pthread_mutex_t init_mutex;
	pthread_mutex_t run_mutex;
	pthread_cond_t init_cond;
	pthread_cond_t run_cond;
	unsigned int done_init;
	bool may_run;
	bool failed;
} global_ctx = {
	.init_mutex = PTHREAD_MUTEX_INITIALIZER,
	.run_mutex = PTHREAD_MUTEX_INITIALIZER,
	.init_cond = PTHREAD_COND_INITIALIZER,
	.run_cond = PTHREAD_COND_INITIALIZER,
};

static inline uint64_t get_uptime_us(void)
{
	struct timespec spec_tv;
	uint64_t res;
	clock_gettime(CLOCK_MONOTONIC, &spec_tv);
	res = spec_tv.tv_sec;
	res *= 1000000;
	res += spec_tv.tv_nsec / 1000;
	return res;
}

static io_bench_params_t init_params;
static io_eng_def_t *io_eng = NULL;

static void kill_all_threads(void)
{
	unsigned int i;
	for (i = 0; i < init_params.ndevs; i++) {
		if (global_ctx.threads[i])
			pthread_kill(global_ctx.threads[i], SIGTERM);
	}
}

static void join_all_threads(void)
{
	unsigned int i;
	for (i = 0; i < init_params.ndevs; i++) {
		if (global_ctx.threads[i])
			pthread_join(global_ctx.threads[i], NULL);
	}
}

static void reset_latencies(io_bench_stats_t *stat)
{
	stat->max_lat = 0;
	stat->min_lat = -1ULL;
	stat->lat = 0;
}

static void update_latencies(io_bench_stats_t *from, io_bench_stats_t *to)
{
	if (from->min_lat < to->min_lat)
		to->min_lat = from->min_lat;
	if (from->max_lat > to->max_lat)
		to->max_lat = from->max_lat;
	if (from->lat != -1ULL)
		to->lat += from->lat;
	reset_latencies(from);
}

#define SAFE_DELTA(__x__, __y__) \
({ \
	uint64_t __res__ = (__x__ - __y__); \
	if (__res__ == 0) \
		__res__ = 1; \
	__res__; \
})

static void update_process_io_stats(uint64_t stamp, bool final)
{
	io_bench_stats_t read_stats = { 0 };
	io_bench_stats_t write_stats = { 0 };
	unsigned int i;

	reset_latencies(&read_stats);
	reset_latencies(&write_stats);

	for (i = 0; i < init_params.ndevs; i++) {
		read_stats.iops += global_ctx.ctx_array[i]->read_stats.iops;
		write_stats.iops += global_ctx.ctx_array[i]->write_stats.iops;
		update_latencies(&global_ctx.ctx_array[i]->read_stats, &read_stats);
		update_latencies(&global_ctx.ctx_array[i]->write_stats, &write_stats);
	}
	if (!final) {
		if (read_stats.min_lat == -1ULL)
			read_stats.min_lat = 0;
		if (write_stats.min_lat == -1ULL)
			write_stats.min_lat = 0;
		INFO_NOPFX("%8.2lf %8.2lf %9.2lf %9.2lf %7.2lf %4lu %4lu %7.2lf %4lu %4lu",
			((read_stats.iops - global_ctx.read_stats.iops) * 1000.0) / (stamp - global_ctx.int_start),
			((write_stats.iops - global_ctx.write_stats.iops) * 1000.0) / (stamp - global_ctx.int_start),
			(read_stats.iops - global_ctx.read_stats.iops) * 0.953674 * init_params.bs / (stamp - global_ctx.int_start),
			(write_stats.iops - global_ctx.write_stats.iops) * 0.953674 * init_params.bs / (stamp - global_ctx.int_start),
			((double)read_stats.lat) / SAFE_DELTA(read_stats.iops, global_ctx.read_stats.iops), read_stats.min_lat, read_stats.max_lat,
			((double)write_stats.lat) / SAFE_DELTA(write_stats.iops,  global_ctx.write_stats.iops), write_stats.min_lat, write_stats.max_lat);
	}
	global_ctx.int_start = stamp;
	global_ctx.read_stats.iops = read_stats.iops;
	global_ctx.write_stats.iops = write_stats.iops;
	update_latencies(&read_stats, &global_ctx.read_stats);
	update_latencies(&write_stats, &global_ctx.write_stats);
	if (unlikely(final)) {
		if (global_ctx.read_stats.min_lat == -1ULL)
			global_ctx.read_stats.min_lat = 0;
		if (global_ctx.write_stats.min_lat == -1ULL)
			global_ctx.write_stats.min_lat = 0;
		INFO_NOPFX("-------------------------------------------------------------------------");
		INFO_NOPFX("%8.2lf %8.2lf %9.2lf %9.2lf %7.2lf %4lu %4lu %7.2lf %4lu %4lu",
			((global_ctx.read_stats.iops) * 1000.0) / (stamp - global_ctx.start),
			((global_ctx.write_stats.iops) * 1000.0) / (stamp - global_ctx.start),
			(global_ctx.read_stats.iops) * 0.953674 * init_params.bs / (stamp - global_ctx.start),
			(global_ctx.write_stats.iops) * 0.953674 * init_params.bs / (stamp - global_ctx.start),
			((double)global_ctx.read_stats.lat) / SAFE_DELTA(global_ctx.read_stats.iops, 0), global_ctx.read_stats.min_lat, global_ctx.read_stats.max_lat,
			((double)global_ctx.write_stats.lat) / SAFE_DELTA(global_ctx.write_stats.iops, 0), global_ctx.write_stats.min_lat, global_ctx.write_stats.max_lat);
	}
}


static void print_final_process_stats(void)
{
	update_process_io_stats(get_uptime_us(), true);
}

static void update_io_stats(io_bench_thr_ctx_t *ctx, io_ctx_t *io, uint64_t stamp)
{
	uint64_t lat = stamp - io->start_stamp;
	io_bench_stats_t *stat = (!io->write) ? &ctx->read_stats : &ctx->write_stats;
	if (lat < stat->min_lat)
		stat->min_lat = lat;
	if (lat > stat->max_lat)
		stat->max_lat = lat;
	stat->lat += lat;
	stat->iops++;
}

static void term_handler(int signo)
{
	if (pthread_self() != global_ctx.main_thread) {
		pthread_kill(global_ctx.main_thread, SIGTERM);
		pthread_exit(NULL);
	}
	kill_all_threads();
	print_final_process_stats();
	join_all_threads();
	exit(global_ctx.failed);
}

static inline uint64_t choose_random_offset(io_bench_thr_ctx_t *thread_ctx)
{
	uint64_t result;

	if (init_params.seq) {
			result = thread_ctx->offset;
			thread_ctx->offset += init_params.bs;
			if (thread_ctx->offset >= thread_ctx->capacity)
				thread_ctx->offset = 0;
	} else {
		result = ((double)rand() * ((thread_ctx->capacity - init_params.bs) / init_params.bs)) / ((unsigned int)RAND_MAX + 1);
		result *= init_params.bs;
	}
	return result;
}

static inline bool is_write_io(void)
{
	if (!init_params.wp)
		return false;

	if (init_params.wp == 100)
		return true;

	uint32_t val = ((double)rand() * 100) / ((unsigned int)RAND_MAX + 1);
	return  (val < init_params.wp);
}

static int submit_one_io(io_bench_thr_ctx_t *ctx, io_ctx_t *io, uint64_t stamp)
{
	io->start_stamp = stamp;
	io->offset = choose_random_offset(ctx);
	io->write = is_write_io();
	return io_eng->queue_io(ctx, io);
}

int io_bench_requeue_io(io_bench_thr_ctx_t *ctx, io_ctx_t *io)
{
	uint64_t stamp = get_uptime_us();
	int rc;
	update_io_stats(ctx, io, stamp);
	if (unlikely(io->status)) {
		ERROR("IO to offset %lu, device %s fails with code %d", io->offset, ctx->dev_name, io->status);
		if (init_params.fail_on_err) {
			rc = io->status;
			goto done;
		}
	}
	rc = submit_one_io(ctx, io, stamp);
done:
	if (unlikely(rc)) {
		if (init_params.fail_on_err) {
			global_ctx.failed = true;
			pthread_kill(global_ctx.main_thread, SIGTERM);
			pthread_exit(NULL);
		}
	}
	return rc;
}

static void *thread_func(void *arg)
{
	int rc;
	uint16_t i;
	unsigned int idx = (unsigned long)(arg);
	rc = io_eng->init_thread_ctx(&global_ctx.ctx_array[idx], &init_params, idx);
	if (rc) {
		ERROR("Thread %u failed to init", idx);
		pthread_kill(global_ctx.main_thread, SIGTERM);
		pthread_exit(NULL);
	}

	pthread_mutex_lock(&global_ctx.init_mutex);
	global_ctx.done_init++;
	if (global_ctx.done_init == init_params.ndevs)
		pthread_cond_signal(&global_ctx.init_cond);
	pthread_mutex_unlock(&global_ctx.init_mutex);

	pthread_mutex_lock(&global_ctx.run_mutex);
	while (!global_ctx.may_run)
		pthread_cond_wait(&global_ctx.run_cond, &global_ctx.run_mutex);
	pthread_mutex_unlock(&global_ctx.run_mutex);

	reset_latencies(&global_ctx.ctx_array[idx]->read_stats);
	reset_latencies(&global_ctx.ctx_array[idx]->write_stats);

	for (i = 0; i < init_params.qs; i++) {
		io_ctx_t *io_ctx = io_eng->get_io_ctx(global_ctx.ctx_array[idx], i);
		ASSERT(io_ctx);
		rc = submit_one_io(global_ctx.ctx_array[idx], io_ctx, get_uptime_us());
		if (unlikely(rc)) {
			if (init_params.fail_on_err) {
				global_ctx.failed = true;
				pthread_kill(global_ctx.main_thread, SIGTERM);
				pthread_exit(NULL);
			}
		}
	}
	while (1) {
		rc = io_eng->poll_completions(global_ctx.ctx_array[idx], init_params.qs);
		ASSERT(rc == 0);
	}
}

static int start_threads(void)
{
	unsigned int i;
	uint64_t stop_stamp, stamp = 0;
	struct sigaction act = {{0}};

	act.sa_handler = term_handler;
	if (sigaction(SIGTERM, &act, NULL)) {
		ERROR("Failed to setup SIGTERM handler");
		return -1;
	}
	if (sigaction(SIGINT, &act, NULL)) {
		ERROR("Failed to setup SIGINT handler");
		return -1;
	}

	global_ctx.main_thread = pthread_self();
	global_ctx.ctx_array = calloc(init_params.ndevs, sizeof(global_ctx.ctx_array[0]));
	global_ctx.threads = calloc(init_params.ndevs, sizeof(global_ctx.threads[0]));
	if (!global_ctx.ctx_array || !global_ctx.threads) {
		ERROR("Cannot malloc");
		return -ENOMEM;
	};
	for (i = 0; i < init_params.ndevs; i++) {
		if (pthread_create(&global_ctx.threads[i], NULL, thread_func, (void *)(unsigned long)i)) {
			global_ctx.threads[i] = 0;
			ERROR("Cannot create thread %u", i);
			kill_all_threads();
			join_all_threads();
			exit(1);
		}
	}
	pthread_mutex_lock(&global_ctx.init_mutex);
	while (global_ctx.done_init != init_params.ndevs)
		pthread_cond_wait(&global_ctx.init_cond, &global_ctx.init_mutex);
	pthread_mutex_unlock(&global_ctx.init_mutex);

	pthread_mutex_lock(&global_ctx.run_mutex);
	global_ctx.start = global_ctx.int_start = get_uptime_us();
	global_ctx.may_run = true;
	pthread_cond_broadcast(&global_ctx.run_cond);
	pthread_mutex_unlock(&global_ctx.run_mutex);

	stop_stamp = (init_params.run_time) ? (get_uptime_us() + init_params.run_time) : -1ULL;
	INFO_NOPFX(" RdKIOPS  WrKIOPS  Rd MiB/s  Wr MiB/s  Rd Lat MnRL MxRL  Wr Lat MnWL MxWL");
	INFO_NOPFX("-------------------------------------------------------------------------");

	while (stamp < stop_stamp) {
		usleep(SLEEP_INT_MS * 1000);
		stamp = get_uptime_us();
		update_process_io_stats(stamp, false);
	}
	kill_all_threads();
	print_final_process_stats();
	join_all_threads();
	return global_ctx.failed;
}

int main(int argc, char *argv[])
{
	int rc;

	srand(time(NULL));
	rc = io_bench_parse_args(argc, argv, &init_params);
	if (rc)
		exit(1);
	switch (init_params.engine) {
		case ENGINE_AIO: io_eng = &aio_engine; break;
#if 0
		case ENGINE_NVNE: io_eng = &nvme_engine; break;
		case ENGINE_SCSI: io_eng = &scsi_engine; break;
#endif
		default: ERROR("Unsupported IO engine"); exit(1); break;
	}
	rc = start_threads();
	return rc;
}
