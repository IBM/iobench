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

#ifndef _IO_BENCH_SCSI_H_
#define _IO_BENCH_SCSI_H_

#include <stdint.h>
#include <unistd.h>
#include <errno.h>
#include <scsi/sg.h>
#include <arpa/inet.h>
#include <byteswap.h>
#include <endian.h>

#ifndef SG_FLAG_MMAP_IO
#define SG_FLAG_MMAP_IO 4
#endif


#if __BYTE_ORDER == __LITTLE_ENDIAN
#ifndef ntohll
#define ntohll bswap_64
#endif
#ifndef htonll
#define htonll bswap_64
#endif
#elif __BYTE_ORDER == __BIG_ENDIAN
#ifndef ntohll
#define ntohll(x) (x)
#endif
#ifndef htonll
#define htonll(x) (x)
#endif
#endif

#define SENSE_BUF_LEN (64)
typedef struct {
	int fd;
	char *buf;
	sg_io_hdr_t sg_cmd;
	sg_io_hdr_t sg_resp;
	uint8_t cdb[16];
	unsigned char sense_buf[SENSE_BUF_LEN];
} sg_ioctx_t;

typedef struct {
	const char *name;
	sg_ioctx_t *ioctx;
	uint32_t qs;
	uint32_t bs;
	uint32_t bs_lbas;
	uint32_t lba_size;
	uint64_t capacity;
} sg_open_params_t;

void close_sg_io_contexts(sg_open_params_t *params);
int open_sg_io_contexts(sg_open_params_t *params);


static inline void fill_rw_cdb16(uint8_t *cdb, uint64_t lba, uint32_t sectors)
{
	uint64_t *p1 = (void *)&cdb[2];
	uint32_t *p2 = (void *)&cdb[10];
	*p1 = htonll(lba);
	*p2 = htonl(sectors);
}

static inline int frame_sg_cmd(sg_ioctx_t *sg_ioctx, int direction, uint32_t bs)
{
	sg_ioctx->sg_cmd = (sg_io_hdr_t) {
		.interface_id = 'S',
		.dxfer_direction = direction,
		.cmd_len = 16,
		.mx_sb_len = sizeof(sg_ioctx->sense_buf),
		.iovec_count = 0,
		.dxfer_len = bs,
		.cmdp = sg_ioctx->cdb,
		.sbp = sg_ioctx->sense_buf,
		.timeout = 300000,
		.flags = SG_FLAG_MMAP_IO
	};
	int rc = write(sg_ioctx->fd, &sg_ioctx->sg_cmd, sizeof(sg_ioctx->sg_cmd));
	if (rc <= 0)
		rc = errno ? -errno : -1;
	return rc > 0 ? 0 : rc;

} 
static inline int frame_sg_read_io(sg_ioctx_t *sg_ioctx, uint64_t lba, uint32_t sectors, uint32_t bs)
{
	memset(sg_ioctx->cdb, 0, sizeof(sg_ioctx->cdb));
	fill_rw_cdb16(sg_ioctx->cdb, lba, sectors);
	sg_ioctx->cdb[0] = 0x88;
	return frame_sg_cmd(sg_ioctx, SG_DXFER_FROM_DEV, bs);
}

static inline int frame_sg_write_io(sg_ioctx_t *sg_ioctx, uint64_t lba, uint32_t sectors, uint32_t bs)
{
	memset(sg_ioctx->cdb, 0, sizeof(sg_ioctx->cdb));
	fill_rw_cdb16(sg_ioctx->cdb, lba, sectors);
	sg_ioctx->cdb[0] = 0x8a;
	return frame_sg_cmd(sg_ioctx, SG_DXFER_TO_DEV, bs);
}

static int inline decode_sg_status(int rc, sg_io_hdr_t *sg_io)
{
	if (rc > 0) {
		if(!sg_io->status && (sg_io->host_status || sg_io->driver_status))
			rc = -1;
		else
			rc = sg_io->status;
	} else if (rc == 0) {
		rc = -1;
	}
	return rc;
}

#endif
