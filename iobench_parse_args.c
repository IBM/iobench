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

#define usage() \
do { \
	ERROR("Use as %s [-bs block_size] [-qs queue_size]  [-fail-on-err] [ -seq ] [-hit-size value] [-t run_time_sec] [-write | -wp value] [ -engine aio|aio_linux|scsi|nvme ] dev_list]", prog_name); \
	return -1; \
} while(0)


int io_bench_parse_args(int argc, char **argv, io_bench_params_t *params)
{
	const char *prog_name = argv[0];
	argc--;
	argv++;
	memset(params, 0, sizeof(*params));
	params->engine = ENGINE_INVALID;

	while (argc) {
		int dec = 2;
		if (!strcmp(argv[0], "-bs")) {
			if (params->bs || argc == 1 || sscanf(argv[1], "%u", &params->bs) != 1)
				usage();
		} else if (!strcmp(argv[0], "-qs")) {
			if (params->qs || argc == 1 || sscanf(argv[1], "%hu", &params->qs) != 1)
				usage();
		} else if (!strcmp(argv[0], "-t")) {
			if (params->run_time || argc == 1 || sscanf(argv[1], "%u", &params->run_time) != 1)
				usage();
		} else if (!strcmp(argv[0], "-hit-size")) {
			char tail[strlen(argv[1])+1];
			if (params->hit_size || argc == 1)
				usage();
			if (sscanf(argv[1], "%lu%s",  &params->hit_size, tail) == 2) {
				if (!strcmp(tail, "G"))
					params->hit_size <<= 30;
				else if (!strcmp(tail, "M"))
					params->hit_size <<= 20;
				else if (!strcmp(tail, "K"))
					params->hit_size <<= 10;
				else
					usage();
			} else if (sscanf(argv[1], "%lu",  &params->hit_size) != 1) {
				usage();
			}
		} else if (!strcmp(argv[0], "-wp")) {
			if (params->wp || argc == 1 || sscanf(argv[1], "%hhu", &params->wp) != 1)
				usage();
		} else if (!strcmp(argv[0], "-fail-on-err")) {
			if (params->fail_on_err)
				usage();
			params->fail_on_err = true; dec = 1;
		} else if (!strcmp(argv[0], "-seq")) {
			if (params->seq)
				usage();
			params->seq = true; dec = 1;
		} else if (!strcmp(argv[0], "-write")) {
			if (params->wp)
				usage();
			params->wp = 100; dec = 1;
		} else if (!strcmp(argv[0], "-engine")) {
			if (params->engine != ENGINE_INVALID || argc == 1)
				usage();
			if (!strcmp(argv[1], "aio"))
				params->engine = ENGINE_AIO;
			else if (!strcmp(argv[1], "aio_linux"))
				params->engine = ENGINE_AIO_LINUX;
			else if (!strcmp(argv[1], "scsi"))
				params->engine = ENGINE_SCSI;
			else if (!strcmp(argv[1], "nvme"))
				params->engine = ENGINE_NVNE;
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
	if (params->engine == ENGINE_INVALID)
		INFO("Falling back to Linux DIO AIO");
	if (params->hit_size && (params->hit_size < params->bs)) {
		ERROR("Invalid hit size -- cannot be smaller than IO size");
		usage();
	}
	return 0;
}
