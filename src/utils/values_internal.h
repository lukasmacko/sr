/**
 * @file values_internal.h
 * @author Rastislav Szabo <raszabo@cisco.com>, Lukas Macko <lmacko@cisco.com>,
 *         Milan Lenco <milan.lenco@pantheon.tech>
 * @brief Internal functions for simplified manipulation with Sysrepo values.
 *
 * @copyright
 * Copyright 2016 Cisco Systems, Inc.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *    http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#ifndef VALUES_INTERNAL_H_
#define VALUES_INTERNAL_H_

/**
 * @brief Copy data from the *source* sysrepo value to the *dest* sysrepo value.
 *
 * @param [in] dest Destination sysrepo value.
 * @param [in] source Source sysrepo value.
 */
int sr_dup_val_data(sr_val_t *dest, const sr_val_t *source);

/**
 * @brief Duplicate value (with or without Sysrepo memory context) into a new
 * instance with memory context. It is possible to specify the destination memory context
 * or let the function to create a new one.
 *
 * @param [in] value Sysrepo value to duplicate
 * @param [in] sr_mem_dest Destination memory context.
 *                         If NULL, a new context will be created.
 * @param [out] value_dup_p Returned duplicate of the input value.
 */
int sr_dup_val_ctx(const sr_val_t *value, sr_mem_ctx_t *sr_mem_dest, sr_val_t **value_dup_p);

/**
 * @brief Duplicate values (with or without Sysrepo memory context) into a new
 * array with memory context. It is possible to specify the destination memory context
 * or let the function to create a new one.
 *
 * @param [in] values Array of sysrepo values to duplicate
 * @param [in] count Size of the array to duplicate.
 * @param [in] sr_mem_dest Destination memory context.
 *                         If NULL, a new context will be created.
 * @param [out] values_dup_p Returned duplicate of the input array.
 */
int sr_dup_values_ctx(const sr_val_t *values, size_t count, sr_mem_ctx_t *sr_mem_dest, sr_val_t **values_dup_p);

/**
 * @brief Print sysrepo value in the given context.
 *
 * @param [in] print_ctx Context for printing.
 * @param [in] value Sysrepo value to print.
 */
int sr_print_val_ctx(sr_print_ctx_t *print_ctx, const sr_val_t *value);

/**
 * @brief Stores data of string type into the Sysrepo value data. The actual data
 * will be built from the a format string and a variable arguments list.
 *
 * @param [in] value Sysrepo value to edit.
 * @param [in] type Exact type of the data.
 * @param [in] format Format string used to build the data.
 * @param [in] args List of variable arguments to the format string.
 */
int sr_val_build_str_data_va(sr_val_t *value, sr_type_t type, const char *format, va_list args);

#endif /* VALUES_INTERNAL_H_ */
