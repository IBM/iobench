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

#ifndef _COMPILER_H_
#define _COMPILER_H_

#define likely(x)      __builtin_expect(!!(x), 1)

#define unlikely(x)    __builtin_expect(!!(x), 0)

#define compile_assert(__must_be__) ((void)sizeof(char[1 - 2*!(__must_be__)]))
#define force_type(__var__, __forced__type__) compile_assert(__builtin_types_compatible_p(__forced__type__, typeof(__var__)))

#define list_parent_struct(__child_p__, __type__, __child_node__) \
({ \
	__type__ *__res__; \
	void *__entry__ = __child_p__; \
	__entry__ -= __builtin_offsetof(__type__, __child_node__); \
	__res__ = __entry__; \
	__res__; \
})

#endif
