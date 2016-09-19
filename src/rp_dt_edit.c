/**
 * @file rp_dt_edit.c
 * @author Rastislav Szabo <raszabo@cisco.com>, Lukas Macko <lmacko@cisco.com>
 * @brief
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

#include "rp_dt_edit.h"
#include "rp_dt_lookup.h"
#include "rp_dt_xpath.h"
#include "sysrepo.h"
#include "sr_common.h"
#include "access_control.h"
#include <pthread.h>
#include <libyang/libyang.h>

/**
 * @brief Checks if the schema node has a key node with the specified name
 * @param [in] node
 * @param [in] name
 * @param [out] res
 * @return Error code (SR_ERR_OK on success)
 */
static int
rp_dt_has_sch_key(const struct lys_node *node, const char *name, bool *res)
{
    CHECK_NULL_ARG2(name, res);
    if (NULL != node && LYS_LIST == node->nodetype) {
        struct lys_node_list *list = (struct lys_node_list *) node;
        for (size_t k = 0; k < list->keys_size; k++) {
            if (NULL == list->keys[k] || NULL == list->keys[k]->name) {
                SR_LOG_ERR_MSG("Missing schema information");
                return SR_ERR_INTERNAL;
            }
            if (0 == strcmp(name, list->keys[k]->name)) {
                *res = true;
                return SR_ERR_OK;
            }
        }
    }
    *res = false;
    return SR_ERR_OK;
}

/**
 * @brief Checks if the data node has a key with the name and sets res.
 * @param [in] node
 * @param [in] name
 * @param [out] res
 * @return Error code (SR_ERR_OK on success)
 */
static int
rp_dt_has_key(const struct lyd_node *node, const char *name, bool *res)
{
    CHECK_NULL_ARG2(name, res);

    if (NULL != node && NULL != node->schema && LYS_LIST == node->schema->nodetype) {
        return rp_dt_has_sch_key(node->schema, name, res);
    }
    *res = false;
    return SR_ERR_OK;
}

/**
 * @brief Checks whether node can be deleted. Node can not be delete if it is a list key
 * and the list content is not deleted.
 */
static int
rp_dt_check_node_deletion(struct lyd_node *node, struct ly_set *delete_nodes, bool *can_be_removed)
{
    CHECK_NULL_ARG3(node, delete_nodes, can_be_removed);
    int rc = SR_ERR_OK;
    if (NULL != node->schema &&
            LYS_LEAF == node->schema->nodetype) {
        bool is_key = false;
        rc = rp_dt_has_key(node->parent, node->schema->name, &is_key);
        CHECK_RC_MSG_RETURN(rc, "Has key failed");
        if (is_key) {
            //check if the whole list is to be deleted
            struct lyd_node *iter = NULL;

            LY_TREE_FOR(node->parent->child, iter)
            {
                bool found = false;
                for (size_t j = 0; j < delete_nodes->number; j++) {
                    if (iter == delete_nodes->set.d[j]) {
                        found = true;
                        break;
                    }
                }
                if (!found) {
                    *can_be_removed = false;
                    return rc;
                }
            }

        }
    }
    *can_be_removed = true;
    return rc;
}

/**
 * @brief Checks whether all list's children are key nodes. In that case it can be remove
 * even with non-recursive flag.
 */
bool
rp_dt_has_only_keys(const struct lyd_node *node)
{
    int rc = SR_ERR_OK;
    CHECK_NULL_ARG_NORET2(rc, node, node->schema);
    if (SR_ERR_OK != rc) {
        return false;
    }

    if (LYS_LIST == node->schema->nodetype) {
        struct lyd_node *child = node->child;
        struct lys_node_list *list = (struct lys_node_list *) node->schema;
        size_t child_cnt = 0;
        while (NULL != child) {
            child = child->next;
            child_cnt++;
        }
        return child_cnt == list->keys_size;
    }
    return false;
}
/**
 * @brief Tests whether the set of nodes contains a non default node
 */
static bool
rp_dt_contains_non_default_node(struct ly_set *nodes)
{
    if (NULL == nodes) {
        return false;
    }
    for (int i = 0; i < nodes->number; i++) {
        if ((LYS_LEAFLIST | LYS_LIST) & nodes->set.d[i]->schema->nodetype ||
            (LYS_CONTAINER == nodes->set.d[i]->schema->nodetype &&
                 NULL != ((struct lys_node_container *) nodes->set.d[i]->schema)->presence) ||
            (LYS_LEAF == nodes->set.d[i]->schema->nodetype && !nodes->set.d[i]->dflt)) {
            return true;
        } else if (LYS_CONTAINER == nodes->set.d[i]->schema->nodetype) {
            struct lyd_node *next = NULL, *iter = NULL;
            LY_TREE_DFS_BEGIN(nodes->set.d[i], next, iter)
            {
                if ((LYS_LEAFLIST | LYS_LIST) & iter->schema->nodetype ||
                    (LYS_LEAF == iter->schema->nodetype && !iter->dflt) ||
                    (LYS_CONTAINER == iter->schema->nodetype &&
                        NULL != ((struct lys_node_container *) iter->schema)->presence)) {
                    return true;
                }
                LYD_TREE_DFS_END(nodes->set.d[i], next, iter);
            }
        }
    }
    return false;
}

