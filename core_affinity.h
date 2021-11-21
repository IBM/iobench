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

#ifndef _CORE_AFFINITY_H_
#define _CORE_AFFINITY_H_

unsigned int get_next_cpu_from_set(char *set_string);
unsigned int get_next_numa_rr_cpu(unsigned int numa_id);
unsigned int get_next_remapped_numa_cpu(char *remap, unsigned int numa_id);
unsigned int get_numa_id_of_block_device(char *device);

int set_thread_affinity(unsigned int cpu);

#endif
