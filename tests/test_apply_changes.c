/**
 * @file test_apply_changes.c
 * @author Michal Vasko <mvasko@cesnet.cz>
 * @brief test for sr_apply_changes()
 *
 * @copyright
 * Copyright 2018 Deutsche Telekom AG.
 * Copyright 2018 - 2019 CESNET, z.s.p.o.
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
#define _GNU_SOURCE

#include <sys/types.h>
#include <unistd.h>
#include <pthread.h>
#include <stdlib.h>
#include <setjmp.h>
#include <string.h>
#include <stdarg.h>

#include <cmocka.h>
#include <libyang/libyang.h>

#include "tests/config.h"
#include "sysrepo.h"

struct state {
    sr_conn_ctx_t *conn;
    volatile int cb_called, cb_called2;
    pthread_barrier_t barrier;
};

static int
setup(void **state)
{
    struct state *st;
    uint32_t conn_count;

    st = calloc(1, sizeof *st);
    *state = st;

    sr_connection_count(&conn_count);
    assert_int_equal(conn_count, 0);

    if (sr_connect(0, &(st->conn)) != SR_ERR_OK) {
        return 1;
    }

    if (sr_install_module(st->conn, TESTS_DIR "/files/test.yang", TESTS_DIR "/files", NULL, 0) != SR_ERR_OK) {
        return 1;
    }
    if (sr_install_module(st->conn, TESTS_DIR "/files/ietf-interfaces.yang", TESTS_DIR "/files", NULL, 0) != SR_ERR_OK) {
        return 1;
    }
    if (sr_install_module(st->conn, TESTS_DIR "/files/ietf-ip.yang", TESTS_DIR "/files", NULL, 0) != SR_ERR_OK) {
        return 1;
    }
    if (sr_install_module(st->conn, TESTS_DIR "/files/iana-if-type.yang", TESTS_DIR "/files", NULL, 0) != SR_ERR_OK) {
        return 1;
    }
    if (sr_install_module(st->conn, TESTS_DIR "/files/when1.yang", TESTS_DIR "/files", NULL, 0) != SR_ERR_OK) {
        return 1;
    }
    if (sr_install_module(st->conn, TESTS_DIR "/files/when2.yang", TESTS_DIR "/files", NULL, 0) != SR_ERR_OK) {
        return 1;
    }
    if (sr_install_module(st->conn, TESTS_DIR "/files/defaults.yang", TESTS_DIR "/files", NULL, 0) != SR_ERR_OK) {
        return 1;
    }
    sr_disconnect(st->conn);

    if (sr_connect(0, &(st->conn)) != SR_ERR_OK) {
        return 1;
    }

    return 0;
}

static int
teardown(void **state)
{
    struct state *st = (struct state *)*state;

    sr_remove_module(st->conn, "defaults");
    sr_remove_module(st->conn, "when2");
    sr_remove_module(st->conn, "when1");
    sr_remove_module(st->conn, "iana-if-type");
    sr_remove_module(st->conn, "ietf-ip");
    sr_remove_module(st->conn, "ietf-interfaces");
    sr_remove_module(st->conn, "test");

    sr_disconnect(st->conn);
    free(st);
    return 0;
}

static int
setup_f(void **state)
{
    struct state *st = (struct state *)*state;

    st->cb_called = 0;
    st->cb_called2 = 0;
    pthread_barrier_init(&st->barrier, NULL, 2);
    return 0;
}

static int
teardown_f(void **state)
{
    struct state *st = (struct state *)*state;

    pthread_barrier_destroy(&st->barrier);
    return 0;
}

/* TEST 1 */
static int
module_change_done_cb(sr_session_ctx_t *session, const char *module_name, const char *xpath, sr_event_t event,
        uint32_t request_id, void *private_data)
{
    struct state *st = (struct state *)private_data;
    sr_change_oper_t op;
    sr_change_iter_t *iter;
    struct lyd_node *subtree;
    const struct lyd_node *node;
    char *str1;
    const char *str2, *prev_val, *prev_list;
    bool prev_dflt;
    int ret;

    (void)request_id;

    assert_int_equal(sr_session_get_nc_id(session), 52);
    assert_string_equal(module_name, "ietf-interfaces");
    assert_null(xpath);

    switch (st->cb_called) {
    case 0:
    case 1:
    case 2:
        if (st->cb_called < 2) {
            assert_int_equal(event, SR_EV_CHANGE);
        } else {
            assert_int_equal(event, SR_EV_DONE);
        }

        /* get changes iter */
        ret = sr_get_changes_iter(session, "/ietf-interfaces:*//.", &iter);
        assert_int_equal(ret, SR_ERR_OK);

        /* 1st change */
        ret = sr_get_change_tree_next(session, iter, &op, &node, &prev_val, &prev_list, &prev_dflt);
        assert_int_equal(ret, SR_ERR_OK);

        assert_int_equal(op, SR_OP_CREATED);
        assert_null(prev_val);
        assert_string_equal(node->schema->name, "interface");

        /* 2nd change */
        ret = sr_get_change_tree_next(session, iter, &op, &node, &prev_val, &prev_list, &prev_dflt);
        assert_int_equal(ret, SR_ERR_OK);

        assert_int_equal(op, SR_OP_CREATED);
        assert_null(prev_val);
        assert_string_equal(node->schema->name, "name");
        assert_string_equal(((struct lyd_node_leaf_list *)node)->value_str, "eth52");

        /* 3rd change */
        ret = sr_get_change_tree_next(session, iter, &op, &node, &prev_val, &prev_list, &prev_dflt);
        assert_int_equal(ret, SR_ERR_OK);

        assert_int_equal(op, SR_OP_CREATED);
        assert_null(prev_val);
        assert_string_equal(node->schema->name, "type");
        assert_string_equal(((struct lyd_node_leaf_list *)node)->value_str, "iana-if-type:ethernetCsmacd");

        /* 4th change */
        ret = sr_get_change_tree_next(session, iter, &op, &node, &prev_val, &prev_list, &prev_dflt);
        assert_int_equal(ret, SR_ERR_OK);

        assert_int_equal(op, SR_OP_CREATED);
        assert_null(prev_val);
        assert_string_equal(node->schema->name, "ipv4");

        /* 5th change */
        ret = sr_get_change_tree_next(session, iter, &op, &node, &prev_val, &prev_list, &prev_dflt);
        assert_int_equal(ret, SR_ERR_OK);

        assert_int_equal(op, SR_OP_CREATED);
        assert_null(prev_val);
        assert_string_equal(node->schema->name, "address");

        /* 6th change */
        ret = sr_get_change_tree_next(session, iter, &op, &node, &prev_val, &prev_list, &prev_dflt);
        assert_int_equal(ret, SR_ERR_OK);

        assert_int_equal(op, SR_OP_CREATED);
        assert_null(prev_val);
        assert_string_equal(node->schema->name, "ip");
        assert_string_equal(((struct lyd_node_leaf_list *)node)->value_str, "192.168.2.100");

        /* 7th change */
        ret = sr_get_change_tree_next(session, iter, &op, &node, &prev_val, &prev_list, &prev_dflt);
        assert_int_equal(ret, SR_ERR_OK);

        assert_int_equal(op, SR_OP_CREATED);
        assert_null(prev_val);
        assert_string_equal(node->schema->name, "prefix-length");
        assert_string_equal(((struct lyd_node_leaf_list *)node)->value_str, "24");

        /* 8th change */
        ret = sr_get_change_tree_next(session, iter, &op, &node, &prev_val, &prev_list, &prev_dflt);
        assert_int_equal(ret, SR_ERR_OK);

        assert_int_equal(op, SR_OP_CREATED);
        assert_null(prev_val);
        assert_string_equal(node->schema->name, "enabled");
        assert_int_equal(node->dflt, 1);

        /* 9th change */
        ret = sr_get_change_tree_next(session, iter, &op, &node, &prev_val, &prev_list, &prev_dflt);
        assert_int_equal(ret, SR_ERR_OK);

        assert_int_equal(op, SR_OP_CREATED);
        assert_null(prev_val);
        assert_string_equal(node->schema->name, "forwarding");
        assert_int_equal(node->dflt, 1);

        /* 10th change */
        ret = sr_get_change_tree_next(session, iter, &op, &node, &prev_val, &prev_list, &prev_dflt);
        assert_int_equal(ret, SR_ERR_OK);

        assert_int_equal(op, SR_OP_CREATED);
        assert_null(prev_val);
        assert_string_equal(node->schema->name, "enabled");
        assert_int_equal(node->dflt, 1);

        /* no more changes */
        ret = sr_get_change_tree_next(session, iter, &op, &node, &prev_val, &prev_list, &prev_dflt);
        assert_int_equal(ret, SR_ERR_NOT_FOUND);

        sr_free_change_iter(iter);

        /* check current data tree */
        ret = sr_get_subtree(session, "/ietf-interfaces:interfaces", 0, &subtree);
        assert_int_equal(ret, SR_ERR_OK);

        ret = lyd_schema_sort(subtree, 1);
        assert_int_equal(ret, 0);
        ret = lyd_print_mem(&str1, subtree, LYD_XML, LYP_WITHSIBLINGS | LYP_WD_IMPL_TAG);
        assert_int_equal(ret, 0);
        lyd_free(subtree);

        str2 =
        "<interfaces xmlns=\"urn:ietf:params:xml:ns:yang:ietf-interfaces\""
            " xmlns:ncwd=\"urn:ietf:params:xml:ns:yang:ietf-netconf-with-defaults\">"
            "<interface>"
                "<name>eth52</name>"
                "<type xmlns:ianaift=\"urn:ietf:params:xml:ns:yang:iana-if-type\">ianaift:ethernetCsmacd</type>"
                "<enabled ncwd:default=\"true\">true</enabled>"
                "<ipv4 xmlns=\"urn:ietf:params:xml:ns:yang:ietf-ip\">"
                    "<enabled ncwd:default=\"true\">true</enabled>"
                    "<forwarding ncwd:default=\"true\">false</forwarding>"
                    "<address>"
                        "<ip>192.168.2.100</ip>"
                        "<prefix-length>24</prefix-length>"
                    "</address>"
                "</ipv4>"
            "</interface>"
        "</interfaces>";

        assert_string_equal(str1, str2);
        free(str1);
        break;
    case 3:
    case 4:
        if (st->cb_called == 3) {
            assert_int_equal(event, SR_EV_CHANGE);
        } else {
            assert_int_equal(event, SR_EV_DONE);
        }

        /* get changes iter */
        ret = sr_get_changes_iter(session, "/ietf-interfaces:*//.", &iter);
        assert_int_equal(ret, SR_ERR_OK);

        /* 1st change */
        ret = sr_get_change_tree_next(session, iter, &op, &node, &prev_val, &prev_list, &prev_dflt);
        assert_int_equal(ret, SR_ERR_OK);

        assert_int_equal(op, SR_OP_DELETED);
        assert_null(prev_val);
        assert_string_equal(node->schema->name, "interface");

        /* 2nd change */
        ret = sr_get_change_tree_next(session, iter, &op, &node, &prev_val, &prev_list, &prev_dflt);
        assert_int_equal(ret, SR_ERR_OK);

        assert_int_equal(op, SR_OP_DELETED);
        assert_null(prev_val);
        assert_string_equal(node->schema->name, "name");
        assert_string_equal(((struct lyd_node_leaf_list *)node)->value_str, "eth52");

        /* 3rd change */
        ret = sr_get_change_tree_next(session, iter, &op, &node, &prev_val, &prev_list, &prev_dflt);
        assert_int_equal(ret, SR_ERR_OK);

        assert_int_equal(op, SR_OP_DELETED);
        assert_null(prev_val);
        assert_string_equal(node->schema->name, "type");
        assert_string_equal(((struct lyd_node_leaf_list *)node)->value_str, "iana-if-type:ethernetCsmacd");

        /* 4th change */
        ret = sr_get_change_tree_next(session, iter, &op, &node, &prev_val, &prev_list, &prev_dflt);
        assert_int_equal(ret, SR_ERR_OK);

        assert_int_equal(op, SR_OP_DELETED);
        assert_null(prev_val);
        assert_string_equal(node->schema->name, "ipv4");

        /* 5th change */
        ret = sr_get_change_tree_next(session, iter, &op, &node, &prev_val, &prev_list, &prev_dflt);
        assert_int_equal(ret, SR_ERR_OK);

        assert_int_equal(op, SR_OP_DELETED);
        assert_null(prev_val);
        assert_string_equal(node->schema->name, "address");

        /* 6th change */
        ret = sr_get_change_tree_next(session, iter, &op, &node, &prev_val, &prev_list, &prev_dflt);
        assert_int_equal(ret, SR_ERR_OK);

        assert_int_equal(op, SR_OP_DELETED);
        assert_null(prev_val);
        assert_string_equal(node->schema->name, "ip");
        assert_string_equal(((struct lyd_node_leaf_list *)node)->value_str, "192.168.2.100");

        /* 7th change */
        ret = sr_get_change_tree_next(session, iter, &op, &node, &prev_val, &prev_list, &prev_dflt);
        assert_int_equal(ret, SR_ERR_OK);

        assert_int_equal(op, SR_OP_DELETED);
        assert_null(prev_val);
        assert_string_equal(node->schema->name, "prefix-length");
        assert_string_equal(((struct lyd_node_leaf_list *)node)->value_str, "24");

        /* 8th change */
        ret = sr_get_change_tree_next(session, iter, &op, &node, &prev_val, &prev_list, &prev_dflt);
        assert_int_equal(ret, SR_ERR_OK);

        assert_int_equal(op, SR_OP_DELETED);
        assert_null(prev_val);
        assert_string_equal(node->schema->name, "enabled");
        assert_int_equal(node->dflt, 1);

        /* 9th change */
        ret = sr_get_change_tree_next(session, iter, &op, &node, &prev_val, &prev_list, &prev_dflt);
        assert_int_equal(ret, SR_ERR_OK);

        assert_int_equal(op, SR_OP_DELETED);
        assert_null(prev_val);
        assert_string_equal(node->schema->name, "forwarding");
        assert_int_equal(node->dflt, 1);

        /* 10th change */
        ret = sr_get_change_tree_next(session, iter, &op, &node, &prev_val, &prev_list, &prev_dflt);
        assert_int_equal(ret, SR_ERR_OK);

        assert_int_equal(op, SR_OP_DELETED);
        assert_null(prev_val);
        assert_string_equal(node->schema->name, "enabled");
        assert_int_equal(node->dflt, 1);

        /* no more changes */
        ret = sr_get_change_tree_next(session, iter, &op, &node, &prev_val, &prev_list, &prev_dflt);
        assert_int_equal(ret, SR_ERR_NOT_FOUND);

        sr_free_change_iter(iter);

        /* check current data tree */
        ret = sr_get_subtree(session, "/ietf-interfaces:interfaces", 0, &subtree);
        assert_int_equal(ret, SR_ERR_OK);

        ret = lyd_print_mem(&str1, subtree, LYD_XML, LYP_WITHSIBLINGS);
        assert_int_equal(ret, 0);
        lyd_free_withsiblings(subtree);

        assert_null(str1);
        break;
    default:
        fail();
    }

    ++st->cb_called;
    if (st->cb_called == 1) {
        return SR_ERR_CALLBACK_SHELVE;
    }
    return SR_ERR_OK;
}