int
rp_dt_delete_item(dm_ctx_t *dm_ctx, dm_session_t *session, const char *xpath, const sr_edit_flag_t options)
{
    CHECK_NULL_ARG3(dm_ctx, session, xpath);

    int rc = SR_ERR_INVAL_ARG;
    dm_data_info_t *info = NULL;
    struct ly_set *nodes = NULL;
    struct ly_set *parents = NULL;
    char *module_name = NULL;
    int ret = 0;

    rc = sr_copy_first_ns(xpath, &module_name);
    CHECK_RC_LOG_RETURN(rc, "Copying module name failed for xpath '%s'", xpath);

    rc = dm_get_data_info(dm_ctx, session, module_name, &info);
    free(module_name);
    CHECK_RC_LOG_RETURN(rc, "Getting data tree failed for xpath '%s'", xpath);

    /* find nodes nodes to be deleted */
    rc = rp_dt_find_nodes(dm_ctx, info->node, xpath, dm_is_running_ds_session(session), &nodes);
    if (SR_ERR_NOT_FOUND == rc) {
        rc = rp_dt_validate_node_xpath(dm_ctx, session, xpath, NULL, NULL);
        if (SR_ERR_OK != rc) {
            SR_LOG_WRN("Validation of xpath %s was not successful", xpath);
        }

        if (SR_EDIT_STRICT & options) {
            SR_LOG_ERR("No nodes to be deleted with strict option %s", xpath);
            return dm_report_error(session, NULL, xpath, SR_ERR_DATA_MISSING);
        } else {
            return SR_ERR_OK;
        }
    } else if (SR_ERR_OK != rc) {
        SR_LOG_ERR("Find nodes failed %s", xpath);
        return rc;
    }

    /* if strict option is set, at least one non default node must be deleted */
    if (SR_EDIT_STRICT & options && !rp_dt_contains_non_default_node(nodes)) {
        SR_LOG_ERR("No nodes to be deleted with strict option %s", xpath);
        rc = dm_report_error(session, NULL, xpath, SR_ERR_DATA_MISSING);
        goto cleanup;
    }

    /* list key can be deleted only when the whole list is deleted */
    for (size_t i = 0; i < nodes->number; i++) {
        bool can_be_deleted = false;
        rc = rp_dt_check_node_deletion(nodes->set.d[i], nodes, &can_be_deleted);
        CHECK_RC_LOG_GOTO(rc, cleanup, "Check node deletion failed %s", xpath);

        if (!can_be_deleted) {
            SR_LOG_ERR("Key leaf can not be delete delete the list instead %s", xpath);
            rc = dm_report_error(session, "List key can not be deleted", xpath, SR_ERR_INVAL_ARG);
            goto cleanup;
        }
    }

    /* check edit flags */
    if (SR_EDIT_NON_RECURSIVE & options) {
        for (size_t i = 0; i < nodes->number; i++) {
            if ((nodes->set.d[i]->schema->nodetype & (LYS_LIST | LYS_CONTAINER)) &&
                    !rp_dt_has_only_keys(nodes->set.d[i])) {
                SR_LOG_ERR("List of the nodes to be deleted contains list or container with non recursive opt %s", xpath);
                rc = dm_report_error(session, NULL, xpath, SR_ERR_DATA_EXISTS);
                goto cleanup;
            }
        }
    }

    parents = ly_set_new();
    CHECK_NULL_NOMEM_GOTO(parents, rc, cleanup);

    /* unlink nodes and save their parents */
    for (size_t i = 0; i < nodes->number; i++) {
        if (NULL != nodes->set.d[i]->parent) {
            ly_set_add(parents, nodes->set.d[i]->parent, 0);
        }

        ret = sr_lyd_unlink(info, nodes->set.d[i]);
        CHECK_ZERO_LOG_GOTO(ret, rc, SR_ERR_INTERNAL, cleanup, "Unlinking of the node %s failed", xpath);
    }

    /* remove parents that are to be deleted by query */
    for (size_t i = 0; i < parents->number; i++) {
        bool will_be_deleted = false;
        for (size_t j = 0; j < nodes->number; j++) {
            if (parents->set.d[i] == nodes->set.d[j]) {
                will_be_deleted = true;
                break;
            }
        }
        if (will_be_deleted) {
            ly_set_rm_index(parents, i);
            i--;
        }
    }

    /* free nodes */
    for (size_t i = 0; i < nodes->number; i++) {
        lyd_free_withsiblings(nodes->set.d[i]);
    }

    /* remove empty parent container/list nodes */
    for (size_t i = 0; i < parents->number; i++) {
        struct lyd_node *node = parents->set.d[i];
        struct lys_node *schema = node->schema;
        struct lyd_node *parent = NULL;

        while (NULL != node) {
            if (NULL == node->child &&
                ((LYS_LIST & node->schema->nodetype) ||
                 ((LYS_CONTAINER & node->schema->nodetype) && NULL == ((struct lys_node_container *)schema)->presence))) {
                /* list or non-presence container with no children */
                parent = node->parent;
                sr_lyd_unlink(info, node);
                lyd_free(node);
                node = parent;
            } else {
                break;
            }
        }
    }
cleanup:
    ly_set_free(parents);
    ly_set_free(nodes);
    /* mark to session copy that some change has been made */
    info->modified = SR_ERR_OK == rc ? true : info->modified;
    return rc;
}

