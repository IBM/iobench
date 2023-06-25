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
#include "iobench.h"
#include "compiler.h"
#include "core_affinity.h"
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <stdlib.h>
#include <sys/mman.h>
#include <unistd.h>
#include <pthread.h>
#include <sys/resource.h>
#include <signal.h>
#include <errno.h>
#include <sys/time.h>
#include <termios.h>

#define SLEEP_INT_MS (2500)
#define PROGRESS_INT_US (10000000)
#define ALIGN(__addr__, __size__) (((__addr__) + (__size__) - 1) & ~((__size__) - 1))

struct {
	io_bench_stats_t read_stats;
	io_bench_stats_t write_stats;
	void *pf_map;
	uint64_t pf_size;
	uint64_t int_start;
	uint64_t start;
	uint64_t reset_stats_stamp;
	io_bench_dev_ctx_t *dev_ctx_array;
	io_bench_thr_ctx_t **ctx_array;
	pthread_t *threads;
	uint64_t progress_int;
	pthread_t main_thread;
	pthread_mutex_t init_mutex;
	pthread_mutex_t run_mutex;
	pthread_cond_t init_cond;
	pthread_cond_t run_cond;
	unsigned int done_init;
	int tty;
	bool may_run;
	bool failed;
	bool exiting;
} global_ctx = {
	.init_mutex = PTHREAD_MUTEX_INITIALIZER,
	.run_mutex = PTHREAD_MUTEX_INITIALIZER,
	.init_cond = PTHREAD_COND_INITIALIZER,
	.run_cond = PTHREAD_COND_INITIALIZER,
	.tty = -1,
};

static io_bench_params_t init_params;
static io_eng_def_t *io_eng = NULL;

static void disable_tty_echo(void)
{
	char *name = ttyname(0);
	char *name1 = ttyname(2);
	struct termios tos;

	if (!name || !name1)
		return;
	if (name != name1 && strcmp(name, name1))
		return;
	global_ctx.tty = open(name, O_RDWR);
	if (global_ctx.tty < 0)
		return;
	if (tcgetattr(global_ctx.tty, &tos)) {
		ERROR("Failed to get tty settings");
		close(global_ctx.tty);
		global_ctx.tty = -1;
		return;
	}
	tos.c_lflag &= ~(ECHO);
	if(tcsetattr(global_ctx.tty, TCSANOW, &tos)) {
		ERROR("Failed to set tty settings");
		close(global_ctx.tty);
		global_ctx.tty = -1;
		return;
	}
}

static void enable_tty_echo(void)
{
	struct termios tos;
	if (tcgetattr(global_ctx.tty, &tos)) {
		ERROR("Failed to get tty settings");
		return;
	}
	tos.c_lflag |= (ECHO);
	if(tcsetattr(global_ctx.tty, TCSANOW, &tos))
		ERROR("Failed to set tty settings");
	return;
}

static void kill_all_threads(void)
{
	unsigned int i;
	for (i = 0; i < init_params.threads; i++) {
		if (global_ctx.threads[i])
			pthread_kill(global_ctx.threads[i], SIGTERM);
	}
}

