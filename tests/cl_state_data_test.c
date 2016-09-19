/**
 * @file cl_state_data_test.c
 * @author Rastislav Szabo <raszabo@cisco.com>, Lukas Macko <lmacko@cisco.com>
 * @brief Notifications unit tests.
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

#include <stdlib.h>
#include <stdio.h>
#include <unistd.h>
#include <stdbool.h>
#include <setjmp.h>
#include <cmocka.h>
#include <string.h>
#include <pthread.h>
#include <rpc/rpc_msg.h>

#include "sysrepo.h"
#include "client_library.h"

#include "sr_common.h"
#include "test_module_helper.h"
#include "sysrepo/xpath_utils.h"
#include "sysrepo/values.h"

#define CHECK_LIST_OF_STRINGS(list, expected)                           \
    do {                                                                \
        size_t exp_cnt = sizeof(expected) / sizeof(*expected);          \
        assert_int_equal(exp_cnt, list->count);                         \
        for (size_t i = 0; i < exp_cnt; i++) {                          \
            bool match = false;                                         \
            for (size_t j = 0; list->count; j++) {                      \
                if (0 == strcmp(expected[i], (char *)list->data[j])) {  \
                    match = true;                                       \
                    break;                                              \
                }                                                       \
            }                                                           \
            if (!match) {                                               \
                /* assert string that can not be found */               \
                assert_string_equal("", expected[i]);                   \
            }                                                           \
        }                                                               \
    } while (0)


static int
sysrepo_setup(void **state)
{
    createDataTreeExampleModule();
    createDataTreeTestModule();
    sr_conn_ctx_t *conn = NULL;
    int rc = SR_ERR_OK;

    sr_log_stderr(SR_LL_DBG);

    /* connect to sysrepo */
    rc = sr_connect("state_data_test", SR_CONN_DEFAULT, &conn);
    assert_int_equal(rc, SR_ERR_OK);

    *state = (void*)conn;
    return 0;
}

static int
sysrepo_teardown(void **state)
{
    sr_conn_ctx_t *conn = *state;
    assert_non_null(conn);

    /* disconnect from sysrepo */
    sr_disconnect(conn);

    return 0;
}

static int
provide_distance_travalled(sr_val_t **values, size_t *values_cnt, void *private_ctx)
{
    sr_list_t *l = (sr_list_t *) private_ctx;
    const char *xpath = "/state-module:bus/distance_travelled";
    if (0 != sr_list_add(l, strdup(xpath))) {
        SR_LOG_ERR_MSG("Error while adding into list");
    }

    *values = calloc(1, sizeof(**values));
    if (NULL == *values) {
        SR_LOG_ERR_MSG("Allocation failed");
        return -2;
    }
    (*values)->xpath = strdup(xpath);
    (*values)->type = SR_UINT32_T;
    (*values)->data.uint32_val = 999;
    *values_cnt = 1;

    return 0;
}

static int
provide_gps_located(sr_val_t **values, size_t *values_cnt, void *private_ctx) {
    sr_list_t *l = (sr_list_t *) private_ctx;
    const char *xpath = "/state-module:bus/gps_located";
    if (0 != sr_list_add(l, strdup(xpath))) {
        SR_LOG_ERR_MSG("Error while adding into list");
    }

    *values = calloc(1, sizeof(**values));
    if (NULL == *values) {
        SR_LOG_ERR_MSG("Allocation failed");
        return -2;
    }
    (*values)->xpath = strdup(xpath);
    (*values)->type = SR_BOOL_T;
    (*values)->data.bool_val = false;
    *values_cnt = 1;

    return 0;
}

int cl_dp_cpu_load (const char *xpath, sr_val_t **values, size_t *values_cnt, void *private_ctx)
{
    const char *expected_xpath = "/state-module:cpu_load";
    if (0 == strcmp(xpath, expected_xpath)) {
        sr_list_t *l = (sr_list_t *) private_ctx;
        if (0 != sr_list_add(l, strdup(xpath))) {
            SR_LOG_ERR_MSG("Error while adding into list");
        }

        *values = calloc(1, sizeof(**values));
        if (NULL == *values) {
            SR_LOG_ERR_MSG("Allocation failed");
            return -2;
        }
        (*values)->xpath = strdup(xpath);
        (*values)->type = SR_DECIMAL64_T;
        (*values)->data.decimal64_val = 75.25;
        *values_cnt = 1;

        return 0;
    }
    SR_LOG_ERR("Data provider received unexpected xpath %s expected %s", xpath, expected_xpath);
    return -1;
}

int cl_dp_bus (const char *xpath, sr_val_t **values, size_t *values_cnt, void *private_ctx)
{
    if (0 == strcmp(xpath, "/state-module:bus/distance_travelled"))
    {
        return provide_distance_travalled(values, values_cnt, private_ctx);
    } else if (0 == strcmp(xpath, "/state-module:bus/gps_located")) {
        return provide_gps_located(values, values_cnt, private_ctx);
    }
    SR_LOG_ERR("Data provider received unexpected xpath %s", xpath);
    return -1;
}

int cl_dp_distance_travelled (const char *xpath, sr_val_t **values, size_t *values_cnt, void *private_ctx)
{
    const char *expected_xpath = "/state-module:bus/distance_travelled";
    if (0 == strcmp(xpath, expected_xpath)) {
        return provide_distance_travalled(values, values_cnt, private_ctx);
    }
    SR_LOG_ERR("Data provider received unexpected xpath %s expected %s", xpath, expected_xpath);
    return -1;
}

int cl_dp_gps_located (const char *xpath, sr_val_t **values, size_t *values_cnt, void *private_ctx)
{
    const char *expected_xpath = "/state-module:bus/gps_located";
    if (0 == strcmp(xpath, "/state-module:bus/gps_located")) {
        return provide_gps_located(values, values_cnt, private_ctx);
    }
    SR_LOG_ERR("Data provider received unexpected xpath %s expected %s", xpath, expected_xpath);
    return -1;
}

int
cl_dp_incorrect_data(const char *xpath, sr_val_t **values, size_t *values_cnt, void *private_ctx)
{
    sr_list_t *l = (sr_list_t *) private_ctx;
    if (0 != sr_list_add(l, strdup(xpath))) {
        SR_LOG_ERR_MSG("Error while adding into list");
    }

    *values = calloc(1, sizeof(**values));
    if (NULL == *values) {
        SR_LOG_ERR_MSG("Allocation failed");
        return -2;
    }
    /* an attempt to to modify config data */
    (*values)->xpath = strdup("/state-module:bus/vendor_name");
    (*values)->type = SR_STRING_T;
    (*values)->data.string_val = strdup("Bus vendor");
    *values_cnt = 1;
    return 0;
}

int cl_dp_weather (const char *xpath, sr_val_t **values, size_t *values_cnt, void *private_ctx)
{
    return SR_ERR_OK;
}

int
cl_whole_module_cb(sr_session_ctx_t *session, const char *module_name, sr_notif_event_t ev, void *private_ctx)
{
    /* do nothing on changes */
    return SR_ERR_OK;
}

