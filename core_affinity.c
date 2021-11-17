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
#define _GNU_SOURCE
#include "logger.h"
DECLARE_BFN
#include <stdint.h>
#include <limits.h>
#include <stdlib.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#include <pthread.h>
#include <stdbool.h>
#include <errno.h>
#include <stdio.h>
#include "core_affinity.h"

#define MAP_SIZE (2)

struct numa_cpu_set
{
	struct {
		uint64_t map[MAP_SIZE];
		unsigned int count;
	} free_cpus;
	struct {
		uint64_t map[MAP_SIZE];
		unsigned int count;
	} numa_cpus;
};

size_t get_numa_set_size(void)
{
	return sizeof(struct numa_cpu_set);
}

static inline void SET_BIT(unsigned int bit, uint64_t *map)
{
	unsigned int off = bit / 64;

	bit = bit % 64;
	if (off >= MAP_SIZE)
		return;
	map[off] |= (1UL << bit);
}

int init_numa_cpu_set(struct numa_cpu_set *set, unsigned int numa_id, unsigned int first_cpu)
{
	char buf[4096];
	char path[PATH_MAX];
	int fd;
	int rc;

	memset(set, 0, sizeof(*set));
	snprintf(path, sizeof(path), "/sys/bus/node/devices/node%u/cpulist", numa_id);
	fd = open(path, O_RDONLY);
	if (fd < 0) {
		rc = errno ? -errno : -1;
		ERROR("Failed to open %s", path);
		return rc;
	}
	rc = read(fd, buf, sizeof(buf)-1);
	if (rc <= 0) {
		rc = errno ? -errno : -1;
		ERROR("Failed to read %s", path);
		close(fd);
		return rc;
	}
	close(fd);
	buf[rc] = '\0';

	return init_cpu_set_from_str(set, buf, first_cpu);
}

int init_cpu_set_from_str(struct numa_cpu_set *set, char *buf, unsigned int first_cpu)
{
	char *p = buf;
	char *next = buf;
	bool found = false;

	memset(set, 0, sizeof(*set));
	while(next) {
		uint32_t start, end;
		next = strchr(p, ',');
		if (next)
			next[0] = '\0';
		if (sscanf(p, "%u-%u", &start, &end) == 2) {
			uint32_t i;
			if (first_cpu > end) {
				p = next + 1;
				continue;
			}
			found = true;
			if (start < first_cpu)
				start = first_cpu;
			for (i = start; i <= end; i++) {
				SET_BIT(i, set->numa_cpus.map);
				set->numa_cpus.count++;
			}
		} else if (sscanf(p, "%u", &start) == 1) {
			if (start >= first_cpu) {
				SET_BIT(start, set->numa_cpus.map);
				set->numa_cpus.count++;
				found = true;
			}
		} else {
			ERROR("Failed to parse cpumap");
			return -EINVAL;
		}
		p = next + 1;
	}
	if (!found) {
		WARN("No CPUs found, setting 0 CPU");
		SET_BIT(0, set->numa_cpus.map);
		set->numa_cpus.count++;
	}
	memcpy(&set->free_cpus, &set->numa_cpus, sizeof(set->free_cpus));
	return 0;
}

unsigned int get_next_cpu(struct numa_cpu_set *set)
{
	unsigned int off;
	for (off = 0; off < MAP_SIZE; off++) {
		if (set->free_cpus.map[off]) {
			unsigned int bit = __builtin_ffsl(set->free_cpus.map[off]);
			bit--;
			set->free_cpus.map[off] &= (~(1UL << bit));
			set->free_cpus.count--;
			if (!set->free_cpus.count)
				memcpy(&set->free_cpus, &set->numa_cpus, sizeof(set->free_cpus));
			return off * 64 + bit;
		}
	}
	return 0;
}

int set_thread_affinity(unsigned int cpu)
{
	uint64_t map[MAP_SIZE];
	cpu_set_t *set = (void *)map;

	memset(map, 0, sizeof(map));
	SET_BIT(cpu, map);
	return pthread_setaffinity_np(pthread_self(), sizeof(map), set);
}

typedef struct {
	int numa_id;
	struct numa_cpu_set cpuset;
} numa_node_t;

static int init_numa_nodes(struct numa_cpu_set *set, numa_node_t **pnuma_data)
{
	char buf[4096];
	int fd;
	int rc;
	const char *path = "/sys/devices/system/node/online";
	int count = 0;
	int off;
	numa_node_t *data;

	*pnuma_data = NULL;
	memset(set, 0, sizeof(*set));
	fd = open(path, O_RDONLY);
	if (fd < 0) {
		rc = errno ? -errno : -1;
		ERROR("Failed to open %s", path);
		return rc;
	}
	rc = read(fd, buf, sizeof(buf)-1);
	if (rc <= 0) {
		rc = errno ? -errno : -1;
		ERROR("Failed to read %s", path);
		close(fd);
		return rc;
	}
	close(fd);
	buf[rc] = '\0';

	rc = init_cpu_set_from_str(set, buf, 0);
	if (rc)
		return -EINVAL;

	for (off = 0; off < MAP_SIZE; off++) {
		if (set->free_cpus.map[off])
			count += __builtin_popcountl(set->free_cpus.map[off]);
	}
	data = calloc(count, sizeof(*data));
	if (!data) {
		ERROR("Cannot malloc");
		return -ENOMEM;
	}
	for (off = 0; off < count; off++) {
		int node = get_next_cpu(set);
		data[off].numa_id = node;
		rc = init_numa_cpu_set(&data[off].cpuset, node, 0);
		if (rc)
			return -EINVAL;
	}
	*pnuma_data = data;
	return count;
}

unsigned int get_next_numa_rr_cpu(void)
{
	static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
	static bool init_done = false;
	static int nodes = 0;
	static struct numa_cpu_set nodes_set;
	static numa_node_t *nodes_data = NULL;
	static int next_numa_idx = 0;
	int res;

	pthread_mutex_lock(&lock);
	if (!init_done) {
		nodes = init_numa_nodes(&nodes_set, &nodes_data);
		init_done = true;
		if (nodes < 0) {
			nodes = 0;
			pthread_mutex_unlock(&lock);
			return -1;
		}
	}
	res = get_next_cpu(&nodes_data[next_numa_idx].cpuset);
	INFO("Selected CPU %d from numa ID %d", res, nodes_data[next_numa_idx].numa_id);
	next_numa_idx++;
	if (next_numa_idx == nodes)
		next_numa_idx = 0;
	pthread_mutex_unlock(&lock);
	return res;
}