static void *
apply_change_done_thread(void *arg)
{
    struct state *st = (struct state *)arg;
    sr_session_ctx_t *sess;
    struct lyd_node *subtree;
    char *str1;
    const char *str2;
    int ret;

    ret = sr_session_start(st->conn, SR_DS_RUNNING, &sess);
    assert_int_equal(ret, SR_ERR_OK);

    /* set NC SID so we can read it in the callback */
    sr_session_set_nc_id(sess, 52);

    ret = sr_set_item_str(sess, "/ietf-interfaces:interfaces/interface[name='eth52']/type", "iana-if-type:ethernetCsmacd", NULL, 0);
    assert_int_equal(ret, SR_ERR_OK);
    ret = sr_set_item_str(sess, "/ietf-interfaces:interfaces/interface[name='eth52']/ietf-ip:ipv4/address[ip='192.168.2.100']"
            "/prefix-length", "24", NULL, 0);
    assert_int_equal(ret, SR_ERR_OK);

    /* wait for subscription before applying changes */
    pthread_barrier_wait(&st->barrier);

    /* perform 1st change */
    ret = sr_apply_changes(sess, 0);
    assert_int_equal(ret, SR_ERR_OK);

    /* check current data tree */
    ret = sr_get_subtree(sess, "/ietf-interfaces:interfaces", 0, &subtree);
    assert_int_equal(ret, SR_ERR_OK);

    ret = lyd_print_mem(&str1, subtree, LYD_XML, LYP_WITHSIBLINGS);
    assert_int_equal(ret, 0);
    lyd_free(subtree);

    str2 =
    "<interfaces xmlns=\"urn:ietf:params:xml:ns:yang:ietf-interfaces\">"
        "<interface>"
            "<name>eth52</name>"
            "<type xmlns:ianaift=\"urn:ietf:params:xml:ns:yang:iana-if-type\">ianaift:ethernetCsmacd</type>"
            "<ipv4 xmlns=\"urn:ietf:params:xml:ns:yang:ietf-ip\">"
                "<address>"
                    "<ip>192.168.2.100</ip>"
                    "<prefix-length>24</prefix-length>"
                "</address>"
            "</ipv4>"
        "</interface>"
    "</interfaces>";

    assert_string_equal(str1, str2);
    free(str1);

    /* perform 2nd change */
    ret = sr_delete_item(sess, "/ietf-interfaces:interfaces", 0);
    assert_int_equal(ret, SR_ERR_OK);
    ret = sr_apply_changes(sess, 0);
    assert_int_equal(ret, SR_ERR_OK);

    /* check current data tree */
    ret = sr_get_subtree(sess, "/ietf-interfaces:interfaces", 0, &subtree);
    assert_int_equal(ret, SR_ERR_OK);

    ret = lyd_print_mem(&str1, subtree, LYD_XML, LYP_WITHSIBLINGS);
    assert_int_equal(ret, 0);

    assert_null(str1);
    lyd_free(subtree);

    /* signal that we have finished applying changes */
    pthread_barrier_wait(&st->barrier);

    sr_session_stop(sess);
    return NULL;
}

static void *
subscribe_change_done_thread(void *arg)
{
    struct state *st = (struct state *)arg;
    sr_session_ctx_t *sess;
    sr_subscription_ctx_t *subscr;
    int count, ret;

    ret = sr_session_start(st->conn, SR_DS_RUNNING, &sess);
    assert_int_equal(ret, SR_ERR_OK);

    ret = sr_module_change_subscribe(sess, "ietf-interfaces", NULL, module_change_done_cb, st, 0, 0, &subscr);
    assert_int_equal(ret, SR_ERR_OK);

    /* signal that subscription was created */
    pthread_barrier_wait(&st->barrier);

    count = 0;
    while ((st->cb_called < 1) && (count < 1500)) {
        usleep(10000);
        ++count;
    }
    assert_int_equal(st->cb_called, 1);

    /* callback was shelved, process it again */
    ret = sr_process_events(subscr, NULL, NULL);
    assert_int_equal(ret, SR_ERR_OK);

    count = 0;
    while ((st->cb_called < 5) && (count < 1500)) {
        usleep(10000);
        ++count;
    }
    assert_int_equal(st->cb_called, 5);

    /* wait for the other thread to finish */
    pthread_barrier_wait(&st->barrier);

    sr_unsubscribe(subscr);
    sr_session_stop(sess);
    return NULL;
}

static void
test_change_done(void **state)
{
    pthread_t tid[2];

    pthread_create(&tid[0], NULL, apply_change_done_thread, *state);
    pthread_create(&tid[1], NULL, subscribe_change_done_thread, *state);

    pthread_join(tid[0], NULL);
    pthread_join(tid[1], NULL);
}

/* TEST 2 */
static int
module_update_cb(sr_session_ctx_t *session, const char *module_name, const char *xpath, sr_event_t event,
        uint32_t request_id, void *private_data)
{
    struct state *st = (struct state *)private_data;
    sr_change_oper_t op;
    sr_change_iter_t *iter;
    sr_val_t *old_val, *new_val;
    int ret;

    (void)request_id;

    assert_string_equal(module_name, "ietf-interfaces");
    assert_null(xpath);

    switch (st->cb_called) {
    case 0:
        assert_int_equal(event, SR_EV_UPDATE);

        /* get changes iter */
        ret = sr_get_changes_iter(session, "/ietf-interfaces:*//.", &iter);
        assert_int_equal(ret, SR_ERR_OK);

        /* 1st change */
        ret = sr_get_change_next(session, iter, &op, &old_val, &new_val);
        assert_int_equal(ret, SR_ERR_OK);

        assert_int_equal(op, SR_OP_CREATED);
        assert_null(old_val);
        assert_non_null(new_val);
        assert_string_equal(new_val->xpath, "/ietf-interfaces:interfaces/interface[name='eth52']");

        sr_free_val(new_val);

        /* 2nd change */
        ret = sr_get_change_next(session, iter, &op, &old_val, &new_val);
        assert_int_equal(ret, SR_ERR_OK);

        assert_int_equal(op, SR_OP_CREATED);
        assert_null(old_val);
        assert_non_null(new_val);
        assert_string_equal(new_val->xpath, "/ietf-interfaces:interfaces/interface[name='eth52']/name");

        sr_free_val(new_val);

        /* 3rd change */
        ret = sr_get_change_next(session, iter, &op, &old_val, &new_val);
        assert_int_equal(ret, SR_ERR_OK);

        assert_int_equal(op, SR_OP_CREATED);
        assert_null(old_val);
        assert_non_null(new_val);
        assert_string_equal(new_val->xpath, "/ietf-interfaces:interfaces/interface[name='eth52']/type");

        sr_free_val(new_val);

        /* 4th change */
        ret = sr_get_change_next(session, iter, &op, &old_val, &new_val);
        assert_int_equal(ret, SR_ERR_OK);

        assert_int_equal(op, SR_OP_CREATED);
        assert_null(old_val);
        assert_non_null(new_val);
        assert_string_equal(new_val->xpath, "/ietf-interfaces:interfaces/interface[name='eth52']/enabled");

        sr_free_val(new_val);

        /* no more changes */
        ret = sr_get_change_next(session, iter, &op, &old_val, &new_val);
        assert_int_equal(ret, SR_ERR_NOT_FOUND);

        sr_free_change_iter(iter);

        /* let's create another interface */
        ret = sr_set_item_str(session, "/ietf-interfaces:interfaces/interface[name='eth64']/type",
                "iana-if-type:ethernetCsmacd", NULL, 0);
        assert_int_equal(ret, SR_ERR_OK);
        break;
    case 1:
    case 4:
        /* not interested in other events */
        assert_int_equal(event, SR_EV_CHANGE);
        break;
    case 2:
    case 5:
        /* not interested in other events */
        assert_int_equal(event, SR_EV_DONE);
        break;
    case 3:
        assert_int_equal(event, SR_EV_UPDATE);

        /* get changes iter */
        ret = sr_get_changes_iter(session, "/ietf-interfaces:*//.", &iter);
        assert_int_equal(ret, SR_ERR_OK);

        /* 1st change */
        ret = sr_get_change_next(session, iter, &op, &old_val, &new_val);
        assert_int_equal(ret, SR_ERR_OK);

        assert_int_equal(op, SR_OP_DELETED);
        assert_non_null(old_val);
        assert_null(new_val);
        assert_string_equal(old_val->xpath, "/ietf-interfaces:interfaces/interface[name='eth52']");

        sr_free_val(old_val);

        /* 2nd change */
        ret = sr_get_change_next(session, iter, &op, &old_val, &new_val);
        assert_int_equal(ret, SR_ERR_OK);

        assert_int_equal(op, SR_OP_DELETED);
        assert_non_null(old_val);
        assert_null(new_val);
        assert_string_equal(old_val->xpath, "/ietf-interfaces:interfaces/interface[name='eth52']/name");

        sr_free_val(old_val);

        /* 3rd change */
        ret = sr_get_change_next(session, iter, &op, &old_val, &new_val);
        assert_int_equal(ret, SR_ERR_OK);

        assert_int_equal(op, SR_OP_DELETED);
        assert_non_null(old_val);
        assert_null(new_val);
        assert_string_equal(old_val->xpath, "/ietf-interfaces:interfaces/interface[name='eth52']/type");

        sr_free_val(old_val);

        /* 4th change */
        ret = sr_get_change_next(session, iter, &op, &old_val, &new_val);
        assert_int_equal(ret, SR_ERR_OK);

        assert_int_equal(op, SR_OP_DELETED);
        assert_non_null(old_val);
        assert_null(new_val);
        assert_string_equal(old_val->xpath, "/ietf-interfaces:interfaces/interface[name='eth52']/enabled");

        sr_free_val(old_val);

        /* no more changes */
        ret = sr_get_change_next(session, iter, &op, &old_val, &new_val);
        assert_int_equal(ret, SR_ERR_NOT_FOUND);

        sr_free_change_iter(iter);

        /* delete the other interface */
        ret = sr_delete_item(session, "/ietf-interfaces:interfaces/interface[name='eth64']", 0);
        assert_int_equal(ret, SR_ERR_OK);
        break;
    default:
        fail();
    }

    ++st->cb_called;
    return SR_ERR_OK;
}

