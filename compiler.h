/*
 * <copyright-info>
 * IBM Confidential
 * OCO Source Materials
 * 2810
 * Author: Constantine Gavrilov <constg@il.ibm.com>
 * (C) Copyright IBM Corp. 2020
 * The source code for this program is not published or otherwise
 * divested of its trade secrets, irrespective of what has
 * been deposited with the U.S. Copyright Office
 * </copyright-info>
 */

#ifndef _COMPILER_H_
#define _COMPILER_H_

#define likely(x)      __builtin_expect(!!(x), 1)

#define unlikely(x)    __builtin_expect(!!(x), 0)

#define compile_assert(__must_be__) ((void)sizeof(char[1 - 2*!(__must_be__)]))
#define force_type(__var__, __forced__type__) compile_assert(__builtin_types_compatible_p(__forced__type__, typeof(__var__)))

#endif
