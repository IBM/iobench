/*
 * <copyright-info>
 * IBM Confidential
 * OCO Source Materials
 * 2810
 * Author: Constantine Gavrilov <constg@il.ibm.com>
 * (C) Copyright IBM Corp. 2016, 2020
 * The source code for this program is not published or otherwise
 * divested of its trade secrets, irrespective of what has
 * been deposited with the U.S. Copyright Office
 * </copyright-info>
 */
#include "logger.h"
#include <stdarg.h>

void set_logger(log_func_t logger)
{
	default_logger = logger;
}

static int stderr_logger(log_facility_t facility, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
static int stderr_logger(log_facility_t facility, const char *fmt, ...)
{
	va_list ap;
	int rc;
	va_start(ap, fmt);
	char hdr_fmt[strlen(fmt)+16];
	switch(facility) {
		case FACILITY_DBG: sprintf(hdr_fmt, "DBG: %s",fmt); break;
		case FACILITY_INF: sprintf(hdr_fmt, "INF: %s",fmt); break;
		case FACILITY_WRN: sprintf(hdr_fmt, "WRN: %s",fmt); break;
		case FACILITY_ERR: sprintf(hdr_fmt, "ERR: %s",fmt); break;
		default: sprintf(hdr_fmt, "%s", fmt); break;
	}
	rc = vfprintf(stderr, hdr_fmt, ap);
	va_end(ap);
	return rc;
}

log_func_t default_logger = &stderr_logger;