static void *
apply_update_thread(void *arg)
{
    struct state *st = (struct state *)arg;
    sr_session_ctx_t *sess;
    sr_val_t sr_val;
    struct lyd_node *subtree;
    char *str1;
    const char *str2;
    int ret;

    ret = sr_session_start(st->conn, SR_DS_RUNNING, &sess);
    assert_int_equal(ret, SR_ERR_OK);

    sr_val.xpath = "/ietf-interfaces:interfaces/interface[name='eth52']/type";
    sr_val.type = SR_STRING_T;
    sr_val.dflt = false;
    sr_val.data.string_val = "iana-if-type:ethernetCsmacd";

    ret = sr_set_item(sess, NULL, &sr_val, 0);
    assert_int_equal(ret, SR_ERR_OK);

    /* wait for subscription before applying changes */
    pthread_barrier_wait(&st->barrier);

    /* perform 1st change */
    ret = sr_apply_changes(sess, 0);
    assert_int_equal(ret, SR_ERR_OK);

    /* check current data tree */
    ret = sr_get_subtree(sess, "/ietf-interfaces:interfaces", 0, &subtree);
    assert_int_equal(ret, SR_ERR_OK);

    ret = lyd_print_mem(&str1, subtree, LYD_XML, LYP_WITHSIBLINGS);
    assert_int_equal(ret, 0);
    lyd_free(subtree);

    str2 =
    "<interfaces xmlns=\"urn:ietf:params:xml:ns:yang:ietf-interfaces\">"
        "<interface>"
            "<name>eth52</name>"
            "<type xmlns:ianaift=\"urn:ietf:params:xml:ns:yang:iana-if-type\">ianaift:ethernetCsmacd</type>"
        "</interface>"
        "<interface>"
            "<name>eth64</name>"
            "<type xmlns:ianaift=\"urn:ietf:params:xml:ns:yang:iana-if-type\">ianaift:ethernetCsmacd</type>"
        "</interface>"
    "</interfaces>";

    assert_string_equal(str1, str2);
    free(str1);

    /* perform 2nd change */
    ret = sr_delete_item(sess, "/ietf-interfaces:interfaces/interface[name='eth52']", 0);
    assert_int_equal(ret, SR_ERR_OK);
    ret = sr_apply_changes(sess, 0);
    assert_int_equal(ret, SR_ERR_OK);

    /* check current data tree */
    ret = sr_get_subtree(sess, "/ietf-interfaces:interfaces", 0, &subtree);
    assert_int_equal(ret, SR_ERR_OK);

    ret = lyd_print_mem(&str1, subtree, LYD_XML, LYP_WITHSIBLINGS);
    assert_int_equal(ret, 0);

    assert_null(str1);
    lyd_free(subtree);

    /* signal that we have finished applying changes */
    pthread_barrier_wait(&st->barrier);

    sr_session_stop(sess);
    return NULL;
}

static void *
subscribe_update_thread(void *arg)
{
    struct state *st = (struct state *)arg;
    sr_session_ctx_t *sess;
    sr_subscription_ctx_t *subscr;
    int count, ret;

    ret = sr_session_start(st->conn, SR_DS_RUNNING, &sess);
    assert_int_equal(ret, SR_ERR_OK);

    /* it should subscribe to "running" as well */
    ret = sr_module_change_subscribe(sess, "ietf-interfaces", NULL, module_update_cb, st, 0, SR_SUBSCR_UPDATE, &subscr);
    assert_int_equal(ret, SR_ERR_OK);

    /* signal that subscription was created */
    pthread_barrier_wait(&st->barrier);

    count = 0;
    while ((st->cb_called < 6) && (count < 1500)) {
        usleep(10000);
        ++count;
    }
    assert_int_equal(st->cb_called, 6);

    /* wait for the other thread to finish */
    pthread_barrier_wait(&st->barrier);

    sr_unsubscribe(subscr);
    sr_session_stop(sess);
    return NULL;
}

static void
test_update(void **state)
{
    pthread_t tid[2];

    pthread_create(&tid[0], NULL, apply_update_thread, *state);
    pthread_create(&tid[1], NULL, subscribe_update_thread, *state);

    pthread_join(tid[0], NULL);
    pthread_join(tid[1], NULL);
}

/* TEST 3 */
static int
module_update_fail_cb(sr_session_ctx_t *session, const char *module_name, const char *xpath, sr_event_t event,
        uint32_t request_id, void *private_data)
{
    struct state *st = (struct state *)private_data;
    int ret = SR_ERR_OK;

    (void)session;
    (void)request_id;

    assert_string_equal(module_name, "ietf-interfaces");
    assert_null(xpath);
    assert_int_equal(event, SR_EV_UPDATE);

    switch (st->cb_called) {
    case 0:
        /* update fails */
        sr_set_error(session, "/path/to/a/node", "Custom user callback error.");
        ret = SR_ERR_UNSUPPORTED;
        break;
    default:
        fail();
    }

    ++st->cb_called;
    return ret;
}

static void *
apply_update_fail_thread(void *arg)
{
    struct state *st = (struct state *)arg;
    sr_session_ctx_t *sess;
    const sr_error_info_t *err_info;
    sr_val_t sr_val;
    struct lyd_node *subtree;
    char *str1;
    int ret;

    ret = sr_session_start(st->conn, SR_DS_RUNNING, &sess);
    assert_int_equal(ret, SR_ERR_OK);

    sr_val.xpath = "/ietf-interfaces:interfaces/interface[name='eth52']/type";
    sr_val.type = SR_STRING_T;
    sr_val.dflt = false;
    sr_val.data.string_val = "iana-if-type:ethernetCsmacd";

    ret = sr_set_item(sess, NULL, &sr_val, 0);
    assert_int_equal(ret, SR_ERR_OK);

    /* wait for subscription before applying changes */
    pthread_barrier_wait(&st->barrier);

    /* perform the change (it should fail) */
    ret = sr_apply_changes(sess, 0);
    assert_int_equal(ret, SR_ERR_CALLBACK_FAILED);
    ret = sr_get_error(sess, &err_info);
    assert_int_equal(ret, SR_ERR_OK);
    assert_int_equal(err_info->err_count, 1);
    assert_string_equal(err_info->err[0].message, "Custom user callback error.");
    assert_string_equal(err_info->err[0].xpath, "/path/to/a/node");

    ret = sr_discard_changes(sess);
    assert_int_equal(ret, SR_ERR_OK);

    /* check current data tree */
    ret = sr_get_subtree(sess, "/ietf-interfaces:interfaces", 0, &subtree);
    assert_int_equal(ret, SR_ERR_OK);

    ret = lyd_print_mem(&str1, subtree, LYD_XML, LYP_WITHSIBLINGS);
    assert_int_equal(ret, 0);

    assert_null(str1);
    lyd_free(subtree);

    /* signal that we have finished applying changes */
    pthread_barrier_wait(&st->barrier);

    sr_session_stop(sess);
    return NULL;
}

static void *
subscribe_update_fail_thread(void *arg)
{
    struct state *st = (struct state *)arg;
    sr_session_ctx_t *sess;
    sr_subscription_ctx_t *subscr;
    int count, ret;

    ret = sr_session_start(st->conn, SR_DS_RUNNING, &sess);
    assert_int_equal(ret, SR_ERR_OK);

    ret = sr_module_change_subscribe(sess, "ietf-interfaces", NULL, module_update_fail_cb, st, 0, SR_SUBSCR_UPDATE, &subscr);
    assert_int_equal(ret, SR_ERR_OK);
    ret = sr_module_change_subscribe(sess, "ietf-interfaces", NULL, module_update_fail_cb, st, 0, SR_SUBSCR_CTX_REUSE, &subscr);
    assert_int_equal(ret, SR_ERR_OK);

    /* signal that subscription was created */
    pthread_barrier_wait(&st->barrier);

    count = 0;
    while ((st->cb_called < 1) && (count < 1500)) {
        usleep(10000);
        ++count;
    }
    assert_int_equal(st->cb_called, 1);

    /* wait for the other thread to finish */
    pthread_barrier_wait(&st->barrier);

    sr_unsubscribe(subscr);
    sr_session_stop(sess);
    return NULL;
}

static void
test_update_fail(void **state)
{
    pthread_t tid[2];

    pthread_create(&tid[0], NULL, apply_update_fail_thread, *state);
    pthread_create(&tid[1], NULL, subscribe_update_fail_thread, *state);

    pthread_join(tid[0], NULL);
    pthread_join(tid[1], NULL);
}

/* TEST 4 */
static int
module_test_change_fail_cb(sr_session_ctx_t *session, const char *module_name, const char *xpath, sr_event_t event,
        uint32_t request_id, void *private_data)
{
    struct state *st = (struct state *)private_data;
    sr_change_oper_t op;
    sr_change_iter_t *iter;
    sr_val_t *old_val, *new_val;
    int ret;

    (void)request_id;

    assert_string_equal(module_name, "test");
    assert_null(xpath);

    switch (st->cb_called) {
    case 0:
        assert_int_equal(event, SR_EV_CHANGE);

        /* get changes iter */
        ret = sr_get_changes_iter(session, "/test:*//.", &iter);
        assert_int_equal(ret, SR_ERR_OK);

        /* 1st change */
        ret = sr_get_change_next(session, iter, &op, &old_val, &new_val);
        assert_int_equal(ret, SR_ERR_OK);

        assert_int_equal(op, SR_OP_MOVED);
        assert_non_null(old_val);
        assert_string_equal(old_val->xpath, "/test:l1[k='key2']");
        assert_non_null(new_val);
        assert_string_equal(new_val->xpath, "/test:l1[k='key1']");

        sr_free_val(old_val);
        sr_free_val(new_val);

        /* no more changes */
        ret = sr_get_change_next(session, iter, &op, &old_val, &new_val);
        assert_int_equal(ret, SR_ERR_NOT_FOUND);

        sr_free_change_iter(iter);
        break;
    case 2:
        assert_int_equal(event, SR_EV_ABORT);

        /* get changes iter */
        ret = sr_get_changes_iter(session, "/test:*//.", &iter);
        assert_int_equal(ret, SR_ERR_OK);

        /* 1st change */
        ret = sr_get_change_next(session, iter, &op, &old_val, &new_val);
        assert_int_equal(ret, SR_ERR_OK);

        assert_int_equal(op, SR_OP_MOVED);
        assert_null(old_val);
        assert_non_null(new_val);
        assert_string_equal(new_val->xpath, "/test:l1[k='key1']");

        sr_free_val(new_val);

        /* no more changes */
        ret = sr_get_change_next(session, iter, &op, &old_val, &new_val);
        assert_int_equal(ret, SR_ERR_NOT_FOUND);

        sr_free_change_iter(iter);
        break;
    default:
        fail();
    }

    ++st->cb_called;
    return SR_ERR_OK;
}

static int
module_ifc_change_fail_cb(sr_session_ctx_t *session, const char *module_name, const char *xpath, sr_event_t event,
        uint32_t request_id, void *private_data)
{
    struct state *st = (struct state *)private_data;
    sr_change_oper_t op;
    sr_change_iter_t *iter;
    sr_val_t *old_val, *new_val;
    int ret = SR_ERR_OK;

    (void)request_id;

    assert_string_equal(module_name, "ietf-interfaces");
    assert_null(xpath);

    switch (st->cb_called) {
    case 1:
        assert_int_equal(event, SR_EV_CHANGE);

        /* get changes iter */
        ret = sr_get_changes_iter(session, "/ietf-interfaces:*//.", &iter);
        assert_int_equal(ret, SR_ERR_OK);

        /* 1st change */
        ret = sr_get_change_next(session, iter, &op, &old_val, &new_val);
        assert_int_equal(ret, SR_ERR_OK);

        assert_int_equal(op, SR_OP_CREATED);
        assert_null(old_val);
        assert_non_null(new_val);
        assert_string_equal(new_val->xpath, "/ietf-interfaces:interfaces/interface[name='eth52']");

        sr_free_val(new_val);

        /* 2nd change */
        ret = sr_get_change_next(session, iter, &op, &old_val, &new_val);
        assert_int_equal(ret, SR_ERR_OK);

        assert_int_equal(op, SR_OP_CREATED);
        assert_null(old_val);
        assert_non_null(new_val);
        assert_string_equal(new_val->xpath, "/ietf-interfaces:interfaces/interface[name='eth52']/name");

        sr_free_val(new_val);

        /* 3rd change */
        ret = sr_get_change_next(session, iter, &op, &old_val, &new_val);
        assert_int_equal(ret, SR_ERR_OK);

        assert_int_equal(op, SR_OP_CREATED);
        assert_null(old_val);
        assert_non_null(new_val);
        assert_string_equal(new_val->xpath, "/ietf-interfaces:interfaces/interface[name='eth52']/type");

        sr_free_val(new_val);

        /* 4th change */
        ret = sr_get_change_next(session, iter, &op, &old_val, &new_val);
        assert_int_equal(ret, SR_ERR_OK);

        assert_int_equal(op, SR_OP_CREATED);
        assert_null(old_val);
        assert_non_null(new_val);
        assert_string_equal(new_val->xpath, "/ietf-interfaces:interfaces/interface[name='eth52']/enabled");

        sr_free_val(new_val);

        /* no more changes */
        ret = sr_get_change_next(session, iter, &op, &old_val, &new_val);
        assert_int_equal(ret, SR_ERR_NOT_FOUND);

        sr_free_change_iter(iter);

        /* callback fails and should not be called again */
        ret = SR_ERR_UNSUPPORTED;
        break;
    case 3:
        assert_int_equal(event, SR_EV_CHANGE);

        /* get changes iter */
        ret = sr_get_changes_iter(session, "/ietf-interfaces:*//.", &iter);
        assert_int_equal(ret, SR_ERR_OK);

        /* 1st change */
        ret = sr_get_change_next(session, iter, &op, &old_val, &new_val);
        assert_int_equal(ret, SR_ERR_OK);

        assert_int_equal(op, SR_OP_CREATED);
        assert_null(old_val);
        assert_non_null(new_val);
        assert_string_equal(new_val->xpath, "/ietf-interfaces:interfaces/interface[name='eth52']");

        sr_free_val(new_val);

        /* 2nd change */
        ret = sr_get_change_next(session, iter, &op, &old_val, &new_val);
        assert_int_equal(ret, SR_ERR_OK);

        assert_int_equal(op, SR_OP_CREATED);
        assert_null(old_val);
        assert_non_null(new_val);
        assert_string_equal(new_val->xpath, "/ietf-interfaces:interfaces/interface[name='eth52']/name");

        sr_free_val(new_val);

        /* 3rd change */
        ret = sr_get_change_next(session, iter, &op, &old_val, &new_val);
        assert_int_equal(ret, SR_ERR_OK);

        assert_int_equal(op, SR_OP_CREATED);
        assert_null(old_val);
        assert_non_null(new_val);
        assert_string_equal(new_val->xpath, "/ietf-interfaces:interfaces/interface[name='eth52']/type");

        sr_free_val(new_val);

        /* 4th change */
        ret = sr_get_change_next(session, iter, &op, &old_val, &new_val);
        assert_int_equal(ret, SR_ERR_OK);

        assert_int_equal(op, SR_OP_CREATED);
        assert_null(old_val);
        assert_non_null(new_val);
        assert_string_equal(new_val->xpath, "/ietf-interfaces:interfaces/interface[name='eth52']/enabled");

        sr_free_val(new_val);

        /* no more changes */
        ret = sr_get_change_next(session, iter, &op, &old_val, &new_val);
        assert_int_equal(ret, SR_ERR_NOT_FOUND);

        sr_free_change_iter(iter);

        /* callback fails and should not be called again */
        ret = SR_ERR_UNSUPPORTED;
        break;
    default:
        fail();
    }

    ++st->cb_called;
    return ret;
}