int
rp_dt_set_item(dm_ctx_t *dm_ctx, dm_session_t *session, const char *xpath, const sr_edit_flag_t options, const sr_val_t *value)
{
    CHECK_NULL_ARG3(dm_ctx, session, xpath);
    /* value can be NULL if the list is created */
    int rc = SR_ERR_OK;
    char *new_value = NULL;

    const struct lys_module *module = NULL;
    struct lys_node *sch_node = NULL;
    dm_data_info_t *info = NULL;
    dm_schema_info_t *schema_info = NULL;
    struct lyd_node *node = NULL;

    /* validate xpath */
    rc = rp_dt_validate_node_xpath_lock(dm_ctx, session, xpath, &schema_info, &sch_node);
    if (SR_ERR_OK != rc) {
        SR_LOG_ERR("Requested node is not valid %s", xpath);
        return rc;
    }
    module = schema_info->module;
    if (NULL == sch_node) {
        SR_LOG_ERR("Node can not be created or update %s", xpath);
        pthread_rwlock_unlock(&schema_info->model_lock);
        return SR_ERR_INVAL_ARG;
    }

    /* get data tree to be update */
    rc = dm_get_data_info(dm_ctx, session, module->name, &info);
    if (SR_ERR_OK != rc) {
        pthread_rwlock_unlock(&schema_info->model_lock);
    }

    CHECK_RC_LOG_RETURN(rc, "Getting data tree failed for xpath '%s'", xpath);

    /* check if node is enabled */
    if (dm_is_running_ds_session(session)) {
        if (!dm_is_enabled_check_recursively(sch_node)) {
            SR_LOG_ERR("The node is not enabled in running datastore %s", xpath);
            pthread_rwlock_unlock(&schema_info->model_lock);
            return SR_ERR_INVAL_ARG;
        }
    }

    pthread_rwlock_unlock(&schema_info->model_lock);

    /* non-presence container can not be created */
    if (LYS_CONTAINER == sch_node->nodetype && NULL == ((struct lys_node_container *) sch_node)->presence) {
        SR_LOG_ERR("Non presence container can not be created %s", xpath);
        return SR_ERR_INVAL_ARG;
    }

    /* key node can not be created, create list instead*/
    if (LYS_LEAF == sch_node->nodetype) {
        bool is_key = false;
        rc = rp_dt_has_sch_key(sch_node->parent, sch_node->name, &is_key);
        CHECK_RC_MSG_RETURN(rc, "Has key failed");

        if (is_key) {
            SR_LOG_ERR("Value of the key can not be set %s", xpath);
            return dm_report_error(session, "Value of the key can not be set", xpath, SR_ERR_INVAL_ARG);
        }
    }

    /* transform new value from sr_val_t to string */
    if (NULL != value) {
        rc = sr_val_to_str(value, sch_node, &new_value);
        CHECK_RC_MSG_RETURN(rc, "Copy new value to string failed");
    } else if (!((LYS_CONTAINER | LYS_LIST) & sch_node->nodetype) &&
            !(LYS_LEAFLIST == sch_node->nodetype && NULL != strstr(xpath, "[.='") && ']' == xpath[strlen(xpath)-1])) {
        /* value can be NULL only if a presence container, list or leaf-list with predicated is being created */
        SR_LOG_ERR_MSG("Argument value not passed");
        return SR_ERR_INVAL_ARG;
    }

    /* non-recursive flag */
    if (SR_EDIT_NON_RECURSIVE & options) {
        if (NULL != sch_node->parent) {
            char *last_slash = rindex(xpath, '/');
            CHECK_NULL_NOMEM_GOTO(last_slash, rc, cleanup);
            char *parent_node = strndup(xpath, last_slash - xpath);
            CHECK_NULL_NOMEM_GOTO(parent_node, rc, cleanup);
            struct ly_set *res = lyd_find_xpath(info->node, parent_node);
            free(parent_node);
            if (NULL == res || 0 == res->number) {
                SR_LOG_ERR("A preceding node is missing '%s' create it or omit the non recursive option", xpath);
                ly_set_free(res);
                free(new_value);
                return dm_report_error(session, "A preceding node is missing", xpath, SR_ERR_DATA_MISSING);
            }
            ly_set_free(res);
        }
    }

    /* strict flag */
    int flags = (SR_EDIT_STRICT & options) ? 0 : LYD_PATH_OPT_UPDATE;

    /* setting a leaf with default value should pass even with SR_EDIT_STRICT */
    if ((SR_EDIT_STRICT & options) && sch_node->nodetype == LYS_LEAF && ((struct lys_node_leaf *) sch_node)->dflt != NULL) {
        rc = rp_dt_find_node(dm_ctx, info->node, xpath, dm_is_running_ds_session(session), &node);
        if (SR_ERR_NOT_FOUND != rc) {
            CHECK_RC_LOG_GOTO(rc, cleanup, "Default node %s not found", xpath);
        } else {
            /* if leaf does not exists, it is ok LYD_PATH_OPT_UPDATE doesn't need to be added */
            rc = SR_ERR_OK;
        }
        if (NULL != node && 0 == strcmp(((struct lyd_node_leaf_list *) node)->value_str, ((struct lys_node_leaf *) sch_node)->dflt)) {
            /* add update flag */
            flags |= LYD_PATH_OPT_UPDATE;
        }
    }


    /* create or update */
    ly_errno = 0;
    node = dm_lyd_new_path(info, xpath, new_value, flags);
    if (NULL == node && LY_SUCCESS != ly_errno) {
        SR_LOG_ERR("Setting of item failed %s %d", xpath, ly_vecode);
        if (LYVE_PATH_EXISTS == ly_vecode) {
            rc = SR_ERR_DATA_EXISTS;
        } else if (LY_EVALID == ly_errno) {
            rc = SR_ERR_INVAL_ARG;
        } else {
            rc = SR_ERR_INTERNAL;
        }
    }

    /* remove default tag if the default value has been explicitly set or overwritten */
    if (SR_ERR_OK == rc && sch_node->nodetype == LYS_LEAF && ((struct lys_node_leaf *) sch_node)->dflt != NULL) {
        if (NULL == node) {
            rc = rp_dt_find_node(dm_ctx, info->node, xpath, dm_is_running_ds_session(session), &node);
            CHECK_RC_LOG_GOTO(rc, cleanup, "Created node %s not found", xpath);
        }
        node->dflt = 0;
    }

cleanup:
    free(new_value);
    if (NULL != info) {
        info->modified = SR_ERR_OK == rc ? true : info->modified;
    }
    return rc;
}

