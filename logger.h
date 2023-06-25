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

#ifndef _IOBENCH_LOGGER_H_
#define _IOBENCH_LOGGER_H_

#include <time.h>
#include <stdint.h>

typedef enum {
	FACILITY_DBG,
	FACILITY_INF,
	FACILITY_WRN,
	FACILITY_ERR
} log_facility_t;

typedef int (*log_func_t)(log_facility_t facility, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
extern log_func_t default_logger;
void set_logger(log_func_t logger);

#ifndef ERROR
#include <stdio.h>
#include <string.h>
#ifdef _USE_DEBUG_PFX_
#define DECLARE_BFN \
static const char *get_base_file_name(void) \
{ \
	static const char *f = NULL; \
	if(!f) { \
		f = strrchr(__FILE__, '/'); \
		if(f) \
			f++; \
		else \
			f = __FILE__; \
	} \
	return f; \
}

#define ASSERT(expr) \
do { \
	if (!(expr)) { \
		ERROR("Assertion %s failed at %s:%u", #expr, __FILE__, __LINE__); \
		abort(); \
	} \
} while (0)

#define DEBUG(fmt, args...) default_logger(FACILITY_DBG, "%s@%s@%u: "fmt"\n", __FUNCTION__, get_base_file_name(), __LINE__, ##args)
#define INFO(fmt, args...)  default_logger(FACILITY_INF, "%s@%s@%u: "fmt"\n", __FUNCTION__, get_base_file_name(), __LINE__, ##args)
#define INFO_NOPFX(fmt, args...)  default_logger(FACILITY_INF, fmt"\n",  ##args)
#define WARN(fmt, args...) default_logger(FACILITY_WRN,  "%s@%s@%u: "fmt"\n", __FUNCTION__, get_base_file_name(), __LINE__, ##args)
#define ERROR(fmt, args...) default_logger(FACILITY_ERR, "%s@%s@%u: "fmt"\n", __FUNCTION__, get_base_file_name(), __LINE__, ##args)
#else
#define DECLARE_BFN
#define INFO_NOPFX INFO
#define DEBUG(fmt, args...) default_logger(FACILITY_DBG, fmt"\n", ##args)
#define INFO(fmt, args...)  default_logger(FACILITY_INF, fmt"\n", ##args)
#define WARN(fmt, args...) default_logger(FACILITY_WRN,  fmt"\n", ##args)
#define ERROR(fmt, args...) default_logger(FACILITY_ERR, fmt"\n", ##args)
#endif
#endif

#define SET_ERR_VAL(err, def) \
do { \
	if(err == -1) err = errno ? -errno : -def; \
	else if(err > 0) err = -err; \
} while(0)


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


#endif