static void *
apply_change_fail_thread(void *arg)
{
    struct state *st = (struct state *)arg;
    sr_session_ctx_t *sess;
    const sr_error_info_t *err_info;
    struct lyd_node *subtree;
    char *str1;
    int ret;

    ret = sr_session_start(st->conn, SR_DS_RUNNING, &sess);
    assert_int_equal(ret, SR_ERR_OK);

    ret = sr_move_item(sess, "/test:l1[k='key1']", SR_MOVE_AFTER, "[k='key2']", NULL, NULL);
    assert_int_equal(ret, SR_ERR_OK);
    ret = sr_set_item_str(sess, "/ietf-interfaces:interfaces/interface[name='eth52']/type", "iana-if-type:ethernetCsmacd", NULL, 0);
    assert_int_equal(ret, SR_ERR_OK);

    /* wait for subscription before applying changes */
    pthread_barrier_wait(&st->barrier);

    /* perform the change (it should fail) */
    ret = sr_apply_changes(sess, 0);
    assert_int_equal(ret, SR_ERR_CALLBACK_FAILED);

    /* no custom error message set */
    ret = sr_get_error(sess, &err_info);
    assert_int_equal(ret, SR_ERR_OK);
    assert_int_equal(err_info->err_count, 1);
    assert_string_equal(err_info->err[0].message, "Operation not supported");
    assert_null(err_info->err[0].xpath);

    ret = sr_discard_changes(sess);
    assert_int_equal(ret, SR_ERR_OK);

    /* check current data tree */
    ret = sr_get_subtree(sess, "/ietf-interfaces:interfaces", 0, &subtree);
    assert_int_equal(ret, SR_ERR_OK);

    ret = lyd_print_mem(&str1, subtree, LYD_XML, LYP_WITHSIBLINGS);
    assert_int_equal(ret, 0);

    assert_null(str1);
    lyd_free(subtree);

    /* signal that we have finished applying changes #1 */
    pthread_barrier_wait(&st->barrier);

    /* perform a single change (it should fail) */
    ret = sr_set_item_str(sess, "/ietf-interfaces:interfaces/interface[name='eth52']/type", "iana-if-type:ethernetCsmacd", NULL, 0);
    assert_int_equal(ret, SR_ERR_OK);
    ret = sr_apply_changes(sess, 0);
    assert_int_equal(ret, SR_ERR_CALLBACK_FAILED);

    ret = sr_discard_changes(sess);
    assert_int_equal(ret, SR_ERR_OK);

    /* check current data tree */
    ret = sr_get_subtree(sess, "/ietf-interfaces:interfaces", 0, &subtree);
    assert_int_equal(ret, SR_ERR_OK);

    ret = lyd_print_mem(&str1, subtree, LYD_XML, LYP_WITHSIBLINGS);
    assert_int_equal(ret, 0);

    assert_null(str1);
    lyd_free(subtree);

    /* signal that we have finished applying changes #2 */
    pthread_barrier_wait(&st->barrier);

    sr_session_stop(sess);
    return NULL;
}

static void *
subscribe_change_fail_thread(void *arg)
{
    struct state *st = (struct state *)arg;
    sr_session_ctx_t *sess;
    sr_subscription_ctx_t *subscr;
    int count, ret;

    ret = sr_session_start(st->conn, SR_DS_RUNNING, &sess);
    assert_int_equal(ret, SR_ERR_OK);

    /* create testing user-ordered list data */
    ret = sr_set_item_str(sess, "/test:l1[k='key1']/v", "1", NULL, 0);
    assert_int_equal(ret, SR_ERR_OK);
    ret = sr_set_item_str(sess, "/test:l1[k='key2']/v", "2", NULL, 0);
    assert_int_equal(ret, SR_ERR_OK);
    ret = sr_apply_changes(sess, 0);
    assert_int_equal(ret, SR_ERR_OK);

    ret = sr_module_change_subscribe(sess, "ietf-interfaces", NULL, module_ifc_change_fail_cb, st, 0, 0, &subscr);
    assert_int_equal(ret, SR_ERR_OK);
    ret = sr_module_change_subscribe(sess, "test", NULL, module_test_change_fail_cb, st, 0, SR_SUBSCR_CTX_REUSE, &subscr);
    assert_int_equal(ret, SR_ERR_OK);

    /* signal that subscription was created */
    pthread_barrier_wait(&st->barrier);

    count = 0;
    while ((st->cb_called < 3) && (count < 1500)) {
        usleep(10000);
        ++count;
    }
    assert_int_equal(st->cb_called, 3);

    /* wait for the other thread to signal #1 */
    pthread_barrier_wait(&st->barrier);

    count = 0;
    while ((st->cb_called < 4) && (count < 1500)) {
        usleep(10000);
        ++count;
    }
    assert_int_equal(st->cb_called, 4);

    /* wait for the other thread to signal #2 */
    pthread_barrier_wait(&st->barrier);

    sr_unsubscribe(subscr);

    /* cleanup after ourselves */
    ret = sr_delete_item(sess, "/test:l1[k='key1']", SR_EDIT_STRICT);
    assert_int_equal(ret, SR_ERR_OK);
    ret = sr_delete_item(sess, "/test:l1[k='key2']", SR_EDIT_STRICT);
    assert_int_equal(ret, SR_ERR_OK);
    ret = sr_apply_changes(sess, 0);
    assert_int_equal(ret, SR_ERR_OK);

    sr_session_stop(sess);
    return NULL;
}

static void
test_change_fail(void **state)
{
    pthread_t tid[2];

    pthread_create(&tid[0], NULL, apply_change_fail_thread, *state);
    pthread_create(&tid[1], NULL, subscribe_change_fail_thread, *state);

    pthread_join(tid[0], NULL);
    pthread_join(tid[1], NULL);
}