int
rp_dt_move_list(dm_ctx_t *dm_ctx, dm_session_t *session, const char *xpath, sr_move_position_t position, const char *relative_item)
{
    CHECK_NULL_ARG3(dm_ctx, session, xpath);
    int rc = SR_ERR_OK;
    struct lyd_node *node = NULL;
    struct lyd_node *sibling = NULL;
    dm_schema_info_t *schema_info = NULL;
    dm_data_info_t *info = NULL;

    rc = rp_dt_validate_node_xpath_lock(dm_ctx, session, xpath, &schema_info, NULL);
    CHECK_RC_LOG_RETURN(rc, "Requested node is not valid %s", xpath);

    rc = dm_get_data_info(dm_ctx, session, schema_info->module_name, &info);
    pthread_rwlock_unlock(&schema_info->model_lock);
    CHECK_RC_LOG_RETURN(rc, "Getting data tree failed for xpath '%s'", xpath);


    rc = rp_dt_find_node(dm_ctx, info->node, xpath, dm_is_running_ds_session(session), &node);
    if (SR_ERR_NOT_FOUND == rc) {
        SR_LOG_ERR("List not found %s", xpath);
        return SR_ERR_INVAL_ARG;
    } else if (SR_ERR_OK != rc) {
        SR_LOG_ERR("Find node failed %s", xpath);
        return rc;
    }

    if (!((LYS_LIST | LYS_LEAFLIST) & node->schema->nodetype) || (!(LYS_USERORDERED & node->schema->flags))) {
        SR_LOG_ERR("Xpath %s does not identify the user ordered list or leaf-list", xpath);
        return SR_ERR_INVAL_ARG;
    }

    if ((SR_MOVE_AFTER == position || SR_MOVE_BEFORE == position) && NULL != relative_item) {
        rc = rp_dt_find_node(dm_ctx, info->node, relative_item, dm_is_running_ds_session(session), &sibling);
        if (SR_ERR_NOT_FOUND == rc) {
            rc = dm_report_error(session, "Relative item for move operation not found", relative_item, SR_ERR_INVAL_ARG);
            goto cleanup;
        } else if (SR_ERR_OK != rc) {
            SR_LOG_ERR_MSG("Find the closest sibling failed");
            return rc;
        }
    } else {
        struct ly_set *siblings = lyd_find_instance(info->node, node->schema);

        if (NULL == siblings || 0 == siblings->number) {
            SR_LOG_ERR_MSG("No siblings found");
            return SR_ERR_INVAL_ARG;
        }
        if (SR_MOVE_FIRST == position) {
            sibling = siblings->set.d[0];
        } else if (SR_MOVE_LAST == position) {
            sibling = siblings->set.d[siblings->number - 1];
        }
        ly_set_free(siblings);
    }

    if (NULL == sibling || !((LYS_LIST | LYS_LEAFLIST) & sibling->schema->nodetype) || (!(LYS_USERORDERED & sibling->schema->flags)) || (node->schema != sibling->schema)) {
        SR_LOG_ERR("Xpath %s does not identify the user ordered list or leaf-list or sibling node", xpath);
        return SR_ERR_INVAL_ARG;
    }

    if (SR_MOVE_FIRST == position) {
        rc = sr_lyd_insert_before(info, sibling, node);
    } else if (SR_MOVE_LAST == position) {
        rc = sr_lyd_insert_after(info, sibling, node);
    } else if (SR_MOVE_BEFORE == position) {
        rc = sr_lyd_insert_before(info, sibling, node);
    } else if (SR_MOVE_AFTER == position) {
        rc = sr_lyd_insert_after(info, sibling, node);
    }

    CHECK_RC_MSG_GOTO(rc, cleanup, "Moving of the node failed");

cleanup:
    info->modified = SR_ERR_OK == rc ? true : info->modified;
    return rc;
}

