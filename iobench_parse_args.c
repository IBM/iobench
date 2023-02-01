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
#include <stdlib.h>
#include <errno.h>

#define usage() \
do { \
	ERROR("Use as %s [-bs block_size] [-qs queue_size]  [-fail-on-err] [-seq] [-mlock] [-rr [-threads n] |-pass-once] [-hit-size value] [-pf pattern_file] [-t run_time_sec] " \
	"[-numa |-cpuset set | -remap-numa numa@numa_list[:numa@numa_list]...] [-poll-idle-kernel-ms value] [-poll-idle-user-ms value] [-poll-kcpu-offset val] " \
	"[-threads-per-dev] [-write | -wp value] [ -engine aio|aio_linux|uring|sg_aio|sg_uring|nvme|dio ] [-kiops kiops] [-max-lease-ms val] dev_list]", prog_name); \
	return -1; \
} while(0)


int io_bench_parse_args(int argc, char **argv, io_bench_params_t *params)
{
	const char *prog_name = argv[0];
	argc--;
	argv++;
	memset(params, 0, sizeof(*params));
	params->engine = ENGINE_INVALID;
	params->threads_per_dev = 1;

	while (argc) {
		int dec = 2;
		if (!strcmp(argv[0], "-bs")) {
			char tail[strlen(argv[1])+1];
			if (params->bs || argc == 1)
				usage();
			if (sscanf(argv[1], "%u%s",  &params->bs, tail) == 2) {
				if (!strcmp(tail, "M") || !strcmp(tail, "m"))
					params->bs <<= 20;
				else if (!strcmp(tail, "K") || !strcmp(tail, "k"))
					params->bs <<= 10;
				else
					usage();
			} else if (sscanf(argv[1], "%u",  &params->bs) != 1) {
				usage();
			}
		} else if (!strcmp(argv[0], "-qs")) {
			if (params->qs || argc == 1 || sscanf(argv[1], "%hu", &params->qs) != 1)
				usage();
		} else if (!strcmp(argv[0], "-t")) {
			if (params->run_time || argc == 1 || sscanf(argv[1], "%u", &params->run_time) != 1)
				usage();
		} else if (!strcmp(argv[0], "-poll-idle-kernel-ms")) {
			if (params->poll_idle_kernel_ms || argc == 1 || sscanf(argv[1], "%hu", &params->poll_idle_kernel_ms) != 1)
				usage();
		} else if (!strcmp(argv[0], "-poll-idle-user-ms")) {
			if (params->poll_idle_user_ms || argc == 1 || sscanf(argv[1], "%hu", &params->poll_idle_user_ms) != 1)
				usage();
		} else if (!strcmp(argv[0], "-poll-kcpu-offset")) {
			if (params->poll_kcpu_offset || argc == 1 || sscanf(argv[1], "%hu", &params->poll_kcpu_offset) != 1)
				usage();
		} else if (!strcmp(argv[0], "-threads-per-dev")) {
			if (params->threads_per_dev != 1 || argc == 1 || sscanf(argv[1], "%u", &params->threads_per_dev) != 1)
				usage();
		} else if (!strcmp(argv[0], "-hit-size")) {
			char tail[strlen(argv[1])+1];
			if (params->hit_size || argc == 1)
				usage();
			if (sscanf(argv[1], "%lu%s",  &params->hit_size, tail) == 2) {
				if (!strcmp(tail, "G") || !strcmp(tail, "g"))
					params->hit_size <<= 30;
				else if (!strcmp(tail, "M") || !strcmp(tail, "m"))
					params->hit_size <<= 20;
				else if (!strcmp(tail, "K") || !strcmp(tail, "k"))
					params->hit_size <<= 10;
				else
					usage();
			} else if (sscanf(argv[1], "%lu",  &params->hit_size) != 1) {
				usage();
			}
		} else if (!strcmp(argv[0], "-wp")) {
			if (params->wp || argc == 1 || sscanf(argv[1], "%hhu", &params->wp) != 1)
				usage();
		} else if (!strcmp(argv[0], "-cpuset")) {
			if (params->cpuset || params->use_numa || argc == 1)
				usage();
			params->cpuset = argv[1];
		} else if (!strcmp(argv[0], "-remap-numa")) {
			if (params->cpuset || params->remap_numa || argc == 1)
				usage();
			params->remap_numa = argv[1];
		} else if (!strcmp(argv[0], "-pf")) {
			if (params->pf_name || argc == 1)
				usage();
			params->pf_name = argv[1];
		} else if (!strcmp(argv[0], "-kiops")) {
			if (params->kiops || argc == 1 || sscanf(argv[1], "%lf", &params->kiops) != 1)
				usage();
		} else if (!strcmp(argv[0], "-max-lease-ms")) {
			if (params->max_dev_lease_usec || argc == 1 || sscanf(argv[1], "%u", &params->max_dev_lease_usec) != 1)
				usage();
			params->max_dev_lease_usec *= 1000;
		} else if (!strcmp(argv[0], "-fail-on-err")) {
			if (params->fail_on_err)
				usage();
			params->fail_on_err = true; dec = 1;
		} else if (!strcmp(argv[0], "-seq")) {
			if (params->seq)
				usage();
			params->seq = true; dec = 1;
		} else if (!strcmp(argv[0], "-rr")) {
			if (params->rr)
				usage();
			params->rr = true; dec = 1;
		} else if (!strcmp(argv[0], "-write")) {
			if (params->wp)
				usage();
			params->wp = 100; dec = 1;
		} else if (!strcmp(argv[0], "-numa")) {
			if (params->use_numa || params->cpuset)
				usage();
			params->use_numa = true; dec = 1;
		} else if (!strcmp(argv[0], "-mlock")) {
			if (params->mlock)
				usage();
			params->mlock = true; dec = 1;
		} else if (!strcmp(argv[0], "-pass-once")) {
			if (params->pass_once)
				usage();
			params->pass_once = true; dec = 1;
		} else if (!strcmp(argv[0], "-threads")) {
			if (params->threads || argc == 1 || sscanf(argv[1], "%u", &params->threads) != 1)
				usage();
		} else if (!strcmp(argv[0], "-engine")) {
			if (params->engine != ENGINE_INVALID || argc == 1)
				usage();
			if (!strcmp(argv[1], "aio"))
				params->engine = ENGINE_AIO;
			else if (!strcmp(argv[1], "aio_linux"))
				params->engine = ENGINE_AIO_LINUX;
			else if (!strcmp(argv[1], "uring"))
				params->engine = ENGINE_AIO_URING;
			else if (!strcmp(argv[1], "dio"))
				params->engine = ENGINE_DIO;
			else if (!strcmp(argv[1], "sg_aio"))
				params->engine = ENGINE_SG_AIO;
			else if (!strcmp(argv[1], "sg_uring"))
				params->engine = ENGINE_SG_URING;
			else if (!strcmp(argv[1], "nvme"))
				params->engine = ENGINE_NVME;
			else
				usage();
		} else {
			params->devices = argv;
			params->ndevs = argc;
			break;
		}
		argc -= dec;
		argv += dec;
	}
	if (!params->devices)
		usage();
	if (!params->bs)
		params->bs = 4096;
	if (!params->qs)
		params->qs = 16;
	if (params->engine == ENGINE_INVALID) {
		INFO("Falling back to Linux AIO");
		params->engine = ENGINE_AIO_LINUX;
	}
	if (params->hit_size && (params->hit_size < params->bs)) {
		ERROR("Invalid hit size -- cannot be smaller than IO size");
		usage();
	}
	if (params->pf_name && !params->wp)
		params->wp = 100;
	if (params->pass_once) {
		params->seq = true;
		if (params->wp && params->wp != 100) {
			ERROR("iobench: write-once mode is for 100%% write only");
			usage();
		}
		params->wp = 100;
		if (params->rr) {
			ERROR("iobench: write-once mode is not compatible with rr mode");
			usage();
		}
	}
	if (params->threads && !params->rr) {
		ERROR("iobench: -threads parameter is valid with -rr only");
		usage();
	}

	if (!params->threads_per_dev)
		params->threads_per_dev = 1;

	if (params->threads && params->threads_per_dev != 1) {
		ERROR("iobench: -threads and -threads-per-dev are mutually exclusive");
		usage();
	}

	if (!params->threads)
		params->threads = params->ndevs;
	else if(params->threads > params->ndevs) {
		WARN("iobench: -threads cannot be larger than number of devices; decreasing to %u", params->ndevs);
		params->threads = params->ndevs;
	}

	if (params->threads_per_dev != 1) {
		unsigned int n = params->ndevs * params->threads_per_dev;
		unsigned int i;
		char **devs = calloc(n, sizeof(char *));
		if (!devs) {
			ERROR("Cannot alloc for device names");
			return -ENOMEM;
		}
		for (i = 0; i < params->ndevs; i++) {
			unsigned int k;
			for (k = 0; k < params->threads_per_dev; k++)
				devs[i * params->threads_per_dev + k] = params->devices[i];
		}
		params->devices = devs;
		params->ndevs = n;
		params->threads = n;
	}
	return 0;
}