/* TEST 5 */
static int
module_change_done_dflt_cb(sr_session_ctx_t *session, const char *module_name, const char *xpath, sr_event_t event,
        uint32_t request_id, void *private_data)
{
    struct state *st = (struct state *)private_data;
    sr_change_oper_t op;
    sr_change_iter_t *iter;
    sr_val_t *old_val, *new_val;
    int ret;

    (void)request_id;

    assert_string_equal(module_name, "defaults");
    assert_null(xpath);

    switch (st->cb_called) {
    case 0:
    case 1:
        if (st->cb_called == 0) {
            assert_int_equal(event, SR_EV_CHANGE);
        } else {
            assert_int_equal(event, SR_EV_DONE);
        }

        /* get changes iter */
        ret = sr_get_changes_iter(session, "/defaults:*//.", &iter);
        assert_int_equal(ret, SR_ERR_OK);

        /* 1st change */
        ret = sr_get_change_next(session, iter, &op, &old_val, &new_val);
        assert_int_equal(ret, SR_ERR_OK);

        assert_int_equal(op, SR_OP_CREATED);
        assert_null(old_val);
        assert_non_null(new_val);
        assert_int_equal(new_val->dflt, 0);
        assert_string_equal(new_val->xpath, "/defaults:l1[k='when-true']");

        sr_free_val(new_val);

        /* 2nd change */
        ret = sr_get_change_next(session, iter, &op, &old_val, &new_val);
        assert_int_equal(ret, SR_ERR_OK);

        assert_int_equal(op, SR_OP_CREATED);
        assert_null(old_val);
        assert_non_null(new_val);
        assert_int_equal(new_val->dflt, 0);
        assert_string_equal(new_val->xpath, "/defaults:l1[k='when-true']/k");

        sr_free_val(new_val);

        /* 3rd change */
        ret = sr_get_change_next(session, iter, &op, &old_val, &new_val);
        assert_int_equal(ret, SR_ERR_OK);

        assert_int_equal(op, SR_OP_CREATED);
        assert_null(old_val);
        assert_non_null(new_val);
        assert_int_equal(new_val->dflt, 1);
        assert_string_equal(new_val->xpath, "/defaults:l1[k='when-true']/cont1");

        sr_free_val(new_val);

        /* 4th change */
        ret = sr_get_change_next(session, iter, &op, &old_val, &new_val);
        assert_int_equal(ret, SR_ERR_OK);

        assert_int_equal(op, SR_OP_CREATED);
        assert_null(old_val);
        assert_non_null(new_val);
        assert_int_equal(new_val->dflt, 1);
        assert_string_equal(new_val->xpath, "/defaults:l1[k='when-true']/cont1/cont2");

        sr_free_val(new_val);

        /* 5th change */
        ret = sr_get_change_next(session, iter, &op, &old_val, &new_val);
        assert_int_equal(ret, SR_ERR_OK);

        assert_int_equal(op, SR_OP_CREATED);
        assert_null(old_val);
        assert_non_null(new_val);
        assert_int_equal(new_val->dflt, 1);
        assert_string_equal(new_val->xpath, "/defaults:l1[k='when-true']/cont1/cont2/dflt1");

        sr_free_val(new_val);

        /* 6th change */
        ret = sr_get_change_next(session, iter, &op, &old_val, &new_val);
        assert_int_equal(ret, SR_ERR_OK);

        assert_int_equal(op, SR_OP_CREATED);
        assert_null(old_val);
        assert_non_null(new_val);
        assert_int_equal(new_val->dflt, 1);
        assert_string_equal(new_val->xpath, "/defaults:dflt2");

        sr_free_val(new_val);

        /* no more changes */
        ret = sr_get_change_next(session, iter, &op, &old_val, &new_val);
        assert_int_equal(ret, SR_ERR_NOT_FOUND);

        sr_free_change_iter(iter);
        break;
    case 2:
    case 3:
        if (st->cb_called == 2) {
            assert_int_equal(event, SR_EV_CHANGE);
        } else {
            assert_int_equal(event, SR_EV_DONE);
        }

        /* get changes iter */
        ret = sr_get_changes_iter(session, "/defaults:*//.", &iter);
        assert_int_equal(ret, SR_ERR_OK);

        /* 1st change */
        ret = sr_get_change_next(session, iter, &op, &old_val, &new_val);
        assert_int_equal(ret, SR_ERR_OK);

        assert_int_equal(op, SR_OP_MODIFIED);
        assert_non_null(old_val);
        assert_int_equal(old_val->dflt, 1);
        assert_string_equal(old_val->xpath, "/defaults:l1[k='when-true']/cont1/cont2/dflt1");
        assert_int_equal(old_val->data.uint8_val, 10);
        assert_non_null(new_val);
        assert_int_equal(new_val->dflt, 0);
        assert_string_equal(new_val->xpath, "/defaults:l1[k='when-true']/cont1/cont2/dflt1");
        assert_int_equal(new_val->data.uint8_val, 5);

        sr_free_val(old_val);
        sr_free_val(new_val);

        /* no more changes */
        ret = sr_get_change_next(session, iter, &op, &old_val, &new_val);
        assert_int_equal(ret, SR_ERR_NOT_FOUND);

        sr_free_change_iter(iter);
        break;
    case 4:
    case 5:
        if (st->cb_called == 4) {
            assert_int_equal(event, SR_EV_CHANGE);
        } else {
            assert_int_equal(event, SR_EV_DONE);
        }

        /* get changes iter */
        ret = sr_get_changes_iter(session, "/defaults:*//.", &iter);
        assert_int_equal(ret, SR_ERR_OK);

        /* 1st change */
        ret = sr_get_change_next(session, iter, &op, &old_val, &new_val);
        assert_int_equal(ret, SR_ERR_OK);

        assert_int_equal(op, SR_OP_MODIFIED);
        assert_non_null(old_val);
        assert_int_equal(old_val->dflt, 0);
        assert_string_equal(old_val->xpath, "/defaults:l1[k='when-true']/cont1/cont2/dflt1");
        assert_int_equal(old_val->data.uint8_val, 5);
        assert_non_null(new_val);
        assert_int_equal(new_val->dflt, 0);
        assert_string_equal(new_val->xpath, "/defaults:l1[k='when-true']/cont1/cont2/dflt1");
        assert_int_equal(new_val->data.uint8_val, 10);

        sr_free_val(old_val);
        sr_free_val(new_val);

        /* no more changes */
        ret = sr_get_change_next(session, iter, &op, &old_val, &new_val);
        assert_int_equal(ret, SR_ERR_NOT_FOUND);

        sr_free_change_iter(iter);
        break;
    case 6:
    case 7:
        if (st->cb_called == 6) {
            assert_int_equal(event, SR_EV_CHANGE);
        } else {
            assert_int_equal(event, SR_EV_DONE);
        }

        /* get changes iter */
        ret = sr_get_changes_iter(session, "/defaults:*//.", &iter);
        assert_int_equal(ret, SR_ERR_OK);

        /* 1st change */
        ret = sr_get_change_next(session, iter, &op, &old_val, &new_val);
        assert_int_equal(ret, SR_ERR_OK);

        assert_int_equal(op, SR_OP_DELETED);
        assert_non_null(old_val);
        assert_null(new_val);
        assert_int_equal(old_val->dflt, 0);
        assert_string_equal(old_val->xpath, "/defaults:l1[k='when-true']");

        sr_free_val(old_val);

        /* 2nd change */
        ret = sr_get_change_next(session, iter, &op, &old_val, &new_val);
        assert_int_equal(ret, SR_ERR_OK);

        assert_int_equal(op, SR_OP_DELETED);
        assert_non_null(old_val);
        assert_null(new_val);
        assert_int_equal(old_val->dflt, 0);
        assert_string_equal(old_val->xpath, "/defaults:l1[k='when-true']/k");

        sr_free_val(old_val);

        /* 3rd change */
        ret = sr_get_change_next(session, iter, &op, &old_val, &new_val);
        assert_int_equal(ret, SR_ERR_OK);

        assert_int_equal(op, SR_OP_DELETED);
        assert_non_null(old_val);
        assert_null(new_val);
        assert_int_equal(old_val->dflt, 1);
        assert_string_equal(old_val->xpath, "/defaults:l1[k='when-true']/cont1");

        sr_free_val(old_val);

        /* 4th change */
        ret = sr_get_change_next(session, iter, &op, &old_val, &new_val);
        assert_int_equal(ret, SR_ERR_OK);

        assert_int_equal(op, SR_OP_DELETED);
        assert_non_null(old_val);
        assert_null(new_val);
        assert_int_equal(old_val->dflt, 1);
        assert_string_equal(old_val->xpath, "/defaults:l1[k='when-true']/cont1/cont2");

        sr_free_val(old_val);

        /* 5th change */
        ret = sr_get_change_next(session, iter, &op, &old_val, &new_val);
        assert_int_equal(ret, SR_ERR_OK);

        assert_int_equal(op, SR_OP_DELETED);
        assert_non_null(old_val);
        assert_null(new_val);
        assert_int_equal(old_val->dflt, 1);
        assert_string_equal(old_val->xpath, "/defaults:l1[k='when-true']/cont1/cont2/dflt1");

        sr_free_val(old_val);

        /* 6th change */
        ret = sr_get_change_next(session, iter, &op, &old_val, &new_val);
        assert_int_equal(ret, SR_ERR_OK);

        assert_int_equal(op, SR_OP_DELETED);
        assert_non_null(old_val);
        assert_null(new_val);
        assert_int_equal(old_val->dflt, 1);
        assert_string_equal(old_val->xpath, "/defaults:dflt2");

        sr_free_val(old_val);

        /* no more changes */
        ret = sr_get_change_next(session, iter, &op, &old_val, &new_val);
        assert_int_equal(ret, SR_ERR_NOT_FOUND);

        sr_free_change_iter(iter);
        break;
    default:
        fail();
    }

    if (event == SR_EV_DONE) {
        /* let other thread now even done event was handled */
        pthread_barrier_wait(&st->barrier);
    }

    ++st->cb_called;
    return SR_ERR_OK;
}

static void *
apply_change_done_dflt_thread(void *arg)
{
    struct state *st = (struct state *)arg;
    sr_session_ctx_t *sess;
    struct lyd_node *data;
    char *str1;
    const char *str2;
    int ret;

    ret = sr_session_start(st->conn, SR_DS_RUNNING, &sess);
    assert_int_equal(ret, SR_ERR_OK);

    ret = sr_set_item_str(sess, "/defaults:l1[k='when-true']", NULL, NULL, 0);
    assert_int_equal(ret, SR_ERR_OK);

    /* wait for subscription before applying changes */
    pthread_barrier_wait(&st->barrier);

    /*
     * perform 1st change
     *
     * (create list that will cause other 2 containers with 1 default value and another default value to be created)
     */
    ret = sr_apply_changes(sess, 0);
    assert_int_equal(ret, SR_ERR_OK);

    /* check current data tree */
    ret = sr_get_data(sess, "/defaults:*", 0, 0, 0, &data);
    assert_int_equal(ret, SR_ERR_OK);

    assert_string_equal(data->schema->name, "l1");
    ret = lyd_print_mem(&str1, data, LYD_XML, LYP_WITHSIBLINGS | LYP_WD_IMPL_TAG);
    assert_int_equal(ret, 0);

    lyd_free_withsiblings(data);

    str2 =
    "<l1 xmlns=\"urn:defaults\" xmlns:ncwd=\"urn:ietf:params:xml:ns:yang:ietf-netconf-with-defaults\">"
        "<k>when-true</k>"
        "<cont1>"
            "<cont2>"
                "<dflt1 ncwd:default=\"true\">10</dflt1>"
            "</cont2>"
        "</cont1>"
    "</l1>"
    "<dflt2 xmlns=\"urn:defaults\" xmlns:ncwd=\"urn:ietf:params:xml:ns:yang:ietf-netconf-with-defaults\" ncwd:default=\"true\">"
        "I exist!"
    "</dflt2>";

    assert_string_equal(str1, str2);
    free(str1);

    pthread_barrier_wait(&st->barrier);

    /*
     * perform 2nd change
     *
     * (change default leaf from default to explicitly set with different value)
     */
    ret = sr_set_item_str(sess, "/defaults:l1[k='when-true']/cont1/cont2/dflt1", "5", NULL, 0);
    assert_int_equal(ret, SR_ERR_OK);

    ret = sr_apply_changes(sess, 0);
    assert_int_equal(ret, SR_ERR_OK);

    /* check current data tree */
    ret = sr_get_data(sess, "/defaults:*", 0, 0, 0, &data);
    assert_int_equal(ret, SR_ERR_OK);

    /* check only first node */
    assert_string_equal(data->schema->name, "l1");
    assert_int_equal(data->child->next->child->child->dflt, 0);
    assert_string_equal(((struct lyd_node_leaf_list *)data->child->next->child->child)->value_str, "5");

    lyd_free_withsiblings(data);

    pthread_barrier_wait(&st->barrier);

    /*
     * perform 3rd change
     *
     * (change leaf value to be equal to the default but should not behave as default)
     */
    ret = sr_set_item_str(sess, "/defaults:l1[k='when-true']/cont1/cont2/dflt1", "10", NULL, 0);
    assert_int_equal(ret, SR_ERR_OK);

    ret = sr_apply_changes(sess, 0);
    assert_int_equal(ret, SR_ERR_OK);

    /* check current data tree */
    ret = sr_get_data(sess, "/defaults:*", 0, 0, 0, &data);
    assert_int_equal(ret, SR_ERR_OK);

    /* check only first node */
    assert_string_equal(data->schema->name, "l1");
    assert_int_equal(data->child->next->child->child->dflt, 0);
    assert_string_equal(((struct lyd_node_leaf_list *)data->child->next->child->child)->value_str, "10");

    lyd_free_withsiblings(data);

    pthread_barrier_wait(&st->barrier);

    /*
     * perform 4th change (empty diff, no callbacks called)
     *
     * (remove the explicitly set leaf so that it is default but with the same value)
     */
    ret = sr_delete_item(sess, "/defaults:l1[k='when-true']/cont1/cont2/dflt1", 0);
    assert_int_equal(ret, SR_ERR_OK);

    ret = sr_apply_changes(sess, 0);
    assert_int_equal(ret, SR_ERR_OK);

    /* check current data tree */
    ret = sr_get_data(sess, "/defaults:*", 0, 0, 0, &data);
    assert_int_equal(ret, SR_ERR_OK);

    /* check only first node */
    assert_string_equal(data->schema->name, "l1");
    assert_int_equal(data->child->next->child->child->dflt, 1);
    assert_string_equal(((struct lyd_node_leaf_list *)data->child->next->child->child)->value_str, "10");

    lyd_free_withsiblings(data);

    /*
     * perform 5th change
     *
     * (remove the list instance and so also the top-level default leaf should be automatically removed)
     */
    ret = sr_delete_item(sess, "/defaults:l1[k='when-true']", 0);
    assert_int_equal(ret, SR_ERR_OK);

    ret = sr_apply_changes(sess, 0);
    assert_int_equal(ret, SR_ERR_OK);

    /* check current data tree */
    ret = sr_get_data(sess, "/defaults:*", 0, 0, 0, &data);
    assert_int_equal(ret, SR_ERR_OK);

    assert_null(data);

    pthread_barrier_wait(&st->barrier);

    sr_session_stop(sess);
    return NULL;
}

static void *
subscribe_change_done_dflt_thread(void *arg)
{
    struct state *st = (struct state *)arg;
    sr_session_ctx_t *sess;
    sr_subscription_ctx_t *subscr;
    int count, ret;

    ret = sr_session_start(st->conn, SR_DS_RUNNING, &sess);
    assert_int_equal(ret, SR_ERR_OK);

    ret = sr_module_change_subscribe(sess, "defaults", NULL, module_change_done_dflt_cb, st, 0, 0, &subscr);
    assert_int_equal(ret, SR_ERR_OK);

    /* signal that subscription was created */
    pthread_barrier_wait(&st->barrier);

    count = 0;
    while ((st->cb_called < 8) && (count < 1500)) {
        usleep(10000);
        ++count;
    }
    assert_int_equal(st->cb_called, 8);

    sr_unsubscribe(subscr);
    sr_session_stop(sess);
    return NULL;
}

static void
test_change_done_dflt(void **state)
{
    pthread_t tid[2];

    pthread_create(&tid[0], NULL, apply_change_done_dflt_thread, *state);
    pthread_create(&tid[1], NULL, subscribe_change_done_dflt_thread, *state);

    pthread_join(tid[0], NULL);
    pthread_join(tid[1], NULL);
}