int
cl_dp_traffic_stats(const char *xpath, sr_val_t **values, size_t *values_cnt, void *private_ctx)
{
    sr_list_t *l = (sr_list_t *) private_ctx;
    int rc = SR_ERR_OK;
    #define MAX_LEN 200
    if (0 != sr_list_add(l, strdup(xpath))) {
        SR_LOG_ERR_MSG("Error while adding into list");
    }

    if (0 == strcmp("/state-module:traffic_stats", xpath)) {
        *values = calloc(2, sizeof(**values));
        if (NULL == *values) {
            SR_LOG_ERR_MSG("Allocation failed");
            return -2;
        }
        (*values)[0].xpath = strdup("/state-module:traffic_stats/number_of_accidents");
        (*values)[0].type = SR_UINT8_T;
        (*values)[0].data.uint8_val = 2;

        (*values)[1].xpath = strdup("/state-module:traffic_stats/cross_roads_offline_count");
        (*values)[1].type = SR_UINT8_T;
        (*values)[1].data.uint8_val = 9;
        *values_cnt = 2;
    } else if (0 == strcmp("/state-module:traffic_stats/cross_road", xpath)) {
        *values = calloc(5, sizeof(**values));
        if (NULL == *values) {
            SR_LOG_ERR_MSG("Allocation failed");
            return -2;
        }
        (*values)[0].xpath = strdup("/state-module:traffic_stats/cross_road[id='0']");
        (*values)[0].type = SR_LIST_T;

        (*values)[1].xpath = strdup("/state-module:traffic_stats/cross_road[id='0']/status");
        (*values)[1].type = SR_ENUM_T;
        (*values)[1].data.enum_val = strdup("manual");

        (*values)[2].xpath = strdup("/state-module:traffic_stats/cross_road[id='1']/status");
        (*values)[2].type = SR_ENUM_T;
        (*values)[2].data.enum_val = strdup("automatic");

        (*values)[3].xpath = strdup("/state-module:traffic_stats/cross_road[id='2']/status");
        (*values)[3].type = SR_ENUM_T;
        (*values)[3].data.enum_val = strdup("automatic");

        (*values)[4].xpath = strdup("/state-module:traffic_stats/cross_road[id='2']/average_wait_time");
        (*values)[4].type = SR_UINT32_T;
        (*values)[4].data.uint32_val = 15;
        *values_cnt = 5;
    } else if (0 == strncmp("traffic_light", sr_xpath_node_name(xpath), strlen("traffic_light"))) {
        char xp[MAX_LEN] = {0};
        const char *colors[] = {"red", "orange", "green"};
        sr_xpath_ctx_t xp_ctx = {0};

        *values = calloc(3, sizeof(**values));
        if (NULL == *values) {
            SR_LOG_ERR_MSG("Allocation failed");
            return -2;
        }

        char *cross_road_id = NULL;
        int cr_index = -1;
        char *xp_dup = strdup(xpath);

        if (NULL != xp_dup) {
            cross_road_id = sr_xpath_key_value(xp_dup, "cross_road", "id", &xp_ctx);
            if (NULL != cross_road_id) {
                cr_index = atoi(cross_road_id);
            }
        }
        free(xp_dup);

        for (int i = 0; i < 3; i++) {
            snprintf(xp, MAX_LEN, "%s[name='%c']/color", xpath, 'a'+i );
            (*values)[i].xpath = strdup(xp);
            (*values)[i].type = SR_ENUM_T;
            (*values)[i].data.enum_val = strdup(colors[(cr_index + i)%3]);
        }
        *values_cnt = 3;
    } else if (0 == strncmp("advanced_info", sr_xpath_node_name(xpath), strlen("advanced_info"))) {
        char xp[MAX_LEN] = {0};
        sr_xpath_ctx_t xp_ctx = {0};
        char *cross_road_id = NULL;
        int cr_index = -1;
        char *xp_dup = strdup(xpath);

        if (NULL != xp_dup) {
            cross_road_id = sr_xpath_key_value(xp_dup, "cross_road", "id", &xp_ctx);
            if (NULL != cross_road_id) {
                cr_index = atoi(cross_road_id);
            }
        }
        free(xp_dup);

        if (0 == cr_index) {
            /* advanced_info container is only in the first list instance */
            *values_cnt = 2;
            rc = sr_new_values(*values_cnt, values);
            if (SR_ERR_OK != rc) return rc;
        } else {
            *values = NULL;
            *values_cnt = 0;
            return 0;
        }

        snprintf(xp, MAX_LEN, "%s/latitude", xpath);
        (*values)[0].type = SR_STRING_T;
        sr_val_set_xpath(&(*values)[0], xp);
        sr_val_set_string(&(*values)[0], "48.729885N");

        snprintf(xp, MAX_LEN, "%s/longitude", xpath);
        (*values)[1].type = SR_STRING_T;
        sr_val_set_xpath(&(*values)[1], xp);
        sr_val_set_string(&(*values)[1], "19.137425E");
    }
    else {
        *values = NULL;
        *values_cnt = 0;
    }

    return 0;
}

static void
cl_parent_subscription(void **state)
{
    sr_conn_ctx_t *conn = *state;
    assert_non_null(conn);
    sr_session_ctx_t *session = NULL;
    sr_subscription_ctx_t *subscription = NULL;
    sr_list_t *xpath_retrieved = NULL;
    sr_val_t *values = NULL;
    size_t cnt = 0;
    int rc = SR_ERR_OK;

    rc = sr_list_init(&xpath_retrieved);
    assert_int_equal(rc, SR_ERR_OK);

    /* start session */
    rc = sr_session_start(conn, SR_DS_RUNNING, SR_SESS_DEFAULT, &session);
    assert_int_equal(rc, SR_ERR_OK);

    rc = sr_module_change_subscribe(session, "state-module", cl_whole_module_cb, NULL,
            0, SR_SUBSCR_DEFAULT, &subscription);
    assert_int_equal(rc, SR_ERR_OK);

    /* subscribe data providers */
    rc = sr_dp_get_items_subscribe(session, "/state-module:bus", cl_dp_bus, xpath_retrieved, SR_SUBSCR_CTX_REUSE, &subscription);
    assert_int_equal(rc, SR_ERR_OK);

    /* retrieve data */
    rc = sr_get_items(session, "/state-module:bus/*", &values, &cnt);
    assert_int_equal(rc, SR_ERR_OK);

    /* check data */
    assert_non_null(values);
    assert_int_equal(2, cnt);

    if (0 == strcmp("/state-module:bus/gps_located", values[0].xpath)) {
        assert_string_equal("/state-module:bus/gps_located", values[0].xpath);
        assert_int_equal(SR_BOOL_T, values[0].type);
        assert_int_equal(false, values[0].data.bool_val);

        assert_string_equal("/state-module:bus/distance_travelled", values[1].xpath);
        assert_int_equal(SR_UINT32_T, values[1].type);
        assert_int_equal(999, values[1].data.uint32_val);
    } else {
        assert_string_equal("/state-module:bus/distance_travelled", values[0].xpath);
        assert_int_equal(SR_UINT32_T, values[0].type);
        assert_int_equal(999, values[0].data.uint32_val);

        assert_string_equal("/state-module:bus/gps_located", values[1].xpath);
        assert_int_equal(SR_BOOL_T, values[1].type);
        assert_int_equal(false, values[1].data.bool_val);
    }

    sr_free_values(values, cnt);

    /* check xpath that were retrieved */
    const char *xpath_expected_to_be_loaded [] = {
        "/state-module:bus/gps_located",
        "/state-module:bus/distance_travelled"
    };
    CHECK_LIST_OF_STRINGS(xpath_retrieved, xpath_expected_to_be_loaded);

    /* cleanup */
    sr_unsubscribe(session, subscription);
    sr_session_stop(session);

    for (size_t i = 0; i < xpath_retrieved->count; i++) {
        free(xpath_retrieved->data[i]);
    }
    sr_list_cleanup(xpath_retrieved);
}

static void
cl_parent_subscription_tree(void **state)
{
    sr_conn_ctx_t *conn = *state;
    assert_non_null(conn);
    sr_session_ctx_t *session = NULL;
    sr_subscription_ctx_t *subscription = NULL;
    sr_list_t *xpath_retrieved = NULL;
    sr_node_t *tree = NULL, *node = NULL;
    int rc = SR_ERR_OK;

    rc = sr_list_init(&xpath_retrieved);
    assert_int_equal(rc, SR_ERR_OK);

    /* start session */
    rc = sr_session_start(conn, SR_DS_RUNNING, SR_SESS_DEFAULT, &session);
    assert_int_equal(rc, SR_ERR_OK);

    rc = sr_module_change_subscribe(session, "state-module", cl_whole_module_cb, NULL,
            0, SR_SUBSCR_DEFAULT, &subscription);
    assert_int_equal(rc, SR_ERR_OK);

    /* subscribe data providers */
    rc = sr_dp_get_items_subscribe(session, "/state-module:bus", cl_dp_bus, xpath_retrieved, SR_SUBSCR_CTX_REUSE, &subscription);
    assert_int_equal(rc, SR_ERR_OK);

    /* retrieve data using the tree API */
    rc = sr_get_subtree(session, "/state-module:bus", 0, &tree);
    assert_int_equal(rc, SR_ERR_OK);

    /* check data */
    assert_non_null(tree);
    assert_string_equal("bus", tree->name);
    assert_string_equal("state-module", tree->module_name);
    assert_false(tree->dflt);
    assert_int_equal(SR_CONTAINER_T, tree->type);
    // gps located
    node = tree->first_child;
    assert_non_null(node);
    assert_string_equal("gps_located", node->name);
    assert_null(node->module_name);
    assert_false(node->dflt);
    assert_int_equal(SR_BOOL_T, node->type);
    assert_false(node->data.bool_val);
    assert_null(node->first_child);
    // distance travelled
    node = node->next;
    assert_non_null(node);
    assert_string_equal("distance_travelled", node->name);
    assert_null(node->module_name);
    assert_false(node->dflt);
    assert_int_equal(SR_UINT32_T, node->type);
    assert_int_equal(999, node->data.uint32_val);
    assert_null(node->first_child);
    assert_null(node->next);

    sr_free_tree(tree);

    /* check xpath that were retrieved */
    const char *xpath_expected_to_be_loaded [] = {
        "/state-module:bus/gps_located",
        "/state-module:bus/distance_travelled"
    };
    CHECK_LIST_OF_STRINGS(xpath_retrieved, xpath_expected_to_be_loaded);

    /* cleanup */
    sr_unsubscribe(session, subscription);
    sr_session_stop(session);

    for (size_t i = 0; i < xpath_retrieved->count; i++) {
        free(xpath_retrieved->data[i]);
    }
    sr_list_cleanup(xpath_retrieved);
}

