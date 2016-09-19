/**
 * @file client_library.h
 * @author Rastislav Szabo <raszabo@cisco.com>, Lukas Macko <lmacko@cisco.com>,
 *         Milan Lenco <milan.lenco@pantheon.tech>
 * @brief Sysrepo Client Library non-public API.
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

#ifndef CLIENT_LIBRARY_H_
#define CLIENT_LIBRARY_H_

/**
 * @brief Notify sysrepo engine about the installation/removal of an YANG module
 * in the repository directory and instruct it to start/stop using it.
 *
 * @param[in] session Session context acquired with ::sr_session_start call.
 * @param[in] module_name Name of the module to be installed/removed.
 * @param[in] revision Revision to be installed/removed.
 * @param[in] file_name Name of the file to be installed (required if installed is TRUE)
 * @param[in] installed Pass TRUE if the module should be installed, FALSE
 * if it should be removed.
 *
 * @return Error code (SR_ERR_OK on success).
 */
int sr_module_install(sr_session_ctx_t *session, const char *module_name, const char *revision, const char *file_name, bool installed);

/**
 * @brief Notify sysrepo engine about the change in the state of YANG features
 * of an YANG module.
 *
 * @param[in] session Session context acquired with ::sr_session_start call.
 * @param[in] module_name Name of the module where the feature is defined.
 * @param[in] feature_name Name of the feature to be enabled or disabled.
 * @param[in] enabled Pass TRUE if the feature shall be enabled, FALSE if it
 * shall be disabled.
 *
 * @return Error code (SR_ERR_OK on success).
 */
int sr_feature_enable(sr_session_ctx_t *session, const char *module_name, const char *feature_name, bool enabled);

/**
 * @brief Checks (via sysrepo engine) whether the module has *any* enabled subtree.
 *
 * @param[in] session Session context acquired with ::sr_session_start call.
 * @param[in] module_name Name of the module to be checked.
 * @param[out] res TRUE if there is at least one enabled subtree in the module.
 *
 * @return Error code (SR_ERR_OK on success).
 */
int sr_check_enabled_running(sr_session_ctx_t *session, const char *module_name, bool *res);

/**
 * @brief Get first or a next chunk of a gradually downloaded subtree(s).
 * A subtree chunk is also a tree, where the root node is the node referenced by XPath,
 * the next level consists of its children skipping the first "offset" nodes and including
 * at most "child_limit" nodes, while the remaining (depth_limit-2) levels always start with 
 * the first child (offset is ignored) but also impose the same limiitation on the number 
 * of transferred children. The chunk consists of at most "depth_limit" levels.
 * @note Order of child nodes is determined by the schema tree.
 *
 * @param[in] session Session context acquired with ::sr_session_start call.
 * @param[in] xpath @ref xp_page "XPath" identifier referencing the starting point 
 *                  for the next chunk.
 * @param[in] single Set to TRUE if the XPath should reference only a single node.
 *                   Function returns SR_ERR_INVAL_ARG if that is not the case.
 * @param[in] offset Index of the first child to include on the second level of each chunk.
 * @param[in] child_limit Maximum number of children to include. Applied on each level.
 * @param[in] depth_limit The maximum number of levels to include in each chunk.
 * @param[out] chunks Array of chunks (sub-sub-trees) returned by this call.
 * @param[out] chunk_cnt Number of returned chunks.
 *
 * @return Error code (SR_ERR_OK on success).
 */
int sr_get_subtree_chunk(sr_session_ctx_t *session, const char *xpath, bool single,
        size_t offset, size_t child_limit, size_t depth_limit,
        sr_node_t **chunks, size_t *chunk_cnt);

#endif /* CLIENT_LIBRARY_H_ */
