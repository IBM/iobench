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

static inline bool IS_SET_BIT(unsigned int bit, uint64_t *map)
{
	unsigned int off = bit / 64;

	bit = bit % 64;
	if (off >= MAP_SIZE)
		return false;
	return map[off] & ((1UL << bit));
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
	uint64_t remapped_numas[MAP_SIZE];
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

static int remap_one_numa_node(char *remap, numa_node_t *to_numa, numa_node_t *from_numa, int nodes)
{
	struct numa_cpu_set set;
	unsigned int count = 0, i;
	int rc;

	rc = init_cpu_set_from_str(&set, remap, 0);
	if (rc)
		return rc;
	memcpy(to_numa->remapped_numas, set.free_cpus.map, sizeof(to_numa->remapped_numas));
	for (i = 0; i < MAP_SIZE; i++) {
		if (set.free_cpus.map[i])
			count += __builtin_popcountl(set.free_cpus.map[i]);
	}
	for (i = 0; i < count; i++) {
		int j;
		int numa = get_next_cpu(&set);
		for (j = 0; j < nodes; j++) {
			if (from_numa[j].numa_id == numa) {
				int k;
				for (k = 0; k < MAP_SIZE; k++)
					to_numa->cpuset.free_cpus.map[k] |= from_numa[j].cpuset.free_cpus.map[k];
				break;
			}
		}
	}
	return 0;
}

static int init_remapped_numa_nodes(char *remap, numa_node_t **premap_nodes_data)
{
	int nodes;
	struct numa_cpu_set nodes_set;
	numa_node_t *nodes_data;
	numa_node_t *remap_nodes_data;
	int remap_nodes = 0;
	char *p;
	int parsed = 0;

	*premap_nodes_data = NULL;
	nodes = init_numa_nodes(&nodes_set, &nodes_data);
	if (nodes < 0)
		return nodes;
	p = remap;
	while(p && p[0] != '\0') {
		p = strchr(p, '@');
		if (p) {
			p++;
			remap_nodes++;
		}
	}
	if (!remap_nodes) {
		ERROR("Cannot find a valid numa remap definition");
		free(nodes_data);
		return -EINVAL;
	}
	remap_nodes_data = calloc(remap_nodes, sizeof(remap_nodes_data));
	if (!remap_nodes) {
		ERROR("Cannot malloc for remapped NUMA nodes");
		free(nodes_data);
		return -ENOMEM;
	}
	p = remap;
	while(p && p[0] != '\0' && parsed < remap_nodes) {
		char *cur = p;
		char *tail;
		int node;
		p = strchr(cur, ':');
		if (p) {
			p[0] = '\0';
			p++;
		}
		tail = strchr(cur, '@');
		if (!tail || tail[1] == '\0')
			break;
		tail[0] = '\0';
		tail++;
		if (sscanf(cur, "%d", &node) != 1)
			break;
		remap_nodes_data[parsed].numa_id = node;
		if (remap_one_numa_node(tail, &remap_nodes_data[parsed], nodes_data, nodes))
			break;
		parsed++;
	}
	if (parsed != remap_nodes || p) {
		ERROR("Invalid remap NUMA string");
		free(nodes_data);
		free(remap_nodes_data);
		return -EINVAL;
	}
	free(nodes_data);
	*premap_nodes_data = remap_nodes_data;
	return remap_nodes;
}

unsigned int get_next_remapped_numa_cpu(char *remap, unsigned int numa_id)
{
	static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
	static bool init_done = false;
	static int nodes = 0;
	static numa_node_t *nodes_data = NULL;
	int i;
	unsigned int cpu = -1U;

	if (numa_id == -1U)
		return -1U;

	pthread_mutex_lock(&lock);
	if (!init_done) {
		nodes = init_remapped_numa_nodes(remap, &nodes_data);
		init_done = true;
	}
	if (nodes <= 0) {
		pthread_mutex_unlock(&lock);
		return -1U;
	}
	for (i = 0; i < nodes; i++) {
		if (IS_SET_BIT(numa_id, nodes_data[i].remapped_numas)) {
			cpu = get_next_cpu(&nodes_data[i].cpuset);
			break;
		}
	}
	pthread_mutex_unlock(&lock);
	return cpu;
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
	next_numa_idx++;
	if (next_numa_idx == nodes)
		next_numa_idx = 0;
	pthread_mutex_unlock(&lock);
	return res;
}

unsigned int get_numa_id_of_block_device(char *device)
{
	char *p = strrchr(device, '/');
	int fd;
	char name[PATH_MAX];
	char buf[32];
	int rc;

	if (p)
		p++;
	else
		p = device;

	snprintf(name, sizeof(name), "/sys/class/block/%s/device/numa_node", p);
	fd = open(name, O_RDONLY);
	if (fd < 0) {
		ERROR("Failed to open %s", name);
		return -1U;
	}
	rc = read(fd, buf, sizeof(buf)-1);
	close(fd);
	if (rc <= 0) {
		ERROR("Failed to read %s", name);
		return -1U;
	}
	buf[rc] = '\0';
	if (sscanf(buf, "%d",&rc) != 1) {
		ERROR("Failed to parse %s", name);
		return -1U;
	}
	return rc;
}