static void
cl_exact_match_subscription(void **state)
{
    sr_conn_ctx_t *conn = *state;
    assert_non_null(conn);
    sr_session_ctx_t *session = NULL;
    sr_subscription_ctx_t *subscription = NULL;
    sr_list_t *xpath_retrieved = NULL;
    sr_val_t *values = NULL;
    size_t cnt = 0;
    int rc = SR_ERR_OK;

    rc = sr_list_init(&xpath_retrieved);
    assert_int_equal(rc, SR_ERR_OK);

    /* start session */
    rc = sr_session_start(conn, SR_DS_RUNNING, SR_SESS_DEFAULT, &session);
    assert_int_equal(rc, SR_ERR_OK);

    rc = sr_module_change_subscribe(session, "state-module", cl_whole_module_cb, NULL,
            0, SR_SUBSCR_DEFAULT, &subscription);
    assert_int_equal(rc, SR_ERR_OK);

    /* subscribe data providers */
    rc = sr_dp_get_items_subscribe(session, "/state-module:bus/distance_travelled", cl_dp_distance_travelled, xpath_retrieved, SR_SUBSCR_CTX_REUSE, &subscription);
    assert_int_equal(rc, SR_ERR_OK);

    rc = sr_dp_get_items_subscribe(session, "/state-module:bus/gps_located", cl_dp_gps_located, xpath_retrieved, SR_SUBSCR_CTX_REUSE, &subscription);
    assert_int_equal(rc, SR_ERR_OK);

    rc = sr_dp_get_items_subscribe(session, "/state-module:cpu_load", cl_dp_cpu_load, xpath_retrieved, SR_SUBSCR_CTX_REUSE, &subscription);
    assert_int_equal(rc, SR_ERR_OK);

    /* retrieve data */
    rc = sr_get_items(session, "/state-module:bus/*", &values, &cnt);
    assert_int_equal(rc, SR_ERR_OK);

    /* check data */
    assert_non_null(values);
    assert_int_equal(2, cnt);

    assert_string_equal("/state-module:bus/gps_located", values[0].xpath);
    assert_int_equal(SR_BOOL_T, values[0].type);
    assert_int_equal(false, values[0].data.bool_val);

    assert_string_equal("/state-module:bus/distance_travelled", values[1].xpath);
    assert_int_equal(SR_UINT32_T, values[1].type);
    assert_int_equal(999, values[1].data.uint32_val);

    sr_free_values(values, cnt);

    /* check xpath that were retrieved */
    const char *xpath_expected_to_be_loaded [] = {
        "/state-module:bus/gps_located",
        "/state-module:bus/distance_travelled"
    };
    CHECK_LIST_OF_STRINGS(xpath_retrieved, xpath_expected_to_be_loaded);

    /* cleanup */
    sr_unsubscribe(session, subscription);
    sr_session_stop(session);

    for (size_t i = 0; i < xpath_retrieved->count; i++) {
        free(xpath_retrieved->data[i]);
    }
    sr_list_cleanup(xpath_retrieved);
}

static void
cl_exact_match_subscription_tree(void **state)
{
    sr_conn_ctx_t *conn = *state;
    assert_non_null(conn);
    sr_session_ctx_t *session = NULL;
    sr_subscription_ctx_t *subscription = NULL;
    sr_list_t *xpath_retrieved = NULL;
    sr_node_t *tree = NULL, *node = NULL;
    int rc = SR_ERR_OK;

    rc = sr_list_init(&xpath_retrieved);
    assert_int_equal(rc, SR_ERR_OK);

    /* start session */
    rc = sr_session_start(conn, SR_DS_RUNNING, SR_SESS_DEFAULT, &session);
    assert_int_equal(rc, SR_ERR_OK);

    rc = sr_module_change_subscribe(session, "state-module", cl_whole_module_cb, NULL,
            0, SR_SUBSCR_DEFAULT, &subscription);
    assert_int_equal(rc, SR_ERR_OK);

    /* subscribe data providers */
    rc = sr_dp_get_items_subscribe(session, "/state-module:bus/distance_travelled", cl_dp_distance_travelled, xpath_retrieved, SR_SUBSCR_CTX_REUSE, &subscription);
    assert_int_equal(rc, SR_ERR_OK);

    rc = sr_dp_get_items_subscribe(session, "/state-module:bus/gps_located", cl_dp_gps_located, xpath_retrieved, SR_SUBSCR_CTX_REUSE, &subscription);
    assert_int_equal(rc, SR_ERR_OK);

    rc = sr_dp_get_items_subscribe(session, "/state-module:cpu_load", cl_dp_cpu_load, xpath_retrieved, SR_SUBSCR_CTX_REUSE, &subscription);
    assert_int_equal(rc, SR_ERR_OK);

    /* retrieve data using the tree API */
    rc = sr_get_subtree(session, "/state-module:bus", 0, &tree);
    assert_int_equal(rc, SR_ERR_OK);

    /* check data */
    assert_non_null(tree);
    assert_string_equal("bus", tree->name);
    assert_string_equal("state-module", tree->module_name);
    assert_false(tree->dflt);
    assert_int_equal(SR_CONTAINER_T, tree->type);
    // gps located
    node = tree->first_child;
    assert_non_null(node);
    assert_string_equal("gps_located", node->name);
    assert_null(node->module_name);
    assert_false(node->dflt);
    assert_int_equal(SR_BOOL_T, node->type);
    assert_false(node->data.bool_val);
    assert_null(node->first_child);
    // distance travelled
    node = node->next;
    assert_non_null(node);
    assert_string_equal("distance_travelled", node->name);
    assert_null(node->module_name);
    assert_false(node->dflt);
    assert_int_equal(SR_UINT32_T, node->type);
    assert_int_equal(999, node->data.uint32_val);
    assert_null(node->first_child);
    assert_null(node->next);

    sr_free_tree(tree);

    /* check xpath that were retrieved */
    const char *xpath_expected_to_be_loaded [] = {
        "/state-module:bus/gps_located",
        "/state-module:bus/distance_travelled"
    };
    CHECK_LIST_OF_STRINGS(xpath_retrieved, xpath_expected_to_be_loaded);

    /* cleanup */
    sr_unsubscribe(session, subscription);
    sr_session_stop(session);

    for (size_t i = 0; i < xpath_retrieved->count; i++) {
        free(xpath_retrieved->data[i]);
    }
    sr_list_cleanup(xpath_retrieved);
}

static void
cl_partialy_covered_by_subscription(void **state)
{
    sr_conn_ctx_t *conn = *state;
    assert_non_null(conn);
    sr_session_ctx_t *session = NULL;
    sr_subscription_ctx_t *subscription = NULL;
    sr_list_t *xpath_retrieved = NULL;
    sr_val_t *values = NULL;
    size_t cnt = 0;
    int rc = SR_ERR_OK;

    rc = sr_list_init(&xpath_retrieved);
    assert_int_equal(rc, SR_ERR_OK);

    /* start session */
    rc = sr_session_start(conn, SR_DS_RUNNING, SR_SESS_DEFAULT, &session);
    assert_int_equal(rc, SR_ERR_OK);

    rc = sr_module_change_subscribe(session, "state-module", cl_whole_module_cb, NULL,
            0, SR_SUBSCR_DEFAULT, &subscription);
    assert_int_equal(rc, SR_ERR_OK);

    /* subscribe data providers - provider for distance_travelled is missing */
    rc = sr_dp_get_items_subscribe(session, "/state-module:bus/gps_located", cl_dp_gps_located, xpath_retrieved, SR_SUBSCR_CTX_REUSE, &subscription);
    assert_int_equal(rc, SR_ERR_OK);

    /* retrieve data */
    rc = sr_get_items(session, "/state-module:bus/*", &values, &cnt);
    assert_int_equal(rc, SR_ERR_OK);

    /* check data */
    assert_non_null(values);
    assert_int_equal(1, cnt);

    assert_string_equal("/state-module:bus/gps_located", values[0].xpath);
    assert_int_equal(SR_BOOL_T, values[0].type);
    assert_int_equal(false, values[0].data.bool_val);

    sr_free_values(values, cnt);

    /* check xpath that were retrieved */
    const char *xpath_expected_to_be_loaded [] = {
        "/state-module:bus/gps_located",
    };
    CHECK_LIST_OF_STRINGS(xpath_retrieved, xpath_expected_to_be_loaded);

    /* cleanup */
    sr_unsubscribe(session, subscription);
    sr_session_stop(session);

    for (size_t i = 0; i < xpath_retrieved->count; i++) {
        free(xpath_retrieved->data[i]);
    }
    sr_list_cleanup(xpath_retrieved);
}