/* TEST 6 */
static int
module_change_done_when_cb(sr_session_ctx_t *session, const char *module_name, const char *xpath, sr_event_t event,
        uint32_t request_id, void *private_data)
{
    struct state *st = (struct state *)private_data;
    sr_change_oper_t op;
    sr_change_iter_t *iter;
    sr_val_t *old_val, *new_val;
    int ret;

    (void)request_id;

    assert_null(xpath);

    if (!strcmp(module_name, "when1")) {
        switch (st->cb_called) {
        case 0:
        case 1:
            if (st->cb_called == 0) {
                assert_int_equal(event, SR_EV_CHANGE);
            } else {
                assert_int_equal(event, SR_EV_DONE);
            }

            /* get changes iter */
            ret = sr_get_changes_iter(session, "/when1:*//.", &iter);
            assert_int_equal(ret, SR_ERR_OK);

            /* 1st change */
            ret = sr_get_change_next(session, iter, &op, &old_val, &new_val);
            assert_int_equal(ret, SR_ERR_OK);

            assert_int_equal(op, SR_OP_CREATED);
            assert_null(old_val);
            assert_non_null(new_val);
            assert_string_equal(new_val->xpath, "/when1:l1");

            sr_free_val(new_val);

            /* no more changes */
            ret = sr_get_change_next(session, iter, &op, &old_val, &new_val);
            assert_int_equal(ret, SR_ERR_NOT_FOUND);

            sr_free_change_iter(iter);
            break;
        case 2:
        case 3:
            if (st->cb_called == 2) {
                assert_int_equal(event, SR_EV_CHANGE);
            } else {
                assert_int_equal(event, SR_EV_DONE);
            }

            /* get changes iter */
            ret = sr_get_changes_iter(session, "/when1:*//.", &iter);
            assert_int_equal(ret, SR_ERR_OK);

            /* 1st change */
            ret = sr_get_change_next(session, iter, &op, &old_val, &new_val);
            assert_int_equal(ret, SR_ERR_OK);

            assert_int_equal(op, SR_OP_DELETED);
            assert_non_null(old_val);
            assert_null(new_val);
            assert_string_equal(old_val->xpath, "/when1:l1");

            sr_free_val(old_val);

            /* 2nd change */
            ret = sr_get_change_next(session, iter, &op, &old_val, &new_val);
            assert_int_equal(ret, SR_ERR_OK);

            assert_int_equal(op, SR_OP_CREATED);
            assert_null(old_val);
            assert_non_null(new_val);
            assert_string_equal(new_val->xpath, "/when1:l2");

            sr_free_val(new_val);

            /* no more changes */
            ret = sr_get_change_next(session, iter, &op, &old_val, &new_val);
            assert_int_equal(ret, SR_ERR_NOT_FOUND);

            sr_free_change_iter(iter);
            break;
        case 4:
        case 5:
            if (st->cb_called == 4) {
                assert_int_equal(event, SR_EV_CHANGE);
            } else {
                assert_int_equal(event, SR_EV_DONE);
            }

            /* get changes iter */
            ret = sr_get_changes_iter(session, "/when1:*//.", &iter);
            assert_int_equal(ret, SR_ERR_OK);

            /* 1st change */
            ret = sr_get_change_next(session, iter, &op, &old_val, &new_val);
            assert_int_equal(ret, SR_ERR_OK);

            assert_int_equal(op, SR_OP_DELETED);
            assert_non_null(old_val);
            assert_null(new_val);
            assert_string_equal(old_val->xpath, "/when1:l2");

            sr_free_val(old_val);

            /* no more changes */
            ret = sr_get_change_next(session, iter, &op, &old_val, &new_val);
            assert_int_equal(ret, SR_ERR_NOT_FOUND);

            sr_free_change_iter(iter);
            break;
        default:
            fail();
        }

        ++st->cb_called;
    } else if (!strcmp(module_name, "when2")) {
        switch (st->cb_called2) {
        case 0:
        case 1:
            if (st->cb_called2 == 0) {
                assert_int_equal(event, SR_EV_CHANGE);
            } else {
                assert_int_equal(event, SR_EV_DONE);
            }

            /* get changes iter */
            ret = sr_get_changes_iter(session, "/when2:*//.", &iter);
            assert_int_equal(ret, SR_ERR_OK);

            /* 1st change */
            ret = sr_get_change_next(session, iter, &op, &old_val, &new_val);
            assert_int_equal(ret, SR_ERR_OK);

            assert_int_equal(op, SR_OP_CREATED);
            assert_null(old_val);
            assert_non_null(new_val);
            assert_string_equal(new_val->xpath, "/when2:cont");

            sr_free_val(new_val);

            /* 2nd change */
            ret = sr_get_change_next(session, iter, &op, &old_val, &new_val);
            assert_int_equal(ret, SR_ERR_OK);

            assert_int_equal(op, SR_OP_CREATED);
            assert_null(old_val);
            assert_non_null(new_val);
            assert_string_equal(new_val->xpath, "/when2:cont/l");

            sr_free_val(new_val);

            /* no more changes */
            ret = sr_get_change_next(session, iter, &op, &old_val, &new_val);
            assert_int_equal(ret, SR_ERR_NOT_FOUND);

            sr_free_change_iter(iter);
            break;
        case 2:
        case 3:
            if (st->cb_called2 == 2) {
                assert_int_equal(event, SR_EV_CHANGE);
            } else {
                assert_int_equal(event, SR_EV_DONE);
            }

            /* get changes iter */
            ret = sr_get_changes_iter(session, "/when2:*//.", &iter);
            assert_int_equal(ret, SR_ERR_OK);

            /* 1st change */
            ret = sr_get_change_next(session, iter, &op, &old_val, &new_val);
            assert_int_equal(ret, SR_ERR_OK);

            assert_int_equal(op, SR_OP_CREATED);
            assert_null(old_val);
            assert_non_null(new_val);
            assert_int_equal(new_val->dflt, 1);
            assert_string_equal(new_val->xpath, "/when2:ll");

            sr_free_val(new_val);

            /* 2nd change */
            ret = sr_get_change_next(session, iter, &op, &old_val, &new_val);
            assert_int_equal(ret, SR_ERR_OK);

            assert_int_equal(op, SR_OP_DELETED);
            assert_non_null(old_val);
            assert_null(new_val);
            assert_string_equal(old_val->xpath, "/when2:cont");

            sr_free_val(old_val);

            /* 3rd change */
            ret = sr_get_change_next(session, iter, &op, &old_val, &new_val);
            assert_int_equal(ret, SR_ERR_OK);

            assert_int_equal(op, SR_OP_DELETED);
            assert_non_null(old_val);
            assert_null(new_val);
            assert_string_equal(old_val->xpath, "/when2:cont/l");

            sr_free_val(old_val);

            /* no more changes */
            ret = sr_get_change_next(session, iter, &op, &old_val, &new_val);
            assert_int_equal(ret, SR_ERR_NOT_FOUND);

            sr_free_change_iter(iter);
            break;
        case 4:
        case 5:
            if (st->cb_called2 == 4) {
                assert_int_equal(event, SR_EV_CHANGE);
            } else {
                assert_int_equal(event, SR_EV_DONE);
            }

            /* get changes iter */
            ret = sr_get_changes_iter(session, "/when2:*//.", &iter);
            assert_int_equal(ret, SR_ERR_OK);

            /* 1st change */
            ret = sr_get_change_next(session, iter, &op, &old_val, &new_val);
            assert_int_equal(ret, SR_ERR_OK);

            assert_int_equal(op, SR_OP_DELETED);
            assert_non_null(old_val);
            assert_null(new_val);
            assert_int_equal(old_val->dflt, 1);
            assert_string_equal(old_val->xpath, "/when2:ll");

            sr_free_val(old_val);

            /* no more changes */
            ret = sr_get_change_next(session, iter, &op, &old_val, &new_val);
            assert_int_equal(ret, SR_ERR_NOT_FOUND);

            sr_free_change_iter(iter);
            break;
        default:
            fail();
        }

        ++st->cb_called2;
    } else {
        fail();
    }

    return SR_ERR_OK;
}

static void *
apply_change_done_when_thread(void *arg)
{
    struct state *st = (struct state *)arg;
    sr_session_ctx_t *sess;
    struct lyd_node *data;
    int ret;

    ret = sr_session_start(st->conn, SR_DS_RUNNING, &sess);
    assert_int_equal(ret, SR_ERR_OK);

    ret = sr_set_item_str(sess, "/when2:cont/l", "bye", NULL, 0);
    assert_int_equal(ret, SR_ERR_OK);

    /* wait for subscription before applying changes */
    pthread_barrier_wait(&st->barrier);

    /*
     * perform 1st change (validation will fail, no callbacks called)
     *
     * (create container with a leaf and false when)
     */
    ret = sr_apply_changes(sess, 0);
    assert_int_equal(ret, SR_ERR_VALIDATION_FAILED);
    ret = sr_discard_changes(sess);
    assert_int_equal(ret, SR_ERR_OK);

    /* check current data tree */
    ret = sr_get_data(sess, "/when2:*", 0, 0, 0, &data);
    assert_int_equal(ret, SR_ERR_OK);

    assert_null(data);

    /*
     * perform 2nd change
     *
     * (create the same container with leaf but also foreign leaf so that when is true)
     */
    ret = sr_set_item_str(sess, "/when2:cont/l", "bye", NULL, 0);
    assert_int_equal(ret, SR_ERR_OK);
    ret = sr_set_item_str(sess, "/when1:l1", "good", NULL, 0);
    assert_int_equal(ret, SR_ERR_OK);

    ret = sr_apply_changes(sess, 0);
    assert_int_equal(ret, SR_ERR_OK);

    /* check current data tree */
    ret = sr_get_data(sess, "/when1:* | /when2:*", 0, 0, 0, &data);
    assert_int_equal(ret, SR_ERR_OK);

    assert_string_equal(data->schema->name, "cont");
    assert_string_equal(((struct lyd_node_leaf_list *)data->child)->value_str, "bye");
    assert_string_equal(data->next->schema->name, "l1");

    lyd_free_withsiblings(data);

    /*
     * perform 3rd change
     *
     * (make the container be removed and a new default leaf be created)
     */
    ret = sr_delete_item(sess, "/when1:l1", 0);
    assert_int_equal(ret, SR_ERR_OK);
    ret = sr_set_item_str(sess, "/when1:l2", "night", NULL, 0);
    assert_int_equal(ret, SR_ERR_OK);

    ret = sr_apply_changes(sess, 0);
    assert_int_equal(ret, SR_ERR_OK);

    /* check current data tree */
    ret = sr_get_data(sess, "/when1:* | /when2:*", 0, 0, 0, &data);
    assert_int_equal(ret, SR_ERR_OK);

    assert_string_equal(data->schema->name, "ll");
    assert_int_equal(data->dflt, 1);
    assert_string_equal(((struct lyd_node_leaf_list *)data)->value_str, "zzZZzz");
    assert_string_equal(data->next->schema->name, "l2");

    lyd_free_withsiblings(data);

    /*
     * perform 4th change
     *
     * (remove leaf so that no when is true and no data present)
     */
    ret = sr_delete_item(sess, "/when1:l2", 0);
    assert_int_equal(ret, SR_ERR_OK);
    ret = sr_apply_changes(sess, 0);
    assert_int_equal(ret, SR_ERR_OK);

    /* check current data tree */
    ret = sr_get_data(sess, "/when1:* | /when2:*", 0, 0, 0, &data);
    assert_int_equal(ret, SR_ERR_OK);
    assert_null(data);

    /* signal that we have finished applying changes */
    pthread_barrier_wait(&st->barrier);

    sr_session_stop(sess);
    return NULL;
}

static void *
subscribe_change_done_when_thread(void *arg)
{
    struct state *st = (struct state *)arg;
    sr_session_ctx_t *sess;
    sr_subscription_ctx_t *subscr;
    int count, ret;

    ret = sr_session_start(st->conn, SR_DS_RUNNING, &sess);
    assert_int_equal(ret, SR_ERR_OK);

    ret = sr_module_change_subscribe(sess, "when1", NULL, module_change_done_when_cb, st, 0, 0, &subscr);
    assert_int_equal(ret, SR_ERR_OK);
    ret = sr_module_change_subscribe(sess, "when2", NULL, module_change_done_when_cb, st, 0, SR_SUBSCR_CTX_REUSE, &subscr);
    assert_int_equal(ret, SR_ERR_OK);

    /* signal that subscription was created */
    pthread_barrier_wait(&st->barrier);

    count = 0;
    while (((st->cb_called < 6) || (st->cb_called2 < 6)) && (count < 1500)) {
        usleep(10000);
        ++count;
    }
    assert_int_equal(st->cb_called, 6);
    assert_int_equal(st->cb_called2, 6);

    /* wait for the other thread to finish */
    pthread_barrier_wait(&st->barrier);

    sr_unsubscribe(subscr);
    sr_session_stop(sess);
    return NULL;
}

static void
test_change_done_when(void **state)
{
    pthread_t tid[2];

    pthread_create(&tid[0], NULL, apply_change_done_when_thread, *state);
    pthread_create(&tid[1], NULL, subscribe_change_done_when_thread, *state);

    pthread_join(tid[0], NULL);
    pthread_join(tid[1], NULL);
}