static void join_all_threads(void)
{
	unsigned int i;
	for (i = 0; i < init_params.threads; i++) {
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

static void print_stats_banner(void)
{
	INFO_NOPFX("                  RdKIOPS  WrKIOPS  Rd MiB/s  Wr MiB/s      Rd Lat  MinRLat  MaxRLat      Wr Lat  MinWLat  MaxWLat");
	INFO_NOPFX("------------------------------------------------------------------------------------------------------------------");
}

static char *time_prefix(char *buf, size_t len)
{
	struct timeval tv;
	struct tm tm;
	if (gettimeofday(&tv, NULL))
		return "";
	if (!localtime_r(&tv.tv_sec, &tm))
		return "";
	snprintf(buf, len, "%02d:%02d:%02d.%06lu", tm.tm_hour, tm.tm_min, tm.tm_sec, tv.tv_usec);
	return buf;
}

static void reset_global_stats(uint64_t stamp)
{
	uint32_t i;
	memset(&global_ctx.read_stats, 0, sizeof(global_ctx.read_stats));
	memset(&global_ctx.write_stats, 0, sizeof(global_ctx.write_stats));
	global_ctx.read_stats.min_lat = global_ctx.write_stats.min_lat = -1ULL;
	global_ctx.start = global_ctx.int_start = stamp;
	global_ctx.reset_stats_stamp = -1UL;
	for (i = 0; i < init_params.threads; i++) {
		global_ctx.ctx_array[i]->read_stats.iops = 0;
		global_ctx.ctx_array[i]->write_stats.iops = 0;
		reset_latencies(&global_ctx.ctx_array[i]->read_stats);
		reset_latencies(&global_ctx.ctx_array[i]->write_stats);
		memset(global_ctx.ctx_array[i]->read_lat_hyst, 0, sizeof(global_ctx.ctx_array[i]->read_lat_hyst));
		memset(global_ctx.ctx_array[i]->write_lat_hyst, 0, sizeof(global_ctx.ctx_array[i]->write_lat_hyst));
	}
}

static void print_hysteresis_stats(uint64_t *array)
{
	unsigned int i;
	uint64_t total = 0;
	double ratio;
	double total_ratio = 0;
	uint64_t val;

	for (i = 0; i < MAX_HYSTERESIS_STATS; i++)
		total += array[i];
	for (i = 0; i < MAX_HYSTERESIS_STATS-1; i++) {
		val = i;
		val <<= 6;
		ratio = 100.0 * array[i] / total;
		total_ratio += ratio;
		if (val)
			INFO_NOPFX("%lu-%lu: %.2lf%% (%.2lf%%)", val, val + 63, ratio, total_ratio);
		if (ratio < 0.005 && total_ratio >= 99.995)
			return;
	}
	val = MAX_HYSTERESIS_STATS-1;
	val <<= 6;
	ratio = 100.0 * array[MAX_HYSTERESIS_STATS-1] / total;
	INFO_NOPFX("%lu-max: %.2lf%%", val, ratio);
}

static void print_latency_hysteresis(void)
{
	uint64_t read_vals[MAX_HYSTERESIS_STATS] = { 0 };
	uint64_t write_vals[MAX_HYSTERESIS_STATS] = { 0 };
	unsigned int i;
	unsigned int j;

	if (!init_params.print_hyst)
		return;
	for (i = 0; i < init_params.threads; i++) {
		for (j = 0; j < MAX_HYSTERESIS_STATS; j++) {
			read_vals[j] += global_ctx.ctx_array[i]->read_lat_hyst[j];
			write_vals[j] += global_ctx.ctx_array[i]->write_lat_hyst[j];
		}
	}
	if (init_params.wp != 100) {
		INFO_NOPFX("Read latency hysteresis:");
		print_hysteresis_stats(read_vals);
	}
	if (init_params.wp) {
		INFO_NOPFX("Write latency hysteresis:");
		print_hysteresis_stats(write_vals);
	}
}

static void update_process_io_stats(uint64_t stamp, bool final)
{
	io_bench_stats_t read_stats = { 0 };
	io_bench_stats_t write_stats = { 0 };
	unsigned int i;

	reset_latencies(&read_stats);
	reset_latencies(&write_stats);

	for (i = 0; i < init_params.threads; i++) {
		read_stats.iops += global_ctx.ctx_array[i]->read_stats.iops;
		write_stats.iops += global_ctx.ctx_array[i]->write_stats.iops;
		update_latencies(&global_ctx.ctx_array[i]->read_stats, &read_stats);
		update_latencies(&global_ctx.ctx_array[i]->write_stats, &write_stats);
	}
	if (unlikely(init_params.pass_once && stamp >= (global_ctx.progress_int + PROGRESS_INT_US))) {
		char buf[1024];
		size_t len=0;
		double avg = 0;
		unsigned int i;
		for (i = 0; i < init_params.threads; i++) {
			double pr = (global_ctx.ctx_array[i]->write_stats.iops + global_ctx.ctx_array[i]->read_stats.iops) * init_params.bs * 100.0 / global_ctx.dev_ctx_array[i].capacity;
			if (len < sizeof(buf))
				len += snprintf(buf+len, sizeof(buf) - len,  "%.2lf ", pr);
			avg += pr;
		}
		global_ctx.progress_int = stamp;
		INFO_NOPFX("Progress: %s avg: %.2lf", buf, avg / init_params.threads);
		print_stats_banner();
	}
	if (!final) {
		char time_str[64];
		if (read_stats.min_lat == -1ULL)
			read_stats.min_lat = 0;
		if (write_stats.min_lat == -1ULL)
			write_stats.min_lat = 0;
		INFO_NOPFX("%s: %8.2lf %8.2lf %9.2lf %9.2lf %11.2lf %8lu %8lu %11.2lf %8lu %8lu",
			time_prefix(time_str, sizeof(time_str)),
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
		char time_str[64];
		if (global_ctx.read_stats.min_lat == -1ULL)
			global_ctx.read_stats.min_lat = 0;
		if (global_ctx.write_stats.min_lat == -1ULL)
			global_ctx.write_stats.min_lat = 0;
		INFO_NOPFX("------------------------------------------------------------------------------------------------------------------");
		INFO_NOPFX("%s: %8.2lf %8.2lf %9.2lf %9.2lf %11.2lf %8lu %8lu %11.2lf %8lu %8lu",
			time_prefix(time_str, sizeof(time_str)),
			((global_ctx.read_stats.iops) * 1000.0) / (stamp - global_ctx.start),
			((global_ctx.write_stats.iops) * 1000.0) / (stamp - global_ctx.start),
			(global_ctx.read_stats.iops) * 0.953674 * init_params.bs / (stamp - global_ctx.start),
			(global_ctx.write_stats.iops) * 0.953674 * init_params.bs / (stamp - global_ctx.start),
			((double)global_ctx.read_stats.lat) / SAFE_DELTA(global_ctx.read_stats.iops, 0), global_ctx.read_stats.min_lat, global_ctx.read_stats.max_lat,
			((double)global_ctx.write_stats.lat) / SAFE_DELTA(global_ctx.write_stats.iops, 0), global_ctx.write_stats.min_lat, global_ctx.write_stats.max_lat);
	}
	if (final)
		print_latency_hysteresis();
	if (global_ctx.reset_stats_stamp < stamp)
		reset_global_stats(stamp);
}


static void print_final_process_stats(void)
{
	update_process_io_stats(get_uptime_us(), true);
}

void update_io_stats(io_bench_thr_ctx_t *ctx, io_ctx_t *io, uint64_t stamp)
{
	uint64_t lat = stamp - io->start_stamp;
	uint64_t index = lat >> 6;

	if (index >= MAX_HYSTERESIS_STATS)
		index = MAX_HYSTERESIS_STATS-1;
	io_bench_stats_t *stat = (!io->write) ? &ctx->read_stats : &ctx->write_stats;
	if (lat < stat->min_lat)
		stat->min_lat = lat;
	if (lat > stat->max_lat)
		stat->max_lat = lat;
	stat->lat += lat;
	stat->iops++;
	if (!io->write)
		ctx->read_lat_hyst[index]++;
	else
		ctx->write_lat_hyst[index]++;
}

static void update_io_stats_atomic(io_bench_thr_ctx_t *ctx, io_ctx_t *io, uint64_t stamp)
{
	uint64_t lat = stamp - io->start_stamp;
	io_bench_stats_t *stat = (!io->write) ? &ctx->read_stats : &ctx->write_stats;
	uint64_t cur_min = stat->min_lat;
	uint64_t cur_max = stat->max_lat;
	uint64_t index = lat << 6;

	if (index >= MAX_HYSTERESIS_STATS)
		index = MAX_HYSTERESIS_STATS-1;

	while (lat < cur_min) {
		if (__sync_bool_compare_and_swap(&stat->min_lat, cur_min, lat))
			break;
		cur_min = stat->min_lat;
	}
	while (lat > cur_max) {
		if (__sync_bool_compare_and_swap(&stat->max_lat, cur_max, lat))
			break;
		cur_max = stat->max_lat;
	}
	__sync_fetch_and_add(&stat->lat, lat);
	__sync_fetch_and_add(&stat->iops, 1);
	if (!io->write)
		__sync_fetch_and_add(ctx->read_lat_hyst, 1);
	else
		__sync_fetch_and_add(ctx->write_lat_hyst, 1);
}

static void term_handler(int signo)
{
	unsigned int i;
	if (pthread_self() != global_ctx.main_thread) {
		unsigned int i;
		if (__sync_bool_compare_and_swap(&global_ctx.exiting, false, true))
			pthread_kill(global_ctx.main_thread, SIGTERM);
		if (io_eng->stop_thread_ctx) {
			for (i = 0; i < init_params.threads; i++) {
				if (global_ctx.threads[i] == pthread_self()) {
					io_eng->stop_thread_ctx(global_ctx.ctx_array[i]);
					break;
				}
			}
		}
		pthread_exit(NULL);
	}
	global_ctx.exiting = true;
	enable_tty_echo();
	kill_all_threads();
	if (global_ctx.may_run)
		print_final_process_stats();
	join_all_threads();
	if (io_eng->destroy_thread_ctx) {
		for (i = 0; i < init_params.threads; i++) {
			if (global_ctx.ctx_array[i])
				io_eng->destroy_thread_ctx(global_ctx.ctx_array[i]);
		}
	}
	_exit(global_ctx.failed);
}

static uint64_t choose_seq_offset_atomic(io_bench_thr_ctx_t *thread_ctx, io_ctx_t *io)
{
	uint64_t result;
	uint64_t new_value;

	do {
		result = global_ctx.dev_ctx_array[io->dev_idx].offset;
		new_value = result + init_params.bs;
		if (new_value >= global_ctx.dev_ctx_array[io->dev_idx].capacity)
				new_value = 0;

	} while (!__sync_bool_compare_and_swap(&global_ctx.dev_ctx_array[io->dev_idx].offset, result, new_value));
	return result;
}

static uint64_t choose_seq_offset(io_bench_thr_ctx_t *thread_ctx, io_ctx_t *io)
{
	uint64_t result;
	uint64_t new_value;

	result = global_ctx.dev_ctx_array[io->dev_idx].offset;
	new_value = result + init_params.bs;

	if (new_value >= global_ctx.dev_ctx_array[io->dev_idx].capacity)
		new_value = 0;
	global_ctx.dev_ctx_array[io->dev_idx].offset = new_value;
	return result;
}

static inline uint64_t choose_random_offset(io_bench_thr_ctx_t *thread_ctx, io_ctx_t *io, bool atomic)
{
	uint64_t result;
	unsigned int *seed = (!io_eng->seed_per_io) ? &thread_ctx->seed : &io->seed;

	if (init_params.seq) {
		result = (!atomic) ? choose_seq_offset(thread_ctx, io) : choose_seq_offset_atomic(thread_ctx, io);
	} else {
		result = ((double)rand_r(seed) * (global_ctx.dev_ctx_array[io->dev_idx].capacity / init_params.bs)) / ((unsigned int)RAND_MAX + 1);
		result *= init_params.bs;
		result += (global_ctx.dev_ctx_array[io->dev_idx].capacity * io->slot_idx);
	}
	return result;
}

static inline bool is_write_io(unsigned int *seed)
{
	if (!init_params.wp)
		return false;

	if (init_params.wp == 100)
		return true;

	uint32_t val = ((double)rand_r(seed) * 100) / ((unsigned int)RAND_MAX + 1);
	return  (val < init_params.wp);
}

static void init_thread_dev_selection(io_bench_thr_ctx_t *thread_ctx)
{
	if (init_params.ndevs > init_params.threads) {
		uint16_t max_dev;
		uint16_t off = max_dev = init_params.ndevs / init_params.threads;
		uint16_t tail = init_params.ndevs % init_params.threads;
		off *= thread_ctx->thr_idx;
		if (tail) {
			off += (thread_ctx->thr_idx < tail) ? thread_ctx->thr_idx : tail;
			if (thread_ctx->thr_idx < tail)
				max_dev++;
		}
		thread_ctx->rr_dev_off = off;
		thread_ctx->max_rr_devs = max_dev;
	} else {
		thread_ctx->rr_dev_off = 0;
		thread_ctx->max_rr_devs = init_params.ndevs;
	}
	thread_ctx->rr_dev_sel = thread_ctx->rr_dev_off + ((double)rand_r(&thread_ctx->seed) * thread_ctx->max_rr_devs) / ((unsigned int)RAND_MAX + 1);
	thread_ctx->rr_dev_sel_stamp = get_uptime_us();
}

static inline unsigned int choose_dev_idx(io_bench_thr_ctx_t *thread_ctx)
{
	uint64_t now = thread_ctx->rr_dev_sel_stamp;
	if (!init_params.max_dev_lease_usec || (now = get_uptime_us()) >= (thread_ctx->rr_dev_sel_stamp + init_params.max_dev_lease_usec)) {
		thread_ctx->rr_dev_sel = thread_ctx->rr_dev_off + ((double)rand_r(&thread_ctx->seed) * thread_ctx->max_rr_devs) / ((unsigned int)RAND_MAX + 1);
		thread_ctx->rr_dev_sel_stamp = now;
	}
	return thread_ctx->rr_dev_sel;
}

static void update_pf_offset(io_bench_thr_ctx_t *thread_ctx, io_ctx_t *io, bool atomic)
{
	void *buf;
	if (likely(!atomic)) {
		buf =  global_ctx.pf_map + thread_ctx->pf_offset;
		thread_ctx->pf_offset += init_params.bs;
		if (thread_ctx->pf_offset >= global_ctx.pf_size)
			thread_ctx->pf_offset = 0;
	} else {
		uint64_t val;
		uint64_t new_val;
		do {
			val = thread_ctx->pf_offset;
			new_val = val + init_params.bs;
			if (new_val >= global_ctx.pf_size)
				new_val = 0;
		} while (!__sync_bool_compare_and_swap(&thread_ctx->pf_offset, val, new_val));
		buf =  global_ctx.pf_map + val;
	}
	if (unlikely(io_eng->need_mr_buffers))
		memcpy(io->buf, buf, init_params.bs);
	else
		io->buf = buf;
}

static void prep_one_io(io_bench_thr_ctx_t *ctx, io_ctx_t *io, uint64_t stamp, bool atomic)
{
	unsigned int *seed = (!io_eng->seed_per_io) ? &ctx->seed : &io->seed;
	io->start_stamp = stamp;
	io->write = is_write_io(seed);
	io->dev_idx = (init_params.rr) ? choose_dev_idx(ctx) : ctx->thr_idx;
	io->offset = choose_random_offset(ctx, io, atomic);
	io->offset += global_ctx.dev_ctx_array[io->dev_idx].base_offset;
	if (unlikely(global_ctx.pf_map && io->write))
		update_pf_offset(global_ctx.ctx_array[ctx->thr_idx], io, atomic);
	else if (!io_eng->need_mr_buffers)
		io->buf = global_ctx.ctx_array[ctx->thr_idx]->buf_head + init_params.bs * io->slot_idx;

	if (unlikely(init_params.pass_once && ((ctx->write_stats.iops + ctx->read_stats.iops) * init_params.bs) >= global_ctx.dev_ctx_array[ctx->thr_idx].capacity)) {
		__sync_fetch_and_sub(&global_ctx.done_init, 1);
		pthread_exit(NULL);
	}
}

static int submit_one_io(io_bench_thr_ctx_t *ctx, io_ctx_t *io, uint64_t stamp)
{
	bool atomic = init_params.rr;
	prep_one_io(ctx, io, stamp, atomic);
	return io_eng->queue_io(ctx, io);
}

static void handle_thread_failure(void)
{
	global_ctx.failed = true;
	pthread_kill(global_ctx.main_thread, SIGTERM);
	pthread_exit(NULL);
}

int io_bench_requeue_io(io_bench_thr_ctx_t *ctx, io_ctx_t *io)
{
	uint64_t stamp = get_uptime_us();
	int rc;
	if (unlikely(io->status)) {
		ERROR("IO to offset %lu, device %s fails with code %d", io->offset, init_params.devices[io->dev_idx], io->status);
		if (init_params.fail_on_err) {
			rc = io->status;
			goto done;
		}
	}
	rc = submit_one_io(ctx, io, stamp);
done:
	if (unlikely(rc)) {
		ERROR("IO submit fails");
		if (init_params.fail_on_err)
			handle_thread_failure();
	}
	return rc;
}

void io_bench_complete_and_prep_io(io_bench_thr_ctx_t *ctx, io_ctx_t *io)
{
	uint64_t stamp = get_uptime_us();
	update_io_stats_atomic(ctx, io, stamp);
	prep_one_io(ctx, io, stamp, true);
	if (unlikely(io->status)) {
		ERROR("IO to offset %lu, device %s fails with code %d", io->offset, init_params.devices[io->dev_idx], io->status);
		if (init_params.fail_on_err)
			handle_thread_failure();
	}
}

static int set_device_context_fields(void)
{
	uint32_t max = init_params.ndevs / init_params.threads_per_dev;
	uint32_t i;
	uint32_t j;

	for (i = 0; i < max; i++) {
		int fd;
		uint64_t base_offset;
		uint64_t capacity;
		fd = open(init_params.devices[i], O_RDONLY);
		if (fd < 0) {
			ERROR("Failed to open %s", init_params.devices[i]);
			return -ENODEV;
		}
		capacity = lseek(fd, 0, SEEK_END);
		close(fd);
		if (capacity == -1ULL) {
			ERROR("Failed to determine capacity for %s", init_params.devices[i]);
			return -ENODEV;
		}

		if (init_params.hit_size && init_params.hit_size < capacity)
			capacity = init_params.hit_size;

		capacity /= init_params.threads_per_dev;
		capacity = base_offset = (capacity / init_params.bs) * init_params.bs;
		if (!init_params.seq) {
			capacity /= init_params.qs;
			capacity = (capacity / init_params.bs) * init_params.bs;
		}
		for (j = 0; j < init_params.threads_per_dev; j++) {
			uint32_t idx = j * init_params.ndevs + i;
			global_ctx.dev_ctx_array[idx].base_offset = base_offset *j;
			global_ctx.dev_ctx_array[idx].capacity = capacity;
		}
	}
	return 0;
}

static void *thread_func(void *arg)
{
	int rc;
	uint16_t i;
	unsigned int idx = (unsigned long)(arg) & 0xffff;
	unsigned int cpu = (unsigned long)(arg) >> 16;
	void *buf_head;
	int flags = (init_params.mlock) ? MAP_LOCKED : 0;
	size_t map_size = init_params.bs;
	unsigned int kcpu = -1;

	if (cpu < 0xffff) {
		set_thread_affinity(cpu);
		if (init_params.poll_kcpu_offset)
			kcpu = cpu + init_params.poll_kcpu_offset;
	}

	map_size *= init_params.qs;
	map_size = ALIGN(map_size, 4096);
	buf_head = mmap(NULL, map_size, PROT_READ|PROT_WRITE, MAP_SHARED|MAP_ANONYMOUS|flags, -1, 0);
	if (buf_head == MAP_FAILED) {
	ERROR("Thread %u failed to allocate buffers", idx);
		handle_thread_failure();
	}

	rc = io_eng->init_thread_ctx(&global_ctx.ctx_array[idx], &init_params, buf_head, idx, kcpu);
	if (rc) {
		ERROR("Thread %u failed to init", idx);
		handle_thread_failure();
	}
	if (!io_eng->need_mr_buffers)
		global_ctx.ctx_array[idx]->buf_head = buf_head;
	if (init_params.wp || !init_params.pf_name) {
		int *buf = buf_head;
		while (map_size) {
			buf[0] = rand_r(&global_ctx.ctx_array[idx]->seed);
			buf++;
			map_size -= sizeof(int);
		}
	}
	global_ctx.ctx_array[idx]->thr_idx = idx;
	global_ctx.ctx_array[idx]->seed = get_uptime_us() + idx;

	pthread_mutex_lock(&global_ctx.init_mutex);
	global_ctx.done_init++;
	if (global_ctx.done_init == init_params.threads)
		pthread_cond_signal(&global_ctx.init_cond);
	pthread_mutex_unlock(&global_ctx.init_mutex);

	pthread_mutex_lock(&global_ctx.run_mutex);
	while (!global_ctx.may_run)
		pthread_cond_wait(&global_ctx.run_cond, &global_ctx.run_mutex);
	pthread_mutex_unlock(&global_ctx.run_mutex);

	reset_latencies(&global_ctx.ctx_array[idx]->read_stats);
	reset_latencies(&global_ctx.ctx_array[idx]->write_stats);
	init_thread_dev_selection(global_ctx.ctx_array[idx]);

	for (i = 0; i < init_params.qs; i++) {
		io_ctx_t *io_ctx = io_eng->get_io_ctx(global_ctx.ctx_array[idx], i);
		ASSERT(io_ctx);
		io_ctx->slot_idx = i;
		if (!io_eng->need_mr_buffers)
			io_ctx->buf = global_ctx.ctx_array[idx]->buf_head + init_params.bs * i;
		rc = submit_one_io(global_ctx.ctx_array[idx], io_ctx, get_uptime_us());
		if (unlikely(rc)) {
			ERROR("IO submit fails");
			if (init_params.fail_on_err)
				handle_thread_failure();
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
	unsigned int cpu = -1U;
	struct rlimit rlim;

	if (init_params.pf_name) {
		int fd;
		int flags = (init_params.mlock) ? MAP_LOCKED : 0;
		fd = open(init_params.pf_name, O_RDONLY);
		if (fd < 0) {
			ERROR("Failed to open pattetn file %s", init_params.pf_name);
			return -1;
		}
		global_ctx.pf_size = lseek(fd, 0, SEEK_END);
		if (global_ctx.pf_size == -1ULL) {
			ERROR("Failed to get size of pattetn file %s", init_params.pf_name);
			close(fd);
			return -1;
		}
		global_ctx.pf_size = (global_ctx.pf_size / init_params.bs) * init_params.bs;
		global_ctx.pf_map = mmap(NULL, global_ctx.pf_size, PROT_READ, MAP_SHARED|flags, fd, 0);
		close(fd);
		if (global_ctx.pf_map == MAP_FAILED) {
			ERROR("Failed to map pattern file %s", init_params.pf_name);
			return -1;
		}
	}

	if (getrlimit(RLIMIT_NOFILE, &rlim)) {
		ERROR("Cannot read file number limit");
		return -1;
	}
	rlim.rlim_cur = init_params.ndevs * init_params.qs * init_params.threads_per_dev + 1024;
	if (rlim.rlim_cur > rlim.rlim_max)
		rlim.rlim_cur = rlim.rlim_max;
	if (setrlimit(RLIMIT_NOFILE, &rlim)) {
		ERROR("Cannot read file number limit");
		return -1;
	}

	act.sa_handler = term_handler;
	if (sigaction(SIGTERM, &act, NULL)) {
		ERROR("Failed to setup SIGTERM handler");
		return -1;
	}
	if (sigaction(SIGINT, &act, NULL)) {
		ERROR("Failed to setup SIGINT handler");
		return -1;
	}

	disable_tty_echo();
	global_ctx.main_thread = pthread_self();
	global_ctx.dev_ctx_array = calloc(init_params.ndevs, sizeof(global_ctx.dev_ctx_array[0]));
	global_ctx.ctx_array = calloc(init_params.threads, sizeof(global_ctx.ctx_array[0]));
	global_ctx.threads = calloc(init_params.threads, sizeof(global_ctx.threads[0]));
	if (!global_ctx.ctx_array || !global_ctx.threads || !global_ctx.dev_ctx_array) {
		ERROR("Cannot malloc");
		return -ENOMEM;
	};
	if (set_device_context_fields()) {
		ERROR("Failed to setup device context fields");
		return -ENODEV;
	}
	for (i = 0; i < init_params.threads; i++) {
		unsigned long val = i;
		cpu = -1;
		if (init_params.cpuset) {
			if (init_params.engine != ENGINE_DIO)
				cpu = get_next_cpu_from_set(init_params.cpuset);
		} else if (init_params.remap_numa ||init_params.use_numa)  {
			unsigned int numa_id = get_numa_id_of_block_device(init_params.devices[i]);
			if (init_params.remap_numa && numa_id != -1U) {
				cpu = get_next_remapped_numa_cpu(init_params.remap_numa, numa_id);
			} else {
				cpu = get_next_numa_rr_cpu(numa_id);
			}
		}
		val |= (cpu << 16);
		if (pthread_create(&global_ctx.threads[i], NULL, thread_func, (void *)val)) {
			global_ctx.threads[i] = 0;
			ERROR("Cannot create thread %u", i);
			global_ctx.exiting = true;
			enable_tty_echo();
			kill_all_threads();
			join_all_threads();
			exit(1);
		}
	}
	pthread_mutex_lock(&global_ctx.init_mutex);
	while (global_ctx.done_init != init_params.threads)
		pthread_cond_wait(&global_ctx.init_cond, &global_ctx.init_mutex);
	pthread_mutex_unlock(&global_ctx.init_mutex);

	pthread_mutex_lock(&global_ctx.run_mutex);
	global_ctx.read_stats.min_lat = global_ctx.write_stats.min_lat = -1ULL;
	global_ctx.start = global_ctx.int_start = get_uptime_us();
	global_ctx.reset_stats_stamp = (init_params.delay_sec != -1U) ? (global_ctx.int_start + init_params.delay_sec * 1000000 ) : -1UL;
	global_ctx.may_run = true;
	pthread_cond_broadcast(&global_ctx.run_cond);
	pthread_mutex_unlock(&global_ctx.run_mutex);

	stop_stamp = (init_params.run_time) ? (get_uptime_us() + 1000000ULL * init_params.run_time) : -1ULL;
	print_stats_banner();

	while (stamp < stop_stamp && global_ctx.done_init) {
		usleep(SLEEP_INT_MS * 1000);
		stamp = get_uptime_us();
		update_process_io_stats(stamp, false);
	}
	global_ctx.exiting = true;
	enable_tty_echo();
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
		case ENGINE_AIO_LINUX: io_eng = &aio_linux_engine; break;
		case ENGINE_AIO_URING: io_eng = &aio_uring_engine; break;
		case ENGINE_DIO: io_eng = &dio_engine; break;
		case ENGINE_SG_AIO: io_eng = &sg_aio_engine; break;
		case ENGINE_SG_URING: io_eng = &sg_uring_engine; break;
		case ENGINE_NVME: io_eng = &nvme_engine; break;
		default: ERROR("Unsupported IO engine"); exit(1); break;
	}
	rc = start_threads();
	return rc;
}