static void
cl_partialy_covered_by_subscription_tree(void **state)
{
    sr_conn_ctx_t *conn = *state;
    assert_non_null(conn);
    sr_session_ctx_t *session = NULL;
    sr_subscription_ctx_t *subscription = NULL;
    sr_list_t *xpath_retrieved = NULL;
    sr_node_t *tree = NULL, *node = NULL;
    int rc = SR_ERR_OK;

    rc = sr_list_init(&xpath_retrieved);
    assert_int_equal(rc, SR_ERR_OK);

    /* start session */
    rc = sr_session_start(conn, SR_DS_RUNNING, SR_SESS_DEFAULT, &session);
    assert_int_equal(rc, SR_ERR_OK);

    rc = sr_module_change_subscribe(session, "state-module", cl_whole_module_cb, NULL,
            0, SR_SUBSCR_DEFAULT, &subscription);
    assert_int_equal(rc, SR_ERR_OK);

    /* subscribe data providers - provider for distance_travelled is missing */
    rc = sr_dp_get_items_subscribe(session, "/state-module:bus/gps_located", cl_dp_gps_located, xpath_retrieved, SR_SUBSCR_CTX_REUSE, &subscription);
    assert_int_equal(rc, SR_ERR_OK);

    /* retrieve data */
    rc = sr_get_subtree(session, "/state-module:bus", 0, &tree);
    assert_int_equal(rc, SR_ERR_OK);

    /* check data */
    assert_non_null(tree);
    assert_string_equal("bus", tree->name);
    assert_string_equal("state-module", tree->module_name);
    assert_false(tree->dflt);
    assert_int_equal(SR_CONTAINER_T, tree->type);
    // gps located
    node = tree->first_child;
    assert_non_null(node);
    assert_string_equal("gps_located", node->name);
    assert_null(node->module_name);
    assert_false(node->dflt);
    assert_int_equal(SR_BOOL_T, node->type);
    assert_false(node->data.bool_val);
    assert_null(node->first_child);
    assert_null(node->next);

    sr_free_tree(tree);

    /* check xpath that were retrieved */
    const char *xpath_expected_to_be_loaded [] = {
        "/state-module:bus/gps_located",
    };
    CHECK_LIST_OF_STRINGS(xpath_retrieved, xpath_expected_to_be_loaded);

    /* cleanup */
    sr_unsubscribe(session, subscription);
    sr_session_stop(session);

    for (size_t i = 0; i < xpath_retrieved->count; i++) {
        free(xpath_retrieved->data[i]);
    }
    sr_list_cleanup(xpath_retrieved);
}

static void
cl_incorrect_data_subscription(void **state)
{
    sr_conn_ctx_t *conn = *state;
    assert_non_null(conn);
    sr_session_ctx_t *session = NULL;
    sr_subscription_ctx_t *subscription = NULL;
    sr_list_t *xpath_retrieved = NULL;
    sr_val_t *values = NULL;
    size_t cnt = 0;
    int rc = SR_ERR_OK;

    rc = sr_list_init(&xpath_retrieved);
    assert_int_equal(rc, SR_ERR_OK);

    /* start session */
    rc = sr_session_start(conn, SR_DS_RUNNING, SR_SESS_DEFAULT, &session);
    assert_int_equal(rc, SR_ERR_OK);

    rc = sr_module_change_subscribe(session, "state-module", cl_whole_module_cb, NULL,
            0, SR_SUBSCR_DEFAULT, &subscription);
    assert_int_equal(rc, SR_ERR_OK);

    /* subscribe data providers - data subscriber will try to provide config data */
    rc = sr_dp_get_items_subscribe(session, "/state-module:bus/gps_located", cl_dp_incorrect_data, xpath_retrieved, SR_SUBSCR_CTX_REUSE, &subscription);
    assert_int_equal(rc, SR_ERR_OK);

    /* retrieve data */
    rc = sr_get_items(session, "/state-module:bus/*", &values, &cnt);
    assert_int_equal(rc, SR_ERR_NOT_FOUND);

    /* check data */
    assert_null(values);
    assert_int_equal(0, cnt);

    /* check xpath that were retrieved */
    const char *xpath_expected_to_be_loaded [] = {
        "/state-module:bus/gps_located",
    };
    CHECK_LIST_OF_STRINGS(xpath_retrieved, xpath_expected_to_be_loaded);

    /* cleanup */
    sr_unsubscribe(session, subscription);
    sr_session_stop(session);

    for (size_t i = 0; i < xpath_retrieved->count; i++) {
        free(xpath_retrieved->data[i]);
    }
    sr_list_cleanup(xpath_retrieved);
}

static void
cl_incorrect_data_subscription_tree(void **state)
{
    sr_conn_ctx_t *conn = *state;
    assert_non_null(conn);
    sr_session_ctx_t *session = NULL;
    sr_subscription_ctx_t *subscription = NULL;
    sr_list_t *xpath_retrieved = NULL;
    sr_node_t *tree = NULL;
    int rc = SR_ERR_OK;

    rc = sr_list_init(&xpath_retrieved);
    assert_int_equal(rc, SR_ERR_OK);

    /* start session */
    rc = sr_session_start(conn, SR_DS_RUNNING, SR_SESS_DEFAULT, &session);
    assert_int_equal(rc, SR_ERR_OK);

    rc = sr_module_change_subscribe(session, "state-module", cl_whole_module_cb, NULL,
            0, SR_SUBSCR_DEFAULT, &subscription);
    assert_int_equal(rc, SR_ERR_OK);

    /* subscribe data providers - data subscriber will try to provide config data */
    rc = sr_dp_get_items_subscribe(session, "/state-module:bus/gps_located", cl_dp_incorrect_data, xpath_retrieved, SR_SUBSCR_CTX_REUSE, &subscription);
    assert_int_equal(rc, SR_ERR_OK);

    /* retrieve data */
    rc = sr_get_subtree(session, "/state-module:bus/gps_located", 0, &tree);
    assert_int_equal(rc, SR_ERR_NOT_FOUND);

    /* check data */
    assert_null(tree);

    /* check xpath that were retrieved */
    const char *xpath_expected_to_be_loaded [] = {
        "/state-module:bus/gps_located",
    };
    CHECK_LIST_OF_STRINGS(xpath_retrieved, xpath_expected_to_be_loaded);

    /* cleanup */
    sr_unsubscribe(session, subscription);
    sr_session_stop(session);

    for (size_t i = 0; i < xpath_retrieved->count; i++) {
        free(xpath_retrieved->data[i]);
    }
    sr_list_cleanup(xpath_retrieved);
}