/* TEST 7 */
static int
module_change_done_xpath_cb(sr_session_ctx_t *session, const char *module_name, const char *xpath, sr_event_t event,
        uint32_t request_id, void *private_data)
{
    struct state *st = (struct state *)private_data;
    sr_change_oper_t op;
    sr_change_iter_t *iter;
    sr_val_t *old_val, *new_val;
    int ret;

    (void)request_id;

    assert_string_equal(module_name, "test");

    switch (st->cb_called) {
    case 0:
    case 2:
        assert_string_equal(xpath, "/test:l1[k='subscr']");
        if (st->cb_called == 0) {
            assert_int_equal(event, SR_EV_CHANGE);
        } else {
            assert_int_equal(event, SR_EV_DONE);
        }

        /* get changes iter */
        ret = sr_get_changes_iter(session, "/test:*//.", &iter);
        assert_int_equal(ret, SR_ERR_OK);

        /* 1st change */
        ret = sr_get_change_next(session, iter, &op, &old_val, &new_val);
        assert_int_equal(ret, SR_ERR_OK);

        assert_int_equal(op, SR_OP_CREATED);
        assert_null(old_val);
        assert_non_null(new_val);
        assert_string_equal(new_val->xpath, "/test:l1[k='subscr']");

        sr_free_val(new_val);

        /* 2nd change */
        ret = sr_get_change_next(session, iter, &op, &old_val, &new_val);
        assert_int_equal(ret, SR_ERR_OK);

        assert_int_equal(op, SR_OP_CREATED);
        assert_null(old_val);
        assert_non_null(new_val);
        assert_string_equal(new_val->xpath, "/test:l1[k='subscr']/k");

        sr_free_val(new_val);

        /* 3rd change */
        ret = sr_get_change_next(session, iter, &op, &old_val, &new_val);
        assert_int_equal(ret, SR_ERR_OK);

        assert_int_equal(op, SR_OP_CREATED);
        assert_null(old_val);
        assert_non_null(new_val);
        assert_string_equal(new_val->xpath, "/test:l1[k='subscr']/v");

        sr_free_val(new_val);

        /* no more changes */
        ret = sr_get_change_next(session, iter, &op, &old_val, &new_val);
        assert_int_equal(ret, SR_ERR_NOT_FOUND);

        sr_free_change_iter(iter);
        break;
    case 1:
    case 3:
        assert_string_equal(xpath, "/test:cont");
        if (st->cb_called == 1) {
            assert_int_equal(event, SR_EV_CHANGE);
        } else {
            assert_int_equal(event, SR_EV_DONE);
        }

        /* get changes iter */
        ret = sr_get_changes_iter(session, "/test:*//.", &iter);
        assert_int_equal(ret, SR_ERR_OK);

        /* 1st change */
        ret = sr_get_change_next(session, iter, &op, &old_val, &new_val);
        assert_int_equal(ret, SR_ERR_OK);

        assert_int_equal(op, SR_OP_CREATED);
        assert_null(old_val);
        assert_non_null(new_val);
        assert_string_equal(new_val->xpath, "/test:cont/l2[k='subscr']");

        sr_free_val(new_val);

        /* 2nd change */
        ret = sr_get_change_next(session, iter, &op, &old_val, &new_val);
        assert_int_equal(ret, SR_ERR_OK);

        assert_int_equal(op, SR_OP_CREATED);
        assert_null(old_val);
        assert_non_null(new_val);
        assert_string_equal(new_val->xpath, "/test:cont/l2[k='subscr']/k");

        sr_free_val(new_val);

        /* 3rd change */
        ret = sr_get_change_next(session, iter, &op, &old_val, &new_val);
        assert_int_equal(ret, SR_ERR_OK);

        assert_int_equal(op, SR_OP_CREATED);
        assert_non_null(old_val);
        assert_string_equal(old_val->xpath, "/test:cont/l2[k='subscr']");
        assert_non_null(new_val);
        assert_string_equal(new_val->xpath, "/test:cont/l2[k='subscr2']");

        sr_free_val(old_val);
        sr_free_val(new_val);

        /* 4th change */
        ret = sr_get_change_next(session, iter, &op, &old_val, &new_val);
        assert_int_equal(ret, SR_ERR_OK);

        assert_int_equal(op, SR_OP_CREATED);
        assert_null(old_val);
        assert_non_null(new_val);
        assert_string_equal(new_val->xpath, "/test:cont/l2[k='subscr2']/k");

        sr_free_val(new_val);

        /* 5th change */
        ret = sr_get_change_next(session, iter, &op, &old_val, &new_val);
        assert_int_equal(ret, SR_ERR_OK);

        assert_int_equal(op, SR_OP_CREATED);
        assert_null(old_val);
        assert_non_null(new_val);
        assert_string_equal(new_val->xpath, "/test:cont/l2[k='subscr2']/v");

        sr_free_val(new_val);

        /* 6th change */
        ret = sr_get_change_next(session, iter, &op, &old_val, &new_val);
        assert_int_equal(ret, SR_ERR_OK);

        assert_int_equal(op, SR_OP_CREATED);
        assert_null(old_val);
        assert_non_null(new_val);
        assert_string_equal(new_val->xpath, "/test:cont/ll2[.='3210']");

        sr_free_val(new_val);

        /* no more changes */
        ret = sr_get_change_next(session, iter, &op, &old_val, &new_val);
        assert_int_equal(ret, SR_ERR_NOT_FOUND);

        sr_free_change_iter(iter);
        break;
    case 4:
    case 6:
        assert_string_equal(xpath, "/test:l1[k='subscr']");
        if (st->cb_called == 4) {
            assert_int_equal(event, SR_EV_CHANGE);
        } else {
            assert_int_equal(event, SR_EV_DONE);
        }

        /* get changes iter */
        ret = sr_get_changes_iter(session, "/test:*//.", &iter);
        assert_int_equal(ret, SR_ERR_OK);

        /* 1st change */
        ret = sr_get_change_next(session, iter, &op, &old_val, &new_val);
        assert_int_equal(ret, SR_ERR_OK);

        assert_int_equal(op, SR_OP_DELETED);
        assert_non_null(old_val);
        assert_null(new_val);
        assert_string_equal(old_val->xpath, "/test:l1[k='subscr']");

        sr_free_val(old_val);

        /* 2nd change */
        ret = sr_get_change_next(session, iter, &op, &old_val, &new_val);
        assert_int_equal(ret, SR_ERR_OK);

        assert_int_equal(op, SR_OP_DELETED);
        assert_non_null(old_val);
        assert_null(new_val);
        assert_string_equal(old_val->xpath, "/test:l1[k='subscr']/k");

        sr_free_val(old_val);

        /* 3rd change */
        ret = sr_get_change_next(session, iter, &op, &old_val, &new_val);
        assert_int_equal(ret, SR_ERR_OK);

        assert_int_equal(op, SR_OP_DELETED);
        assert_non_null(old_val);
        assert_null(new_val);
        assert_string_equal(old_val->xpath, "/test:l1[k='subscr']/v");

        sr_free_val(old_val);

        /* no more changes */
        ret = sr_get_change_next(session, iter, &op, &old_val, &new_val);
        assert_int_equal(ret, SR_ERR_NOT_FOUND);

        sr_free_change_iter(iter);
        break;
    case 5:
    case 7:
        assert_string_equal(xpath, "/test:cont");
        if (st->cb_called == 5) {
            assert_int_equal(event, SR_EV_CHANGE);
        } else {
            assert_int_equal(event, SR_EV_DONE);
        }

        /* get changes iter */
        ret = sr_get_changes_iter(session, "/test:*//.", &iter);
        assert_int_equal(ret, SR_ERR_OK);

        /* 1st change */
        ret = sr_get_change_next(session, iter, &op, &old_val, &new_val);
        assert_int_equal(ret, SR_ERR_OK);

        assert_int_equal(op, SR_OP_DELETED);
        assert_non_null(old_val);
        assert_null(new_val);
        assert_string_equal(old_val->xpath, "/test:cont/l2[k='subscr']");

        sr_free_val(old_val);

        /* 2nd change */
        ret = sr_get_change_next(session, iter, &op, &old_val, &new_val);
        assert_int_equal(ret, SR_ERR_OK);

        assert_int_equal(op, SR_OP_DELETED);
        assert_non_null(old_val);
        assert_null(new_val);
        assert_string_equal(old_val->xpath, "/test:cont/l2[k='subscr']/k");

        sr_free_val(old_val);

        /* 3rd change */
        ret = sr_get_change_next(session, iter, &op, &old_val, &new_val);
        assert_int_equal(ret, SR_ERR_OK);

        assert_int_equal(op, SR_OP_DELETED);
        assert_non_null(old_val);
        assert_null(new_val);
        assert_string_equal(old_val->xpath, "/test:cont/l2[k='subscr2']");

        sr_free_val(old_val);

        /* 4th change */
        ret = sr_get_change_next(session, iter, &op, &old_val, &new_val);
        assert_int_equal(ret, SR_ERR_OK);

        assert_int_equal(op, SR_OP_DELETED);
        assert_non_null(old_val);
        assert_null(new_val);
        assert_string_equal(old_val->xpath, "/test:cont/l2[k='subscr2']/k");

        sr_free_val(old_val);

        /* 5th change */
        ret = sr_get_change_next(session, iter, &op, &old_val, &new_val);
        assert_int_equal(ret, SR_ERR_OK);

        assert_int_equal(op, SR_OP_DELETED);
        assert_non_null(old_val);
        assert_null(new_val);
        assert_string_equal(old_val->xpath, "/test:cont/l2[k='subscr2']/v");

        sr_free_val(old_val);

        /* 6th change */
        ret = sr_get_change_next(session, iter, &op, &old_val, &new_val);
        assert_int_equal(ret, SR_ERR_OK);

        assert_int_equal(op, SR_OP_DELETED);
        assert_non_null(old_val);
        assert_null(new_val);
        assert_string_equal(old_val->xpath, "/test:cont/ll2[.='3210']");

        sr_free_val(old_val);

        /* no more changes */
        ret = sr_get_change_next(session, iter, &op, &old_val, &new_val);
        assert_int_equal(ret, SR_ERR_NOT_FOUND);

        sr_free_change_iter(iter);
        break;
    default:
        fail();
    }

    ++st->cb_called;
    return SR_ERR_OK;
}

static void *
apply_change_done_xpath_thread(void *arg)
{
    struct state *st = (struct state *)arg;
    sr_session_ctx_t *sess;
    int ret;

    ret = sr_session_start(st->conn, SR_DS_RUNNING, &sess);
    assert_int_equal(ret, SR_ERR_OK);

    ret = sr_set_item_str(sess, "/test:l1[k='subscr']/v", "25", NULL, 0);
    assert_int_equal(ret, SR_ERR_OK);
    ret = sr_set_item_str(sess, "/test:l1[k='no-subscr']/v", "52", NULL, 0);
    assert_int_equal(ret, SR_ERR_OK);
    ret = sr_set_item_str(sess, "/test:ll1[.='30052']", NULL, NULL, 0);
    assert_int_equal(ret, SR_ERR_OK);
    ret = sr_set_item_str(sess, "/test:cont/l2[k='subscr']", NULL, NULL, 0);
    assert_int_equal(ret, SR_ERR_OK);
    ret = sr_set_item_str(sess, "/test:cont/l2[k='subscr2']/v", "35", NULL, 0);
    assert_int_equal(ret, SR_ERR_OK);
    ret = sr_set_item_str(sess, "/test:cont/ll2[.='3210']", NULL, NULL, 0);
    assert_int_equal(ret, SR_ERR_OK);

    /* wait for subscription before applying changes */
    pthread_barrier_wait(&st->barrier);

    /* perform 1st change */
    ret = sr_apply_changes(sess, 0);
    assert_int_equal(ret, SR_ERR_OK);

    /* perform 2nd change */
    ret = sr_delete_item(sess, "/test:l1[k='subscr']", 0);
    assert_int_equal(ret, SR_ERR_OK);
    ret = sr_delete_item(sess, "/test:l1[k='no-subscr']", 0);
    assert_int_equal(ret, SR_ERR_OK);
    ret = sr_delete_item(sess, "/test:ll1[.='30052']", 0);
    assert_int_equal(ret, SR_ERR_OK);
    ret = sr_delete_item(sess, "/test:cont", 0);
    assert_int_equal(ret, SR_ERR_OK);
    ret = sr_apply_changes(sess, 0);
    assert_int_equal(ret, SR_ERR_OK);

    /* signal that we have finished applying changes */
    pthread_barrier_wait(&st->barrier);

    sr_session_stop(sess);
    return NULL;
}

static void *
subscribe_change_done_xpath_thread(void *arg)
{
    struct state *st = (struct state *)arg;
    sr_session_ctx_t *sess;
    sr_subscription_ctx_t *subscr;
    int count, ret;

    ret = sr_session_start(st->conn, SR_DS_RUNNING, &sess);
    assert_int_equal(ret, SR_ERR_OK);

    ret = sr_module_change_subscribe(sess, "test", "/test:l1[k='subscr']", module_change_done_xpath_cb, st, 0, 0, &subscr);
    assert_int_equal(ret, SR_ERR_OK);
    ret = sr_module_change_subscribe(sess, "test", "/test:cont", module_change_done_xpath_cb, st, 0,
            SR_SUBSCR_CTX_REUSE, &subscr);
    assert_int_equal(ret, SR_ERR_OK);
    ret = sr_module_change_subscribe(sess, "test", "/test:test-leaf", module_change_done_xpath_cb, st, 0,
            SR_SUBSCR_CTX_REUSE, &subscr);
    assert_int_equal(ret, SR_ERR_OK);

    /* signal that subscription was created */
    pthread_barrier_wait(&st->barrier);

    count = 0;
    while ((st->cb_called < 8) && (count < 1500)) {
        usleep(10000);
        ++count;
    }
    assert_int_equal(st->cb_called, 8);

    /* wait for the other thread to finish */
    pthread_barrier_wait(&st->barrier);

    sr_unsubscribe(subscr);
    sr_session_stop(sess);
    return NULL;
}

