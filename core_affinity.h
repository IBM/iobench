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

struct numa_cpu_set;

size_t get_numa_set_size(void);
int init_numa_cpu_set(struct numa_cpu_set *set, unsigned int numa_id, unsigned int first_cpu);
int init_cpu_set_from_str(struct numa_cpu_set *set, char *str, unsigned int first_cpu);
unsigned int get_next_cpu(struct numa_cpu_set *set);
int set_thread_affinity(unsigned int cpu);
unsigned int get_next_numa_rr_cpu(void);
unsigned int get_next_remapped_numa_cpu(char *remap, unsigned int numa_id);
unsigned int get_numa_id_of_block_device(char *device);

#endif