static void
cl_missing_subscription(void **state)
{
    sr_conn_ctx_t *conn = *state;
    assert_non_null(conn);
    sr_session_ctx_t *session = NULL;
    sr_subscription_ctx_t *subscription = NULL;
    sr_list_t *xpath_retrieved = NULL;
    sr_val_t *values = NULL;
    size_t cnt = 0;
    int rc = SR_ERR_OK;

    rc = sr_list_init(&xpath_retrieved);
    assert_int_equal(rc, SR_ERR_OK);

    /* start session */
    rc = sr_session_start(conn, SR_DS_RUNNING, SR_SESS_DEFAULT, &session);
    assert_int_equal(rc, SR_ERR_OK);

    rc = sr_module_change_subscribe(session, "state-module", cl_whole_module_cb, NULL,
            0, SR_SUBSCR_DEFAULT, &subscription);
    assert_int_equal(rc, SR_ERR_OK);

    /* subscribe data providers - provider for cpu_load is missing */
    rc = sr_dp_get_items_subscribe(session, "/state-module:bus/gps_located", cl_dp_gps_located, xpath_retrieved, SR_SUBSCR_CTX_REUSE, &subscription);
    assert_int_equal(rc, SR_ERR_OK);

    rc = sr_dp_get_items_subscribe(session, "/state-module:bus/distance_travelled", cl_dp_distance_travelled, xpath_retrieved, SR_SUBSCR_CTX_REUSE, &subscription);
    assert_int_equal(rc, SR_ERR_OK);

    /* retrieve data */
    rc = sr_get_items(session, "/state-module:cpu_load", &values, &cnt);
    assert_int_equal(rc, SR_ERR_NOT_FOUND);

    /* check data */
    assert_null(values);
    assert_int_equal(0, cnt);

    /* check xpath that were retrieved */
    assert_int_equal(0, xpath_retrieved->count);

    /* cleanup */
    sr_unsubscribe(session, subscription);
    sr_session_stop(session);

    for (size_t i = 0; i < xpath_retrieved->count; i++) {
        free(xpath_retrieved->data[i]);
    }
    sr_list_cleanup(xpath_retrieved);
}

static void
cl_missing_subscription_tree(void **state)
{
    sr_conn_ctx_t *conn = *state;
    assert_non_null(conn);
    sr_session_ctx_t *session = NULL;
    sr_subscription_ctx_t *subscription = NULL;
    sr_list_t *xpath_retrieved = NULL;
    sr_node_t *tree = NULL;
    int rc = SR_ERR_OK;

    rc = sr_list_init(&xpath_retrieved);
    assert_int_equal(rc, SR_ERR_OK);

    /* start session */
    rc = sr_session_start(conn, SR_DS_RUNNING, SR_SESS_DEFAULT, &session);
    assert_int_equal(rc, SR_ERR_OK);

    rc = sr_module_change_subscribe(session, "state-module", cl_whole_module_cb, NULL,
            0, SR_SUBSCR_DEFAULT, &subscription);
    assert_int_equal(rc, SR_ERR_OK);

    /* subscribe data providers - provider for cpu_load is missing */
    rc = sr_dp_get_items_subscribe(session, "/state-module:bus/gps_located", cl_dp_gps_located, xpath_retrieved, SR_SUBSCR_CTX_REUSE, &subscription);
    assert_int_equal(rc, SR_ERR_OK);

    rc = sr_dp_get_items_subscribe(session, "/state-module:bus/distance_travelled", cl_dp_distance_travelled, xpath_retrieved, SR_SUBSCR_CTX_REUSE, &subscription);
    assert_int_equal(rc, SR_ERR_OK);

    /* retrieve data */
    rc = sr_get_subtree(session, "/state-module:cpu_load", 0, &tree);
    assert_int_equal(rc, SR_ERR_NOT_FOUND);

    /* check data */
    assert_null(tree);

    /* check xpath that were retrieved */
    assert_int_equal(0, xpath_retrieved->count);

    /* cleanup */
    sr_unsubscribe(session, subscription);
    sr_session_stop(session);

    for (size_t i = 0; i < xpath_retrieved->count; i++) {
        free(xpath_retrieved->data[i]);
    }
    sr_list_cleanup(xpath_retrieved);
}

static void
cl_dp_neg_subscription(void **state)
{
    sr_conn_ctx_t *conn = *state;
    assert_non_null(conn);
    sr_session_ctx_t *session = NULL;
    sr_subscription_ctx_t *subscription = NULL;

    int rc = SR_ERR_OK;

    /* start session */
    rc = sr_session_start(conn, SR_DS_RUNNING, SR_SESS_DEFAULT, &session);
    assert_int_equal(rc, SR_ERR_OK);

    rc = sr_module_change_subscribe(session, "state-module", cl_whole_module_cb, NULL,
            0, SR_SUBSCR_DEFAULT, &subscription);
    assert_int_equal(rc, SR_ERR_OK);

    /* subscribe data not existing node */
    rc = sr_dp_get_items_subscribe(session, "/state-module:bus/unknown", cl_dp_gps_located, NULL, SR_SUBSCR_CTX_REUSE, &subscription);
    assert_int_equal(rc, SR_ERR_BAD_ELEMENT);

    /* subscribe not existing module */
    rc = sr_dp_get_items_subscribe(session, "/unknown-module:state-data", cl_dp_distance_travelled, NULL, SR_SUBSCR_CTX_REUSE, &subscription);
    assert_int_equal(rc, SR_ERR_INTERNAL);

    sr_unsubscribe(session, subscription);
    sr_session_stop(session);
}

static void
cl_nested_data_subscription(void **state)
{
    sr_conn_ctx_t *conn = *state;
    assert_non_null(conn);
    sr_session_ctx_t *session = NULL;
    sr_subscription_ctx_t *subscription = NULL;
    sr_list_t *xpath_retrieved = NULL;
    sr_val_t *values = NULL;
    size_t cnt = 0;
    int rc = SR_ERR_OK;

    rc = sr_list_init(&xpath_retrieved);
    assert_int_equal(rc, SR_ERR_OK);

    /* start session */
    rc = sr_session_start(conn, SR_DS_RUNNING, SR_SESS_DEFAULT, &session);
    assert_int_equal(rc, SR_ERR_OK);

    rc = sr_module_change_subscribe(session, "state-module", cl_whole_module_cb, NULL,
            0, SR_SUBSCR_DEFAULT, &subscription);
    assert_int_equal(rc, SR_ERR_OK);

    /* subscribe data provider */
    rc = sr_dp_get_items_subscribe(session, "/state-module:traffic_stats", cl_dp_traffic_stats, xpath_retrieved, SR_SUBSCR_CTX_REUSE, &subscription);
    assert_int_equal(rc, SR_ERR_OK);

    /* retrieve data */
    rc = sr_get_items(session, "/state-module:traffic_stats/*", &values, &cnt);
    assert_int_equal(rc, SR_ERR_OK);

    /* check data */
    assert_non_null(values);
    assert_int_equal(5, cnt);

    sr_free_values(values, cnt);

    /* check xpath that were retrieved */
    const char *xpath_expected_to_be_loaded [] = {
        "/state-module:traffic_stats",
        "/state-module:traffic_stats/cross_road",
        "/state-module:traffic_stats/cross_road[id='0']/traffic_light",
        "/state-module:traffic_stats/cross_road[id='0']/advanced_info",
        "/state-module:traffic_stats/cross_road[id='1']/traffic_light",
        "/state-module:traffic_stats/cross_road[id='1']/advanced_info",
        "/state-module:traffic_stats/cross_road[id='2']/traffic_light",
        "/state-module:traffic_stats/cross_road[id='2']/advanced_info",
    };
    CHECK_LIST_OF_STRINGS(xpath_retrieved, xpath_expected_to_be_loaded);

    /* cleanup */
    sr_unsubscribe(session, subscription);
    sr_session_stop(session);

    for (size_t i = 0; i < xpath_retrieved->count; i++) {
        free(xpath_retrieved->data[i]);
    }
    sr_list_cleanup(xpath_retrieved);
}