static void
test_change_done_xpath(void **state)
{
    pthread_t tid[2];

    pthread_create(&tid[0], NULL, apply_change_done_xpath_thread, *state);
    pthread_create(&tid[1], NULL, subscribe_change_done_xpath_thread, *state);

    pthread_join(tid[0], NULL);
    pthread_join(tid[1], NULL);
}

/* TEST 8 */
static int
module_change_unlocked_cb(sr_session_ctx_t *session, const char *module_name, const char *xpath, sr_event_t event,
        uint32_t request_id, void *private_data)
{
    struct state *st = (struct state *)private_data;
    sr_subscription_ctx_t *tmp;
    int ret;

    (void)request_id;

    assert_string_equal(module_name, "test");

    switch (st->cb_called) {
    case 0:
    case 1:
        assert_string_equal(xpath, "/test:l1[k='subscr']");
        if (st->cb_called == 0) {
            assert_int_equal(event, SR_EV_CHANGE);
        } else {
            assert_int_equal(event, SR_EV_DONE);
        }

        /* subscribe to something and then unsubscribe */
        sr_session_switch_ds(session, SR_DS_STARTUP);
        ret = sr_module_change_subscribe(session, "test", "/test:cont", module_change_unlocked_cb, st, 0, 0, &tmp);
        assert_int_equal(ret, SR_ERR_OK);
        sr_unsubscribe(tmp);
        break;
    default:
        fail();
    }

    ++st->cb_called;
    return SR_ERR_OK;
}

static void *
apply_change_unlocked_thread(void *arg)
{
    struct state *st = (struct state *)arg;
    sr_session_ctx_t *sess;
    int ret;

    ret = sr_session_start(st->conn, SR_DS_RUNNING, &sess);
    assert_int_equal(ret, SR_ERR_OK);

    ret = sr_set_item_str(sess, "/test:l1[k='subscr']/v", "25", NULL, 0);
    assert_int_equal(ret, SR_ERR_OK);

    /* wait for subscription before applying changes */
    pthread_barrier_wait(&st->barrier);

    /* perform 1st change */
    ret = sr_apply_changes(sess, 0);
    assert_int_equal(ret, SR_ERR_OK);

    /* signal that we have finished applying changes */
    pthread_barrier_wait(&st->barrier);

    sr_session_stop(sess);
    return NULL;
}

static void *
subscribe_change_unlocked_thread(void *arg)
{
    struct state *st = (struct state *)arg;
    sr_session_ctx_t *sess;
    sr_subscription_ctx_t *subscr;
    int count, ret;

    ret = sr_session_start(st->conn, SR_DS_RUNNING, &sess);
    assert_int_equal(ret, SR_ERR_OK);

    ret = sr_module_change_subscribe(sess, "test", "/test:l1[k='subscr']", module_change_unlocked_cb, st, 0,
            SR_SUBSCR_UNLOCKED, &subscr);
    assert_int_equal(ret, SR_ERR_OK);

    /* signal that subscription was created */
    pthread_barrier_wait(&st->barrier);

    count = 0;
    while ((st->cb_called < 2) && (count < 1500)) {
        usleep(10000);
        ++count;
    }
    assert_int_equal(st->cb_called, 2);

    /* wait for the other thread to finish */
    pthread_barrier_wait(&st->barrier);

    sr_unsubscribe(subscr);
    sr_session_stop(sess);
    return NULL;
}

static void
test_change_unlocked(void **state)
{
    pthread_t tid[2];

    pthread_create(&tid[0], NULL, apply_change_unlocked_thread, *state);
    pthread_create(&tid[1], NULL, subscribe_change_unlocked_thread, *state);

    pthread_join(tid[0], NULL);
    pthread_join(tid[1], NULL);
}

/* TEST 9 */
static int
module_change_timeout_cb(sr_session_ctx_t *session, const char *module_name, const char *xpath, sr_event_t event,
        uint32_t request_id, void *private_data)
{
    struct state *st = (struct state *)private_data;

    (void)session;
    (void)xpath;
    (void)request_id;

    assert_string_equal(module_name, "test");

    switch (st->cb_called) {
    case 0:
        assert_int_equal(event, SR_EV_CHANGE);

        /* time out */
        usleep(10000);
        break;
    case 1:
        assert_int_equal(event, SR_EV_CHANGE);

        /* do not time out now */
        break;
    case 2:
        assert_int_equal(event, SR_EV_DONE);

        /* do not time out now */
        break;
    default:
        fail();
    }

    ++st->cb_called;
    return SR_ERR_OK;
}

static void *
apply_change_timeout_thread(void *arg)
{
    struct state *st = (struct state *)arg;
    sr_session_ctx_t *sess;
    int ret;

    ret = sr_session_start(st->conn, SR_DS_RUNNING, &sess);
    assert_int_equal(ret, SR_ERR_OK);

    ret = sr_set_item_str(sess, "/test:l1[k='subscr']/v", "28", NULL, 0);
    assert_int_equal(ret, SR_ERR_OK);

    /* wait for subscription before applying changes */
    pthread_barrier_wait(&st->barrier);

    /* perform the change, it will time out */
    ret = sr_apply_changes(sess, 1);
    assert_int_equal(ret, SR_ERR_CALLBACK_FAILED);

    /* signal that we have tried first time */
    pthread_barrier_wait(&st->barrier);

    /* try again, will succeed now */
    ret = sr_apply_changes(sess, 0);
    assert_int_equal(ret, SR_ERR_OK);

    /* signal that we have finished applying changes */
    pthread_barrier_wait(&st->barrier);

    sr_session_stop(sess);
    return NULL;
}

static void *
subscribe_change_timeout_thread(void *arg)
{
    struct state *st = (struct state *)arg;
    sr_session_ctx_t *sess;
    sr_subscription_ctx_t *subscr;
    int count, ret;

    ret = sr_session_start(st->conn, SR_DS_RUNNING, &sess);
    assert_int_equal(ret, SR_ERR_OK);

    ret = sr_module_change_subscribe(sess, "test", "/test:l1[k='subscr']", module_change_timeout_cb, st, 0, 0, &subscr);
    assert_int_equal(ret, SR_ERR_OK);

    /* signal that subscription was created */
    pthread_barrier_wait(&st->barrier);

    count = 0;
    while ((st->cb_called < 1) && (count < 1500)) {
        usleep(10000);
        ++count;
    }
    assert_int_equal(st->cb_called, 1);

    /* wait for the other thread to try first time */
    pthread_barrier_wait(&st->barrier);

    count = 0;
    while ((st->cb_called < 3) && (count < 1500)) {
        usleep(10000);
        ++count;
    }
    assert_int_equal(st->cb_called, 3);

    /* wait for the other thread to finish */
    pthread_barrier_wait(&st->barrier);

    sr_unsubscribe(subscr);
    sr_session_stop(sess);
    return NULL;
}

static void
test_change_timeout(void **state)
{
    pthread_t tid[2];

    pthread_create(&tid[0], NULL, apply_change_timeout_thread, *state);
    pthread_create(&tid[1], NULL, subscribe_change_timeout_thread, *state);

    pthread_join(tid[0], NULL);
    pthread_join(tid[1], NULL);
}

/* TEST 10 */
static int
module_change_order_cb(sr_session_ctx_t *session, const char *module_name, const char *xpath, sr_event_t event,
        uint32_t request_id, void *private_data)
{
    struct state *st = (struct state *)private_data;

    (void)session;
    (void)xpath;
    (void)request_id;

    switch (st->cb_called) {
    case 0:
    case 5:
        assert_string_equal(module_name, "test");
        assert_int_equal(event, SR_EV_CHANGE);
        break;
    case 1:
    case 4:
        assert_string_equal(module_name, "ietf-interfaces");
        assert_int_equal(event, SR_EV_CHANGE);
        break;
    case 2:
    case 3:
    case 6:
    case 7:
        /* we cannot rely on any order for DONE event */
        assert_int_equal(event, SR_EV_DONE);
        break;
    default:
        fail();
    }

    ++st->cb_called;
    return SR_ERR_OK;
}

static void *
apply_change_order_thread(void *arg)
{
    struct state *st = (struct state *)arg;
    sr_session_ctx_t *sess;
    int ret;

    ret = sr_session_start(st->conn, SR_DS_RUNNING, &sess);
    assert_int_equal(ret, SR_ERR_OK);

    /* edit will be created in this order */
    ret = sr_set_item_str(sess, "/test:l1[k='one']/v", "30", NULL, 0);
    assert_int_equal(ret, SR_ERR_OK);
    ret = sr_set_item_str(sess, "/ietf-interfaces:interfaces/interface[name='eth0']/type", "iana-if-type:ethernetCsmacd", NULL, 0);
    assert_int_equal(ret, SR_ERR_OK);

    /* wait for subscription before applying changes */
    pthread_barrier_wait(&st->barrier);

    /* perform the change */
    ret = sr_apply_changes(sess, 0);
    assert_int_equal(ret, SR_ERR_OK);

    pthread_barrier_wait(&st->barrier);

    /* create edit in different order */
    ret = sr_set_item_str(sess, "/ietf-interfaces:interfaces/interface[name='eth1']/type", "iana-if-type:ethernetCsmacd", NULL, 0);
    assert_int_equal(ret, SR_ERR_OK);
    ret = sr_set_item_str(sess, "/test:l1[k='two']/v", "30", NULL, 0);
    assert_int_equal(ret, SR_ERR_OK);

    /* perform the second change */
    ret = sr_apply_changes(sess, 0);
    assert_int_equal(ret, SR_ERR_OK);

    /* signal that we have finished applying changes */
    pthread_barrier_wait(&st->barrier);

    sr_session_stop(sess);
    return NULL;
}

static void *
subscribe_change_order_thread(void *arg)
{
    struct state *st = (struct state *)arg;
    sr_session_ctx_t *sess;
    sr_subscription_ctx_t *subscr;
    int count, ret;

    ret = sr_session_start(st->conn, SR_DS_RUNNING, &sess);
    assert_int_equal(ret, SR_ERR_OK);

    /* make subscription to 2 different modules */
    ret = sr_module_change_subscribe(sess, "test", NULL, module_change_order_cb, st, 0, 0, &subscr);
    assert_int_equal(ret, SR_ERR_OK);
    ret = sr_module_change_subscribe(sess, "ietf-interfaces", NULL, module_change_order_cb, st, 0, SR_SUBSCR_CTX_REUSE, &subscr);
    assert_int_equal(ret, SR_ERR_OK);

    /* signal that subscription was created */
    pthread_barrier_wait(&st->barrier);

    count = 0;
    while ((st->cb_called < 4) && (count < 1500)) {
        usleep(10000);
        ++count;
    }
    assert_int_equal(st->cb_called, 4);

    /* wait for the other thread to try first time */
    pthread_barrier_wait(&st->barrier);

    count = 0;
    while ((st->cb_called < 8) && (count < 1500)) {
        usleep(10000);
        ++count;
    }
    assert_int_equal(st->cb_called, 8);

    /* wait for the other thread to finish */
    pthread_barrier_wait(&st->barrier);

    sr_unsubscribe(subscr);
    sr_session_stop(sess);
    return NULL;
}

static void
test_change_order(void **state)
{
    pthread_t tid[2];

    pthread_create(&tid[0], NULL, apply_change_order_thread, *state);
    pthread_create(&tid[1], NULL, subscribe_change_order_thread, *state);

    pthread_join(tid[0], NULL);
    pthread_join(tid[1], NULL);
}

/* MAIN */
int
main(void)
{
    const struct CMUnitTest tests[] = {
        cmocka_unit_test_setup_teardown(test_change_done, setup_f, teardown_f),
        cmocka_unit_test_setup_teardown(test_update, setup_f, teardown_f),
        cmocka_unit_test_setup_teardown(test_update_fail, setup_f, teardown_f),
        cmocka_unit_test_setup_teardown(test_change_fail, setup_f, teardown_f),
        cmocka_unit_test_setup_teardown(test_change_done_dflt, setup_f, teardown_f),
        cmocka_unit_test_setup_teardown(test_change_done_when, setup_f, teardown_f),
        cmocka_unit_test_setup_teardown(test_change_done_xpath, setup_f, teardown_f),
        cmocka_unit_test_setup_teardown(test_change_unlocked, setup_f, teardown_f),
        cmocka_unit_test_setup_teardown(test_change_timeout, setup_f, teardown_f),
        cmocka_unit_test_setup_teardown(test_change_order, setup_f, teardown_f),
    };

    setenv("CMOCKA_TEST_ABORT", "1", 1);
    sr_log_stderr(SR_LL_INF);
    return cmocka_run_group_tests(tests, setup, teardown);
}
