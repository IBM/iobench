/*
 * <copyright-info>
 * IBM Confidential
 * OCO Source Materials
 * 2810
 * Authors:
 *  Constantine Gavrilov <constg@il.ibm.com>
 * (C) Copyright IBM Corp. 2016, 2020
 * The source code for this program is not published or otherwise
 * divested of its trade secrets, irrespective of what has
 * been deposited with the U.S. Copyright Office.
 * </copyright-info>
 */

#ifndef _IOBENCH_LOGGER_H_
#define _IOBENCH_LOGGER_H_

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

#endif