static void
cl_nested_data_subscription_tree(void **state)
{
    sr_conn_ctx_t *conn = *state;
    assert_non_null(conn);
    sr_session_ctx_t *session = NULL;
    sr_subscription_ctx_t *subscription = NULL;
    sr_list_t *xpath_retrieved = NULL;
    sr_node_t *tree = NULL, *node = NULL;
    int rc = SR_ERR_OK;

    rc = sr_list_init(&xpath_retrieved);
    assert_int_equal(rc, SR_ERR_OK);

    /* start session */
    rc = sr_session_start(conn, SR_DS_RUNNING, SR_SESS_DEFAULT, &session);
    assert_int_equal(rc, SR_ERR_OK);

    rc = sr_module_change_subscribe(session, "state-module", cl_whole_module_cb, NULL,
            0, SR_SUBSCR_DEFAULT, &subscription);
    assert_int_equal(rc, SR_ERR_OK);

    /* subscribe data provider */
    rc = sr_dp_get_items_subscribe(session, "/state-module:traffic_stats", cl_dp_traffic_stats, xpath_retrieved, SR_SUBSCR_CTX_REUSE, &subscription);
    assert_int_equal(rc, SR_ERR_OK);

    /* retrieve data */
    rc = sr_get_subtree(session, "/state-module:traffic_stats", 0, &tree);
    assert_int_equal(rc, SR_ERR_OK);

    /* check data */
    // traffic stats
    node = tree;
    assert_non_null(node);
    assert_string_equal("traffic_stats", node->name);
    assert_string_equal("state-module", node->module_name);
    assert_false(node->dflt);
    assert_int_equal(SR_CONTAINER_T, node->type);
    assert_null(node->next);
    // num. of accidents
    node = node->first_child;
    assert_string_equal("number_of_accidents", node->name);
    assert_null(node->module_name);
    assert_false(node->dflt);
    assert_int_equal(SR_UINT8_T, node->type);
    assert_int_equal(2, node->data.uint8_val);
    assert_null(node->first_child);
    // cross roads offline count
    node = node->next;
    assert_string_equal("cross_roads_offline_count", node->name);
    assert_null(node->module_name);
    assert_false(node->dflt);
    assert_int_equal(SR_UINT8_T, node->type);
    assert_int_equal(9, node->data.uint8_val);
    assert_null(node->first_child);
    // cross road, id=0
    node = node->next;
    assert_string_equal("cross_road", node->name);
    assert_null(node->module_name);
    assert_false(node->dflt);
    assert_int_equal(SR_LIST_T, node->type);
    // id
    node = node->first_child;
    assert_string_equal("id", node->name);
    assert_null(node->module_name);
    assert_false(node->dflt);
    assert_int_equal(SR_UINT32_T, node->type);
    assert_int_equal(0, node->data.uint32_val);
    assert_null(node->first_child);
    // status
    node = node->next;
    assert_string_equal("status", node->name);
    assert_null(node->module_name);
    assert_false(node->dflt);
    assert_int_equal(SR_ENUM_T, node->type);
    assert_string_equal("manual", node->data.enum_val);
    assert_null(node->first_child);
    // traffic light, name = a
    node = node->next;
    assert_string_equal("traffic_light", node->name);
    assert_null(node->module_name);
    assert_false(node->dflt);
    assert_int_equal(SR_LIST_T, node->type);
    // traffic light, name
    node = node->first_child;
    assert_string_equal("name", node->name);
    assert_null(node->module_name);
    assert_false(node->dflt);
    assert_int_equal(SR_STRING_T, node->type);
    assert_string_equal("a", node->data.enum_val);
    assert_null(node->first_child);
    // traffic light, color
    node = node->next;
    assert_string_equal("color", node->name);
    assert_null(node->module_name);
    assert_false(node->dflt);
    assert_int_equal(SR_ENUM_T, node->type);
    assert_string_equal("red", node->data.enum_val);
    assert_null(node->first_child);
    assert_null(node->next);
    // traffic light, name = b
    node = node->parent->next;
    assert_string_equal("traffic_light", node->name);
    assert_null(node->module_name);
    assert_false(node->dflt);
    assert_int_equal(SR_LIST_T, node->type);
    // traffic light, name
    node = node->first_child;
    assert_string_equal("name", node->name);
    assert_null(node->module_name);
    assert_false(node->dflt);
    assert_int_equal(SR_STRING_T, node->type);
    assert_string_equal("b", node->data.enum_val);
    assert_null(node->first_child);
    // traffic light, color
    node = node->next;
    assert_string_equal("color", node->name);
    assert_null(node->module_name);
    assert_false(node->dflt);
    assert_int_equal(SR_ENUM_T, node->type);
    assert_string_equal("orange", node->data.enum_val);
    assert_null(node->first_child);
    assert_null(node->next);
    // traffic light, name = c
    node = node->parent->next;
    assert_string_equal("traffic_light", node->name);
    assert_null(node->module_name);
    assert_false(node->dflt);
    assert_int_equal(SR_LIST_T, node->type);
    // traffic light, name
    node = node->first_child;
    assert_string_equal("name", node->name);
    assert_null(node->module_name);
    assert_false(node->dflt);
    assert_int_equal(SR_STRING_T, node->type);
    assert_string_equal("c", node->data.enum_val);
    assert_null(node->first_child);
    // traffic light, color
    node = node->next;
    assert_string_equal("color", node->name);
    assert_null(node->module_name);
    assert_false(node->dflt);
    assert_int_equal(SR_ENUM_T, node->type);
    assert_string_equal("green", node->data.enum_val);
    assert_null(node->first_child);
    assert_null(node->next);
    // advanced info
    node = node->parent->next;
    assert_string_equal("advanced_info", node->name);
    assert_null(node->module_name);
    assert_false(node->dflt);
    assert_int_equal(SR_CONTAINER_T, node->type);
    // latitude
    node = node->first_child;
    assert_string_equal("latitude", node->name);
    assert_null(node->module_name);
    assert_false(node->dflt);
    assert_int_equal(SR_STRING_T, node->type);
    assert_string_equal("48.729885N", node->data.string_val);
    // longitude
    node = node->next;
    assert_string_equal("longitude", node->name);
    assert_null(node->module_name);
    assert_false(node->dflt);
    assert_int_equal(SR_STRING_T, node->type);
    assert_string_equal("19.137425E", node->data.string_val);
    assert_null(node->next);
    assert_null(node->parent->next);
    // cross road, id=1
    node = node->parent->parent->next;
    assert_string_equal("cross_road", node->name);
    assert_null(node->module_name);
    assert_false(node->dflt);
    assert_int_equal(SR_LIST_T, node->type);
    // id
    node = node->first_child;
    assert_string_equal("id", node->name);
    assert_null(node->module_name);
    assert_false(node->dflt);
    assert_int_equal(SR_UINT32_T, node->type);
    assert_int_equal(1, node->data.uint32_val);
    assert_null(node->first_child);
    // status
    node = node->next;
    assert_string_equal("status", node->name);
    assert_null(node->module_name);
    assert_false(node->dflt);
    assert_int_equal(SR_ENUM_T, node->type);
    assert_string_equal("automatic", node->data.enum_val);
    assert_null(node->first_child);
    // traffic light, name = a
    node = node->next;
    assert_string_equal("traffic_light", node->name);
    assert_null(node->module_name);
    assert_false(node->dflt);
    assert_int_equal(SR_LIST_T, node->type);
    // traffic light, name
    node = node->first_child;
    assert_string_equal("name", node->name);
    assert_null(node->module_name);
    assert_false(node->dflt);
    assert_int_equal(SR_STRING_T, node->type);
    assert_string_equal("a", node->data.enum_val);
    assert_null(node->first_child);
    // traffic light, color
    node = node->next;
    assert_string_equal("color", node->name);
    assert_null(node->module_name);
    assert_false(node->dflt);
    assert_int_equal(SR_ENUM_T, node->type);
    assert_string_equal("orange", node->data.enum_val);
    assert_null(node->first_child);
    assert_null(node->next);
    // traffic light, name = b
    node = node->parent->next;
    assert_string_equal("traffic_light", node->name);
    assert_null(node->module_name);
    assert_false(node->dflt);
    assert_int_equal(SR_LIST_T, node->type);
    // traffic light, name
    node = node->first_child;
    assert_string_equal("name", node->name);
    assert_null(node->module_name);
    assert_false(node->dflt);
    assert_int_equal(SR_STRING_T, node->type);
    assert_string_equal("b", node->data.enum_val);
    assert_null(node->first_child);
    // traffic light, color
    node = node->next;
    assert_string_equal("color", node->name);
    assert_null(node->module_name);
    assert_false(node->dflt);
    assert_int_equal(SR_ENUM_T, node->type);
    assert_string_equal("green", node->data.enum_val);
    assert_null(node->first_child);
    assert_null(node->next);
    // traffic light, name = c
    node = node->parent->next;
    assert_string_equal("traffic_light", node->name);
    assert_null(node->module_name);
    assert_false(node->dflt);
    assert_int_equal(SR_LIST_T, node->type);
    // traffic light, name
    node = node->first_child;
    assert_string_equal("name", node->name);
    assert_null(node->module_name);
    assert_false(node->dflt);
    assert_int_equal(SR_STRING_T, node->type);
    assert_string_equal("c", node->data.enum_val);
    assert_null(node->first_child);
    // traffic light, color
    node = node->next;
    assert_string_equal("color", node->name);
    assert_null(node->module_name);
    assert_false(node->dflt);
    assert_int_equal(SR_ENUM_T, node->type);
    assert_string_equal("red", node->data.enum_val);
    assert_null(node->first_child);
    assert_null(node->next);
    // no advanced info
    assert_null(node->parent->next);
    // cross road, id=1
    node = node->parent->parent->next;
    assert_string_equal("cross_road", node->name);
    assert_null(node->module_name);
    assert_false(node->dflt);
    assert_int_equal(SR_LIST_T, node->type);
    // id
    node = node->first_child;
    assert_string_equal("id", node->name);
    assert_null(node->module_name);
    assert_false(node->dflt);
    assert_int_equal(SR_UINT32_T, node->type);
    assert_int_equal(2, node->data.uint32_val);
    assert_null(node->first_child);
    // status
    node = node->next;
    assert_string_equal("status", node->name);
    assert_null(node->module_name);
    assert_false(node->dflt);
    assert_int_equal(SR_ENUM_T, node->type);
    assert_string_equal("automatic", node->data.enum_val);
    assert_null(node->first_child);
    // average wait time
    node = node->next;
    assert_string_equal("average_wait_time", node->name);
    assert_null(node->module_name);
    assert_false(node->dflt);
    assert_int_equal(SR_UINT32_T, node->type);
    assert_int_equal(15, node->data.uint32_val);
    assert_null(node->first_child);
    // traffic light, name = a
    node = node->next;
    assert_string_equal("traffic_light", node->name);
    assert_null(node->module_name);
    assert_false(node->dflt);
    assert_int_equal(SR_LIST_T, node->type);
    // traffic light, name
    node = node->first_child;
    assert_string_equal("name", node->name);
    assert_null(node->module_name);
    assert_false(node->dflt);
    assert_int_equal(SR_STRING_T, node->type);
    assert_string_equal("a", node->data.enum_val);
    assert_null(node->first_child);
    // traffic light, color
    node = node->next;
    assert_string_equal("color", node->name);
    assert_null(node->module_name);
    assert_false(node->dflt);
    assert_int_equal(SR_ENUM_T, node->type);
    assert_string_equal("green", node->data.enum_val);
    assert_null(node->first_child);
    assert_null(node->next);
    // traffic light, name = b
    node = node->parent->next;
    assert_string_equal("traffic_light", node->name);
    assert_null(node->module_name);
    assert_false(node->dflt);
    assert_int_equal(SR_LIST_T, node->type);
    // traffic light, name
    node = node->first_child;
    assert_string_equal("name", node->name);
    assert_null(node->module_name);
    assert_false(node->dflt);
    assert_int_equal(SR_STRING_T, node->type);
    assert_string_equal("b", node->data.enum_val);
    assert_null(node->first_child);
    // traffic light, color
    node = node->next;
    assert_string_equal("color", node->name);
    assert_null(node->module_name);
    assert_false(node->dflt);
    assert_int_equal(SR_ENUM_T, node->type);
    assert_string_equal("red", node->data.enum_val);
    assert_null(node->first_child);
    assert_null(node->next);
    // traffic light, name = c
    node = node->parent->next;
    assert_string_equal("traffic_light", node->name);
    assert_null(node->module_name);
    assert_false(node->dflt);
    assert_int_equal(SR_LIST_T, node->type);
    // traffic light, name
    node = node->first_child;
    assert_string_equal("name", node->name);
    assert_null(node->module_name);
    assert_false(node->dflt);
    assert_int_equal(SR_STRING_T, node->type);
    assert_string_equal("c", node->data.enum_val);
    assert_null(node->first_child);
    // traffic light, color
    node = node->next;
    assert_string_equal("color", node->name);
    assert_null(node->module_name);
    assert_false(node->dflt);
    assert_int_equal(SR_ENUM_T, node->type);
    assert_string_equal("orange", node->data.enum_val);
    assert_null(node->first_child);
    assert_null(node->next);
    // no advanced info
    assert_null(node->parent->next);
    // no more cross roads
    assert_null(node->parent->parent->next);

    sr_free_tree(tree);

    /* check xpath that were retrieved */
    const char *xpath_expected_to_be_loaded [] = {
        "/state-module:traffic_stats",
        "/state-module:traffic_stats/cross_road",
        "/state-module:traffic_stats/cross_road[id='0']/traffic_light",
        "/state-module:traffic_stats/cross_road[id='0']/advanced_info",
        "/state-module:traffic_stats/cross_road[id='1']/traffic_light",
        "/state-module:traffic_stats/cross_road[id='1']/advanced_info",
        "/state-module:traffic_stats/cross_road[id='2']/traffic_light",
        "/state-module:traffic_stats/cross_road[id='2']/advanced_info",
    };
    CHECK_LIST_OF_STRINGS(xpath_retrieved, xpath_expected_to_be_loaded);

    /* cleanup */
    sr_unsubscribe(session, subscription);
    sr_session_stop(session);

    for (size_t i = 0; i < xpath_retrieved->count; i++) {
        free(xpath_retrieved->data[i]);
    }
    sr_list_cleanup(xpath_retrieved);
}

