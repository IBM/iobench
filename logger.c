/* Copyright 2016-2020 IBM Corporation
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