int
rp_dt_move_list_wrapper(rp_ctx_t *rp_ctx, rp_session_t *session, const char *xpath, sr_move_position_t position, const char *relative_item)
{
    CHECK_NULL_ARG5(rp_ctx, rp_ctx->dm_ctx, session, session->dm_session, xpath);

    int rc = SR_ERR_OK;

    SR_LOG_INF("Move item request %s datastore, xpath: %s", sr_ds_to_str(session->datastore), xpath);

    rc = ac_check_node_permissions(session->ac_session, xpath, AC_OPER_READ_WRITE);
    CHECK_RC_LOG_RETURN(rc, "Access control check failed for xpath '%s'", xpath);

    rc = dm_add_operation(session->dm_session, DM_MOVE_OP, xpath, NULL, 0, position, relative_item);
    CHECK_RC_MSG_RETURN(rc, "Adding operation to session op list failed");

    rc = rp_dt_move_list(rp_ctx->dm_ctx, session->dm_session, xpath, position, relative_item);
    if (SR_ERR_OK != rc) {
        SR_LOG_ERR_MSG("List move failed");
        dm_remove_last_operation(session->dm_session);
    }
    return rc;

}

int
rp_dt_set_item_wrapper(rp_ctx_t *rp_ctx, rp_session_t *session, const char *xpath, sr_val_t *val, sr_edit_options_t opt)
{
    CHECK_NULL_ARG5(rp_ctx, rp_ctx->dm_ctx, session, session->dm_session, xpath);

    int rc = SR_ERR_OK;

    SR_LOG_INF("Set item request %s datastore, xpath: %s", sr_ds_to_str(session->datastore), xpath);

    rc = ac_check_node_permissions(session->ac_session, xpath, AC_OPER_READ_WRITE);
    if (SR_ERR_OK != rc) {
        SR_LOG_ERR("Access control check failed for xpath '%s'", xpath);
        sr_free_val(val);
        return rc;
    }

    rc = dm_add_operation(session->dm_session, DM_SET_OP, xpath, val, opt, 0, NULL);
    /* val is freed by dm_add_operation */
    CHECK_RC_MSG_RETURN(rc, "Adding operation to session op list failed");

    rc = rp_dt_set_item(rp_ctx->dm_ctx, session->dm_session, xpath, opt, val);
    if (SR_ERR_OK != rc) {
        SR_LOG_ERR_MSG("Set item failed");
        dm_remove_last_operation(session->dm_session);
    }
    return rc;
}

int
rp_dt_delete_item_wrapper(rp_ctx_t *rp_ctx, rp_session_t *session, const char *xpath, sr_edit_options_t opts)
{
    CHECK_NULL_ARG5(rp_ctx, rp_ctx->dm_ctx, session, session->dm_session, xpath);
    int rc = SR_ERR_OK;

    SR_LOG_INF("Delete item request %s datastore, xpath: %s", sr_ds_to_str(session->datastore), xpath);

    rc = ac_check_node_permissions(session->ac_session, xpath, AC_OPER_READ_WRITE);
    CHECK_RC_LOG_RETURN(rc, "Access control check failed for xpath '%s'", xpath);

    rc = dm_add_operation(session->dm_session, DM_DELETE_OP, xpath, NULL, opts, 0, NULL);
    CHECK_RC_MSG_RETURN(rc, "Adding operation to session op list failed");

    rc = rp_dt_delete_item(rp_ctx->dm_ctx, session->dm_session, xpath, opts);
    if (SR_ERR_OK != rc) {
        SR_LOG_ERR_MSG("List delete failed");
        dm_remove_last_operation(session->dm_session);
    }
    return rc;
}

/**
 * @brief Perform the list of provided operations on the session. Stops
 * on the first error, if continue on error is false. If the continue on error
 * is set to true, operation is marked with has_error flag.
 * @param [in] ctx
 * @param [in] session
 * @param [in] operations can be null in case of candidate session
 * @param [in] count
 * @param [in] continue_on_error flag denoting whether replay should be stopped on first error
 * @param [in] models_to_skip - set of model's name where the current modify timestamp
 * matches the timestamp of the session copy. Operation for this models skipped.
 * @return Error code (SR_ERR_OK on success)
 */