static void
cl_nested_data_subscription2(void **state)
{
    sr_conn_ctx_t *conn = *state;
    assert_non_null(conn);
    sr_session_ctx_t *session = NULL;
    sr_subscription_ctx_t *subscription = NULL;
    sr_list_t *xpath_retrieved = NULL;
    sr_val_t *values = NULL;
    size_t cnt = 0;
    int rc = SR_ERR_OK;

    rc = sr_list_init(&xpath_retrieved);
    assert_int_equal(rc, SR_ERR_OK);

    /* start session */
    rc = sr_session_start(conn, SR_DS_RUNNING, SR_SESS_DEFAULT, &session);
    assert_int_equal(rc, SR_ERR_OK);

    rc = sr_module_change_subscribe(session, "state-module", cl_whole_module_cb, NULL,
            0, SR_SUBSCR_DEFAULT, &subscription);
    assert_int_equal(rc, SR_ERR_OK);

    /* subscribe data providers */
    rc = sr_dp_get_items_subscribe(session, "/state-module:bus", cl_dp_bus, xpath_retrieved, SR_SUBSCR_CTX_REUSE, &subscription);
    assert_int_equal(rc, SR_ERR_OK);

    rc = sr_dp_get_items_subscribe(session, "/state-module:traffic_stats", cl_dp_traffic_stats, xpath_retrieved, SR_SUBSCR_CTX_REUSE, &subscription);
    assert_int_equal(rc, SR_ERR_OK);

    /* retrieve data */
    rc = sr_get_items(session, "/state-module:traffic_stats/cross_road[id='0']/advanced_info/*", &values, &cnt);
    assert_int_equal(rc, SR_ERR_OK);

    /* check data */
    assert_non_null(values);
    assert_int_equal(2, cnt);

    assert_int_equal(values[0].type, SR_STRING_T);
    assert_string_equal(values[0].xpath, "/state-module:traffic_stats/cross_road[id='0']/advanced_info/latitude");
    assert_string_equal(values[0].data.string_val, "48.729885N");

    assert_int_equal(values[1].type, SR_STRING_T);
    assert_string_equal(values[1].xpath, "/state-module:traffic_stats/cross_road[id='0']/advanced_info/longitude");
    assert_string_equal(values[1].data.string_val, "19.137425E");

    sr_free_values(values, cnt);

    /* check xpath that were retrieved */
    const char *xpath_expected_to_be_loaded [] = {
        "/state-module:traffic_stats",
        "/state-module:traffic_stats/cross_road",
        "/state-module:traffic_stats/cross_road[id='0']/traffic_light",
        "/state-module:traffic_stats/cross_road[id='0']/advanced_info",
        "/state-module:traffic_stats/cross_road[id='1']/traffic_light",
        "/state-module:traffic_stats/cross_road[id='1']/advanced_info",
        "/state-module:traffic_stats/cross_road[id='2']/traffic_light",
        "/state-module:traffic_stats/cross_road[id='2']/advanced_info",
    };
    CHECK_LIST_OF_STRINGS(xpath_retrieved, xpath_expected_to_be_loaded);

    /* cleanup */
    sr_unsubscribe(session, subscription);
    sr_session_stop(session);

    for (size_t i = 0; i < xpath_retrieved->count; i++) {
        free(xpath_retrieved->data[i]);
    }
    sr_list_cleanup(xpath_retrieved);
}

