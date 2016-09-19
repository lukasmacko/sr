/**
 * @file Session.h
 * @author Mislav Novakovic <mislav.novakovic@sartura.hr>
 * @brief Sysrepo Session class header.
 *
 * @copyright
 * Copyright 2016 Deutsche Telekom AG.
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

#ifndef SESSION_H
#define SESSION_H

#include <iostream>
#include <memory>
#include <vector>

#include "Internal.h"
#include "Struct.h"
#include "Tree.h"
#include "Sysrepo.h"
#include "Connection.h"
#include "Session.h"

extern "C" {
#include "sysrepo.h"
}

using namespace std;

class Session:public Throw_Exception
{

public:
    Session(S_Connection conn, sr_datastore_t datastore = SR_DS_RUNNING, \
            const sr_conn_options_t opts = SESS_DEFAULT, const char *user_name = NULL);
    Session(sr_session_ctx_t *sess, sr_conn_options_t opts = SR_CONN_DEFAULT);
    void session_stop();
    void session_switch_ds(sr_datastore_t ds);
    S_Error get_last_error();
    S_Errors get_last_errors();
    S_Schemas list_schemas();
    S_Schema_Content get_schema(const char *module_name, const char *revision,\
                               const char *submodule_name, sr_schema_format_t format);
    S_Val get_item(const char *xpath);
    S_Vals get_items(const char *xpath);
    S_Iter_Value get_items_iter(const char *xpath);
    S_Val get_item_next(S_Iter_Value iter);
    void set_item(const char *xpath, S_Val value, const sr_edit_options_t opts = EDIT_DEFAULT);
    void delete_item(const char *xpath, const sr_edit_options_t opts = EDIT_DEFAULT);
    void move_item(const char *xpath, const sr_move_position_t position, const char *relative_item = NULL);
    void refresh();
    void validate();
    void commit();
    void lock_datastore();
    void unlock_datastore();
    void lock_module(const char *module_name);
    void unlock_module(const char *module_name);
    void discard_changes();
    void copy_config(const char *module_name, sr_datastore_t src_datastore, sr_datastore_t dst_datastore);
    void set_options(const sr_sess_options_t opts);
    ~Session();
    sr_session_ctx_t *get();

private:
    sr_session_ctx_t *_sess;
    sr_datastore_t _datastore;
    sr_conn_options_t _opts;
    S_Connection _conn;
};

#ifndef SWIG
typedef int (*cpp_module_change_cb)(S_Session session, const char *module_name, sr_notif_event_t event, void *private_ctx);
typedef int (*cpp_subtree_change_cb)(S_Session session, const char *xpath, sr_notif_event_t event, void *private_ctx);
typedef int (*cpp_rpc_cb)(const char *xpath, S_Vals input, S_Vals output, void *private_ctx);
typedef int (*cpp_rpc_tree_cb)(const char *xpath, S_Trees input, S_Trees output, void *private_ctx);
typedef void (*cpp_event_notif_cb)(const char *xpath, S_Vals vals, void *private_ctx);
typedef void (*cpp_event_notif_tree_cb)(const char *xpath, S_Trees trees, void *private_ctx);
typedef int (*cpp_dp_get_items_cb)(const char *xpath, S_Vals vals, void *private_ctx);
class wrap_cb {
public:
    void *private_ctx;
    cpp_module_change_cb module_change;
    cpp_subtree_change_cb subtree_change;
    cpp_rpc_cb rpc;
    cpp_rpc_tree_cb rpc_tree;
	cpp_event_notif_cb event_notif;
	cpp_event_notif_tree_cb event_notif_tree;
	cpp_dp_get_items_cb dp_get_items;
};
#endif

class Subscribe:public Throw_Exception
{

public:
    Subscribe(S_Session sess);

#ifndef SWIG
    void module_change_subscribe(const char *module_name, cpp_module_change_cb callback, void *private_ctx = \
                                NULL, uint32_t priority = 0, sr_subscr_options_t opts = SUBSCR_DEFAULT);
    void subtree_change_subscribe(const char *xpath, cpp_subtree_change_cb callback, void *private_ctx = NULL,\
                                 uint32_t priority = 0, sr_subscr_options_t opts = SUBSCR_DEFAULT);
    void module_install_subscribe(sr_module_install_cb callback, void *private_ctx = NULL,\
                                  sr_subscr_options_t opts = SUBSCR_DEFAULT);
    void feature_enable_subscribe(sr_feature_enable_cb callback, void *private_ctx = NULL,\
                                  sr_subscr_options_t opts = SUBSCR_DEFAULT);
    void rpc_subscribe(const char *xpath, cpp_rpc_cb callback, void *private_ctx = NULL,\
                       sr_subscr_options_t opts = SUBSCR_DEFAULT);
    void event_notif_subscribe_tree(const char *xpath, cpp_event_notif_tree_cb callback, void *private_ctx = NULL,\
                                    sr_subscr_options_t opts = SUBSCR_DEFAULT);
    void event_notif_subscribe(const char *xpath, cpp_event_notif_cb callback, void *private_ctx = NULL,\
                               sr_subscr_options_t opts = SUBSCR_DEFAULT);
    void rpc_subscribe_tree(const char *xpath, cpp_rpc_tree_cb callback, void *private_ctx = NULL,\
                            sr_subscr_options_t opts = SUBSCR_DEFAULT);
    void dp_get_items_subscribe(const char *xpath, cpp_dp_get_items_cb callback, void *private_ctx, \
                               sr_subscr_options_t opts = SUBSCR_DEFAULT);
#endif
    void unsubscribe();

    S_Iter_Change get_changes_iter(const char *xpath);
    S_Change get_change_next(S_Iter_Change iter);
    S_Vals rpc_send(const char *xpath, S_Vals input);
    S_Trees rpc_send_tree(const char *xpath, S_Trees input);

#ifdef SWIG
    void Destructor_Subscribe();
    sr_subscription_ctx_t *swig_sub;
    S_Session swig_sess;
    std::vector<void*> wrap_cb_l;
#else
    std::vector<S_wrap_cb> _wrap_cb_l;
    ~Subscribe();
#endif

private:
    sr_subscription_ctx_t *_sub;
    S_Session _sess;
    void d_Subscribe();
};

#endif