static int
rp_dt_replay_operations(dm_ctx_t *ctx, dm_session_t *session, dm_sess_op_t *operations, size_t count,
        bool continue_on_error, sr_list_t *models_to_skip)
{
    CHECK_NULL_ARG2(ctx, session);
    int rc = SR_ERR_OK;
    bool err_occured = false; /* flag used in case of continue_on_err */

    for (size_t i = 0; i < count; i++) {
        dm_sess_op_t *op = &operations[i];
        if (op->has_error) {
            continue;
        }
        /* check if the operation should be skipped */
        bool match = false;
        for (unsigned int m = 0; m < models_to_skip->count; m++) {
            if (0 == sr_cmp_first_ns(op->xpath, (char *) models_to_skip->data[m])) {
                SR_LOG_DBG("Skipping op for model %s", (char *) models_to_skip->data[m]);
                match = true;
                break;
            }
        }
        if (match) {
            continue;
        }

        switch (op->op) {
        case DM_SET_OP:
            rc = rp_dt_set_item(ctx, session, op->xpath, op->detail.set.options, op->detail.set.val);
            break;
        case DM_DELETE_OP:
            rc = rp_dt_delete_item(ctx, session, op->xpath, op->detail.del.options);
            break;
        case DM_MOVE_OP:
            rc = rp_dt_move_list(ctx, session, op->xpath, op->detail.mov.position, op->detail.mov.relative_item);
            break;
        }

        if (SR_ERR_OK != rc) {
            SR_LOG_ERR("Replay of operation %zu / %zu failed", i, count);
            if (!continue_on_error) {
                return rc;
            } else {
                op->has_error = true;
                err_occured = true;
            }
        }
    }
    if (continue_on_error && err_occured) {
        return SR_ERR_INTERNAL;
    } else {
        return rc;
    }
}

int
rp_dt_commit(rp_ctx_t *rp_ctx, rp_session_t *session, sr_error_info_t **errors, size_t *err_cnt)
{
    CHECK_NULL_ARG4(rp_ctx, session, errors, err_cnt);
    int rc = SR_ERR_OK;
    dm_commit_context_t *commit_ctx = NULL;

    SR_LOG_DBG_MSG("Commit (1/7): process stared");

    //TODO send validate notifications

    /* YANG validation */
    rc = dm_validate_session_data_trees(rp_ctx->dm_ctx, session->dm_session, errors, err_cnt);
    if (SR_ERR_OK != rc) {
        SR_LOG_ERR("Data validation failed: %s", *err_cnt > 0 ? errors[0]->message : "(no error)");
        return SR_ERR_VALIDATION_FAILED;
    }
    SR_LOG_DBG_MSG("Commit (2/7): validation succeeded");


    rc = dm_commit_prepare_context(rp_ctx->dm_ctx, session->dm_session, &commit_ctx);
    if (SR_ERR_OK != rc) {
        SR_LOG_ERR_MSG("commit prepare context failed");
        return rc;
    } else if (0 == commit_ctx->modif_count) {
        SR_LOG_DBG_MSG("Commit: Finished - no model modified");
        dm_free_commit_context(commit_ctx);
        return SR_ERR_OK;
    }

    /* open all files */
    rc = dm_commit_load_modified_models(rp_ctx->dm_ctx, session->dm_session, commit_ctx);
    if (SR_ERR_OK != rc) {
        SR_LOG_ERR_MSG("Loading of modified models failed");
        goto cleanup;
    }
    SR_LOG_DBG_MSG("Commit (3/7): all modified models loaded successfully");

    /* replay operations */
    rc = rp_dt_replay_operations(rp_ctx->dm_ctx, commit_ctx->session, commit_ctx->operations,
            commit_ctx->oper_count, false, commit_ctx->up_to_date_models);
    if (SR_ERR_OK != rc) {
        SR_LOG_ERR_MSG("Replay of operations failed");
        goto cleanup;
    }
    SR_LOG_DBG_MSG("Commit (4/7): replay of operation succeeded");

    /* validate data trees after merge */
    rc = dm_validate_session_data_trees(rp_ctx->dm_ctx, commit_ctx->session, errors, err_cnt);
    if (SR_ERR_OK != rc) {
        SR_LOG_ERR_MSG("Validation after merging failed");
        rc = SR_ERR_VALIDATION_FAILED;
        goto cleanup;
    }
    SR_LOG_DBG_MSG("Commit (5/7): merged models validation succeeded");

    rc = dm_commit_write_files(session->dm_session, commit_ctx);

    if (SR_ERR_OK == rc) {
        SR_LOG_DBG_MSG("Commit (6/7): data write succeeded");

        rc = dm_commit_notify(rp_ctx->dm_ctx, session->dm_session, commit_ctx);
    }

cleanup:
    /* In case of running datastore, commit context will be freed when
     * all notifications session are closed.
     */
    if (SR_ERR_OK != rc || SR_DS_STARTUP == session->datastore) {
        dm_free_commit_context(commit_ctx);
    }

    if (SR_ERR_OK == rc) {
        /* discard changes in session in next get_data_tree call newly committed content will be loaded */
        if (SR_DS_CANDIDATE != session->datastore) {
            rc = dm_discard_changes(rp_ctx->dm_ctx, session->dm_session);
        } else {
            dm_remove_session_operations(session->dm_session);
            rc = dm_remove_modified_flag(session->dm_session);
        }
        SR_LOG_DBG_MSG("Commit (7/7): finished successfully");
    }
    return rc;
}