static void
cl_nested_data_subscription2_tree(void **state)
{
    sr_conn_ctx_t *conn = *state;
    assert_non_null(conn);
    sr_session_ctx_t *session = NULL;
    sr_subscription_ctx_t *subscription = NULL;
    sr_list_t *xpath_retrieved = NULL;
    sr_node_t *tree = NULL, *node = NULL;
    int rc = SR_ERR_OK;

    rc = sr_list_init(&xpath_retrieved);
    assert_int_equal(rc, SR_ERR_OK);

    /* start session */
    rc = sr_session_start(conn, SR_DS_RUNNING, SR_SESS_DEFAULT, &session);
    assert_int_equal(rc, SR_ERR_OK);

    rc = sr_module_change_subscribe(session, "state-module", cl_whole_module_cb, NULL,
            0, SR_SUBSCR_DEFAULT, &subscription);
    assert_int_equal(rc, SR_ERR_OK);

    /* subscribe data providers */
    rc = sr_dp_get_items_subscribe(session, "/state-module:bus", cl_dp_bus, xpath_retrieved, SR_SUBSCR_CTX_REUSE, &subscription);
    assert_int_equal(rc, SR_ERR_OK);

    rc = sr_dp_get_items_subscribe(session, "/state-module:traffic_stats", cl_dp_traffic_stats, xpath_retrieved, SR_SUBSCR_CTX_REUSE, &subscription);
    assert_int_equal(rc, SR_ERR_OK);

    /* retrieve data */
    rc = sr_get_subtree(session, "/state-module:traffic_stats/cross_road[id='0']/advanced_info", 0, &tree);
    assert_int_equal(rc, SR_ERR_OK);

    /* check data */
    assert_non_null(tree);
    // advanced info
    node = tree;
    assert_string_equal("advanced_info", node->name);
    assert_string_equal("state-module", node->module_name);
    assert_false(node->dflt);
    assert_int_equal(SR_CONTAINER_T, node->type);
    // latitude
    node = node->first_child;
    assert_string_equal("latitude", node->name);
    assert_null(node->module_name);
    assert_false(node->dflt);
    assert_int_equal(SR_STRING_T, node->type);
    assert_string_equal("48.729885N", node->data.string_val);
    // longitude
    node = node->next;
    assert_string_equal("longitude", node->name);
    assert_null(node->module_name);
    assert_false(node->dflt);
    assert_int_equal(SR_STRING_T, node->type);
    assert_string_equal("19.137425E", node->data.string_val);
    assert_null(node->next);
    assert_null(node->parent->next);

    sr_free_tree(tree);

    /* check xpath that were retrieved */
    const char *xpath_expected_to_be_loaded [] = {
        "/state-module:traffic_stats",
        "/state-module:traffic_stats/cross_road",
        "/state-module:traffic_stats/cross_road[id='0']/traffic_light",
        "/state-module:traffic_stats/cross_road[id='0']/advanced_info",
        "/state-module:traffic_stats/cross_road[id='1']/traffic_light",
        "/state-module:traffic_stats/cross_road[id='1']/advanced_info",
        "/state-module:traffic_stats/cross_road[id='2']/traffic_light",
        "/state-module:traffic_stats/cross_road[id='2']/advanced_info",
    };
    CHECK_LIST_OF_STRINGS(xpath_retrieved, xpath_expected_to_be_loaded);

    /* cleanup */
    sr_unsubscribe(session, subscription);
    sr_session_stop(session);

    for (size_t i = 0; i < xpath_retrieved->count; i++) {
        free(xpath_retrieved->data[i]);
    }
    sr_list_cleanup(xpath_retrieved);
}

static void
cl_all_state_data(void **state)
{
    sr_conn_ctx_t *conn = *state;
    assert_non_null(conn);
    sr_session_ctx_t *session = NULL;
    sr_subscription_ctx_t *subscription = NULL;
    sr_list_t *xpath_retrieved = NULL;
    sr_val_t *values = NULL;
    size_t cnt = 0;
    int rc = SR_ERR_OK;

    rc = sr_list_init(&xpath_retrieved);
    assert_int_equal(rc, SR_ERR_OK);

    /* start session */
    rc = sr_session_start(conn, SR_DS_RUNNING, SR_SESS_DEFAULT, &session);
    assert_int_equal(rc, SR_ERR_OK);

    rc = sr_module_change_subscribe(session, "state-module", cl_whole_module_cb, NULL,
            0, SR_SUBSCR_DEFAULT, &subscription);
    assert_int_equal(rc, SR_ERR_OK);

    /* subscribe data providers */
    rc = sr_dp_get_items_subscribe(session, "/state-module:bus", cl_dp_bus, xpath_retrieved, SR_SUBSCR_CTX_REUSE, &subscription);
    assert_int_equal(rc, SR_ERR_OK);

    rc = sr_dp_get_items_subscribe(session, "/state-module:traffic_stats", cl_dp_traffic_stats, xpath_retrieved, SR_SUBSCR_CTX_REUSE, &subscription);
    assert_int_equal(rc, SR_ERR_OK);

    /* retrieve data */
    rc = sr_get_items(session, "/state-module:*", &values, &cnt);
    assert_int_equal(rc, SR_ERR_OK);

    /* check data */
    assert_non_null(values);
    assert_int_equal(2, cnt);

    sr_free_values(values, cnt);

    /* check xpath that were retrieved */
    const char *xpath_expected_to_be_loaded [] = {
        "/state-module:traffic_stats",
        "/state-module:traffic_stats/cross_road",
        "/state-module:traffic_stats/cross_road[id='0']/traffic_light",
        "/state-module:traffic_stats/cross_road[id='0']/advanced_info",
        "/state-module:traffic_stats/cross_road[id='1']/traffic_light",
        "/state-module:traffic_stats/cross_road[id='1']/advanced_info",
        "/state-module:traffic_stats/cross_road[id='2']/traffic_light",
        "/state-module:traffic_stats/cross_road[id='2']/advanced_info",
    };
    size_t expected_xp_cnt = sizeof(xpath_expected_to_be_loaded) / sizeof(*xpath_expected_to_be_loaded);
    assert_int_equal(expected_xp_cnt, xpath_retrieved->count);

    for (size_t i = 0; i < expected_xp_cnt; i++) {
        bool match = false;
        for (size_t j = 0; xpath_retrieved->count; j++) {
            if (0 == strcmp(xpath_expected_to_be_loaded[i], (char *) xpath_retrieved->data[j])) {
                match = true;
                break;
            }
        }
        if (!match) {
            /* assert xpath that can not be found */
            assert_string_equal("", xpath_expected_to_be_loaded[i]);
        }
    }

    /* cleanup */
    sr_unsubscribe(session, subscription);
    sr_session_stop(session);

    for (size_t i = 0; i < xpath_retrieved->count; i++) {
        free(xpath_retrieved->data[i]);
    }
    sr_list_cleanup(xpath_retrieved);
}

int
main()
{
    const struct CMUnitTest tests[] = {
        cmocka_unit_test_setup_teardown(cl_exact_match_subscription, sysrepo_setup, sysrepo_teardown),
        cmocka_unit_test_setup_teardown(cl_exact_match_subscription_tree, sysrepo_setup, sysrepo_teardown),
        cmocka_unit_test_setup_teardown(cl_parent_subscription, sysrepo_setup, sysrepo_teardown),
        cmocka_unit_test_setup_teardown(cl_parent_subscription_tree, sysrepo_setup, sysrepo_teardown),
        cmocka_unit_test_setup_teardown(cl_partialy_covered_by_subscription, sysrepo_setup, sysrepo_teardown),
        cmocka_unit_test_setup_teardown(cl_partialy_covered_by_subscription_tree, sysrepo_setup, sysrepo_teardown),
        cmocka_unit_test_setup_teardown(cl_missing_subscription, sysrepo_setup, sysrepo_teardown),
        cmocka_unit_test_setup_teardown(cl_missing_subscription_tree, sysrepo_setup, sysrepo_teardown),
        cmocka_unit_test_setup_teardown(cl_incorrect_data_subscription, sysrepo_setup, sysrepo_teardown),
        cmocka_unit_test_setup_teardown(cl_incorrect_data_subscription_tree, sysrepo_setup, sysrepo_teardown),
        cmocka_unit_test_setup_teardown(cl_dp_neg_subscription, sysrepo_setup, sysrepo_teardown),
        cmocka_unit_test_setup_teardown(cl_nested_data_subscription, sysrepo_setup, sysrepo_teardown),
        cmocka_unit_test_setup_teardown(cl_nested_data_subscription_tree, sysrepo_setup, sysrepo_teardown),
        cmocka_unit_test_setup_teardown(cl_nested_data_subscription2, sysrepo_setup, sysrepo_teardown),
        cmocka_unit_test_setup_teardown(cl_nested_data_subscription2_tree, sysrepo_setup, sysrepo_teardown),
        cmocka_unit_test_setup_teardown(cl_all_state_data, sysrepo_setup, sysrepo_teardown),
    };

    return cmocka_run_group_tests(tests, NULL, NULL);
}
