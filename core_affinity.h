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

#ifndef _CORE_AFFINITY_H_
#define _CORE_AFFINITY_H_

unsigned int get_next_cpu_from_set(char *set_string);
unsigned int get_next_numa_rr_cpu(unsigned int numa_id);
unsigned int get_next_remapped_numa_cpu(char *remap, unsigned int numa_id);
unsigned int get_numa_id_of_block_device(char *device);

int set_thread_affinity(unsigned int cpu);

#endif