static void
rp_dt_create_refresh_errors(const dm_sess_op_t *ops, size_t op_count, sr_error_info_t **errors, size_t *err_cnt)
{
    CHECK_NULL_ARG_VOID3(ops, errors, err_cnt);
    for (size_t i = 0; i < op_count; i++) {
        const dm_sess_op_t *op = &ops[i];
        if (!op->has_error) {
            continue;
        }
        sr_error_info_t *tmp_err = realloc(*errors, (*err_cnt + 1) * sizeof(**errors));
        if (NULL == tmp_err) {
            SR_LOG_ERR_MSG("Memory allocation failed");
            return;
        }
        *errors = tmp_err;
        switch (op->op) {
        case DM_SET_OP:
            (*errors)[*err_cnt].message = strdup("SET operation can not be merged with current datastore state");
            break;
        case DM_DELETE_OP:
            (*errors)[*err_cnt].message = strdup("DELETE Operation can not be merged with current datastore state");
            break;
        case DM_MOVE_OP:
            (*errors)[*err_cnt].message = strdup("MOVE Operation can not be merged with current datastore state");
            break;
        default:
            (*errors)[*err_cnt].message = strdup("An operation can not be merged with current datastore state");
        }
        (*errors)[*err_cnt].xpath = strdup(op->xpath);
        (*err_cnt)++;
    }
}

int
rp_dt_refresh_session(rp_ctx_t *rp_ctx, rp_session_t *session, sr_error_info_t **errors, size_t *err_cnt)
{
    CHECK_NULL_ARG2(rp_ctx, session);
    int rc = SR_ERR_OK;
    sr_list_t *up_to_date = NULL;
    dm_sess_op_t *ops = NULL;
    size_t op_count = 0;
    *err_cnt = 0;
    *errors = NULL;

    SR_LOG_INF("Refresh session request %s datastore", sr_ds_to_str(session->datastore));

    /* update models and retrieve list of data models-to be skipped in replay */
    rc = dm_update_session_data_trees(rp_ctx->dm_ctx, session->dm_session, &up_to_date);
    if (SR_ERR_OK != rc) {
        SR_LOG_ERR_MSG("Update of data trees failed");
        return rc;
    }

    dm_get_session_operations(session->dm_session, &ops, &op_count);

    if (0 == op_count) {
        SR_LOG_INF_MSG("No operation has been performed on this session so far");
        goto cleanup;
    }

    /* replay operations continue on error */
    rc = rp_dt_replay_operations(rp_ctx->dm_ctx, session->dm_session,
            ops, op_count, true, up_to_date);

    if (SR_ERR_OK != rc) {
        /* report errors for the ops that could not be performed */
        rp_dt_create_refresh_errors(ops, op_count, errors, err_cnt);
        /* remove operations that has an error */
        dm_remove_operations_with_error(session->dm_session);
        /* generate errors and remove ops with error */
        SR_LOG_ERR_MSG("Replay of some operations failed");
    }
    SR_LOG_DBG_MSG("End of session refresh");
cleanup:
    sr_list_cleanup(up_to_date);
    return rc;
}

/**
 * @brief Performs copy config to running datastore. It is done by commit to perform
 * all validation and notifications as needed
 * @param [in] rp_ctx
 * @param [in] session
 * @param [in] module_name
 * @param [in] src
 * @return Error code (SR_ERR_OK on success)
 */
static int
rp_dt_copy_config_to_running(rp_ctx_t* rp_ctx, rp_session_t* session, const char* module_name, sr_datastore_t src)
{
    CHECK_NULL_ARG2(rp_ctx, session);
    int rc = SR_ERR_OK;
    sr_list_t *modules = NULL;
    dm_session_t *backup = NULL;
    sr_datastore_t prev_ds = session->datastore;
    dm_data_info_t *info = NULL;
    int first_err = SR_ERR_OK;
    sr_error_info_t *errors = NULL;
    size_t e_cnt = 0;

    /* copy to running is candidate commit behind the scenes */
    rc = dm_session_start(rp_ctx->dm_ctx, session->user_credentials, src, &backup);
    CHECK_RC_MSG_RETURN(rc, "Session start of temporary session failed");

    /* move datatrees & session ops -> backup */
    rc = dm_move_session_tree_and_ops_all_ds(rp_ctx->dm_ctx, session->dm_session, backup);
    CHECK_RC_MSG_GOTO(rc, cleanup_sess_stop, "Moving session data trees failed");

    rc = rp_dt_switch_datastore(rp_ctx, session, src);

    /* load models to be committed to the session */
    if (NULL != module_name) {
        if (SR_DS_CANDIDATE == src) {
            rc = dm_copy_session_tree(rp_ctx->dm_ctx, backup, session->dm_session, module_name);
            CHECK_RC_MSG_GOTO(rc, cleanup, "Copy session data trees failed");
        }
        /* load data tree if it was not copied from backup session */
        rc = dm_get_data_info(rp_ctx->dm_ctx, session->dm_session, module_name, &info);
        CHECK_RC_MSG_GOTO(rc, cleanup, "Get data info failed");
        info->modified = true;
    } else {

        /* load all enabled models */
        if (SR_DS_CANDIDATE == src) {
            rc = dm_copy_modified_session_trees(rp_ctx->dm_ctx, backup, session->dm_session);
            CHECK_RC_MSG_GOTO(rc, cleanup, "Copy session data trees failed");
        }
        rc = dm_get_all_modules(rp_ctx->dm_ctx, session->dm_session, true, &modules);
        CHECK_RC_MSG_GOTO(rc, cleanup, "Get all modules failed");
        for (size_t i = 0; i < modules->count; i++) {
            char *module = modules->data[i];
            rc = dm_get_data_info(rp_ctx->dm_ctx, session->dm_session, module, &info);
            CHECK_RC_LOG_GOTO(rc, cleanup, "Get data info failed %s", module);
            info->modified = true;
        }

    }
    /* change session to candidate */
    if (SR_DS_STARTUP == src) {
        rc = rp_dt_switch_datastore(rp_ctx, session, SR_DS_CANDIDATE);
        CHECK_RC_MSG_GOTO(rc, cleanup, "Data tree switch failed");
        rc = dm_move_session_trees_in_session(rp_ctx->dm_ctx, session->dm_session, SR_DS_STARTUP, SR_DS_CANDIDATE);
        CHECK_RC_MSG_GOTO(rc, cleanup, "Data tree move failed");
    }
    /* commit */
    rc = rp_dt_commit(rp_ctx, session, &errors, &e_cnt);
    sr_free_errors(errors, e_cnt);

cleanup:
    first_err = rc;
    /* move datatrees & ops backup -> session */
    rc = dm_move_session_tree_and_ops_all_ds(rp_ctx->dm_ctx, backup, session->dm_session);

    /* change session to prev type */
    rc = rp_dt_switch_datastore(rp_ctx, session, prev_ds);
    sr_list_cleanup(modules);
cleanup_sess_stop:
    dm_session_stop(rp_ctx->dm_ctx, backup);
    return first_err == SR_ERR_OK ? rc : first_err;
}

