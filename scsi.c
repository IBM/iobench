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
#include "scsi.h"
#include <stdlib.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <limits.h>
#include <stdio.h>
#include <dirent.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <string.h>
#include <poll.h>
#include <errno.h>

static int sdx_to_sgx(const char *sdx, char *sgx, size_t name_len)
{
	char path[PATH_MAX];
	char sg_path[PATH_MAX+64];
	int rc = -1;
	char *p;
	int i;
	DIR *dir;
	struct dirent *entry;
	int link_len;

	p = strrchr(sdx, '/');
	if (p)
		p++;
	else
		p = (void *)sdx;
	snprintf(sg_path, sizeof(sg_path), "/sys/block/%s", p);
	link_len = readlink(sg_path, path, sizeof(path)-1);
	if (link_len < 0) {
		if (errno) rc = -errno;
		ERROR("Failed to resolve path %s", sdx);
		goto cleanup;
	}
	path[link_len] = '\0';
	for (i = 0; i < 2; i++) {
		p = strrchr(path, '/');
		if (!p) {
			rc = -EINVAL;
			ERROR("Failed to resolve path %s", path);
			goto cleanup;
		}
		p[0] = '\0';
	}

	if (path[0] != '/' ) 
		snprintf(sg_path, sizeof(sg_path), "/sys/block/%s/scsi_generic", path);
	else
		snprintf(sg_path, sizeof(sg_path), "%s/scsi_generic", path);

	dir = opendir(sg_path);
	if (!dir) {
		if (errno) rc = -errno;
		ERROR("Failed to open %s", sg_path);
		goto cleanup;
	}

	while ((entry = readdir(dir))) {
		if (entry->d_name[0] == 's' && entry->d_name[0] == 's' && entry->d_name[1] == 'g') {
			rc = 0;
			snprintf(sgx, name_len, "/dev/%s", entry->d_name);
			closedir(dir);
			rc = 0;
			goto cleanup;
		}
	}
	ERROR("Failed to find sg device in %s", sg_path);
	rc = -ENOENT;
cleanup:
	return rc;
}

static int map_one_sg_instance(sg_ioctx_t *ioctx, uint32_t map_size)
{
	uint32_t aligned_bs = (map_size + 4095) & (~4095);
	uint32_t used_size;
	int rc = -1;

	if(ioctl(ioctx->fd, SG_GET_RESERVED_SIZE, &used_size) < 0) {
		if (errno) rc = -errno;
		ERROR("SG_GET_RESERVED_SIZE fails");
		return rc;
	}
	if(used_size < aligned_bs && ioctl(ioctx->fd, SG_SET_RESERVED_SIZE, &aligned_bs) < 0) {
		if (errno) rc = -errno;
		ERROR("SG_SET_RESERVED_SIZE fails");
		return rc;
	}
	ioctx->buf = mmap(0, aligned_bs, PROT_READ|PROT_WRITE, MAP_SHARED, ioctx->fd, 0);
	if (ioctx->buf == MAP_FAILED) {
		if (errno) rc = -errno;
		ioctx->buf = NULL;
		ERROR("SG mmap fails");
		return rc;
	}
	return 0;
}

static int open_sg_ioctx(sg_ioctx_t *ioctx, const char *sg_dev, uint32_t map_size)
{
	int rc = -1;
	ioctx->fd = open(sg_dev, O_RDWR|O_NONBLOCK);
	if (ioctx->fd < 0) {
		if (errno) rc = -errno;
		ERROR("Failed to open %s", sg_dev);
		return rc;
	}
	rc = map_one_sg_instance(ioctx, map_size);
	if (rc) {
		close(ioctx->fd);
		ioctx->fd = -1;
	}
	return rc;
}

static void close_sg_ctx(sg_ioctx_t *ioctx, uint32_t map_size)
{
	if (ioctx->fd >= 0) {
		close(ioctx->fd);
		ioctx->fd = -1;
		if (ioctx->buf)
			munmap(ioctx->buf, map_size);
	}
}

void close_sg_io_contexts(sg_open_params_t *params)
{
	uint32_t i;
	if (!params->ioctx)
		return;
	for (i = 0; i < params->qs; i++)
		close_sg_ctx(params->ioctx + i, params->bs);
	free(params->ioctx);
	params->ioctx = NULL;
}


static int sg_get_capacity(sg_open_params_t *params)
{
	typedef struct {
		uint64_t capacity;
		uint32_t sector_size;
	} sg_cap_t;
	sg_cap_t *cap = (void *)params->ioctx[0].buf;
	int rc;

	memset(params->ioctx[0].cdb, 0, sizeof(params->ioctx[0].cdb));
	params->ioctx[0].cdb[0] = 0x9e;
	params->ioctx[0].cdb[1] = 0x10;
	uint32_t *p = (void *)&params->ioctx[0].cdb[10];
	*p = htonl(sizeof(sg_cap_t));
	rc = frame_sg_cmd(&params->ioctx[0], SG_DXFER_FROM_DEV, sizeof(*cap));
	if(rc < 0) {
		ERROR("Failed to request capacity");
		return rc;
	}

	struct pollfd obj = {
		.fd = params->ioctx[0].fd,
		.events = POLLIN
	};
	rc = poll(&obj, 1 , 10000);
	if(rc != 1) {
		if(rc == 0)
			rc = -ETIMEDOUT;
	} else {
		rc = read(params->ioctx[0].fd, &params->ioctx[0].sg_resp, sizeof(params->ioctx[0].sg_resp));
		if (rc <= 0) {
			ERROR("Failed to read capacity response");
		} else {
			rc = decode_sg_status(rc, &params->ioctx[0].sg_resp);
			if (rc)
				ERROR("Get capacity has failed");
		}
	}
	if(!rc) {
		params->capacity = htonll(cap->capacity);
		params->lba_size = htonl(cap->sector_size);
		params->bs_lbas = params->bs / params->lba_size;
		if ( params->bs % params->lba_size) {
			ERROR("Unaligned block size to sector size");
			return -EINVAL;
		}
	}
	return rc;
}

int open_sg_io_contexts(sg_open_params_t *params)
{
	uint32_t i;
	char sg_dev[PATH_MAX];
	int rc;

	params->ioctx = calloc(params->qs, sizeof(*params->ioctx));
	if (!params->ioctx) {
		ERROR("Cannot allocate memorty");
		return -ENOMEM;
	}
	for (i = 0; i < params->qs; i++)
	params->ioctx[i].fd = -1;

	if (!strncmp(params->name, "/dev/sg", 7)) {
		snprintf(sg_dev, sizeof(sg_dev), "%s", params->name);
	} else {
		rc = sdx_to_sgx(params->name, sg_dev, sizeof(sg_dev));
		if (rc)
			return rc;
	}
	for (i = 0; i < params->qs; i++) {
		rc = open_sg_ioctx(params->ioctx + i, sg_dev, params->bs);
		if (rc) {
			close_sg_io_contexts(params);
			return rc;
		}
	}

	rc = sg_get_capacity(params);
	if (rc) {
		close_sg_io_contexts(params);
		return rc;
	}
	return 0;
}
