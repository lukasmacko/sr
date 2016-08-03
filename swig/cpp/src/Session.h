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
#include <vector>

#include "Sysrepo.h"
#include "Value.h"
#include "Connection.h"
#include "Session.h"

extern "C" {
#include "sysrepo.h"
}

using namespace std;

class Session:public Iter_Value, public Values
{

public:
    Session(Connection& conn, sr_datastore_t datastore = SR_DS_RUNNING, \
            const sr_conn_options_t opts = SESS_DEFAULT, const char *user_name = NULL);
    Session(sr_session_ctx_t *sess);
    void session_stop();
    void session_switch_ds(sr_datastore_t ds);
    void get_last_error(Errors& err);
    void get_last_errors(Errors& err);
    void list_schemas(Schema& schema);
    void get_schema(Schema& schema, const char *module_name, const char *revision,
		    const char *submodule_name,  sr_schema_format_t format);
    void get_item(const char *xpath, Value *value);
    void get_items(const char *xpath, Values *values);
    void get_items_iter(const char *xpath, Iter_Value *iter);
    bool get_item_next(Iter_Value *iter, Value *value);
    void set_item(const char *xpath, Value& value, const sr_edit_options_t opts = EDIT_DEFAULT);
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
    ~Session();
    sr_session_ctx_t *Get();

private:
    sr_session_ctx_t *_sess;
    sr_val_t *_values;
    sr_datastore_t _datastore;
    sr_conn_options_t _opts;
};

class Subscribe:public Throw_Exception, public Iter_Change
{

public:
    Subscribe(Session *sess);

    void module_change_subscribe(const char *module_name, sr_module_change_cb callback, void *private_ctx = \
                                NULL, uint32_t priority = 0, sr_subscr_options_t opts = SUBSCR_DEFAULT);
    void subtree_change_subscribe(const char *xpath, sr_subtree_change_cb callback, void *private_ctx = NULL,\
                                 uint32_t priority = 0, sr_subscr_options_t opts = SUBSCR_DEFAULT);
    void module_install_subscribe(sr_module_install_cb callback, void *private_ctx = NULL,\
                                  sr_subscr_options_t opts = SUBSCR_DEFAULT);
    void feature_enable_subscribe(sr_feature_enable_cb callback, void *private_ctx = NULL,\
                                  sr_subscr_options_t opts = SUBSCR_DEFAULT);
    void unsubscribe();
    void get_changes_iter(const char *xpath, Iter_Change *iter);
    sr_change_oper_t get_change_next(Iter_Change *iter, Values *new_value, Values *old_value);
    /*void rpc_subscribe(const char *xpath, sr_rpc_cb callback, void *private_ctx = NULL,\
                       sr_subscr_options_t opts = SUBSCR_DEFAULT);*/
    //void rpc_send(const char *xpath, Values *input, Values *output);
    void dp_get_items_subscribe(const char *xpath, sr_dp_get_items_cb callback, void *private_ctx, \
                               sr_subscr_options_t opts = SUBSCR_DEFAULT);

#ifndef SWIG
        void Destructor_Subscribe();
        sr_subscription_ctx_t *swig_sub;
        Session *swig_sess;
        std::vector<void*> wrap_cb_l;
#else
        ~Subscribe();
#endif

private:
    sr_subscription_ctx_t *_sub;
    Session *_sess;
    void d_Subscribe();
};

#endif