int
rp_dt_copy_config(rp_ctx_t *rp_ctx, rp_session_t *session, const char *module_name, sr_datastore_t src, sr_datastore_t dst)
{
    CHECK_NULL_ARG2(rp_ctx, session);
    SR_LOG_INF("Copy config: %s -> %s, model: %s", sr_ds_to_str(src), sr_ds_to_str(dst), module_name);
    int rc = SR_ERR_OK;
    int prev_ds = session->datastore;

    if (src == dst) {
        return rc;
    }

    if ((SR_DS_CANDIDATE == src || SR_DS_CANDIDATE == dst) && SR_DS_CANDIDATE != session->datastore) {
        rc = rp_dt_switch_datastore(rp_ctx, session, SR_DS_CANDIDATE);
        CHECK_RC_MSG_RETURN(rc, "Datastore switch failed");
    }

    if (SR_DS_RUNNING != dst) {
        if (NULL != module_name) {
            /* copy module content in DM */
            rc = dm_copy_module(rp_ctx->dm_ctx, session->dm_session, module_name, src, dst);
        } else {
            /* copy all enabled modules */
            rc = dm_copy_all_models(rp_ctx->dm_ctx, session->dm_session, src, dst);
        }

    } else {
        rc = rp_dt_copy_config_to_running(rp_ctx, session, module_name, src);
    }

    rp_dt_switch_datastore(rp_ctx, session, prev_ds);
    return rc;
}

int
rp_dt_switch_datastore(rp_ctx_t *rp_ctx, rp_session_t *session, sr_datastore_t ds)
{
    CHECK_NULL_ARG3(rp_ctx, session, session->dm_session);
    int rc = SR_ERR_OK;
    SR_LOG_INF("Switch datastore request %s -> %s", sr_ds_to_str(session->datastore), sr_ds_to_str(ds));
    session->datastore = ds;
    rc = dm_session_switch_ds(session->dm_session, ds);
    return rc;
}

int
rp_dt_lock(const rp_ctx_t *rp_ctx, const rp_session_t *session, const char *module_name)
{
    CHECK_NULL_ARG2(rp_ctx, session);
    int rc = SR_ERR_OK;
    bool modif = false;

    SR_LOG_INF("Lock request module: '%s', datastore %s", module_name, sr_ds_to_str(session->datastore));

    sr_schema_t *schemas = NULL;
    size_t count = 0;

    if (NULL != module_name) {
        /* module-level lock */
        rc = dm_is_model_modified(rp_ctx->dm_ctx, session->dm_session, module_name, &modif);
        CHECK_RC_MSG_RETURN(rc, "is model modified failed");
        if (modif) {
            SR_LOG_ERR("Modified model %s can not be locked", module_name);
            return dm_report_error(session->dm_session, "Module has been modified, it can not be locked. Discard or commit changes", module_name, SR_ERR_OPERATION_FAILED);
        }
        rc = dm_lock_module(rp_ctx->dm_ctx, session->dm_session, module_name);
    } else {
        /* datastore-level lock */
        rc = dm_list_schemas(rp_ctx->dm_ctx, session->dm_session, &schemas, &count);
        CHECK_RC_MSG_GOTO(rc, cleanup, "List schemas failed");

        for (size_t i = 0; i < count; i++) {
            rc = dm_is_model_modified(rp_ctx->dm_ctx, session->dm_session, schemas[i].module_name, &modif);
            CHECK_RC_MSG_GOTO(rc, cleanup, "is model modified failed");

            if (modif) {
                SR_LOG_ERR("Modified model %s can not be locked", schemas[i].module_name);
                rc = dm_report_error(session->dm_session, "Module has been modified, it can not be locked. Discard or commit changes", schemas[i].module_name, SR_ERR_OPERATION_FAILED);
                goto cleanup;
            }
        }
        rc = dm_lock_datastore(rp_ctx->dm_ctx, session->dm_session);
    }
cleanup:
    sr_free_schemas(schemas, count);
    return rc;
}
