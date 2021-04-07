/**
 * @file sysrepo.c
 * @author Michal Vasko <mvasko@cesnet.cz>
 * @brief sysrepo API routines
 *
 * @copyright
 * Copyright 2018 - 2021 Deutsche Telekom AG.
 * Copyright 2018 - 2021 CESNET, z.s.p.o.
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
#include "common.h"

#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>

#include <libyang/libyang.h>

static sr_error_info_t *sr_session_notif_buf_stop(sr_session_ctx_t *session);
static sr_error_info_t *_sr_session_stop(sr_session_ctx_t *session);
static sr_error_info_t *_sr_unsubscribe(sr_subscription_ctx_t *subscription);

/**
 * @brief Allocate a new connection structure.
 *
 * @param[in] opts Connection options.
 * @param[out] conn_p Allocated connection.
 * @return err_info, NULL on success.
 */
static sr_error_info_t *
sr_conn_new(const sr_conn_options_t opts, sr_conn_ctx_t **conn_p)
{
    sr_conn_ctx_t *conn;
    sr_error_info_t *err_info = NULL;

    conn = calloc(1, sizeof *conn);
    SR_CHECK_MEM_RET(!conn, err_info);

    if ((err_info = sr_shmmain_ly_ctx_init(&conn->ly_ctx))) {
        goto error1;
    }

    conn->opts = opts;

    if ((err_info = sr_mutex_init(&conn->ptr_lock, 0))) {
        goto error2;
    }

    if ((err_info = sr_shmmain_createlock_open(&conn->main_create_lock))) {
        goto error3;
    }

    if ((err_info = sr_rwlock_init(&conn->ext_remap_lock, 0))) {
        goto error4;
    }

    conn->main_shm.fd = -1;
    conn->ext_shm.fd = -1;

    if ((conn->opts & SR_CONN_CACHE_RUNNING) && (err_info = sr_rwlock_init(&conn->mod_cache.lock, 0))) {
        goto error5;
    }

    *conn_p = conn;
    return NULL;

error5:
    sr_rwlock_destroy(&conn->ext_remap_lock);
error4:
    close(conn->main_create_lock);
error3:
    pthread_mutex_destroy(&conn->ptr_lock);
error2:
    ly_ctx_destroy(conn->ly_ctx, NULL);
error1:
    free(conn);
    return err_info;
}

/**
 * @brief Free a connection structure.
 *
 * @param[in] conn Connection to free.
 */
static void
sr_conn_free(sr_conn_ctx_t *conn)
{
    if (conn) {
        /* free cache before context */
        if (conn->opts & SR_CONN_CACHE_RUNNING) {
            sr_rwlock_destroy(&conn->mod_cache.lock);
            lyd_free_all(conn->mod_cache.data);
            free(conn->mod_cache.mods);
        }

        ly_ctx_destroy(conn->ly_ctx, NULL);
        pthread_mutex_destroy(&conn->ptr_lock);
        if (conn->main_create_lock > -1) {
            close(conn->main_create_lock);
        }
        sr_rwlock_destroy(&conn->ext_remap_lock);
        sr_shm_clear(&conn->main_shm);
        sr_shm_clear(&conn->ext_shm);

        free(conn);
    }
}

API int
sr_connect(const sr_conn_options_t opts, sr_conn_ctx_t **conn_p)
{
    sr_error_info_t *err_info = NULL;
    sr_conn_ctx_t *conn = NULL;
    struct lyd_node *sr_mods = NULL;
    int created = 0, changed;
    sr_main_shm_t *main_shm;
    sr_ext_hole_t *hole;

    SR_CHECK_ARG_APIRET(!conn_p, NULL, err_info);

    /* check that all required directories exist */
    if ((err_info = sr_shmmain_check_dirs())) {
        goto cleanup;
    }

    /* create basic connection structure */
    if ((err_info = sr_conn_new(opts, &conn))) {
        goto cleanup;
    }

    /* CREATE LOCK */
    if ((err_info = sr_shmmain_createlock(conn->main_create_lock))) {
        goto cleanup;
    }

    /* open the main SHM */
    if ((err_info = sr_shmmain_main_open(&conn->main_shm, &created))) {
        goto cleanup_unlock;
    }

    /* open the ext SHM */
    if ((err_info = sr_shmmain_ext_open(&conn->ext_shm, created))) {
        goto cleanup_unlock;
    }

    main_shm = SR_CONN_MAIN_SHM(conn);

    /* allocate next unique Connection ID */
    conn->cid = ATOMIC_INC_RELAXED(main_shm->new_sr_cid);

    /* update connection context based on stored lydmods data */
    if ((err_info = sr_lydmods_conn_ctx_update(main_shm, &conn->ly_ctx, created || !(opts & SR_CONN_NO_SCHED_CHANGES),
            opts & SR_CONN_ERR_ON_SCHED_FAIL, &changed))) {
        goto cleanup_unlock;
    }

    if (changed || created) {
        /* recover anything left in ext SHM */
        sr_shmext_recover_sub_all(conn);

        /* clear all main SHM modules (if main SHM was just created, there aren't any anyway) */
        if ((err_info = sr_shm_remap(&conn->main_shm, sizeof(sr_main_shm_t)))) {
            goto cleanup_unlock;
        }
        main_shm = SR_CONN_MAIN_SHM(conn);
        main_shm->mod_count = 0;

        /* add all the modules in lydmods data into main SHM */
        if ((err_info = sr_lydmods_parse(conn->ly_ctx, &sr_mods))) {
            goto cleanup_unlock;
        }
        if ((err_info = sr_shmmain_store_modules(conn, lyd_child(sr_mods)))) {
            goto cleanup_unlock;
        }

        assert((conn->ext_shm.size == SR_SHM_SIZE(sizeof(sr_ext_shm_t))) || sr_ext_hole_next(NULL, SR_CONN_EXT_SHM(conn)));
        if ((hole = sr_ext_hole_next(NULL, SR_CONN_EXT_SHM(conn)))) {
            /* there is something in ext SHM, is it only a single memory hole? */
            if (conn->ext_shm.size != SR_SHM_SIZE(sizeof(sr_ext_shm_t)) + hole->size) {
                /* no, this should never happen */
                SR_ERRINFO_INT(&err_info);
            }

            /* clear ext SHM */
            if ((err_info = sr_shm_remap(&conn->ext_shm, SR_SHM_SIZE(sizeof(sr_ext_shm_t))))) {
                goto cleanup_unlock;
            }
            SR_CONN_EXT_SHM(conn)->first_hole_off = 0;
        }

        /* copy full datastore from <startup> to <running> */
        if ((err_info = sr_shmmain_files_startup2running(SR_CONN_MAIN_SHM(conn), created))) {
            goto cleanup_unlock;
        }

        /* check data file existence and owner/permissions of all installed modules */
        if ((err_info = sr_shmmain_check_data_files(SR_CONN_MAIN_SHM(conn)))) {
            goto cleanup_unlock;
        }
    }

    /* track our connections */
    if ((err_info = sr_shmmain_conn_list_add(conn->cid))) {
        goto cleanup_unlock;
    }

    SR_LOG_INF("Connection %" PRIu32 " created.", conn->cid);

cleanup_unlock:
    /* CREATE UNLOCK */
    sr_shmmain_createunlock(conn->main_create_lock);

cleanup:
    lyd_free_all(sr_mods);
    if (err_info) {
        sr_conn_free(conn);
        if (created) {
            /* remove any created SHM so it is not considered properly created */
            sr_error_info_t *tmp_err = NULL;
            char *shm_name = NULL;
            if ((tmp_err = sr_path_main_shm(&shm_name))) {
                sr_errinfo_merge(&err_info, tmp_err);
            } else {
                unlink(shm_name);
                free(shm_name);
            }
            if ((tmp_err = sr_path_ext_shm(&shm_name))) {
                sr_errinfo_merge(&err_info, tmp_err);
            } else {
                unlink(shm_name);
                free(shm_name);
            }
        }
    } else {
        *conn_p = conn;
    }
    return sr_api_ret(NULL, err_info);
}

API int
sr_disconnect(sr_conn_ctx_t *conn)
{
    sr_error_info_t *err_info = NULL, *tmp_err;
    uint32_t i;

    if (!conn) {
        return sr_api_ret(NULL, NULL);
    }

    /* stop all session notification buffer threads, they use read lock so they need conn state in SHM */
    for (i = 0; i < conn->session_count; ++i) {
        tmp_err = sr_session_notif_buf_stop(conn->sessions[i]);
        sr_errinfo_merge(&err_info, tmp_err);
    }

    /* stop all subscriptions */
    for (i = 0; i < conn->session_count; ++i) {
        while (conn->sessions[i]->subscription_count && conn->sessions[i]->subscriptions[0]) {
            tmp_err = _sr_unsubscribe(conn->sessions[i]->subscriptions[0]);
            sr_errinfo_merge(&err_info, tmp_err);
        }
    }

    /* stop all the sessions */
    while (conn->session_count) {
        tmp_err = _sr_session_stop(conn->sessions[0]);
        sr_errinfo_merge(&err_info, tmp_err);
    }

    /* free any stored operational data */
    tmp_err = sr_shmmod_oper_stored_del_conn(conn, conn->cid);
    sr_errinfo_merge(&err_info, tmp_err);

    /* stop tracking this connection */
    tmp_err = sr_shmmain_conn_list_del(conn->cid);
    sr_errinfo_merge(&err_info, tmp_err);

    /* free attributes */
    sr_conn_free(conn);

    return sr_api_ret(NULL, err_info);
}

API int
sr_connection_count(uint32_t *conn_count)
{
    sr_error_info_t *err_info = NULL;

    SR_CHECK_ARG_APIRET(!conn_count, NULL, err_info);

    if ((err_info = sr_conn_info(NULL, NULL, conn_count, NULL, NULL))) {
        return sr_api_ret(NULL, err_info);
    }

    return sr_api_ret(NULL, NULL);
}

API const struct ly_ctx *
sr_get_context(sr_conn_ctx_t *conn)
{
    if (!conn) {
        return NULL;
    }

    return conn->ly_ctx;
}

API uint32_t
sr_get_content_id(sr_conn_ctx_t *conn)
{
    sr_error_info_t *err_info = NULL;
    uint32_t content_id;

    if (!conn) {
        return 0;
    }

    if ((err_info = sr_lydmods_get_content_id(SR_CONN_MAIN_SHM(conn), conn->ly_ctx, &content_id))) {
        sr_errinfo_free(&err_info);
        return 0;
    }

    return content_id;
}

API int
sr_set_diff_check_callback(sr_conn_ctx_t *conn, sr_diff_check_cb callback)
{
    sr_error_info_t *err_info = NULL;

    SR_CHECK_ARG_APIRET(!conn, NULL, err_info);

    if (geteuid() != SR_SU_UID) {
        /* not the superuser */
        sr_errinfo_new(&err_info, SR_ERR_UNAUTHORIZED, "Superuser access required.");
        return sr_api_ret(NULL, err_info);
    }

    conn->diff_check_cb = callback;
    return sr_api_ret(NULL, NULL);
}

sr_error_info_t *
_sr_session_start(sr_conn_ctx_t *conn, const sr_datastore_t datastore, sr_sub_event_t event, char **shm_data_ptr,
        sr_session_ctx_t **session)
{
    sr_error_info_t *err_info = NULL;
    uid_t uid;

    assert(conn && session);
    assert((event != SR_SUB_EV_SUCCESS) && (event != SR_SUB_EV_ERROR));

    *session = calloc(1, sizeof **session);
    if (!*session) {
        SR_ERRINFO_MEM(&err_info);
        return err_info;
    }

    /* use new SR session ID and increment it (no lock needed, we are just reading and main SHM is never remapped) */
    (*session)->sid.sr = ATOMIC_INC_RELAXED(SR_CONN_MAIN_SHM(conn)->new_sr_sid);
    if ((*session)->sid.sr == (uint32_t)(ATOMIC_T_MAX - 1)) {
        /* the value in the main SHM is actually ATOMIC_T_MAX and calling another INC would cause an overflow */
        ATOMIC_STORE_RELAXED(SR_CONN_MAIN_SHM(conn)->new_sr_sid, 1);
    }

    /* remember current real process owner */
    uid = getuid();
    if ((err_info = sr_get_pwd(&uid, &(*session)->sid.user))) {
        goto error;
    }

    /* add the session into conn */
    if ((err_info = sr_ptr_add(&conn->ptr_lock, (void ***)&conn->sessions, &conn->session_count, *session))) {
        goto error;
    }

    (*session)->conn = conn;
    (*session)->ds = datastore;
    (*session)->ev = event;
    if (shm_data_ptr) {
        (*session)->ev_data.orig_name = strdup(*shm_data_ptr);
        *shm_data_ptr += sr_strshmlen(*shm_data_ptr);

        (*session)->ev_data.orig_data = malloc(sr_ev_data_size(*shm_data_ptr));
        memcpy((*session)->ev_data.orig_data, *shm_data_ptr, sr_ev_data_size(*shm_data_ptr));
        *shm_data_ptr += SR_SHM_SIZE(sr_ev_data_size(*shm_data_ptr));
    }
    if ((err_info = sr_mutex_init(&(*session)->ptr_lock, 0))) {
        goto error;
    }
    if ((err_info = sr_rwlock_init(&(*session)->notif_buf.lock, 0))) {
        goto error;
    }

    if (!event) {
        SR_LOG_INF("Session %u (user \"%s\", CID %" PRIu32 ") created.", (*session)->sid.sr, (*session)->sid.user,
                conn->cid);
    }

    return NULL;

error:
    free((*session)->sid.user);
    free(*session);
    *session = NULL;
    return err_info;
}

API int
sr_session_start(sr_conn_ctx_t *conn, const sr_datastore_t datastore, sr_session_ctx_t **session)
{
    sr_error_info_t *err_info = NULL;

    SR_CHECK_ARG_APIRET(!conn || !session, NULL, err_info);

    err_info = _sr_session_start(conn, datastore, SR_SUB_EV_NONE, NULL, session);
    return sr_api_ret(NULL, err_info);
}

/**
 * @brief Stop session notif buffering thread.
 *
 * @param[in] session Session whose notif buf to stop.
 * @return err_info, NULL on success.
 */
static sr_error_info_t *
sr_session_notif_buf_stop(sr_session_ctx_t *session)
{
    sr_error_info_t *err_info = NULL;
    struct timespec timeout_ts;
    int ret;

    if (!session->notif_buf.tid) {
        return NULL;
    }

    /* signal the thread to terminate */
    ATOMIC_STORE_RELAXED(session->notif_buf.thread_running, 0);

    /* wake up the thread */
    sr_time_get(&timeout_ts, SR_NOTIF_BUF_LOCK_TIMEOUT);

    /* MUTEX LOCK */
    ret = pthread_mutex_timedlock(&session->notif_buf.lock.mutex, &timeout_ts);
    if (ret) {
        SR_ERRINFO_LOCK(&err_info, __func__, ret);
        return err_info;
    }

    pthread_cond_broadcast(&session->notif_buf.lock.cond);

    /* MUTEX UNLOCK */
    pthread_mutex_unlock(&session->notif_buf.lock.mutex);

    /* join the thread, it will make sure all the buffered notifications are stored */
    ret = pthread_join(session->notif_buf.tid, NULL);
    if (ret) {
        sr_errinfo_new(&err_info, SR_ERR_SYS, "Joining the notification buffer thread failed (%s).", strerror(ret));
        return err_info;
    }

    session->notif_buf.tid = 0;
    assert(!session->notif_buf.first);

    return NULL;
}

/**
 * @brief Unlocked stop (free) a session.
 *
 * @param[in] session Session to stop.
 * @return err_info, NULL on success.
 */
static sr_error_info_t *
_sr_session_stop(sr_session_ctx_t *session)
{
    sr_error_info_t *err_info = NULL, *tmp_err;
    sr_datastore_t ds;

    /* subscriptions need to be freed before, with a WRITE lock */
    assert(!session->subscription_count && !session->subscriptions);

    /* remove ourselves from conn sessions */
    tmp_err = sr_ptr_del(&session->conn->ptr_lock, (void ***)&session->conn->sessions, &session->conn->session_count, session);
    sr_errinfo_merge(&err_info, tmp_err);

    /* release any held locks */
    sr_shmmod_release_locks(session->conn, session->sid);

    /* stop notification buffering thread */
    sr_session_notif_buf_stop(session);

    /* free attributes */
    free(session->sid.user);
    sr_errinfo_free(&session->err_info);
    free(session->orig_name);
    free(session->orig_data);
    free(session->ev_data.orig_name);
    free(session->ev_data.orig_data);
    free(session->ev_error.message);
    free(session->ev_error.format);
    free(session->ev_error.data);
    pthread_mutex_destroy(&session->ptr_lock);
    for (ds = 0; ds < SR_DS_COUNT; ++ds) {
        lyd_free_all(session->dt[ds].edit);
        lyd_free_all(session->dt[ds].diff);
    }
    sr_rwlock_destroy(&session->notif_buf.lock);
    free(session);

    return err_info;
}

API int
sr_session_stop(sr_session_ctx_t *session)
{
    sr_error_info_t *err_info = NULL, *tmp_err;

    if (!session) {
        return sr_api_ret(NULL, NULL);
    }

    /* stop all subscriptions of this session */
    while (session->subscription_count) {
        tmp_err = sr_subscr_session_del(session->subscriptions[0], session, SR_LOCK_NONE);
        sr_errinfo_merge(&err_info, tmp_err);
    }

    /* free the session itself */
    tmp_err = _sr_session_stop(session);
    sr_errinfo_merge(&err_info, tmp_err);

    return sr_api_ret(NULL, err_info);
}

API int
sr_session_notif_buffer(sr_session_ctx_t *session)
{
    sr_error_info_t *err_info = NULL;
    int ret;

    if (!session || session->notif_buf.tid) {
        return sr_api_ret(NULL, NULL);
    }

    /* it could not be running */
    ret = ATOMIC_INC_RELAXED(session->notif_buf.thread_running);
    assert(!ret);

    /* start the buffering thread */
    ret = pthread_create(&session->notif_buf.tid, NULL, sr_notif_buf_thread, session);
    if (ret) {
        sr_errinfo_new(&err_info, SR_ERR_INTERNAL, "Creating a new thread failed (%s).", strerror(ret));
        ATOMIC_STORE_RELAXED(session->notif_buf.thread_running, 0);
        return sr_api_ret(session, err_info);
    }

    return sr_api_ret(NULL, NULL);
}

API int
sr_session_switch_ds(sr_session_ctx_t *session, sr_datastore_t ds)
{
    sr_error_info_t *err_info = NULL;

    SR_CHECK_ARG_APIRET(!session, session, err_info);

    session->ds = ds;
    return sr_api_ret(session, err_info);
}

API sr_datastore_t
sr_session_get_ds(sr_session_ctx_t *session)
{
    if (!session) {
        return 0;
    }

    return session->ds;
}

API int
sr_session_set_orig_name(sr_session_ctx_t *session, const char *orig_name)
{
    sr_error_info_t *err_info = NULL;
    char *new_orig_name;

    SR_CHECK_ARG_APIRET(!session, session, err_info);

    new_orig_name = orig_name ? strdup(orig_name) : NULL;
    if (!new_orig_name && orig_name) {
        SR_ERRINFO_MEM(&err_info);
        return sr_api_ret(session, err_info);
    }

    free(session->orig_name);
    session->orig_name = new_orig_name;

    return sr_api_ret(session, NULL);
}

API const char *
sr_session_get_orig_name(sr_session_ctx_t *session)
{
    if (!session || !session->ev) {
        return NULL;
    }

    return session->ev_data.orig_name;
}

API int
sr_session_push_orig_data(sr_session_ctx_t *session, uint32_t size, const void *data)
{
    sr_error_info_t *err_info = NULL;

    SR_CHECK_ARG_APIRET(!session || !session->orig_name || !size || !data, session, err_info);

    err_info = sr_ev_data_push(&session->orig_data, size, data);
    return sr_api_ret(session, err_info);
}

API void
sr_session_del_orig_data(sr_session_ctx_t *session)
{
    if (!session) {
        return;
    }

    free(session->orig_data);
    session->orig_data = NULL;
}

API int
sr_session_get_orig_data(sr_session_ctx_t *session, uint32_t idx, uint32_t *size, const void **data)
{
    sr_error_info_t *err_info = NULL;

    SR_CHECK_ARG_APIRET(!session || !session->ev || !data, session, err_info);

    return sr_ev_data_get(session->ev_data.orig_data, idx, size, (void **)data);
}

API int
sr_session_get_error(sr_session_ctx_t *session, const sr_error_info_t **error_info)
{
    sr_error_info_t *err_info = NULL;

    SR_CHECK_ARG_APIRET(!session || !error_info, session, err_info);

    *error_info = session->err_info;

    /* do not modify session errors */
    return SR_ERR_OK;
}

API int
sr_session_set_error_message(sr_session_ctx_t *session, const char *format, ...)
{
    sr_error_info_t *err_info = NULL;
    va_list vargs;
    char *err_msg;

    SR_CHECK_ARG_APIRET(!session || ((session->ev != SR_SUB_EV_CHANGE) && (session->ev != SR_SUB_EV_UPDATE) &&
            (session->ev != SR_SUB_EV_OPER) && (session->ev != SR_SUB_EV_RPC)) || !format, session, err_info);

    va_start(vargs, format);
    if (vasprintf(&err_msg, format, vargs) == -1) {
        SR_ERRINFO_MEM(&err_info);
    } else {
        free(session->ev_error.message);
        session->ev_error.message = err_msg;
    }
    va_end(vargs);

    return sr_api_ret(session, err_info);
}

API int
sr_session_set_error_format(sr_session_ctx_t *session, const char *error_format)
{
    sr_error_info_t *err_info = NULL;
    char *err_format;

    SR_CHECK_ARG_APIRET(!session || ((session->ev != SR_SUB_EV_CHANGE) && (session->ev != SR_SUB_EV_UPDATE) &&
            (session->ev != SR_SUB_EV_OPER) && (session->ev != SR_SUB_EV_RPC)), session, err_info);

    if (error_format) {
        if (!(err_format = strdup(error_format))) {
            SR_ERRINFO_MEM(&err_info);
            return sr_api_ret(session, err_info);
        }
    } else {
        err_format = NULL;
    }

    free(session->ev_error.format);
    session->ev_error.format = err_format;

    return sr_api_ret(session, NULL);
}

API int
sr_session_push_error_data(sr_session_ctx_t *session, uint32_t size, const void *data)
{
    sr_error_info_t *err_info = NULL;

    SR_CHECK_ARG_APIRET(!session || ((session->ev != SR_SUB_EV_CHANGE) && (session->ev != SR_SUB_EV_UPDATE) &&
            (session->ev != SR_SUB_EV_OPER) && (session->ev != SR_SUB_EV_RPC)) || !session->ev_error.format || !size ||
            !data, session, err_info);

    err_info = sr_ev_data_push(&session->ev_error.data, size, data);
    return sr_api_ret(session, err_info);
}

API int
sr_get_error_data(sr_error_info_err_t *err, uint32_t idx, uint32_t *size, const void **data)
{
    sr_error_info_t *err_info = NULL;

    SR_CHECK_ARG_APIRET(!err || !data, NULL, err_info);

    return sr_ev_data_get(err->error_data, idx, size, (void **)data);
}

API uint32_t
sr_session_get_id(sr_session_ctx_t *session)
{
    if (!session) {
        return 0;
    }

    return session->sid.sr;
}

API int
sr_session_set_user(sr_session_ctx_t *session, const char *user)
{
    sr_error_info_t *err_info = NULL;
    uid_t uid;

    SR_CHECK_ARG_APIRET(!session || !user, session, err_info);

    if (geteuid() != SR_SU_UID) {
        /* not the superuser */
        sr_errinfo_new(&err_info, SR_ERR_UNAUTHORIZED, "Superuser access required.");
        return sr_api_ret(session, err_info);
    }

    /* check that the user is valid */
    if ((err_info = sr_get_pwd(&uid, (char **)&user))) {
        return sr_api_ret(session, err_info);
    }

    /* replace the user */
    free(session->sid.user);
    session->sid.user = strdup(user);
    if (!session->sid.user) {
        SR_ERRINFO_MEM(&err_info);
    }

    return sr_api_ret(session, err_info);
}

API const char *
sr_session_get_user(sr_session_ctx_t *session)
{
    if (!session) {
        return NULL;
    }

    return session->sid.user;
}

API sr_conn_ctx_t *
sr_session_get_connection(sr_session_ctx_t *session)
{
    if (!session) {
        return NULL;
    }

    return session->conn;
}

API const char *
sr_get_repo_path(void)
{
    char *value;

    value = getenv(SR_REPO_PATH_ENV);
    if (value) {
        return value;
    }

    return SR_REPO_PATH;
}

/**
 * @brief Learn YANG module name and format.
 *
 * @param[in] schema_path Path to the module file.
 * @param[out] module_name Name of the module.
 * @param[out] format Module format.
 * @return err_info, NULL on success.
 */
static sr_error_info_t *
sr_get_module_name_format(const char *schema_path, char **module_name, LYS_INFORMAT *format)
{
    sr_error_info_t *err_info = NULL;
    const char *ptr;
    int index;

    /* learn the format */
    if ((strlen(schema_path) > 4) && !strcmp(schema_path + strlen(schema_path) - 4, ".yin")) {
        *format = LYS_IN_YIN;
        ptr = schema_path + strlen(schema_path) - 4;
    } else if ((strlen(schema_path) > 5) && !strcmp(schema_path + strlen(schema_path) - 5, ".yang")) {
        *format = LYS_IN_YANG;
        ptr = schema_path + strlen(schema_path) - 5;
    } else {
        sr_errinfo_new(&err_info, SR_ERR_INVAL_ARG, "Unknown format of module \"%s\".", schema_path);
        return err_info;
    }

    /* parse module name */
    for (index = 0; (ptr != schema_path) && (ptr[0] != '/'); ++index, --ptr) {}
    if (ptr[0] == '/') {
        ++ptr;
        --index;
    }
    *module_name = strndup(ptr, index);
    SR_CHECK_MEM_RET(!*module_name, err_info);
    ptr = strchr(*module_name, '@');
    if (ptr) {
        /* truncate revision */
        ((char *)ptr)[0] = '\0';
    }

    return NULL;
}

/**
 * @brief Parse a YANG module.
 *
 * @param[in] ly_ctx Context to use.
 * @param[in] schema_path Path to the module file.
 * @param[in] format Module format.
 * @param[in] features Features to enable.
 * @param[in] search_dirs Optional search dirs, in format <dir>[:<dir>]*.
 * @return err_info, NULL on success.
 */
static const struct lys_module *
sr_parse_module(struct ly_ctx *ly_ctx, const char *schema_path, LYS_INFORMAT format, const char **features,
        const char *search_dirs)
{
    sr_error_info_t *err_info = NULL;
    const struct lys_module *ly_mod = NULL;
    char *sdirs_str = NULL, *ptr, *ptr2 = NULL;
    size_t sdir_count = 0;
    struct ly_in *in = NULL;

    if (search_dirs) {
        sdirs_str = strdup(search_dirs);
        if (!sdirs_str) {
            SR_ERRINFO_MEM(&err_info);
            goto cleanup;
        }

        /* add each search dir */
        for (ptr = strtok_r(sdirs_str, ":", &ptr2); ptr; ptr = strtok_r(NULL, ":", &ptr2)) {
            if (!ly_ctx_set_searchdir(ly_ctx, ptr)) {
                /* added (it was not already there) */
                ++sdir_count;
            }
        }
    }

    /* parse the module */
    if (ly_in_new_filepath(schema_path, 0, &in)) {
        SR_ERRINFO_MEM(&err_info);
        goto cleanup;
    }
    lys_parse(ly_ctx, in, format, features, &ly_mod);

cleanup:
    /* remove added search dirs */
    ly_ctx_unset_searchdir_last(ly_ctx, sdir_count);

    ly_in_free(in, 0);
    free(sdirs_str);
    sr_errinfo_free(&err_info);
    return ly_mod;
}

API int
sr_install_module(sr_conn_ctx_t *conn, const char *schema_path, const char *search_dirs, const char **features)
{
    sr_error_info_t *err_info = NULL;
    struct ly_ctx *tmp_ly_ctx = NULL, *sr_mods_ctx = NULL;
    struct lyd_node *sr_mods = NULL;
    const struct lys_module *ly_mod, *ly_iter, *ly_iter2;
    LYS_INFORMAT format;
    char *mod_name = NULL;
    uint32_t i;

    SR_CHECK_ARG_APIRET(!conn || !schema_path, NULL, err_info);

    /* create new temporary context */
    if ((err_info = sr_shmmain_ly_ctx_init(&tmp_ly_ctx))) {
        goto cleanup;
    }
    /* create temporary context for sr_mods to free memory correctly */
    if ((err_info = sr_shmmain_ly_ctx_init(&sr_mods_ctx))) {
        goto cleanup;
    }
    /* create a link between sr_mods and sr_mods_ctx */
    if ((err_info = sr_lydmods_parse(sr_mods_ctx, &sr_mods))) {
        goto cleanup;
    }
    /* use temporary context to load modules */
    if ((err_info = sr_lydmods_ctx_load_modules(sr_mods, tmp_ly_ctx, 1, 1, 0, NULL))) {
        goto cleanup;
    }

    /* learn module name and format */
    if ((err_info = sr_get_module_name_format(schema_path, &mod_name, &format))) {
        goto cleanup;
    }

    /* check whether the module is not already in the context */
    ly_mod = ly_ctx_get_module_implemented(conn->ly_ctx, mod_name);
    if (ly_mod) {
        /* it is currently in the context, try to parse it again to check revisions */
        ly_mod = sr_parse_module(tmp_ly_ctx, schema_path, format, features, search_dirs);
        if (!ly_mod) {
            sr_errinfo_new_ly_first(&err_info, tmp_ly_ctx);
            sr_errinfo_new(&err_info, SR_ERR_EXISTS, "Module \"%s\" is already in sysrepo.", mod_name);
            goto cleanup;
        }

        /* same modules, so if it is scheduled for deletion, we can unschedule it */
        err_info = sr_lydmods_unsched_del_module_with_imps(SR_CONN_MAIN_SHM(conn), conn->ly_ctx, ly_mod);
        if (err_info && (err_info->err[0].err_code == SR_ERR_NOT_FOUND)) {
            sr_errinfo_free(&err_info);
            sr_errinfo_new(&err_info, SR_ERR_EXISTS, "Module \"%s\" is already in sysrepo.", ly_mod->name);
            goto cleanup;
        }
        goto cleanup;
    }

    /* parse the module with the features */
    if (!(ly_mod = sr_parse_module(tmp_ly_ctx, schema_path, format, features, search_dirs))) {
        sr_errinfo_new_ly(&err_info, tmp_ly_ctx);
        goto cleanup;
    }

    /* check that the module does not implement some other modules in different revisions than already in the context */
    i = 0;
    while ((ly_iter = ly_ctx_get_module_iter(tmp_ly_ctx, &i))) {
        if (!ly_iter->implemented) {
            continue;
        }

        ly_iter2 = ly_ctx_get_module_implemented(conn->ly_ctx, ly_iter->name);
        if (!ly_iter2) {
            continue;
        }

        /* modules are implemented in both contexts, compare revisions */
        if ((!ly_iter->revision && ly_iter2->revision) || (ly_iter->revision && !ly_iter2->revision) ||
                (ly_iter->revision && ly_iter2->revision && strcmp(ly_iter->revision, ly_iter2->revision))) {
            sr_errinfo_new(&err_info, SR_ERR_UNSUPPORTED, "Module \"%s\" implements module \"%s@%s\" that is already"
                    " in sysrepo in revision %s.", ly_mod->name, ly_iter->name,
                    ly_iter->revision ? ly_iter->revision : "<none>", ly_iter2->revision ? ly_iter2->revision : "<none>");
            goto cleanup;
        }
    }

    /* schedule module installation */
    if ((err_info = sr_lydmods_deferred_add_module(SR_CONN_MAIN_SHM(conn), conn->ly_ctx, ly_mod, features))) {
        goto cleanup;
    }

    /* store new module imports */
    if ((err_info = sr_create_module_imps_incs_r(ly_mod, NULL))) {
        goto cleanup;
    }

    /* success */

cleanup:
    ly_ctx_destroy(tmp_ly_ctx, NULL);
    lyd_free_all(sr_mods);
    ly_ctx_destroy(sr_mods_ctx, NULL);
    free(mod_name);
    return sr_api_ret(NULL, err_info);
}

API int
sr_install_module_data(sr_conn_ctx_t *conn, const char *module_name, const char *data, const char *data_path,
        LYD_FORMAT format)
{
    sr_error_info_t *err_info = NULL;
    struct ly_ctx *tmp_ly_ctx = NULL;

    SR_CHECK_ARG_APIRET(!conn || !module_name || (data && data_path) || (!data && !data_path) || !format, NULL, err_info);

    /* create new temporary context */
    if ((err_info = sr_shmmain_ly_ctx_init(&tmp_ly_ctx))) {
        goto cleanup;
    }

    /* set new startup data for the module */
    if ((err_info = sr_lydmods_deferred_add_module_data(SR_CONN_MAIN_SHM(conn), tmp_ly_ctx, module_name, data,
            data_path, format))) {
        goto cleanup;
    }

    /* success */

cleanup:
    ly_ctx_destroy(tmp_ly_ctx, NULL);
    return sr_api_ret(NULL, err_info);
}

API int
sr_remove_module(sr_conn_ctx_t *conn, const char *module_name)
{
    sr_error_info_t *err_info = NULL;
    const struct lys_module *ly_mod;

    SR_CHECK_ARG_APIRET(!conn || !module_name, NULL, err_info);

    /* try to find this module */
    ly_mod = ly_ctx_get_module_implemented(conn->ly_ctx, module_name);
    if (!ly_mod) {
        /* if it is scheduled for installation, we can unschedule it */
        err_info = sr_lydmods_unsched_add_module(SR_CONN_MAIN_SHM(conn), conn->ly_ctx, module_name);
        if (err_info && (err_info->err[0].err_code == SR_ERR_NOT_FOUND)) {
            sr_errinfo_new(&err_info, SR_ERR_NOT_FOUND, "Module \"%s\" was not found in sysrepo.", module_name);
        }
        goto cleanup;
    }

    if (sr_module_is_internal(ly_mod)) {
        sr_errinfo_new(&err_info, SR_ERR_INVAL_ARG, "Internal module \"%s\" cannot be uninstalled.", module_name);
        goto cleanup;
    }

    /* check write permission */
    if ((err_info = sr_perm_check(module_name, 1, NULL))) {
        goto cleanup;
    }

    /* schedule module removal from sysrepo */
    if ((err_info = sr_lydmods_deferred_del_module(SR_CONN_MAIN_SHM(conn), conn->ly_ctx, module_name))) {
        goto cleanup;
    }

    /* success */

cleanup:
    return sr_api_ret(NULL, err_info);
}

API int
sr_update_module(sr_conn_ctx_t *conn, const char *schema_path, const char *search_dirs)
{
    sr_error_info_t *err_info = NULL;
    struct ly_ctx *tmp_ly_ctx = NULL;
    const struct lys_module *ly_mod, *upd_ly_mod;
    LYS_INFORMAT format;
    char *mod_name = NULL;

    SR_CHECK_ARG_APIRET(!conn || !schema_path, NULL, err_info);

    /* learn about the module */
    if ((err_info = sr_get_module_name_format(schema_path, &mod_name, &format))) {
        goto cleanup;
    }

    /* try to find this module */
    ly_mod = ly_ctx_get_module_implemented(conn->ly_ctx, mod_name);
    if (!ly_mod) {
        sr_errinfo_new(&err_info, SR_ERR_NOT_FOUND, "Module \"%s\" was not found in sysrepo.", mod_name);
        goto cleanup;
    }

    /* check write permission */
    if ((err_info = sr_perm_check(mod_name, 1, NULL))) {
        goto cleanup;
    }

    /* create new temporary context */
    if ((err_info = sr_ly_ctx_new(&tmp_ly_ctx))) {
        goto cleanup;
    }

    /* try to parse the update module */
    if (!(upd_ly_mod = sr_parse_module(tmp_ly_ctx, schema_path, format, NULL, search_dirs))) {
        sr_errinfo_new_ly(&err_info, tmp_ly_ctx);
        goto cleanup;
    }

    /* it must have a revision */
    if (!upd_ly_mod->revision) {
        sr_errinfo_new(&err_info, SR_ERR_INVAL_ARG, "Update module \"%s\" does not have a revision.", mod_name);
        goto cleanup;
    }

    /* it must be a different and newer module than the installed one */
    if (ly_mod->revision) {
        if (!strcmp(upd_ly_mod->revision, ly_mod->revision)) {
            sr_errinfo_new(&err_info, SR_ERR_EXISTS, "Module \"%s@%s\" already installed.", mod_name,
                    ly_mod->revision);
            goto cleanup;
        } else if (strcmp(upd_ly_mod->revision, ly_mod->revision) < 0) {
            sr_errinfo_new(&err_info, SR_ERR_INVAL_ARG, "Module \"%s@%s\" installed in a newer revision.",
                    mod_name, ly_mod->revision);
            goto cleanup;
        }
    }

    /* schedule module update */
    if ((err_info = sr_lydmods_deferred_upd_module(SR_CONN_MAIN_SHM(conn), conn->ly_ctx, upd_ly_mod))) {
        goto cleanup;
    }

    /* store update module imports */
    if ((err_info = sr_create_module_imps_incs_r(upd_ly_mod, NULL))) {
        goto cleanup;
    }

    /* success */

cleanup:
    ly_ctx_destroy(tmp_ly_ctx, NULL);
    free(mod_name);
    return sr_api_ret(NULL, err_info);
}

API int
sr_cancel_update_module(sr_conn_ctx_t *conn, const char *module_name)
{
    sr_error_info_t *err_info = NULL;
    const struct lys_module *ly_mod;
    char *path = NULL;

    SR_CHECK_ARG_APIRET(!conn || !module_name, NULL, err_info);

    /* try to find this module */
    ly_mod = ly_ctx_get_module_implemented(conn->ly_ctx, module_name);
    if (!ly_mod) {
        sr_errinfo_new(&err_info, SR_ERR_NOT_FOUND, "Module \"%s\" was not found in sysrepo.", module_name);
        goto cleanup;
    }

    /* check write permission */
    if ((err_info = sr_perm_check(module_name, 1, NULL))) {
        goto cleanup;
    }

    /* unschedule module update */
    if ((err_info = sr_lydmods_unsched_upd_module(SR_CONN_MAIN_SHM(conn), conn->ly_ctx, module_name))) {
        goto cleanup;
    }

cleanup:
    free(path);
    return sr_api_ret(NULL, err_info);
}

API int
sr_set_module_replay_support(sr_conn_ctx_t *conn, const char *module_name, int replay_support)
{
    sr_error_info_t *err_info = NULL;
    const struct lys_module *ly_mod;

    SR_CHECK_ARG_APIRET(!conn, NULL, err_info);

    if (module_name) {
        /* try to find this module */
        ly_mod = ly_ctx_get_module_implemented(conn->ly_ctx, module_name);
        if (!ly_mod) {
            sr_errinfo_new(&err_info, SR_ERR_NOT_FOUND, "Module \"%s\" was not found in sysrepo.", module_name);
            goto cleanup;
        }
    }

    /* update replay-support flag both in LY data tree and in main SHM */
    if ((err_info = sr_lydmods_update_replay_support(SR_CONN_MAIN_SHM(conn), conn->ly_ctx, module_name, replay_support))) {
        goto cleanup;
    }
    if ((err_info = sr_shmmain_update_replay_support(SR_CONN_MAIN_SHM(conn), module_name, replay_support))) {
        goto cleanup;
    }

cleanup:
    return sr_api_ret(NULL, err_info);
}

API int
sr_set_module_access(sr_conn_ctx_t *conn, const char *module_name, const char *owner, const char *group, mode_t perm)
{
    sr_error_info_t *err_info = NULL;
    sr_mod_t *shm_mod;
    time_t from_ts, to_ts;
    char *path = NULL;
    const struct lys_module *ly_mod;

    SR_CHECK_ARG_APIRET(!conn || !module_name || (!owner && !group && ((int)perm == -1)), NULL, err_info);

    /* try to find the module */
    ly_mod = ly_ctx_get_module_implemented(conn->ly_ctx, module_name);
    if (!ly_mod) {
        sr_errinfo_new(&err_info, SR_ERR_NOT_FOUND, "Module \"%s\" was not found in sysrepo.", module_name);
        goto cleanup;
    }

    /* get startup file path */
    if ((err_info = sr_path_startup_file(module_name, &path))) {
        goto cleanup;
    }

    /* update startup file permissions and owner */
    if ((err_info = sr_chmodown(path, owner, group, perm))) {
        goto cleanup;
    }

    /* get running SHM file path */
    free(path);
    if ((err_info = sr_path_ds_shm(module_name, SR_DS_RUNNING, &path))) {
        goto cleanup;
    }

    /* update running file permissions and owner */
    if ((err_info = sr_chmodown(path, owner, group, perm))) {
        goto cleanup;
    }

    /* get operational SHM file path */
    free(path);
    if ((err_info = sr_path_ds_shm(module_name, SR_DS_OPERATIONAL, &path))) {
        goto cleanup;
    }

    /* update operational file permissions and owner */
    if ((err_info = sr_chmodown(path, owner, group, perm))) {
        goto cleanup;
    }

    shm_mod = sr_shmmain_find_module(SR_CONN_MAIN_SHM(conn), module_name);
    SR_CHECK_INT_GOTO(!shm_mod, err_info, cleanup);

    if (ATOMIC_LOAD_RELAXED(shm_mod->replay_supp)) {
        if ((err_info = sr_replay_find_file(module_name, 1, 1, &from_ts, &to_ts))) {
            goto cleanup;
        }
        while (from_ts && to_ts) {
            /* get next notification file path */
            free(path);
            if ((err_info = sr_path_notif_file(module_name, from_ts, to_ts, &path))) {
                goto cleanup;
            }

            /* update notification file permissions and owner */
            if ((err_info = sr_chmodown(path, owner, group, perm))) {
                goto cleanup;
            }
        }
    }

cleanup:
    free(path);
    return sr_api_ret(NULL, err_info);
}

API int
sr_get_module_access(sr_conn_ctx_t *conn, const char *module_name, char **owner, char **group, mode_t *perm)
{
    sr_error_info_t *err_info = NULL;
    const struct lys_module *ly_mod;

    SR_CHECK_ARG_APIRET(!conn || !module_name || (!owner && !group && !perm), NULL, err_info);

    /* try to find this module */
    ly_mod = ly_ctx_get_module_implemented(conn->ly_ctx, module_name);
    if (!ly_mod) {
        sr_errinfo_new(&err_info, SR_ERR_NOT_FOUND, "Module \"%s\" was not found in sysrepo.", module_name);
        return sr_api_ret(NULL, err_info);
    }

    /* learn owner and permissions */
    if ((err_info = sr_perm_get(module_name, SR_DS_STARTUP, owner, group, perm))) {
        return sr_api_ret(NULL, err_info);
    }

    return sr_api_ret(NULL, NULL);
}

/**
 * @brief En/disable module feature.
 *
 * @param[in] conn Connection to use.
 * @param[in] module_name Module to change.
 * @param[in] feature_name Feature to change.
 * @param[in] enable Whether to enable or disable the feature.
 * @return err_info, NULL on success.
 */
static sr_error_info_t *
sr_change_module_feature(sr_conn_ctx_t *conn, const char *module_name, const char *feature_name, int enable)
{
    sr_error_info_t *err_info = NULL;
    const struct lys_module *ly_mod;
    LY_ERR lyrc;

    /* try to find this module */
    ly_mod = ly_ctx_get_module_implemented(conn->ly_ctx, module_name);
    if (!ly_mod) {
        sr_errinfo_new(&err_info, SR_ERR_NOT_FOUND, "Module \"%s\" was not found in sysrepo.", module_name);
        goto cleanup;
    }

    /* check write perm */
    if ((err_info = sr_perm_check(module_name, 1, NULL))) {
        goto cleanup;
    }

    /* check feature in the current context */
    lyrc = lys_feature_value(ly_mod, feature_name);
    if (lyrc == LY_ENOTFOUND) {
        sr_errinfo_new(&err_info, SR_ERR_NOT_FOUND, "Feature \"%s\" was not found in module \"%s\".",
                feature_name, module_name);
        goto cleanup;
    }

    /* mark the change (if any) in LY data tree */
    if ((err_info = sr_lydmods_deferred_change_feature(SR_CONN_MAIN_SHM(conn), conn->ly_ctx, ly_mod, feature_name,
            enable, !lyrc))) {
        goto cleanup;
    }

    /* success */

cleanup:
    return err_info;
}

API int
sr_enable_module_feature(sr_conn_ctx_t *conn, const char *module_name, const char *feature_name)
{
    sr_error_info_t *err_info;

    SR_CHECK_ARG_APIRET(!conn || !module_name || !feature_name, NULL, err_info);

    err_info = sr_change_module_feature(conn, module_name, feature_name, 1);

    return sr_api_ret(NULL, err_info);
}

API int
sr_disable_module_feature(sr_conn_ctx_t *conn, const char *module_name, const char *feature_name)
{
    sr_error_info_t *err_info;

    SR_CHECK_ARG_APIRET(!conn || !module_name || !feature_name, NULL, err_info);

    err_info = sr_change_module_feature(conn, module_name, feature_name, 0);

    return sr_api_ret(NULL, err_info);
}

API int
sr_get_module_info(sr_conn_ctx_t *conn, struct lyd_node **sysrepo_data)
{
    sr_error_info_t *err_info;

    SR_CHECK_ARG_APIRET(!conn || !sysrepo_data, NULL, err_info);

    /* LYDMODS LOCK */
    if ((err_info = sr_lydmods_lock(&SR_CONN_MAIN_SHM(conn)->lydmods_lock, conn->ly_ctx, __func__))) {
        return sr_api_ret(NULL, err_info);
    }

    err_info = sr_lydmods_parse(conn->ly_ctx, sysrepo_data);

    /* LYDMODS UNLOCK */
    sr_munlock(&SR_CONN_MAIN_SHM(conn)->lydmods_lock);

    return sr_api_ret(NULL, err_info);
}

API int
sr_get_item(sr_session_ctx_t *session, const char *path, uint32_t timeout_ms, sr_val_t **value)
{
    sr_error_info_t *err_info = NULL;
    struct ly_set *set = NULL, mod_set = {0};
    struct sr_mod_info_s mod_info;

    SR_CHECK_ARG_APIRET(!session || !path || !value, session, err_info);

    if (!timeout_ms) {
        timeout_ms = SR_OPER_CB_TIMEOUT;
    }
    *value = NULL;
    /* for operational, use operational and running datastore */
    SR_MODINFO_INIT(mod_info, session->conn, session->ds, session->ds == SR_DS_OPERATIONAL ? SR_DS_RUNNING : session->ds);

    /* collect all required modules */
    if ((err_info = sr_shmmod_collect_xpath(session->conn->ly_ctx, path, session->ds, &mod_set))) {
        goto cleanup;
    }

    /* add modules into mod_info with deps, locking, and their data */
    if ((err_info = sr_modinfo_add_modules(&mod_info, &mod_set, 0, SR_LOCK_READ, SR_MI_DATA_CACHE | SR_MI_PERM_READ,
            session->sid, session->orig_name, session->orig_data, path, timeout_ms, 0))) {
        goto cleanup;
    }

    /* filter the required data */
    if ((err_info = sr_modinfo_get_filter(&mod_info, path, session, &set))) {
        goto cleanup;
    }

    if (set->count > 1) {
        sr_errinfo_new(&err_info, SR_ERR_INVAL_ARG, "More subtrees match \"%s\".", path);
        goto cleanup;
    } else if (!set->count) {
        sr_errinfo_new(&err_info, SR_ERR_NOT_FOUND, "No data found for \"%s\".", path);
        goto cleanup;
    }

    /* create return value */
    *value = malloc(sizeof **value);
    SR_CHECK_MEM_GOTO(!*value, err_info, cleanup);

    if ((err_info = sr_val_ly2sr(set->dnodes[0], *value))) {
        goto cleanup;
    }

    /* success */

cleanup:
    /* MODULES UNLOCK */
    sr_shmmod_modinfo_unlock(&mod_info, session->sid);

    ly_set_free(set, NULL);
    ly_set_erase(&mod_set, NULL);
    sr_modinfo_free(&mod_info);
    return sr_api_ret(session, err_info);
}

API int
sr_get_items(sr_session_ctx_t *session, const char *xpath, uint32_t timeout_ms, const sr_get_oper_options_t opts,
        sr_val_t **values, size_t *value_cnt)
{
    sr_error_info_t *err_info = NULL;
    struct ly_set *set = NULL, mod_set = {0};
    struct sr_mod_info_s mod_info;
    uint32_t i;

    SR_CHECK_ARG_APIRET(!session || !xpath || !values || !value_cnt || ((session->ds != SR_DS_OPERATIONAL) && opts),
            session, err_info);

    if (!timeout_ms) {
        timeout_ms = SR_OPER_CB_TIMEOUT;
    }
    *values = NULL;
    *value_cnt = 0;
    /* for operational, use operational and running datastore */
    SR_MODINFO_INIT(mod_info, session->conn, session->ds, session->ds == SR_DS_OPERATIONAL ? SR_DS_RUNNING : session->ds);

    /* collect all required modules */
    if ((err_info = sr_shmmod_collect_xpath(session->conn->ly_ctx, xpath, session->ds, &mod_set))) {
        goto cleanup;
    }

    /* add modules into mod_info with deps, locking, and their data */
    if ((err_info = sr_modinfo_add_modules(&mod_info, &mod_set, 0, SR_LOCK_READ, SR_MI_DATA_CACHE | SR_MI_PERM_READ,
            session->sid, session->orig_name, session->orig_data, xpath, timeout_ms, 0))) {
        goto cleanup;
    }

    /* filter the required data */
    if ((err_info = sr_modinfo_get_filter(&mod_info, xpath, session, &set))) {
        goto cleanup;
    }

    if (set->count) {
        *values = calloc(set->count, sizeof **values);
        SR_CHECK_MEM_GOTO(!*values, err_info, cleanup);
    }

    for (i = 0; i < set->count; ++i) {
        if ((err_info = sr_val_ly2sr(set->dnodes[i], (*values) + i))) {
            goto cleanup;
        }
        ++(*value_cnt);
    }

    /* success */

cleanup:
    /* MODULES UNLOCK */
    sr_shmmod_modinfo_unlock(&mod_info, session->sid);

    ly_set_free(set, NULL);
    ly_set_erase(&mod_set, NULL);
    sr_modinfo_free(&mod_info);
    if (err_info) {
        sr_free_values(*values, *value_cnt);
        *values = NULL;
        *value_cnt = 0;
    }
    return sr_api_ret(session, err_info);
}

API int
sr_get_subtree(sr_session_ctx_t *session, const char *path, uint32_t timeout_ms, struct lyd_node **subtree)
{
    sr_error_info_t *err_info = NULL;
    struct sr_mod_info_s mod_info;
    struct ly_set *set = NULL, mod_set = {0};

    SR_CHECK_ARG_APIRET(!session || !path || !subtree, session, err_info);

    if (!timeout_ms) {
        timeout_ms = SR_OPER_CB_TIMEOUT;
    }
    /* for operational, use operational and running datastore */
    SR_MODINFO_INIT(mod_info, session->conn, session->ds, session->ds == SR_DS_OPERATIONAL ? SR_DS_RUNNING : session->ds);

    /* collect all required modules */
    if ((err_info = sr_shmmod_collect_xpath(session->conn->ly_ctx, path, session->ds, &mod_set))) {
        goto cleanup;
    }

    /* add modules into mod_info with deps, locking, and their data */
    if ((err_info = sr_modinfo_add_modules(&mod_info, &mod_set, 0, SR_LOCK_READ, SR_MI_DATA_CACHE | SR_MI_PERM_READ,
            session->sid, session->orig_name, session->orig_data, path, timeout_ms, 0))) {
        goto cleanup;
    }

    /* filter the required data */
    if ((err_info = sr_modinfo_get_filter(&mod_info, path, session, &set))) {
        goto cleanup;
    }

    if (set->count > 1) {
        sr_errinfo_new(&err_info, SR_ERR_INVAL_ARG, "More subtrees match \"%s\".", path);
        goto cleanup;
    }

    if (set->count == 1) {
        if (lyd_dup_single(set->dnodes[0], NULL, LYD_DUP_RECURSIVE, subtree)) {
            sr_errinfo_new_ly(&err_info, session->conn->ly_ctx);
            goto cleanup;
        }
    } else {
        *subtree = NULL;
    }

    /* success */

cleanup:
    /* MODULES UNLOCK */
    sr_shmmod_modinfo_unlock(&mod_info, session->sid);

    ly_set_free(set, NULL);
    ly_set_erase(&mod_set, NULL);
    sr_modinfo_free(&mod_info);
    return sr_api_ret(session, err_info);
}

API int
sr_get_data(sr_session_ctx_t *session, const char *xpath, uint32_t max_depth, uint32_t timeout_ms,
        const sr_get_oper_options_t opts, struct lyd_node **data)
{
    sr_error_info_t *err_info = NULL;
    uint32_t i;
    int dup_opts;
    struct sr_mod_info_s mod_info;
    struct ly_set *subtrees = NULL, mod_set = {0};
    struct lyd_node *node;

    SR_CHECK_ARG_APIRET(!session || !xpath || !data || ((session->ds != SR_DS_OPERATIONAL) && opts), session, err_info);

    if (!timeout_ms) {
        timeout_ms = SR_OPER_CB_TIMEOUT;
    }
    *data = NULL;
    /* for operational, use operational and running datastore */
    SR_MODINFO_INIT(mod_info, session->conn, session->ds, session->ds == SR_DS_OPERATIONAL ? SR_DS_RUNNING : session->ds);

    /* collect all required modules */
    if ((err_info = sr_shmmod_collect_xpath(session->conn->ly_ctx, xpath, session->ds, &mod_set))) {
        goto cleanup;
    }

    /* add modules into mod_info with deps, locking, and their data */
    if ((err_info = sr_modinfo_add_modules(&mod_info, &mod_set, 0, SR_LOCK_READ, SR_MI_DATA_CACHE | SR_MI_PERM_READ,
            session->sid, session->orig_name, session->orig_data, xpath, timeout_ms, opts))) {
        goto cleanup;
    }

    /* filter the required data */
    if ((err_info = sr_modinfo_get_filter(&mod_info, xpath, session, &subtrees))) {
        goto cleanup;
    }

    /* duplicate all returned subtrees with their parents and merge into one data tree */
    for (i = 0; i < subtrees->count; ++i) {
        dup_opts = (max_depth ? 0 : LYD_DUP_RECURSIVE) | LYD_DUP_WITH_PARENTS | LYD_DUP_WITH_FLAGS;
        if (lyd_dup_single(subtrees->dnodes[i], NULL, dup_opts, &node)) {
            sr_errinfo_new_ly(&err_info, session->conn->ly_ctx);
            lyd_free_all(*data);
            *data = NULL;
            goto cleanup;
        }

        /* duplicate only to the specified depth */
        if ((err_info = sr_lyd_dup(subtrees->dnodes[i], max_depth ? max_depth - 1 : 0, node))) {
            lyd_free_all(node);
            lyd_free_all(*data);
            *data = NULL;
            goto cleanup;
        }

        /* always find parent */
        while (node->parent) {
            node = lyd_parent(node);
        }

        /* connect to the result */
        if (!*data) {
            *data = node;
        } else {
            if (lyd_merge_tree(data, node, LYD_MERGE_DESTRUCT)) {
                sr_errinfo_new_ly(&err_info, session->conn->ly_ctx);
                lyd_free_tree(node);
                lyd_free_all(*data);
                *data = NULL;
                goto cleanup;
            }
        }
    }

    /* success */

cleanup:
    /* MODULES UNLOCK */
    sr_shmmod_modinfo_unlock(&mod_info, session->sid);

    ly_set_free(subtrees, NULL);
    ly_set_erase(&mod_set, NULL);
    sr_modinfo_free(&mod_info);
    return sr_api_ret(session, err_info);
}

API void
sr_free_val(sr_val_t *value)
{
    if (!value) {
        return;
    }

    free(value->xpath);
    free(value->origin);
    switch (value->type) {
    case SR_BINARY_T:
    case SR_BITS_T:
    case SR_ENUM_T:
    case SR_IDENTITYREF_T:
    case SR_INSTANCEID_T:
    case SR_STRING_T:
    case SR_ANYXML_T:
    case SR_ANYDATA_T:
        free(value->data.string_val);
        break;
    default:
        /* nothing to free */
        break;
    }

    free(value);
}

API void
sr_free_values(sr_val_t *values, size_t count)
{
    size_t i;

    if (!values || !count) {
        return;
    }

    for (i = 0; i < count; ++i) {
        free(values[i].xpath);
        free(values[i].origin);
        switch (values[i].type) {
        case SR_BINARY_T:
        case SR_BITS_T:
        case SR_ENUM_T:
        case SR_IDENTITYREF_T:
        case SR_INSTANCEID_T:
        case SR_STRING_T:
        case SR_ANYXML_T:
        case SR_ANYDATA_T:
            free(values[i].data.string_val);
            break;
        default:
            /* nothing to free */
            break;
        }
    }

    free(values);
}

API int
sr_set_item(sr_session_ctx_t *session, const char *path, const sr_val_t *value, const sr_edit_options_t opts)
{
    sr_error_info_t *err_info = NULL;
    char str[22], *str_val;

    SR_CHECK_ARG_APIRET(!session || (!path && (!value || !value->xpath)), session, err_info);

    if (!path) {
        path = value->xpath;
    }
    str_val = sr_val_sr2ly_str(session->conn->ly_ctx, value, path, str, 0);

    /* API function */
    return sr_set_item_str(session, path, str_val, value ? value->origin : NULL, opts);
}

API int
sr_set_item_str(sr_session_ctx_t *session, const char *path, const char *value, const char *origin, const sr_edit_options_t opts)
{
    sr_error_info_t *err_info = NULL;
    char *pref_origin = NULL;

    SR_CHECK_ARG_APIRET(!session || !path, session, err_info);

    /* we do not need any lock, ext SHM is not accessed */

    if (origin) {
        if (!strchr(origin, ':')) {
            /* add ietf-origin prefix if none used */
            pref_origin = malloc(11 + 1 + strlen(origin) + 1);
            sprintf(pref_origin, "ietf-origin:%s", origin);
        } else {
            pref_origin = strdup(origin);
        }
    }

    /* add the operation into edit */
    err_info = sr_edit_add(session, path, value, opts & SR_EDIT_STRICT ? "create" : "merge",
            opts & SR_EDIT_NON_RECURSIVE ? "none" : "merge", NULL, NULL, NULL, pref_origin, opts & SR_EDIT_ISOLATE);

    free(pref_origin);
    return sr_api_ret(session, err_info);
}

API int
sr_delete_item(sr_session_ctx_t *session, const char *path, const sr_edit_options_t opts)
{
    sr_error_info_t *err_info = NULL;
    const char *operation;
    const struct lysc_node *snode;
    int ly_log_opts;

    SR_CHECK_ARG_APIRET(!session || !path, session, err_info);

    /* turn off logging */
    ly_log_opts = ly_log_options(0);

    if ((path[strlen(path) - 1] != ']') && (snode = lys_find_path(session->conn->ly_ctx, NULL, path, 0)) &&
            (snode->nodetype & (LYS_LEAFLIST | LYS_LIST)) && !strcmp((path + strlen(path)) - strlen(snode->name), snode->name)) {
        operation = "purge";
    } else if (opts & SR_EDIT_STRICT) {
        operation = "delete";
    } else {
        operation = "remove";
    }

    ly_log_options(ly_log_opts);

    /* add the operation into edit */
    err_info = sr_edit_add(session, path, NULL, operation, opts & SR_EDIT_STRICT ? "none" : "ether", NULL, NULL, NULL,
            NULL, opts & SR_EDIT_ISOLATE);

    return sr_api_ret(session, err_info);
}

API int
sr_move_item(sr_session_ctx_t *session, const char *path, const sr_move_position_t position, const char *list_keys,
        const char *leaflist_value, const char *origin, const sr_edit_options_t opts)
{
    sr_error_info_t *err_info = NULL;
    char *pref_origin = NULL;

    SR_CHECK_ARG_APIRET(!session || !path, session, err_info);

    if (origin) {
        if (!strchr(origin, ':')) {
            /* add ietf-origin prefix if none used */
            pref_origin = malloc(11 + 1 + strlen(origin) + 1);
            sprintf(pref_origin, "ietf-origin:%s", origin);
        } else {
            pref_origin = strdup(origin);
        }
    }

    /* add the operation into edit */
    err_info = sr_edit_add(session, path, NULL, opts & SR_EDIT_STRICT ? "create" : "merge",
            opts & SR_EDIT_NON_RECURSIVE ? "none" : "merge", &position, list_keys, leaflist_value, pref_origin,
            opts & SR_EDIT_ISOLATE);

    free(pref_origin);
    return sr_api_ret(session, err_info);
}

API int
sr_edit_batch(sr_session_ctx_t *session, const struct lyd_node *edit, const char *default_operation)
{
    sr_error_info_t *err_info = NULL;
    struct lyd_node *dup_edit = NULL, *node;

    SR_CHECK_ARG_APIRET(!session || !edit || !default_operation, session, err_info);
    SR_CHECK_ARG_APIRET(strcmp(default_operation, "merge") && strcmp(default_operation, "replace") &&
            strcmp(default_operation, "none"), session, err_info);

    if (session->conn->ly_ctx != edit->schema->module->ctx) {
        sr_errinfo_new(&err_info, SR_ERR_INVAL_ARG, "Data trees must be created using the session connection libyang context.");
        return sr_api_ret(session, err_info);
    } else if (session->dt[session->ds].edit) {
        /* do not allow merging NETCONF edits into sysrepo ones, it can cause some unexpected results */
        sr_errinfo_new(&err_info, SR_ERR_UNSUPPORTED, "There are already some session changes.");
        return sr_api_ret(session, err_info);
    }

    if (lyd_dup_siblings(edit, NULL, LYD_DUP_RECURSIVE, &dup_edit)) {
        sr_errinfo_new_ly(&err_info, session->conn->ly_ctx);
        goto error;
    }

    /* add default operation and default origin */
    LY_LIST_FOR(dup_edit, node) {
        if (!sr_edit_diff_find_oper(node, 0, NULL) && (err_info = sr_edit_set_oper(node, default_operation))) {
            goto error;
        }
        if ((session->ds == SR_DS_OPERATIONAL) && (err_info = sr_edit_diff_set_origin(node, SR_OPER_ORIGIN, 0))) {
            goto error;
        }
    }

    session->dt[session->ds].edit = dup_edit;
    return sr_api_ret(session, NULL);

error:
    lyd_free_siblings(dup_edit);
    return sr_api_ret(session, err_info);
}

API int
sr_validate(sr_session_ctx_t *session, const char *module_name, uint32_t timeout_ms)
{
    sr_error_info_t *err_info = NULL;
    const struct lys_module *ly_mod = NULL;
    const struct lyd_node *node;
    struct ly_set mod_set = {0};
    struct sr_mod_info_s mod_info;

    SR_CHECK_ARG_APIRET(!session, session, err_info);

    if (!timeout_ms) {
        timeout_ms = SR_OPER_CB_TIMEOUT;
    }
    /* for operational, use operational and running datastore */
    SR_MODINFO_INIT(mod_info, session->conn, session->ds, session->ds == SR_DS_OPERATIONAL ? SR_DS_RUNNING : session->ds);

    if (module_name) {
        /* try to find this module */
        ly_mod = ly_ctx_get_module_implemented(session->conn->ly_ctx, module_name);
        if (!ly_mod) {
            sr_errinfo_new(&err_info, SR_ERR_NOT_FOUND, "Module \"%s\" was not found in sysrepo.", module_name);
            goto cleanup;
        }
    }

    switch (session->ds) {
    case SR_DS_STARTUP:
    case SR_DS_RUNNING:
        if (!session->dt[session->ds].edit) {
            /* nothing to validate */
            goto cleanup;
        }

        if (ly_mod) {
            /* check that there are some changes for this module */
            LY_LIST_FOR(session->dt[session->ds].edit, node) {
                if (lyd_owner_module(node) == ly_mod) {
                    break;
                }
            }
            if (!ly_mod) {
                /* nothing to validate */
                goto cleanup;
            }

            if (ly_set_add(&mod_set, (void *)ly_mod, 0, NULL)) {
                SR_ERRINFO_MEM(&err_info);
                goto cleanup;
            }
        } else {
            /* collect all modified modules (other modules must be valid) */
            if ((err_info = sr_shmmod_collect_edit(session->dt[session->ds].edit, &mod_set))) {
                goto cleanup;
            }
        }
        break;
    case SR_DS_CANDIDATE:
    case SR_DS_OPERATIONAL:
        /* specific module/all modules */
        if (ly_mod) {
            if (ly_set_add(&mod_set, (void *)ly_mod, 0, NULL)) {
                SR_ERRINFO_MEM(&err_info);
                goto cleanup;
            }
        } else {
            sr_ly_set_add_all_modules_with_data(&mod_set, session->conn->ly_ctx, 0);
        }
        break;
    }

    /* add modules into mod_info with deps, locking, and their data (we need inverse dependencies because the data will
     * likely be changed) */
    if ((err_info = sr_modinfo_add_modules(&mod_info, &mod_set, MOD_INFO_DEP | MOD_INFO_INV_DEP, SR_LOCK_READ,
            SR_MI_PERM_NO, session->sid, session->orig_name, session->orig_data, NULL, timeout_ms, 0))) {
        goto cleanup;
    }

    /* apply any changes */
    if ((err_info = sr_modinfo_edit_apply(&mod_info, session->dt[session->ds].edit, 0))) {
        goto cleanup;
    }

    /* collect any inst-id dependencies and add those to mod_info as well (after we have the final data that will
     * be validated) */
    ly_set_clean(&mod_set, NULL);
    if ((err_info = sr_shmmod_collect_instid_deps_modinfo(&mod_info, &mod_set))) {
        goto cleanup;
    }
    if ((err_info = sr_modinfo_add_modules(&mod_info, &mod_set, 0, SR_LOCK_READ,
            SR_MI_MOD_DEPS | SR_MI_PERM_NO, session->sid, session->orig_name, session->orig_data, NULL, timeout_ms, 0))) {
        goto cleanup;
    }

    /* validate the data trees */
    switch (session->ds) {
    case SR_DS_STARTUP:
    case SR_DS_RUNNING:
        /* validate only changed modules and any that can become invalid because of the changes */
        if ((err_info = sr_modinfo_validate(&mod_info, MOD_INFO_CHANGED | MOD_INFO_INV_DEP, 0))) {
            goto cleanup;
        }
        break;
    case SR_DS_CANDIDATE:
    case SR_DS_OPERATIONAL:
        /* validate all the modules because they may be invalid without any changes */
        if ((err_info = sr_modinfo_validate(&mod_info, MOD_INFO_REQ | MOD_INFO_INV_DEP, 0))) {
            goto cleanup;
        }
        break;
    }

    /* success */

cleanup:
    /* MODULES UNLOCK */
    sr_shmmod_modinfo_unlock(&mod_info, session->sid);

    ly_set_erase(&mod_set, NULL);
    sr_modinfo_free(&mod_info);
    return sr_api_ret(session, err_info);
}

/**
 * @brief Notify subscribers about the changes in diff and store the data in mod info.
 * Mod info modules are expected to be READ-locked with the ability to upgrade to WRITE-lock!
 *
 * @param[in] mod_info Read-locked mod info with diff and data.
 * @param[in] session Originator session.
 * @param[in] timeout_ms Timeout in milliseconds.
 * @param[out] cb_err_info Callback error information generated by a subscriber, if any.
 * @return err_info, NULL on success.
 */
static sr_error_info_t *
sr_changes_notify_store(struct sr_mod_info_s *mod_info, sr_session_ctx_t *session, uint32_t timeout_ms,
        sr_error_info_t **cb_err_info)
{
    sr_error_info_t *err_info = NULL;
    struct lyd_node *update_edit = NULL, *old_diff = NULL, *new_diff = NULL;
    sr_session_ctx_t *ev_sess = NULL;
    struct ly_set mod_set = {0};
    int ret;

    *cb_err_info = NULL;

    if (!mod_info->diff) {
        SR_LOG_INF("No datastore changes to apply.");
        goto cleanup;
    }

    /* call connection diff callback */
    if (session->conn->diff_check_cb) {
        /* create event session */
        if ((err_info = _sr_session_start(session->conn, session->ds, SR_SUB_EV_CHANGE, NULL, &ev_sess))) {
            goto cleanup;
        }

        ret = session->conn->diff_check_cb(ev_sess, mod_info->diff);
        if (ret) {
            /* create cb_err_info */
            if (ev_sess->ev_error.message) {
                sr_errinfo_new_data(cb_err_info, ret, ev_sess->ev_error.format, ev_sess->ev_error.data,
                        ev_sess->ev_error.message);
            } else {
                sr_errinfo_new_data(cb_err_info, ret, ev_sess->ev_error.format, ev_sess->ev_error.data,
                        "Diff check callback failed (%s).", sr_strerror(ret));
            }
            goto cleanup;
        }
    }

    /* validate new data trees */
    switch (session->ds) {
    case SR_DS_STARTUP:
    case SR_DS_RUNNING:
        /* collect any inst-id dependencies and add those to mod_info as well */
        if ((err_info = sr_shmmod_collect_instid_deps_modinfo(mod_info, &mod_set))) {
            goto cleanup;
        }
        if ((err_info = sr_modinfo_add_modules(mod_info, &mod_set, 0, SR_LOCK_READ,
                SR_MI_MOD_DEPS | SR_MI_PERM_NO, session->sid, session->orig_name, session->orig_data, NULL, 0, 0))) {
            goto cleanup;
        }
        ly_set_clean(&mod_set, NULL);

        if ((err_info = sr_modinfo_validate(mod_info, MOD_INFO_CHANGED | MOD_INFO_INV_DEP, 1))) {
            goto cleanup;
        }
        break;
    case SR_DS_CANDIDATE:
        /* does not have to be valid but we need all default values */
        if ((err_info = sr_modinfo_add_defaults(mod_info, 1))) {
            goto cleanup;
        }
        break;
    case SR_DS_OPERATIONAL:
        /* not valid, but we need NP containers */
        if ((err_info = sr_modinfo_add_np_cont(mod_info))) {
            goto cleanup;
        }
        break;
    }

    if (!mod_info->diff) {
        /* diff can disappear after validation */
        SR_LOG_INF("No datastore changes to apply.");
        goto cleanup;
    }

    /* check write perm (we must wait until after validation, some additional modules can be modified) */
    if ((err_info = sr_modinfo_perm_check(mod_info, 1, 1))) {
        goto cleanup;
    }

    /* CHANGE SUB READ LOCK */
    if ((err_info = sr_modinfo_changesub_rdlock(mod_info))) {
        goto cleanup;
    }

    /* publish current diff in an "update" event for the subscribers to update it */
    if ((err_info = sr_shmsub_change_notify_update(mod_info, session->orig_name, session->orig_data, timeout_ms,
            &update_edit, cb_err_info))) {
        goto cleanup_unlock;
    }
    if (*cb_err_info) {
        /* "update" event failed, just clear the sub SHM and finish */
        err_info = sr_shmsub_change_notify_clear(mod_info);
        goto cleanup_unlock;
    }

    /* create new diff if we have an update edit */
    if (update_edit) {
        /* backup the old diff */
        old_diff = mod_info->diff;
        mod_info->diff = NULL;

        /* get new diff using the updated edit */
        if ((err_info = sr_modinfo_edit_apply(mod_info, update_edit, 1))) {
            goto cleanup_unlock;
        }

        /* unlock so that we can lock after additonal modules were marked as changed */

        /* CHANGE SUB READ UNLOCK */
        sr_modinfo_changesub_rdunlock(mod_info);

        /* validate updated data trees and finish new diff */
        switch (session->ds) {
        case SR_DS_STARTUP:
        case SR_DS_RUNNING:
            if ((err_info = sr_shmmod_collect_instid_deps_modinfo(mod_info, &mod_set))) {
                goto cleanup;
            }

            /* add new modules */
            if ((err_info = sr_modinfo_add_modules(mod_info, &mod_set, 0, SR_LOCK_READ,
                    SR_MI_MOD_DEPS | SR_MI_PERM_NO, session->sid, session->orig_name, session->orig_data, NULL, 0, 0))) {
                goto cleanup;
            }
            ly_set_clean(&mod_set, NULL);

            /* validate */
            if ((err_info = sr_modinfo_validate(mod_info, MOD_INFO_CHANGED | MOD_INFO_INV_DEP, 1))) {
                goto cleanup;
            }
            break;
        case SR_DS_CANDIDATE:
            if ((err_info = sr_modinfo_add_defaults(mod_info, 1))) {
                goto cleanup;
            }
            break;
        case SR_DS_OPERATIONAL:
            if ((err_info = sr_modinfo_add_np_cont(mod_info))) {
                goto cleanup;
            }
            break;
        }

        /* CHANGE SUB READ LOCK */
        if ((err_info = sr_modinfo_changesub_rdlock(mod_info))) {
            goto cleanup;
        }

        /* put the old diff back */
        new_diff = mod_info->diff;
        mod_info->diff = old_diff;
        old_diff = NULL;

        /* merge diffs into one */
        if ((err_info = sr_modinfo_diff_merge(mod_info, new_diff))) {
            goto cleanup_unlock;
        }
    }

    if (!mod_info->diff) {
        SR_LOG_INF("No datastore changes to apply.");
        goto cleanup_unlock;
    }

    /* publish final diff in a "change" event for any subscribers and wait for them */
    if ((err_info = sr_shmsub_change_notify_change(mod_info, session->orig_name, session->orig_data, timeout_ms, cb_err_info))) {
        goto cleanup_unlock;
    }
    if (*cb_err_info) {
        /* "change" event failed, publish "abort" event and finish */
        err_info = sr_shmsub_change_notify_change_abort(mod_info, session->orig_name, session->orig_data, timeout_ms);
        goto cleanup_unlock;
    }

    /* MODULES WRITE LOCK (upgrade) */
    if ((err_info = sr_shmmod_modinfo_rdlock_upgrade(mod_info, session->sid))) {
        goto cleanup_unlock;
    }

    /* store updated datastore */
    if ((err_info = sr_modinfo_data_store(mod_info))) {
        goto cleanup_unlock;
    }

    /* MODULES READ LOCK (downgrade) */
    if ((err_info = sr_shmmod_modinfo_wrlock_downgrade(mod_info, session->sid))) {
        goto cleanup_unlock;
    }

    /* publish "done" event, all changes were applied */
    if ((err_info = sr_shmsub_change_notify_change_done(mod_info, session->orig_name, session->orig_data, timeout_ms))) {
        goto cleanup_unlock;
    }

    /* generate netconf-config-change notification */
    if ((err_info = sr_modinfo_generate_config_change_notif(mod_info, session))) {
        goto cleanup_unlock;
    }

    /* success */

cleanup_unlock:
    /* CHANGE SUB READ UNLOCK */
    sr_modinfo_changesub_rdunlock(mod_info);

cleanup:
    sr_session_stop(ev_sess);
    ly_set_erase(&mod_set, NULL);
    lyd_free_all(update_edit);
    lyd_free_all(old_diff);
    lyd_free_all(new_diff);
    return err_info;
}

API int
sr_apply_changes(sr_session_ctx_t *session, uint32_t timeout_ms)
{
    sr_error_info_t *err_info = NULL, *cb_err_info = NULL;
    struct sr_mod_info_s mod_info;
    struct ly_set mod_set = {0};
    sr_get_oper_options_t get_opts;

    SR_CHECK_ARG_APIRET(!session, session, err_info);

    if (!session->dt[session->ds].edit) {
        return sr_api_ret(session, NULL);
    }

    if (!timeout_ms) {
        timeout_ms = SR_CHANGE_CB_TIMEOUT;
    }
    /* for operational, use operational and running datastore */
    SR_MODINFO_INIT(mod_info, session->conn, session->ds, session->ds == SR_DS_OPERATIONAL ? SR_DS_RUNNING : session->ds);

    if (session->ds == SR_DS_OPERATIONAL) {
        /* when updating stored oper data, we will not validate them so we do not need data from oper subscribers */
        get_opts = SR_OPER_NO_SUBS;
    } else {
        get_opts = 0;
    }

    /* collect all required modules */
    if ((err_info = sr_shmmod_collect_edit(session->dt[session->ds].edit, &mod_set))) {
        goto cleanup;
    }

    /* add modules into mod_info with deps, locking, and their data */
    if ((err_info = sr_modinfo_add_modules(&mod_info, &mod_set, MOD_INFO_DEP | MOD_INFO_INV_DEP, SR_LOCK_READ,
            SR_MI_LOCK_UPGRADEABLE | SR_MI_PERM_NO, session->sid, session->orig_name, session->orig_data, NULL, 0, get_opts))) {
        goto cleanup;
    }

    /* create diff */
    if ((err_info = sr_modinfo_edit_apply(&mod_info, session->dt[session->ds].edit, 1))) {
        goto cleanup;
    }

    /* notify all the subscribers and store the changes */
    err_info = sr_changes_notify_store(&mod_info, session, timeout_ms, &cb_err_info);

cleanup:
    /* MODULES UNLOCK */
    sr_shmmod_modinfo_unlock(&mod_info, session->sid);

    if (!err_info && !cb_err_info) {
        /* free applied edit */
        lyd_free_all(session->dt[session->ds].edit);
        session->dt[session->ds].edit = NULL;
    }

    ly_set_erase(&mod_set, NULL);
    sr_modinfo_free(&mod_info);
    if (cb_err_info) {
        /* return callback error if some was generated */
        sr_errinfo_merge(&err_info, cb_err_info);
        sr_errinfo_new(&err_info, SR_ERR_CALLBACK_FAILED, "User callback failed.");
    }
    return sr_api_ret(session, err_info);
}

API int
sr_has_changes(sr_session_ctx_t *session)
{
    if (session && session->dt[session->ds].edit) {
        return 1;
    }

    return 0;
}

API int
sr_discard_changes(sr_session_ctx_t *session)
{
    sr_error_info_t *err_info = NULL;

    SR_CHECK_ARG_APIRET(!session, session, err_info);

    if (!session->dt[session->ds].edit) {
        return sr_api_ret(session, NULL);
    }

    lyd_free_all(session->dt[session->ds].edit);
    session->dt[session->ds].edit = NULL;
    return sr_api_ret(session, NULL);
}

/**
 * @brief Replace config data of all or some modules.
 *
 * @param[in] session Session to use.
 * @param[in] ly_mod Optional specific module.
 * @param[in] src_config Source data for the replace, they are spent.
 * @param[in] timeout_ms Change callback timeout in milliseconds.
 * @return err_info, NULL on success.
 */
static sr_error_info_t *
_sr_replace_config(sr_session_ctx_t *session, const struct lys_module *ly_mod, struct lyd_node **src_config,
        uint32_t timeout_ms)
{
    sr_error_info_t *err_info = NULL, *cb_err_info = NULL;
    struct ly_set mod_set = {0};
    struct sr_mod_info_s mod_info;

    assert(!*src_config || !(*src_config)->prev->next);
    assert(session->ds != SR_DS_OPERATIONAL);
    SR_MODINFO_INIT(mod_info, session->conn, session->ds, session->ds);

    /* single module/all modules */
    if (ly_mod) {
        ly_set_add(&mod_set, (void *)ly_mod, 0, NULL);
    } else {
        sr_ly_set_add_all_modules_with_data(&mod_set, session->conn->ly_ctx, 0);
    }

    /* add modules into mod_info */
    if ((err_info = sr_modinfo_add_modules(&mod_info, &mod_set, MOD_INFO_DEP | MOD_INFO_INV_DEP, SR_LOCK_READ,
            SR_MI_LOCK_UPGRADEABLE | SR_MI_PERM_NO, session->sid, session->orig_name, session->orig_data, NULL, 0, 0))) {
        goto cleanup;
    }

    /* update affected data and create corresponding diff, src_config is spent */
    if ((err_info = sr_modinfo_replace(&mod_info, src_config))) {
        goto cleanup;
    }

    /* notify all the subscribers and store the changes */
    err_info = sr_changes_notify_store(&mod_info, session, timeout_ms, &cb_err_info);

cleanup:
    /* MODULES UNLOCK */
    sr_shmmod_modinfo_unlock(&mod_info, session->sid);

    ly_set_erase(&mod_set, NULL);
    sr_modinfo_free(&mod_info);
    if (cb_err_info) {
        /* return callback error if some was generated */
        sr_errinfo_merge(&err_info, cb_err_info);
        sr_errinfo_new(&err_info, SR_ERR_CALLBACK_FAILED, "User callback failed.");
    }
    return err_info;
}

API int
sr_replace_config(sr_session_ctx_t *session, const char *module_name, struct lyd_node *src_config, uint32_t timeout_ms)
{
    sr_error_info_t *err_info = NULL;
    const struct lys_module *ly_mod = NULL;

    SR_CHECK_ARG_APIRET(!session || !SR_IS_CONVENTIONAL_DS(session->ds), session, err_info);

    if (src_config && (session->conn->ly_ctx != src_config->schema->module->ctx)) {
        sr_errinfo_new(&err_info, SR_ERR_INVAL_ARG, "Data trees must be created using the session connection libyang context.");
        return sr_api_ret(session, err_info);
    }

    if (!timeout_ms) {
        timeout_ms = SR_CHANGE_CB_TIMEOUT;
    }

    /* find first sibling */
    for ( ; src_config && src_config->prev->next; src_config = src_config->prev) {}

    if (module_name) {
        /* try to find this module */
        ly_mod = ly_ctx_get_module_implemented(session->conn->ly_ctx, module_name);
        if (!ly_mod) {
            sr_errinfo_new(&err_info, SR_ERR_NOT_FOUND, "Module \"%s\" was not found in sysrepo.", module_name);
            goto cleanup;
        }
    }

    /* replace the data */
    if ((err_info = _sr_replace_config(session, ly_mod, &src_config, timeout_ms))) {
        goto cleanup;
    }

    /* success */

cleanup:
    lyd_free_all(src_config);
    return sr_api_ret(session, err_info);
}

API int
sr_copy_config(sr_session_ctx_t *session, const char *module_name, sr_datastore_t src_datastore, uint32_t timeout_ms)
{
    sr_error_info_t *err_info = NULL;
    struct sr_mod_info_s mod_info;
    struct ly_set mod_set = {0};
    const struct lys_module *ly_mod = NULL;

    SR_CHECK_ARG_APIRET(!session || !SR_IS_CONVENTIONAL_DS(src_datastore) || !SR_IS_CONVENTIONAL_DS(session->ds),
            session, err_info);

    if (src_datastore == session->ds) {
        /* nothing to do */
        return sr_api_ret(session, NULL);
    }

    if (!timeout_ms) {
        timeout_ms = SR_CHANGE_CB_TIMEOUT;
    }
    SR_MODINFO_INIT(mod_info, session->conn, src_datastore, src_datastore);

    if (module_name) {
        /* try to find this module */
        ly_mod = ly_ctx_get_module_implemented(session->conn->ly_ctx, module_name);
        if (!ly_mod) {
            sr_errinfo_new(&err_info, SR_ERR_NOT_FOUND, "Module \"%s\" was not found in sysrepo.", module_name);
            goto cleanup;
        }
    }

    /* collect all required modules */
    if (ly_mod) {
        ly_set_add(&mod_set, (void *)ly_mod, 0, NULL);
    } else {
        sr_ly_set_add_all_modules_with_data(&mod_set, session->conn->ly_ctx, 0);
    }

    if ((src_datastore == SR_DS_RUNNING) && (session->ds == SR_DS_CANDIDATE)) {
        /* add modules into mod_info without data */
        if ((err_info = sr_modinfo_add_modules(&mod_info, &mod_set, 0, SR_LOCK_WRITE, SR_MI_DATA_NO | SR_MI_PERM_NO,
                session->sid, session->orig_name, session->orig_data, NULL, 0, 0))) {
            goto cleanup;
        }

        /* special case, just reset candidate */
        err_info = sr_modinfo_candidate_reset(&mod_info);
        goto cleanup;
    }

    /* add modules into mod_info */
    if ((err_info = sr_modinfo_add_modules(&mod_info, &mod_set, 0, SR_LOCK_READ, SR_MI_PERM_NO, session->sid,
            session->orig_name, session->orig_data, NULL, 0, 0))) {
        goto cleanup;
    }

    /* MODULES UNLOCK */
    sr_shmmod_modinfo_unlock(&mod_info, session->sid);

    /* replace the data */
    if ((err_info = _sr_replace_config(session, ly_mod, &mod_info.data, timeout_ms))) {
        goto cleanup;
    }

    if ((src_datastore == SR_DS_CANDIDATE) && (session->ds == SR_DS_RUNNING)) {
        /* MODULES WRITE LOCK */
        if ((err_info = sr_shmmod_modinfo_wrlock(&mod_info, session->sid))) {
            goto cleanup;
        }

        /* reset candidate after it was applied in running */
        err_info = sr_modinfo_candidate_reset(&mod_info);
        goto cleanup;
    }

    /* success */

cleanup:
    /* MODULES UNLOCK */
    sr_shmmod_modinfo_unlock(&mod_info, session->sid);

    ly_set_erase(&mod_set, NULL);
    sr_modinfo_free(&mod_info);
    return sr_api_ret(session, err_info);
}

/**
 * @brief (Un)lock datastore locks.
 *
 * @param[in] mod_info Mod info to use.
 * @param[in] lock Whether to lock or unlock.
 * @param[in] sid Sysrepo session ID.
 * @return err_info, NULL on success.
 */
static sr_error_info_t *
sr_change_dslock(struct sr_mod_info_s *mod_info, int lock, sr_sid_t sid)
{
    sr_error_info_t *err_info = NULL;
    uint32_t i, j;
    char *path;
    int r;
    struct sr_mod_info_mod_s *mod;
    struct sr_mod_lock_s *shm_lock;

    for (i = 0; i < mod_info->mod_count; ++i) {
        mod = &mod_info->mods[i];
        shm_lock = &mod->shm_mod->data_lock_info[mod_info->ds];

        assert(mod->state & MOD_INFO_REQ);

        /* we assume these modules are write-locked by this session */
        assert(shm_lock->sid.sr == sid.sr);

        /* it was successfully WRITE-locked, check that DS lock state is as expected */
        if (ATOMIC_LOAD_RELAXED(shm_lock->ds_locked) && lock) {
            assert(shm_lock->sid.sr == sid.sr);
            sr_errinfo_new(&err_info, SR_ERR_LOCKED, "Module \"%s\" is already locked by this session %u (NC SID %u).",
                    mod->ly_mod->name, sid.sr, sid.nc);
            goto error;
        } else if (!ATOMIC_LOAD_RELAXED(shm_lock->ds_locked) && !lock) {
            assert(shm_lock->sid.sr == sid.sr);
            sr_errinfo_new(&err_info, SR_ERR_OPERATION_FAILED, "Module \"%s\" was not locked by this session %u (NC SID %u).",
                    mod->ly_mod->name, sid.sr, sid.nc);
            goto error;
        } else if (lock && (mod_info->ds == SR_DS_CANDIDATE)) {
            /* candidate DS file cannot exist */
            if ((err_info = sr_path_ds_shm(mod->ly_mod->name, SR_DS_CANDIDATE, &path))) {
                goto error;
            }
            r = access(path, F_OK);
            if ((r == -1) && (errno != ENOENT)) {
                SR_ERRINFO_SYSERRPATH(&err_info, "access", path);
                free(path);
                goto error;
            } else if (!r) {
                sr_errinfo_new(&err_info, SR_ERR_UNSUPPORTED, "Module \"%s\" candidate datastore data have "
                        "already been modified.", mod->ly_mod->name);
                free(path);
                goto error;
            }
            free(path);
        }

        /* change DS lock state and remember the time */
        ATOMIC_STORE_RELAXED(shm_lock->ds_locked, lock);
        if (lock) {
            shm_lock->ds_ts = time(NULL);
        } else {
            shm_lock->ds_ts = 0;
        }
    }

    return NULL;

error:
    /* reverse any DS lock state changes */
    for (j = 0; j < i; ++j) {
        shm_lock = &mod_info->mods[j].shm_mod->data_lock_info[mod_info->ds];

        assert((ATOMIC_LOAD_RELAXED(shm_lock->ds_locked) && lock) || (!ATOMIC_LOAD_RELAXED(shm_lock->ds_locked) && !lock));

        if (lock) {
            ATOMIC_STORE_RELAXED(shm_lock->ds_locked, 0);
        } else {
            ATOMIC_STORE_RELAXED(shm_lock->ds_locked, 1);
        }
    }

    return err_info;
}

/**
 * @brief (Un)lock a specific or all modules datastore locks.
 *
 * @param[in] session Session to use.
 * @param[in] module_name Optional specific module.
 * @param[in] lock Whether to lock or unlock.
 * @return err_code (SR_ERR_OK on success).
 */
static int
_sr_un_lock(sr_session_ctx_t *session, const char *module_name, int lock)
{
    sr_error_info_t *err_info = NULL;
    struct sr_mod_info_s mod_info;
    struct ly_set mod_set = {0};
    const struct lys_module *ly_mod = NULL;

    SR_CHECK_ARG_APIRET(!session || !SR_IS_CONVENTIONAL_DS(session->ds), session, err_info);

    SR_MODINFO_INIT(mod_info, session->conn, session->ds, session->ds);

    if (module_name) {
        /* try to find this module */
        ly_mod = ly_ctx_get_module_implemented(session->conn->ly_ctx, module_name);
        if (!ly_mod) {
            sr_errinfo_new(&err_info, SR_ERR_NOT_FOUND, "Module \"%s\" was not found in sysrepo.", module_name);
            goto cleanup;
        }
    }

    /* collect all required modules and lock */
    if (ly_mod) {
        ly_set_add(&mod_set, (void *)ly_mod, 0, NULL);
    } else {
        sr_ly_set_add_all_modules_with_data(&mod_set, session->conn->ly_ctx, 0);
    }
    if ((err_info = sr_modinfo_add_modules(&mod_info, &mod_set, 0, SR_LOCK_READ,
            SR_MI_LOCK_UPGRADEABLE | SR_MI_DATA_NO | SR_MI_PERM_READ | SR_MI_PERM_STRICT, session->sid, session->orig_name,
            session->orig_data, NULL, 0, 0))) {
        goto cleanup;
    }

    /* DS-(un)lock them */
    if ((err_info = sr_change_dslock(&mod_info, lock, session->sid))) {
        goto cleanup;
    }

    /* candidate datastore unlocked, reset its state */
    if (!lock && (mod_info.ds == SR_DS_CANDIDATE)) {
        /* MODULES WRITE LOCK (upgrade) */
        if ((err_info = sr_shmmod_modinfo_rdlock_upgrade(&mod_info, session->sid))) {
            goto cleanup;
        }

        if ((err_info = sr_modinfo_candidate_reset(&mod_info))) {
            goto cleanup;
        }
    }

    /* success */

cleanup:
    /* MODULES UNLOCK */
    sr_shmmod_modinfo_unlock(&mod_info, session->sid);

    ly_set_erase(&mod_set, NULL);
    sr_modinfo_free(&mod_info);
    return sr_api_ret(session, err_info);
}

API int
sr_lock(sr_session_ctx_t *session, const char *module_name)
{
    return _sr_un_lock(session, module_name, 1);
}

API int
sr_unlock(sr_session_ctx_t *session, const char *module_name)
{
    return _sr_un_lock(session, module_name, 0);
}

API int
sr_get_lock(sr_conn_ctx_t *conn, sr_datastore_t datastore, const char *module_name, int *is_locked, uint32_t *id,
        uint32_t *nc_id, time_t *timestamp)
{
    sr_error_info_t *err_info = NULL;
    struct sr_mod_info_s mod_info;
    struct ly_set mod_set = {0};
    const struct lys_module *ly_mod = NULL;
    struct sr_mod_lock_s *shm_lock;
    uint32_t i;
    sr_sid_t sid;

    SR_CHECK_ARG_APIRET(!conn || !SR_IS_CONVENTIONAL_DS(datastore) || !is_locked, NULL, err_info);

    if (id) {
        *id = 0;
    }
    if (nc_id) {
        *nc_id = 0;
    }
    if (timestamp) {
        *timestamp = 0;
    }
    SR_MODINFO_INIT(mod_info, conn, datastore, datastore);
    memset(&sid, 0, sizeof sid);

    /* no lock required, accessing only main SHM (modules) */

    if (module_name) {
        /* try to find this module */
        ly_mod = ly_ctx_get_module_implemented(conn->ly_ctx, module_name);
        if (!ly_mod) {
            sr_errinfo_new(&err_info, SR_ERR_NOT_FOUND, "Module \"%s\" was not found in sysrepo.", module_name);
            goto cleanup;
        }
    }

    /* collect all required modules into mod_info */
    if (ly_mod) {
        ly_set_add(&mod_set, (void *)ly_mod, 0, NULL);
    } else {
        sr_ly_set_add_all_modules_with_data(&mod_set, conn->ly_ctx, 0);
    }
    if ((err_info = sr_modinfo_add_modules(&mod_info, &mod_set, 0, SR_LOCK_NONE,
            SR_MI_DATA_NO | SR_MI_PERM_READ | SR_MI_PERM_STRICT, sid, NULL, NULL, NULL, 0, 0))) {
        goto cleanup;
    }

    /* check DS-lock of the module(s) */
    for (i = 0; i < mod_info.mod_count; ++i) {
        shm_lock = &mod_info.mods[i].shm_mod->data_lock_info[mod_info.ds];

        if (!ATOMIC_LOAD_RELAXED(shm_lock->ds_locked)) {
            /* there is at least one module that is not DS-locked */
            break;
        }

        if (!sid.sr) {
            /* remember the first DS lock owner */
            sid = shm_lock->sid;
        } else if (sid.sr != shm_lock->sid.sr) {
            /* more DS module lock owners, not a full DS lock */
            break;
        }
    }

    if (i < mod_info.mod_count) {
        /* not full DS lock */
        *is_locked = 0;
    } else if (mod_info.mod_count) {
        /* the module or all modules is DS locked by a single SR session */
        *is_locked = 1;
        if (id) {
            *id = sid.sr;
        }
        if (nc_id) {
            *nc_id = sid.nc;
        }
        if (timestamp) {
            *timestamp = shm_lock->ds_ts;
        }
    }

    /* success */

cleanup:
    ly_set_erase(&mod_set, NULL);
    sr_modinfo_free(&mod_info);
    return sr_api_ret(NULL, err_info);
}

API int
sr_get_event_pipe(sr_subscription_ctx_t *subscription, int *event_pipe)
{
    sr_error_info_t *err_info = NULL;

    SR_CHECK_ARG_APIRET(!subscription || !event_pipe, NULL, err_info);

    *event_pipe = subscription->evpipe;
    return SR_ERR_OK;
}

API int
sr_process_events(sr_subscription_ctx_t *subscription, sr_session_ctx_t *session, time_t *stop_time_in)
{
    sr_error_info_t *err_info = NULL;
    int ret, mod_finished;
    char buf[1];
    uint32_t i;

    /* session does not have to be set */
    SR_CHECK_ARG_APIRET(!subscription, session, err_info);

    if (stop_time_in) {
        *stop_time_in = 0;
    }

    /* get only READ lock to allow event processing even during unsubscribe */

    /* SUBS READ LOCK */
    if ((err_info = sr_rwlock(&subscription->subs_lock, SR_SUBSCR_LOCK_TIMEOUT, SR_LOCK_READ, subscription->conn->cid,
            __func__, NULL, NULL))) {
        return sr_api_ret(session, err_info);
    }

    /* read all bytes from the pipe, there can be several events by now */
    do {
        ret = read(subscription->evpipe, buf, 1);
    } while (ret == 1);
    if ((ret == -1) && (errno != EAGAIN)) {
        SR_ERRINFO_SYSERRNO(&err_info, "read");
        sr_errinfo_new(&err_info, SR_ERR_INTERNAL, "Failed to read from an event pipe.");
        goto cleanup_unlock;
    }

    /* change subscriptions */
    for (i = 0; i < subscription->change_sub_count; ++i) {
        if ((err_info = sr_shmsub_change_listen_process_module_events(&subscription->change_subs[i], subscription->conn))) {
            goto cleanup_unlock;
        }
    }

    /* operational subscriptions */
    for (i = 0; i < subscription->oper_sub_count; ++i) {
        if ((err_info = sr_shmsub_oper_listen_process_module_events(&subscription->oper_subs[i], subscription->conn))) {
            goto cleanup_unlock;
        }
    }

    /* RPC/action subscriptions */
    for (i = 0; i < subscription->rpc_sub_count; ++i) {
        if ((err_info = sr_shmsub_rpc_listen_process_rpc_events(&subscription->rpc_subs[i], subscription->conn))) {
            goto cleanup_unlock;
        }
    }

    /* notification subscriptions */
    i = 0;
    while (i < subscription->notif_sub_count) {
        /* perform any replays requested */
        if ((err_info = sr_shmsub_notif_listen_module_replay(&subscription->notif_subs[i], subscription))) {
            goto cleanup_unlock;
        }

        /* check whether a subscription did not finish */
        mod_finished = 0;
        if ((err_info = sr_shmsub_notif_listen_module_stop_time(&subscription->notif_subs[i], SR_LOCK_READ,
                subscription, &mod_finished))) {
            goto cleanup_unlock;
        }

        if (mod_finished) {
            /* all subscriptions of this module have finished, try the next */
            continue;
        }

        /* standard event processing */
        if ((err_info = sr_shmsub_notif_listen_process_module_events(&subscription->notif_subs[i], subscription->conn))) {
            goto cleanup_unlock;
        }

        /* find nearest stop time */
        sr_shmsub_notif_listen_module_get_stop_time_in(&subscription->notif_subs[i], stop_time_in);

        /* next iteration */
        ++i;
    }

cleanup_unlock:
    /* SUBS READ UNLOCK */
    sr_rwunlock(&subscription->subs_lock, SR_SUBSCR_LOCK_TIMEOUT, SR_LOCK_READ, subscription->conn->cid, __func__);

    return sr_api_ret(session, err_info);
}

API uint32_t
sr_subscription_get_last_sub_id(const sr_subscription_ctx_t *subscription)
{
    if (!subscription) {
        return 0;
    }

    return subscription->last_sub_id;
}

API int
sr_subscription_get_suspended(sr_subscription_ctx_t *subscription, uint32_t sub_id, int *suspended)
{
    sr_error_info_t *err_info = NULL;
    const char *module_name, *path;
    sr_datastore_t ds;

    SR_CHECK_ARG_APIRET(!subscription || !sub_id || !suspended, NULL, err_info);

    /* SUBS READ LOCK */
    if ((err_info = sr_rwlock(&subscription->subs_lock, SR_SUBSCR_LOCK_TIMEOUT, SR_LOCK_READ, subscription->conn->cid,
            __func__, NULL, NULL))) {
        return sr_api_ret(NULL, err_info);
    }

    /* find the subscription in the subscription context and read its suspended from ext SHM */
    if (sr_subscr_change_sub_find(subscription, sub_id, &module_name, &ds)) {
        /* change sub */
        if ((err_info = sr_shmext_change_sub_suspended(subscription->conn, module_name, ds, sub_id, -1, suspended))) {
            goto cleanup_unlock;
        }
    } else if (sr_subscr_oper_sub_find(subscription, sub_id, &module_name)) {
        /* oper sub */
        if ((err_info = sr_shmext_oper_sub_suspended(subscription->conn, module_name, sub_id, -1, suspended))) {
            goto cleanup_unlock;
        }
    } else if (sr_subscr_notif_sub_find(subscription, sub_id, &module_name)) {
        /* notif sub */
        if ((err_info = sr_shmext_notif_sub_suspended(subscription->conn, module_name, sub_id, -1, suspended))) {
            goto cleanup_unlock;
        }
    } else if (sr_subscr_rpc_sub_find(subscription, sub_id, &path)) {
        /* RPC/action sub */
        if ((err_info = sr_shmext_rpc_sub_suspended(subscription->conn, path, sub_id, -1, suspended))) {
            goto cleanup_unlock;
        }
    } else {
        sr_errinfo_new(&err_info, SR_ERR_NOT_FOUND, "Subscription with ID %" PRIu32 " was not found.", sub_id);
        goto cleanup_unlock;
    }

cleanup_unlock:
    /* SUBS READ UNLOCK */
    sr_rwunlock(&subscription->subs_lock, SR_SUBSCR_LOCK_TIMEOUT, SR_LOCK_READ, subscription->conn->cid, __func__);

    return sr_api_ret(NULL, err_info);
}

/**
 * @brief Change suspended state of a subscription.
 *
 * @param[in] subscription Subscription context to use.
 * @param[in] sub_id Subscription notification ID.
 * @param[in] suspend Whether to suspend or resume the subscription.
 * @return err_info, NULL on success.
 */
static sr_error_info_t *
_sr_subscription_suspend_change(sr_subscription_ctx_t *subscription, uint32_t sub_id, int suspend)
{
    sr_error_info_t *err_info = NULL;
    struct modsub_notifsub_s *notif_sub = NULL;
    const char *module_name, *path;
    sr_datastore_t ds;
    sr_session_ctx_t *ev_sess = NULL;

    assert(subscription && sub_id);

    /* find the subscription in the subscription context and read its suspended from ext SHM */
    if (sr_subscr_change_sub_find(subscription, sub_id, &module_name, &ds)) {
        /* change sub */
        if ((err_info = sr_shmext_change_sub_suspended(subscription->conn, module_name, ds, sub_id, suspend, NULL))) {
            goto cleanup;
        }
    } else if (sr_subscr_oper_sub_find(subscription, sub_id, &module_name)) {
        /* oper sub */
        if ((err_info = sr_shmext_oper_sub_suspended(subscription->conn, module_name, sub_id, suspend, NULL))) {
            goto cleanup;
        }
    } else if ((notif_sub = sr_subscr_notif_sub_find(subscription, sub_id, &module_name))) {
        /* notif sub */
        if ((err_info = sr_shmext_notif_sub_suspended(subscription->conn, module_name, sub_id, suspend, NULL))) {
            goto cleanup;
        }
    } else if (sr_subscr_rpc_sub_find(subscription, sub_id, &path)) {
        /* RPC/action sub */
        if ((err_info = sr_shmext_rpc_sub_suspended(subscription->conn, path, sub_id, suspend, NULL))) {
            goto cleanup;
        }
    } else {
        sr_errinfo_new(&err_info, SR_ERR_NOT_FOUND, "Subscription with ID %" PRIu32 " was not found.", sub_id);
        goto cleanup;
    }

    if (notif_sub) {
        /* create event session */
        if ((err_info = _sr_session_start(subscription->conn, SR_DS_OPERATIONAL, SR_SUB_EV_NOTIF, NULL, &ev_sess))) {
            goto cleanup;
        }

        /* send the special notification */
        if ((err_info = sr_notif_call_callback(ev_sess, notif_sub->cb, notif_sub->tree_cb, notif_sub->private_data,
                suspend ? SR_EV_NOTIF_SUSPENDED : SR_EV_NOTIF_RESUMED, sub_id, NULL, time(NULL)))) {
            goto cleanup;
        }
    }

cleanup:
    sr_session_stop(ev_sess);
    return err_info;
}

API int
sr_subscription_suspend(sr_subscription_ctx_t *subscription, uint32_t sub_id)
{
    sr_error_info_t *err_info = NULL;

    SR_CHECK_ARG_APIRET(!subscription || !sub_id, NULL, err_info);

    /* SUBS READ LOCK */
    if ((err_info = sr_rwlock(&subscription->subs_lock, SR_SUBSCR_LOCK_TIMEOUT, SR_LOCK_READ, subscription->conn->cid,
            __func__, NULL, NULL))) {
        return sr_api_ret(NULL, err_info);
    }

    /* suspend */
    err_info = _sr_subscription_suspend_change(subscription, sub_id, 1);

    /* SUBS READ UNLOCK */
    sr_rwunlock(&subscription->subs_lock, SR_SUBSCR_LOCK_TIMEOUT, SR_LOCK_READ, subscription->conn->cid, __func__);

    return sr_api_ret(NULL, err_info);
}

API int
sr_subscription_resume(sr_subscription_ctx_t *subscription, uint32_t sub_id)
{
    sr_error_info_t *err_info = NULL;

    SR_CHECK_ARG_APIRET(!subscription || !sub_id, NULL, err_info);

    /* SUBS READ LOCK */
    if ((err_info = sr_rwlock(&subscription->subs_lock, SR_SUBSCR_LOCK_TIMEOUT, SR_LOCK_READ, subscription->conn->cid,
            __func__, NULL, NULL))) {
        return sr_api_ret(NULL, err_info);
    }

    /* resume */
    err_info = _sr_subscription_suspend_change(subscription, sub_id, 0);

    /* SUBS READ UNLOCK */
    sr_rwunlock(&subscription->subs_lock, SR_SUBSCR_LOCK_TIMEOUT, SR_LOCK_READ, subscription->conn->cid, __func__);

    return sr_api_ret(NULL, err_info);
}

API int
sr_unsubscribe_sub(sr_subscription_ctx_t *subscription, uint32_t sub_id)
{
    sr_error_info_t *err_info = NULL;

    if (!subscription) {
        return sr_api_ret(NULL, NULL);
    }

    err_info = sr_subscr_del(subscription, sub_id, SR_LOCK_NONE);
    return sr_api_ret(NULL, err_info);
}

/**
 * @brief Unlocked unsubscribe (free) of all the subscriptions in a subscription structure.
 *
 * @param[in] subscription Subscription to unsubscribe and free.
 * @return err_info, NULL on success.
 */
static sr_error_info_t *
_sr_unsubscribe(sr_subscription_ctx_t *subscription)
{
    sr_error_info_t *err_info = NULL, *tmp_err;
    char *path;
    int ret;

    assert(subscription);

    /* delete a specific subscription or delete all subscriptions which also removes this subscription from all the sessions */
    if ((tmp_err = sr_subscr_del(subscription, 0, SR_LOCK_NONE))) {
        /* continue */
        sr_errinfo_merge(&err_info, tmp_err);
    }

    /* no new events can be generated at this point */

    if (ATOMIC_LOAD_RELAXED(subscription->thread_running)) {
        /* signal the thread to quit */
        ATOMIC_STORE_RELAXED(subscription->thread_running, 0);

        /* generate a new event for the thread to wake up */
        err_info = sr_shmsub_notify_evpipe(subscription->evpipe_num);

        if (!err_info) {
            /* join the thread */
            ret = pthread_join(subscription->tid, NULL);
            if (ret) {
                sr_errinfo_new(&err_info, SR_ERR_SYS, "Joining the subscriber thread failed (%s).", strerror(ret));
            }
        }
    }

    /* unlink event pipe */
    if ((tmp_err = sr_path_evpipe(subscription->evpipe_num, &path))) {
        /* continue */
        sr_errinfo_merge(&err_info, tmp_err);
    } else {
        ret = unlink(path);
        free(path);
        if (ret == -1) {
            SR_ERRINFO_SYSERRNO(&err_info, "unlink");
        }
    }

    /* free attributes */
    close(subscription->evpipe);
    sr_rwlock_destroy(&subscription->subs_lock);
    free(subscription);
    return err_info;
}

API int
sr_unsubscribe(sr_subscription_ctx_t *subscription)
{
    sr_error_info_t *err_info = NULL;

    if (!subscription) {
        return sr_api_ret(NULL, NULL);
    }

    err_info = _sr_unsubscribe(subscription);
    return sr_api_ret(NULL, err_info);
}

/**
 * @brief Perform enabled event on a subscription.
 *
 * @param[in] session Session to use.
 * @param[in,out] mod_info Empty mod info structure to use. If any modules were locked, they are kept that way.
 * @param[in] ly_mod Specific module.
 * @param[in] xpath Optional subscription xpath.
 * @param[in] callback Callback to call.
 * @param[in] private_data Arbitrary callback data.
 * @param[in] sub_id Subscription ID.
 * @param[in] opts Subscription options.
 * @return err_info, NULL on success.
 */
static sr_error_info_t *
sr_module_change_subscribe_enable(sr_session_ctx_t *session, struct sr_mod_info_s *mod_info,
        const struct lys_module *ly_mod, const char *xpath, sr_module_change_cb callback, void *private_data,
        uint32_t sub_id, int opts)
{
    sr_error_info_t *err_info = NULL;
    struct lyd_node *enabled_data = NULL, *node;
    struct ly_set mod_set = {0};
    sr_session_ctx_t *ev_sess = NULL;
    sr_error_t err_code;

    SR_MODINFO_INIT((*mod_info), session->conn, session->ds, session->ds == SR_DS_OPERATIONAL ? SR_DS_RUNNING : session->ds);

    /* create mod_info structure with this module only, do not use cache to allow reading data in the callback
     * (avoid dead-lock) */
    ly_set_add(&mod_set, (void *)ly_mod, 0, NULL);
    if ((err_info = sr_modinfo_add_modules(mod_info, &mod_set, 0, SR_LOCK_READ, SR_MI_PERM_NO, session->sid,
            session->orig_name, session->orig_data, NULL, 0, 0))) {
        goto cleanup;
    }

    /* start with any existing config NP containers */
    if ((err_info = sr_lyd_dup_module_np_cont(mod_info->data, ly_mod, 0, &enabled_data))) {
        goto cleanup;
    }

    /* select only the subscribed-to subtree */
    if (mod_info->data) {
        if (xpath) {
            if ((err_info = sr_lyd_dup_enabled_xpath(mod_info->data, (char **)&xpath, 1, &enabled_data))) {
                goto cleanup;
            }
        } else {
            if ((err_info = sr_lyd_dup_module_data(mod_info->data, ly_mod, 0, &enabled_data))) {
                goto cleanup;
            }
        }
    }

    /* these data will be presented as newly created, make such a diff */
    LY_LIST_FOR(enabled_data, node) {
        /* top-level "create" operation that is inherited */
        if ((err_info = sr_diff_set_oper(node, "create"))) {
            goto cleanup;
        }

        /* user-ordered lists need information about position */
        if ((err_info = sr_edit_created_subtree_apply_move(node))) {
            goto cleanup;
        }
    }

    /* create event session */
    if ((err_info = _sr_session_start(session->conn, session->ds, SR_SUB_EV_ENABLED, NULL, &ev_sess))) {
        goto cleanup;
    }
    ev_sess->dt[ev_sess->ds].diff = enabled_data;
    enabled_data = NULL;

    if (!(opts & SR_SUBSCR_DONE_ONLY)) {
        SR_LOG_INF("Triggering \"%s\" \"%s\" event on enabled data.", ly_mod->name, sr_ev2str(ev_sess->ev));

        /* present all changes in an "enabled" event */
        err_code = callback(ev_sess, sub_id, ly_mod->name, xpath, sr_ev2api(ev_sess->ev), 0, private_data);
        if (err_code != SR_ERR_OK) {
            /* callback failed but it is the only one so no "abort" event is necessary */
            if (ev_sess->ev_error.message || ev_sess->ev_error.format) {
                /* remember callback error info */
                sr_errinfo_new_data(&err_info, err_code, ev_sess->ev_error.format, ev_sess->ev_error.data,
                        ev_sess->ev_error.message ? ev_sess->ev_error.message : sr_strerror(err_code));
            }
            sr_errinfo_new(&err_info, SR_ERR_CALLBACK_FAILED, "Subscribing to \"%s\" changes failed.", ly_mod->name);
            goto cleanup;
        }
    }

    /* finish with a "done" event just because this event should imitate a regular change */
    ev_sess->ev = SR_SUB_EV_DONE;
    SR_LOG_INF("Triggering \"%s\" \"%s\" event on enabled data.", ly_mod->name, sr_ev2str(ev_sess->ev));
    callback(ev_sess, sub_id, ly_mod->name, xpath, sr_ev2api(ev_sess->ev), 0, private_data);

cleanup:
    sr_session_stop(ev_sess);
    ly_set_erase(&mod_set, NULL);
    lyd_free_all(enabled_data);
    return err_info;
}

/**
 * @brief Allocate and start listening on a new subscription.
 *
 * @param[in] conn Connection to use.
 * @param[in] opts Subscription options.
 * @param[out] subs_p Allocated subscription.
 * @return err_info, NULL on success.
 */
static sr_error_info_t *
sr_subscr_new(sr_conn_ctx_t *conn, sr_subscr_options_t opts, sr_subscription_ctx_t **subs_p)
{
    sr_error_info_t *err_info = NULL;
    char *path = NULL;
    int ret;
    mode_t um;

    /* allocate new subscription */
    *subs_p = calloc(1, sizeof **subs_p);
    SR_CHECK_MEM_RET(!*subs_p, err_info);
    sr_rwlock_init(&(*subs_p)->subs_lock, 0);
    (*subs_p)->conn = conn;
    (*subs_p)->evpipe = -1;

    /* get new event pipe number and increment it */
    (*subs_p)->evpipe_num = ATOMIC_INC_RELAXED(SR_CONN_MAIN_SHM((*subs_p)->conn)->new_evpipe_num);
    if ((*subs_p)->evpipe_num == (uint32_t)(ATOMIC_T_MAX - 1)) {
        /* the value in the main SHM is actually ATOMIC_T_MAX and calling another INC would cause an overflow */
        ATOMIC_STORE_RELAXED(SR_CONN_MAIN_SHM((*subs_p)->conn)->new_evpipe_num, 1);
    }

    /* get event pipe name */
    if ((err_info = sr_path_evpipe((*subs_p)->evpipe_num, &path))) {
        goto error;
    }

    /* set umask so that the correct permissions are really set */
    um = umask(SR_UMASK);

    /* create the event pipe */
    ret = mkfifo(path, SR_EVPIPE_PERM);
    umask(um);
    if (ret == -1) {
        SR_ERRINFO_SYSERRNO(&err_info, "mkfifo");
        goto error;
    }

    /* open it for reading AND writing (just so that there always is a "writer", otherwise it is always ready
     * for reading by select() but returns just EOF on read) */
    (*subs_p)->evpipe = sr_open(path, O_RDWR | O_NONBLOCK, 0);
    if ((*subs_p)->evpipe == -1) {
        SR_ERRINFO_SYSERRPATH(&err_info, "open", path);
        goto error;
    }

    if (!(opts & SR_SUBSCR_NO_THREAD)) {
        /* set thread_running to non-zero so that thread does not immediately quit */
        ATOMIC_STORE_RELAXED((*subs_p)->thread_running, 1);

        /* start the listen thread */
        ret = pthread_create(&(*subs_p)->tid, NULL, sr_shmsub_listen_thread, *subs_p);
        if (ret) {
            sr_errinfo_new(&err_info, SR_ERR_INTERNAL, "Creating a new thread failed (%s).", strerror(ret));
            goto error;
        }
    }

    free(path);
    return NULL;

error:
    free(path);
    if ((*subs_p)->evpipe > -1) {
        close((*subs_p)->evpipe);
    }
    free(*subs_p);
    return err_info;
}

API int
sr_module_change_subscribe(sr_session_ctx_t *session, const char *module_name, const char *xpath,
        sr_module_change_cb callback, void *private_data, uint32_t priority, sr_subscr_options_t opts,
        sr_subscription_ctx_t **subscription)
{
    sr_error_info_t *err_info = NULL, *tmp_err;
    sr_lock_mode_t chsub_lock_mode = SR_LOCK_NONE;
    const struct lys_module *ly_mod;
    struct sr_mod_info_s mod_info;
    sr_conn_ctx_t *conn;
    uint32_t sub_id;
    sr_subscr_options_t sub_opts;
    sr_mod_t *shm_mod;

    SR_CHECK_ARG_APIRET(!session || SR_IS_EVENT_SESS(session) || !module_name || !callback ||
            ((opts & SR_SUBSCR_PASSIVE) && (opts & SR_SUBSCR_ENABLED)) || !subscription, session, err_info);

    if ((opts & SR_SUBSCR_CTX_REUSE) && !*subscription) {
        /* invalid option, remove */
        opts &= ~SR_SUBSCR_CTX_REUSE;
    }

    /* just make it valid */
    SR_MODINFO_INIT(mod_info, session->conn, SR_DS_RUNNING, SR_DS_RUNNING);

    conn = session->conn;
    /* only these options are relevant outside this function and will be stored */
    sub_opts = opts & (SR_SUBSCR_DONE_ONLY | SR_SUBSCR_PASSIVE | SR_SUBSCR_UPDATE);

    /* is the module name valid? */
    ly_mod = ly_ctx_get_module_implemented(conn->ly_ctx, module_name);
    if (!ly_mod) {
        sr_errinfo_new(&err_info, SR_ERR_NOT_FOUND, "Module \"%s\" was not found in sysrepo.", module_name);
        goto cleanup;
    }

    /* check write/read perm */
    if ((err_info = sr_perm_check(module_name, (opts & SR_SUBSCR_PASSIVE) ? 0 : 1, NULL))) {
        goto cleanup;
    }

    /* get new sub ID */
    sub_id = ATOMIC_INC_RELAXED(SR_CONN_MAIN_SHM(conn)->new_sub_id);
    if (sub_id == (uint32_t)(ATOMIC_T_MAX - 1)) {
        /* the value in the main SHM is actually ATOMIC_T_MAX and calling another INC would cause an overflow */
        ATOMIC_STORE_RELAXED(SR_CONN_MAIN_SHM(conn)->new_sub_id, 1);
    }

    /* find the module in SHM */
    shm_mod = sr_shmmain_find_module(SR_CONN_MAIN_SHM(conn), module_name);
    SR_CHECK_INT_GOTO(!shm_mod, err_info, cleanup);

    if (opts & SR_SUBSCR_ENABLED) {
        /* we need to lock write subscriptions here to keep CHANGE SUB and MODULES lock order */

        /* CHANGE SUB WRITE LOCK */
        if ((err_info = sr_rwlock(&shm_mod->change_sub[session->ds].lock, SR_SHMEXT_SUB_LOCK_TIMEOUT, SR_LOCK_WRITE,
                conn->cid, __func__, NULL, NULL))) {
            goto cleanup;
        }
        chsub_lock_mode = SR_LOCK_WRITE;

        /* call the callback with the current configuration, keep any used modules locked in mod_info */
        if ((err_info = sr_module_change_subscribe_enable(session, &mod_info, ly_mod, xpath, callback, private_data,
                sub_id, opts))) {
            goto cleanup;
        }
    }

    if (!(opts & SR_SUBSCR_CTX_REUSE)) {
        /* create a new subscription */
        if ((err_info = sr_subscr_new(conn, opts, subscription))) {
            goto cleanup;
        }
    }

    /* add module subscription into ext SHM */
    if ((err_info = sr_shmext_change_sub_add(conn, shm_mod, chsub_lock_mode, session->ds, sub_id, xpath, priority,
            sub_opts, (*subscription)->evpipe_num))) {
        goto error1;
    }

    /* add subscription into structure and create separate specific SHM segment */
    if ((err_info = sr_subscr_change_sub_add(*subscription, sub_id, session, module_name, xpath, callback, private_data,
            priority, sub_opts, 0))) {
        goto error2;
    }

    /* add the subscription into session */
    if ((err_info = sr_ptr_add(&session->ptr_lock, (void ***)&session->subscriptions, &session->subscription_count,
            *subscription))) {
        goto error3;
    }

    /* success */
    goto cleanup;

error3:
    sr_subscr_change_sub_del(*subscription, sub_id, SR_LOCK_NONE);

error2:
    if ((tmp_err = sr_shmext_change_sub_del(conn, shm_mod, chsub_lock_mode, session->ds, sub_id))) {
        sr_errinfo_merge(&err_info, tmp_err);
    }

error1:
    if (!(opts & SR_SUBSCR_CTX_REUSE)) {
        _sr_unsubscribe(*subscription);
        *subscription = NULL;
    }

cleanup:
    if (chsub_lock_mode != SR_LOCK_NONE) {
        /* CHANGE SUB UNLOCK */
        sr_rwunlock(&shm_mod->change_sub[session->ds].lock, SR_SHMEXT_SUB_LOCK_TIMEOUT, chsub_lock_mode, conn->cid, __func__);
    }

    /* if there are any modules, unlock them after the enabled event was handled and the subscription was added
     * to avoid losing any changes */

    /* MODULES UNLOCK */
    sr_shmmod_modinfo_unlock(&mod_info, session->sid);

    sr_modinfo_free(&mod_info);
    return sr_api_ret(session, err_info);
}

API int
sr_module_change_sub_get_info(sr_subscription_ctx_t *subscription, uint32_t sub_id, const char **module_name,
        sr_datastore_t *ds, const char **xpath, uint32_t *filtered_out)
{
    sr_error_info_t *err_info = NULL;
    struct modsub_changesub_s *change_sub;

    SR_CHECK_ARG_APIRET(!subscription || !sub_id, NULL, err_info);

    /* SUBS READ LOCK */
    if ((err_info = sr_rwlock(&subscription->subs_lock, SR_SUBSCR_LOCK_TIMEOUT, SR_LOCK_READ, subscription->conn->cid,
            __func__, NULL, NULL))) {
        return sr_api_ret(NULL, err_info);
    }

    /* find the subscription in the subscription context */
    change_sub = sr_subscr_change_sub_find(subscription, sub_id, module_name, ds);
    if (!change_sub) {
        sr_errinfo_new(&err_info, SR_ERR_NOT_FOUND, "Change subscription with ID \"%u\" not found.", sub_id);
        goto cleanup_unlock;
    }

    /* fill parameters */
    if (xpath) {
        *xpath = change_sub->xpath;
    }
    if (filtered_out) {
        *filtered_out = ATOMIC_LOAD_RELAXED(change_sub->filtered_out);
    }

cleanup_unlock:
    /* SUBS READ UNLOCK */
    sr_rwunlock(&subscription->subs_lock, SR_SUBSCR_LOCK_TIMEOUT, SR_LOCK_READ, subscription->conn->cid, __func__);

    return sr_api_ret(NULL, err_info);
}

API int
sr_module_change_sub_modify_xpath(sr_subscription_ctx_t *subscription, uint32_t sub_id, const char *xpath)
{
    sr_error_info_t *err_info = NULL;
    struct modsub_changesub_s *change_sub;
    sr_mod_t *shm_mod;
    const char *module_name;
    sr_datastore_t ds;

    SR_CHECK_ARG_APIRET(!subscription || !sub_id, NULL, err_info);

    /* SUBS WRITE LOCK */
    if ((err_info = sr_rwlock(&subscription->subs_lock, SR_SUBSCR_LOCK_TIMEOUT, SR_LOCK_WRITE, subscription->conn->cid,
            __func__, NULL, NULL))) {
        return sr_api_ret(NULL, err_info);
    }

    /* find the subscription in the subscription context */
    change_sub = sr_subscr_change_sub_find(subscription, sub_id, &module_name, &ds);
    if (!change_sub) {
        sr_errinfo_new(&err_info, SR_ERR_NOT_FOUND, "Change subscription with ID \"%u\" not found.", sub_id);
        goto cleanup_unlock;
    }

    /* if the xpath is the same, there is nothing to modify */
    if (!xpath && !change_sub->xpath) {
        goto cleanup_unlock;
    } else if (xpath && change_sub->xpath && !strcmp(xpath, change_sub->xpath)) {
        goto cleanup_unlock;
    }

    /* update xpath in the subscription */
    free(change_sub->xpath);
    change_sub->xpath = NULL;
    if (xpath) {
        change_sub->xpath = strdup(xpath);
        SR_CHECK_MEM_GOTO(!change_sub->xpath, err_info, cleanup_unlock);
    }

    /* find the module in SHM */
    shm_mod = sr_shmmain_find_module(SR_CONN_MAIN_SHM(subscription->conn), module_name);
    SR_CHECK_INT_GOTO(!shm_mod, err_info, cleanup_unlock);

    /* modify the subscription in ext SHM */
    if ((err_info = sr_shmext_change_sub_modify(subscription->conn, shm_mod, ds, sub_id, xpath))) {
        goto cleanup_unlock;
    }

cleanup_unlock:
    /* SUBS WRITE UNLOCK */
    sr_rwunlock(&subscription->subs_lock, SR_SUBSCR_LOCK_TIMEOUT, SR_LOCK_WRITE, subscription->conn->cid, __func__);

    return sr_api_ret(NULL, err_info);
}

static int
_sr_get_changes_iter(sr_session_ctx_t *session, const char *xpath, int dup, sr_change_iter_t **iter)
{
    sr_error_info_t *err_info = NULL;

    SR_CHECK_ARG_APIRET(!session || !SR_IS_EVENT_SESS(session) || !xpath || !iter, session, err_info);

    if ((session->ev != SR_SUB_EV_ENABLED) && (session->ev != SR_SUB_EV_DONE) && !session->dt[session->ds].diff) {
        sr_errinfo_new(&err_info, SR_ERR_INVAL_ARG, "Session without changes.");
        return sr_api_ret(session, err_info);
    }

    *iter = calloc(1, sizeof **iter);
    if (!*iter) {
        SR_ERRINFO_MEM(&err_info);
        return sr_api_ret(session, err_info);
    }

    if (session->dt[session->ds].diff) {
        if (dup) {
            if (lyd_dup_siblings(session->dt[session->ds].diff, NULL, LYD_DUP_RECURSIVE, &(*iter)->diff)) {
                sr_errinfo_new_ly(&err_info, session->conn->ly_ctx);
                goto error;
            }
        }
        if (lyd_find_xpath(session->dt[session->ds].diff, xpath, &(*iter)->set)) {
            sr_errinfo_new_ly(&err_info, session->conn->ly_ctx);
            goto error;
        }
    } else {
        ly_set_new(&(*iter)->set);
    }
    SR_CHECK_MEM_GOTO(!(*iter)->set, err_info, error);
    (*iter)->idx = 0;

    return sr_api_ret(session, NULL);

error:
    sr_free_change_iter(*iter);
    return sr_api_ret(session, err_info);
}

API int
sr_get_changes_iter(sr_session_ctx_t *session, const char *xpath, sr_change_iter_t **iter)
{
    return _sr_get_changes_iter(session, xpath, 0, iter);
}

API int
sr_dup_changes_iter(sr_session_ctx_t *session, const char *xpath, sr_change_iter_t **iter)
{
    return _sr_get_changes_iter(session, xpath, 1, iter);
}

/**
 * @brief Transform change from a libyang node tree into sysrepo value.
 *
 * @param[in] node libyang node.
 * @param[in] value_str Optional value to override.
 * @param[in] keys_predicate Optional keys predicate to override.
 * @param[out] sr_val_p Transformed sysrepo value.
 * @return err_info, NULL on success.
 */
static sr_error_info_t *
sr_change_ly2sr(const struct lyd_node *node, const char *value_str, const char *keys_predicate, sr_val_t **sr_val_p)
{
    sr_error_info_t *err_info = NULL;
    uint32_t end;
    sr_val_t *sr_val;
    struct lyd_node *node_dup = NULL;
    const struct lyd_node *node_ptr;
    LY_ERR lyrc;

    sr_val = calloc(1, sizeof *sr_val);
    SR_CHECK_MEM_GOTO(!sr_val, err_info, cleanup);

    if (value_str) {
        /* replace the value in a node copy so that this specific one is stored */
        assert(node->schema->nodetype & (LYS_LEAF | LYS_LEAFLIST));
        lyrc = lyd_dup_single(node, NULL, 0, &node_dup);
        if (lyrc) {
            sr_errinfo_new_ly(&err_info, LYD_CTX(node));
            goto cleanup;
        }

        lyrc = lyd_change_term(node_dup, value_str);
        if (lyrc && (lyrc != LY_EEXIST) && (lyrc != LY_ENOT)) {
            sr_errinfo_new_ly(&err_info, LYD_CTX(node));
            goto cleanup;
        }
        node_dup->parent = node->parent;
        node_dup->flags |= node->flags & LYD_DEFAULT;

        node_ptr = node_dup;
    } else {
        node_ptr = node;
    }

    /* fill the sr value */
    if ((err_info = sr_val_ly2sr(node_ptr, sr_val))) {
        goto cleanup;
    }

    /* adjust specific members for changes */
    switch (node->schema->nodetype) {
    case LYS_LIST:
        /* fix the xpath if needed */
        if (keys_predicate) {
            /* get xpath without the keys predicate */
            free(sr_val->xpath);
            sr_val->xpath = lyd_path(node, LYD_PATH_STD_NO_LAST_PRED, NULL, 0);
            SR_CHECK_MEM_GOTO(!sr_val->xpath, err_info, cleanup);

            end = strlen(sr_val->xpath);

            /* original length + keys_predicate + ending 0 */
            sr_val->xpath = sr_realloc(sr_val->xpath, end + strlen(keys_predicate) + 1);
            SR_CHECK_MEM_GOTO(!sr_val->xpath, err_info, cleanup);

            /* concatenate the specific predicate */
            strcpy(sr_val->xpath + end, keys_predicate);
        }
        break;
    case LYS_LEAFLIST:
        /* do not include the value predicate */
        free(sr_val->xpath);
        sr_val->xpath = lyd_path(node, LYD_PATH_STD_NO_LAST_PRED, NULL, 0);
        SR_CHECK_MEM_GOTO(!sr_val->xpath, err_info, cleanup);
        break;
    case LYS_LEAF:
    case LYS_CONTAINER:
    case LYS_NOTIF:
        /* nothing to do */
        break;
    case LYS_ANYXML:
    case LYS_ANYDATA:
        /* TODO */
        break;
    default:
        SR_ERRINFO_INT(&err_info);
        goto cleanup;
    }

cleanup:
    lyd_free_tree(node_dup);
    if (err_info) {
        if (sr_val) {
            free(sr_val->xpath);
        }
        free(sr_val);
    } else {
        *sr_val_p = sr_val;
     }
     return err_info;
}

API int
sr_get_change_next(sr_session_ctx_t *session, sr_change_iter_t *iter, sr_change_oper_t *operation,
        sr_val_t **old_value, sr_val_t **new_value)
{
    sr_error_info_t *err_info = NULL;
    struct lyd_meta *meta, *meta2;
    struct lyd_node *node;
    const char *meta_name;
    sr_change_oper_t op;

    SR_CHECK_ARG_APIRET(!session || !iter || !operation || !old_value || !new_value, session, err_info);

    /* get next change */
    if ((err_info = sr_diff_set_getnext(iter->set, &iter->idx, &node, &op))) {
        return sr_api_ret(session, err_info);
    }

    if (!node) {
        /* no more changes */
        return SR_ERR_NOT_FOUND;
    }

    /* create values */
    switch (op) {
    case SR_OP_DELETED:
        if ((err_info = sr_change_ly2sr(node, NULL, NULL, old_value))) {
            return sr_api_ret(session, err_info);
        }
        *new_value = NULL;
        break;
    case SR_OP_MODIFIED:
        /* "orig-value" metadata contains the previous value */
        meta = lyd_find_meta(node->meta, NULL, "yang:orig-value");

        /* "orig-default" holds the previous default flag value */
        meta2 = lyd_find_meta(node->meta, NULL, "yang:orig-default");

        if (!meta || !meta2) {
            SR_ERRINFO_INT(&err_info);
            return sr_api_ret(session, err_info);
        }
        if ((err_info = sr_change_ly2sr(node, meta->value.canonical, NULL, old_value))) {
            return sr_api_ret(session, err_info);
        }
        if (meta2->value.boolean) {
            (*old_value)->dflt = 1;
        } else {
            (*old_value)->dflt = 0;
        }
        if ((err_info = sr_change_ly2sr(node, NULL, NULL, new_value))) {
            return sr_api_ret(session, err_info);
        }
        break;
    case SR_OP_CREATED:
        if (!lysc_is_userordered(node->schema)) {
            /* not a user-ordered list, so the operation is a simple creation */
            *old_value = NULL;
            if ((err_info = sr_change_ly2sr(node, NULL, NULL, new_value))) {
                return sr_api_ret(session, err_info);
            }
            break;
        }
    /* fallthrough */
    case SR_OP_MOVED:
        if (node->schema->nodetype == LYS_LEAFLIST) {
            meta_name = "yang:value";
        } else {
            assert(node->schema->nodetype == LYS_LIST);
            meta_name = "yang:key";
        }
        /* attribute contains the value of the node before in the order */
        meta = lyd_find_meta(node->meta, NULL, meta_name);
        if (!meta) {
            SR_ERRINFO_INT(&err_info);
            return sr_api_ret(session, err_info);
        }

        if (meta->value.canonical[0]) {
            if (node->schema->nodetype == LYS_LEAFLIST) {
                err_info = sr_change_ly2sr(node, meta->value.canonical, NULL, old_value);
            } else {
                err_info = sr_change_ly2sr(node, NULL, meta->value.canonical, old_value);
            }
            if (err_info) {
                return sr_api_ret(session, err_info);
            }
        } else {
            /* inserted as the first item */
            *old_value = NULL;
        }
        if ((err_info = sr_change_ly2sr(node, NULL, NULL, new_value))) {
            return sr_api_ret(session, err_info);
        }
        break;
    }

    *operation = op;
    return sr_api_ret(session, NULL);
}

API int
sr_get_change_tree_next(sr_session_ctx_t *session, sr_change_iter_t *iter, sr_change_oper_t *operation,
        const struct lyd_node **node, const char **prev_value, const char **prev_list, int *prev_dflt)
{
    sr_error_info_t *err_info = NULL;
    struct lyd_meta *meta, *meta2;
    const char *meta_name;

    SR_CHECK_ARG_APIRET(!session || !iter || !operation || !node, session, err_info);

    if (prev_value) {
        *prev_value = NULL;
    }
    if (prev_list) {
        *prev_list = NULL;
    }
    if (prev_dflt) {
        *prev_dflt = 0;
    }

    /* get next change */
    if ((err_info = sr_diff_set_getnext(iter->set, &iter->idx, (struct lyd_node **)node, operation))) {
        return sr_api_ret(session, err_info);
    }

    if (!*node) {
        /* no more changes */
        return SR_ERR_NOT_FOUND;
    }

    /* create values */
    switch (*operation) {
    case SR_OP_DELETED:
        /* nothing to do */
        break;
    case SR_OP_MODIFIED:
        /* "orig-value" metadata contains the previous value */
        for (meta = (*node)->meta;
             meta && (strcmp(meta->annotation->module->name, "yang") || strcmp(meta->name, "orig-value"));
             meta = meta->next);

        /* "orig-default" holds the previous default flag value */
        for (meta2 = (*node)->meta;
             meta2 && (strcmp(meta2->annotation->module->name, "yang") || strcmp(meta2->name, "orig-default"));
             meta2 = meta2->next);

        if (!meta || !meta2) {
            SR_ERRINFO_INT(&err_info);
            return sr_api_ret(session, err_info);
        }
        if (prev_value) {
            *prev_value = meta->value.canonical;
        }
        if (prev_dflt && meta2->value.boolean) {
            *prev_dflt = 1;
        }
        break;
    case SR_OP_CREATED:
        if (!lysc_is_userordered((*node)->schema)) {
            /* nothing to do */
            break;
        }
    /* fallthrough */
    case SR_OP_MOVED:
        if ((*node)->schema->nodetype == LYS_LEAFLIST) {
            meta_name = "value";
        } else {
            assert((*node)->schema->nodetype == LYS_LIST);
            meta_name = "key";
        }

        /* attribute contains the value (predicates) of the preceding instance in the order */
        for (meta = (*node)->meta;
             meta && (strcmp(meta->annotation->module->name, "yang") || strcmp(meta->name, meta_name));
             meta = meta->next);
        if (!meta) {
            SR_ERRINFO_INT(&err_info);
            return sr_api_ret(session, err_info);
        }
        if ((*node)->schema->nodetype == LYS_LEAFLIST) {
            if (prev_value) {
                *prev_value = meta->value.canonical;
            }
        } else {
            assert((*node)->schema->nodetype == LYS_LIST);
            if (prev_list) {
                *prev_list = meta->value.canonical;
            }
        }
        break;
    }

    return sr_api_ret(session, NULL);
}

API void
sr_free_change_iter(sr_change_iter_t *iter)
{
    if (!iter) {
        return;
    }

    lyd_free_all(iter->diff);
    ly_set_free(iter->set, NULL);
    free(iter);
}

/**
 * @brief Subscribe to an RPC/action.
 *
 * @param[in] session Session to use.
 * @param[in] path Path to subscribe to.
 * @param[in] callback Callback.
 * @param[in] tree_callback Tree callback.
 * @param[in] private_data Arbitrary callback data.
 * @param[in] opts Subscription options.
 * @param[out] subscription Subscription structure.
 * @return err_code (SR_ERR_OK on success).
 */
static int
_sr_rpc_subscribe(sr_session_ctx_t *session, const char *xpath, sr_rpc_cb callback, sr_rpc_tree_cb tree_callback,
        void *private_data, uint32_t priority, sr_subscr_options_t opts, sr_subscription_ctx_t **subscription)
{
    sr_error_info_t *err_info = NULL, *tmp_err;
    char *module_name = NULL, *path = NULL;
    const struct lysc_node *op;
    const struct lys_module *ly_mod;
    uint32_t sub_id;
    sr_conn_ctx_t *conn;
    sr_rpc_t *shm_rpc;

    SR_CHECK_ARG_APIRET(!session || SR_IS_EVENT_SESS(session) || !xpath || (!callback && !tree_callback) || !subscription,
            session, err_info);

    if ((opts & SR_SUBSCR_CTX_REUSE) && !*subscription) {
        /* invalid option, remove */
        opts &= ~SR_SUBSCR_CTX_REUSE;
    }

    conn = session->conn;
    module_name = sr_get_first_ns(xpath);
    if (!module_name) {
        sr_errinfo_new(&err_info, SR_ERR_INVAL_ARG, "Invalid xpath \"%s\".", xpath);
        goto error1;
    }

    /* is the module name valid? */
    ly_mod = ly_ctx_get_module_implemented(conn->ly_ctx, module_name);
    if (!ly_mod) {
        sr_errinfo_new(&err_info, SR_ERR_NOT_FOUND, "Module \"%s\" was not found in sysrepo.", module_name);
        goto error1;
    }

    /* check write perm */
    if ((err_info = sr_perm_check(module_name, 1, NULL))) {
        goto error1;
    }

    /* is the xpath valid? */
    if ((err_info = sr_get_trim_predicates(xpath, &path))) {
        goto error1;
    }

    if (!(op = lys_find_path(conn->ly_ctx, NULL, path, 0))) {
        sr_errinfo_new_ly(&err_info, conn->ly_ctx);
        goto error1;
    }
    if (!(op->nodetype & (LYS_RPC | LYS_ACTION))) {
        sr_errinfo_new(&err_info, SR_ERR_INVAL_ARG, "Path \"%s\" does not identify an RPC nor an action.", path);
        goto error1;
    }

    if (!(opts & SR_SUBSCR_CTX_REUSE)) {
        /* create a new subscription */
        if ((err_info = sr_subscr_new(conn, opts, subscription))) {
            goto error1;
        }
    }

    /* get new sub ID */
    sub_id = ATOMIC_INC_RELAXED(SR_CONN_MAIN_SHM(conn)->new_sub_id);
    if (sub_id == (uint32_t)(ATOMIC_T_MAX - 1)) {
        /* the value in the main SHM is actually ATOMIC_T_MAX and calling another INC would cause an overflow */
        ATOMIC_STORE_RELAXED(SR_CONN_MAIN_SHM(conn)->new_sub_id, 1);
    }

    /* find the RPC */
    shm_rpc = sr_shmmain_find_rpc(SR_CONN_MAIN_SHM(conn), path);
    SR_CHECK_INT_GOTO(!shm_rpc, err_info, error2);

    /* add RPC/action subscription into ext SHM */
    if ((err_info = sr_shmext_rpc_sub_add(conn, shm_rpc, sub_id, xpath, priority, 0, (*subscription)->evpipe_num))) {
        goto error2;
    }

    /* add subscription into structure and create separate specific SHM segment */
    if ((err_info = sr_subscr_rpc_sub_add(*subscription, sub_id, session, path, xpath, callback, tree_callback,
            private_data, priority, 0))) {
        goto error3;
    }

    /* add the subscription into session */
    if ((err_info = sr_ptr_add(&session->ptr_lock, (void ***)&session->subscriptions, &session->subscription_count,
            *subscription))) {
        goto error4;
    }

    free(module_name);
    free(path);
    return sr_api_ret(session, err_info);

error4:
    sr_subscr_rpc_sub_del(*subscription, sub_id, SR_LOCK_NONE);

error3:
    if ((tmp_err = sr_shmext_rpc_sub_del(conn, shm_rpc, sub_id))) {
        sr_errinfo_merge(&err_info, tmp_err);
    }

error2:
    if (!(opts & SR_SUBSCR_CTX_REUSE)) {
        _sr_unsubscribe(*subscription);
        *subscription = NULL;
    }

error1:
    free(module_name);
    free(path);
    return sr_api_ret(session, err_info);
}

API int
sr_rpc_subscribe(sr_session_ctx_t *session, const char *xpath, sr_rpc_cb callback, void *private_data,
        uint32_t priority, sr_subscr_options_t opts, sr_subscription_ctx_t **subscription)
{
    return _sr_rpc_subscribe(session, xpath, callback, NULL, private_data, priority, opts, subscription);
}

API int
sr_rpc_subscribe_tree(sr_session_ctx_t *session, const char *xpath, sr_rpc_tree_cb callback, void *private_data,
        uint32_t priority, sr_subscr_options_t opts, sr_subscription_ctx_t **subscription)
{
    return _sr_rpc_subscribe(session, xpath, NULL, callback, private_data, priority, opts, subscription);
}

API int
sr_rpc_send(sr_session_ctx_t *session, const char *path, const sr_val_t *input, const size_t input_cnt, uint32_t timeout_ms,
        sr_val_t **output, size_t *output_cnt)
{
    sr_error_info_t *err_info = NULL;
    struct lyd_node *input_tree = NULL, *output_tree = NULL, *elem;
    char *val_str, buf[22];
    size_t i;
    int ret;

    SR_CHECK_ARG_APIRET(!session || !output || !output_cnt, session, err_info);

    if (!timeout_ms) {
        timeout_ms = SR_RPC_CB_TIMEOUT;
    }
    *output = NULL;
    *output_cnt = 0;

    /* create the container */
    if ((err_info = sr_val_sr2ly(session->conn->ly_ctx, path, NULL, 0, 0, &input_tree))) {
        goto cleanup;
    }

    /* transform input into a data tree */
    for (i = 0; i < input_cnt; ++i) {
        val_str = sr_val_sr2ly_str(session->conn->ly_ctx, &input[i], input[i].xpath, buf, 0);
        if ((err_info = sr_val_sr2ly(session->conn->ly_ctx, input[i].xpath, val_str, input[i].dflt, 0, &input_tree))) {
            goto cleanup;
        }
    }

    /* API function */
    if ((ret = sr_rpc_send_tree(session, input_tree, timeout_ms, &output_tree)) != SR_ERR_OK) {
        lyd_free_all(input_tree);
        return ret;
    }

    /* transform data tree into an output */
    assert(output_tree && (output_tree->schema->nodetype & (LYS_RPC | LYS_ACTION)));
    *output_cnt = 0;
    *output = NULL;
    LYD_TREE_DFS_BEGIN(output_tree, elem) {
        if (elem != output_tree) {
            /* allocate new sr_val */
            *output = sr_realloc(*output, (*output_cnt + 1) * sizeof **output);
            SR_CHECK_MEM_GOTO(!*output, err_info, cleanup);

            /* fill it */
            if ((err_info = sr_val_ly2sr(elem, &(*output)[*output_cnt]))) {
                goto cleanup;
            }

            /* now the new value is valid */
            ++(*output_cnt);
        }

        LYD_TREE_DFS_END(output_tree, elem);
    }

    /* success */

cleanup:
    lyd_free_all(input_tree);
    lyd_free_all(output_tree);
    if (err_info) {
        sr_free_values(*output, *output_cnt);
    }
    return sr_api_ret(session, err_info);
}

API int
sr_rpc_send_tree(sr_session_ctx_t *session, struct lyd_node *input, uint32_t timeout_ms, struct lyd_node **output)
{
    sr_error_info_t *err_info = NULL, *cb_err_info = NULL;
    struct sr_mod_info_s mod_info;
    sr_rpc_t *shm_rpc;
    struct ly_set mod_set = {0};
    struct lyd_node *input_op;
    sr_dep_t *shm_deps;
    uint16_t shm_dep_count;
    char *path = NULL, *str;
    uint32_t event_id = 0;

    SR_CHECK_ARG_APIRET(!session || !input || !output, session, err_info);
    if (session->conn->ly_ctx != input->schema->module->ctx) {
        sr_errinfo_new(&err_info, SR_ERR_INVAL_ARG, "Data trees must be created using the session connection libyang context.");
        return sr_api_ret(session, err_info);
    }

    if (!timeout_ms) {
        timeout_ms = SR_RPC_CB_TIMEOUT;
    }
    *output = NULL;
    SR_MODINFO_INIT(mod_info, session->conn, SR_DS_OPERATIONAL, SR_DS_RUNNING);

    /* check input data tree */
    switch (input->schema->nodetype) {
    case LYS_ACTION:
        for (input_op = input; input->parent; input = lyd_parent(input)) {}
        break;
    case LYS_RPC:
        input_op = input;
        break;
    case LYS_CONTAINER:
    case LYS_LIST:
        /* find the action */
        input_op = input;
        if ((err_info = sr_ly_find_last_parent(&input_op, LYS_ACTION))) {
            goto cleanup;
        }
        if (input_op->schema->nodetype == LYS_ACTION) {
            break;
        }
    /* fallthrough */
    default:
        sr_errinfo_new(&err_info, SR_ERR_INVAL_ARG, "Provided input is not a valid RPC or action invocation.");
        goto cleanup;
    }

    /* check read perm */
    if ((err_info = sr_perm_check(lyd_owner_module(input)->name, 0, NULL))) {
        goto cleanup;
    }

    /* get operation path (without predicates) */
    str = lyd_path(input_op, LYD_PATH_STD, NULL, 0);
    SR_CHECK_INT_GOTO(!str, err_info, cleanup);
    err_info = sr_get_trim_predicates(str, &path);
    free(str);
    if (err_info) {
        goto cleanup;
    }

    if (input != input_op) {
        /* we need the OP module for checking parent existence */
        ly_set_add(&mod_set, (void *)lyd_owner_module(input), 0, NULL);
        if ((err_info = sr_modinfo_add_modules(&mod_info, &mod_set, 0, SR_LOCK_READ, SR_MI_DATA_CACHE | SR_MI_PERM_NO,
                session->sid, session->orig_name, session->orig_data, NULL, SR_OPER_CB_TIMEOUT, 0))) {
            goto cleanup;
        }
        ly_set_clean(&mod_set, NULL);
    }

    /* collect all required module dependencies for input validation */
    if ((err_info = sr_shmmod_collect_rpc_deps(SR_CONN_MAIN_SHM(session->conn), session->conn->ly_ctx, path, 0,
            &mod_set, &shm_deps, &shm_dep_count))) {
        goto cleanup;
    }
    if ((err_info = sr_modinfo_add_modules(&mod_info, &mod_set, 0, SR_LOCK_READ,
            SR_MI_MOD_DEPS | SR_MI_DATA_CACHE | SR_MI_PERM_NO, session->sid, session->orig_name, session->orig_data,
            NULL, SR_OPER_CB_TIMEOUT, 0))) {
        goto cleanup;
    }

    /* collect also any inst-id target modules */
    ly_set_clean(&mod_set, NULL);
    if ((err_info = sr_shmmod_collect_instid_deps_data(SR_CONN_MAIN_SHM(session->conn), shm_deps, shm_dep_count,
            session->conn->ly_ctx, input, &mod_set))) {
        goto cleanup;
    }
    if ((err_info = sr_modinfo_add_modules(&mod_info, &mod_set, 0, SR_LOCK_READ,
            SR_MI_MOD_DEPS | SR_MI_DATA_CACHE | SR_MI_PERM_NO, session->sid, session->orig_name, session->orig_data,
            NULL, SR_OPER_CB_TIMEOUT, 0))) {
        goto cleanup;
    }

    /* validate the operation, must be valid only at the time of execution */
    if ((err_info = sr_modinfo_op_validate(&mod_info, input_op, 0))) {
        goto cleanup;
    }

    /* MODULES UNLOCK */
    sr_shmmod_modinfo_unlock(&mod_info, session->sid);

    ly_set_clean(&mod_set, NULL);
    sr_modinfo_free(&mod_info);
    SR_MODINFO_INIT(mod_info, session->conn, SR_DS_OPERATIONAL, SR_DS_RUNNING);

    /* find the RPC */
    shm_rpc = sr_shmmain_find_rpc(SR_CONN_MAIN_SHM(session->conn), path);
    SR_CHECK_INT_GOTO(!shm_rpc, err_info, cleanup);

    /* RPC SUB READ LOCK */
    if ((err_info = sr_rwlock(&shm_rpc->lock, SR_SHMEXT_SUB_LOCK_TIMEOUT, SR_LOCK_READ, session->conn->cid, __func__,
            NULL, NULL))) {
        goto cleanup;
    }

    /* publish RPC in an event and wait for a reply from the last subscriber */
    if ((err_info = sr_shmsub_rpc_notify(session->conn, shm_rpc, path, input, session->orig_name, session->orig_data,
            timeout_ms, &event_id, output, &cb_err_info))) {
        goto cleanup_rpcsub_unlock;
    }

    if (cb_err_info) {
        /* "rpc" event failed, publish "abort" event and finish */
        err_info = sr_shmsub_rpc_notify_abort(session->conn, shm_rpc, path, input, session->orig_name, session->orig_data,
                timeout_ms, event_id);
        goto cleanup_rpcsub_unlock;
    }

    /* RPC SUB READ UNLOCK */
    sr_rwunlock(&shm_rpc->lock, SR_SHMEXT_SUB_LOCK_TIMEOUT, SR_LOCK_READ, session->conn->cid, __func__);

    /* find operation */
    if ((err_info = sr_ly_find_last_parent(output, LYS_RPC | LYS_ACTION))) {
        goto cleanup;
    }

    /* collect all required modules for output validation */
    if ((err_info = sr_shmmod_collect_rpc_deps(SR_CONN_MAIN_SHM(session->conn), session->conn->ly_ctx, path, 1,
            &mod_set, &shm_deps, &shm_dep_count))) {
        goto cleanup;
    }
    if ((err_info = sr_modinfo_add_modules(&mod_info, &mod_set, 0, SR_LOCK_READ,
            SR_MI_MOD_DEPS | SR_MI_DATA_CACHE | SR_MI_PERM_NO, session->sid, session->orig_name, session->orig_data,
            NULL, SR_OPER_CB_TIMEOUT, 0))) {
        goto cleanup;
    }

    /* collect also any inst-id target modules */
    ly_set_clean(&mod_set, NULL);
    if ((err_info = sr_shmmod_collect_instid_deps_data(SR_CONN_MAIN_SHM(session->conn), shm_deps, shm_dep_count,
            session->conn->ly_ctx, input, &mod_set))) {
        goto cleanup;
    }
    if ((err_info = sr_modinfo_add_modules(&mod_info, &mod_set, 0, SR_LOCK_READ,
            SR_MI_MOD_DEPS | SR_MI_DATA_CACHE | SR_MI_PERM_NO, session->sid, session->orig_name, session->orig_data,
            NULL, SR_OPER_CB_TIMEOUT, 0))) {
        goto cleanup;
    }

    /* validate the output */
    if ((err_info = sr_modinfo_op_validate(&mod_info, *output, 1))) {
        goto cleanup;
    }

    /* success */
    goto cleanup;

cleanup_rpcsub_unlock:
    /* RPC SUB READ UNLOCK */
    sr_rwunlock(&shm_rpc->lock, SR_SHMEXT_SUB_LOCK_TIMEOUT, SR_LOCK_READ, session->conn->cid, __func__);

cleanup:
    /* MODULES UNLOCK */
    sr_shmmod_modinfo_unlock(&mod_info, session->sid);

    free(path);
    ly_set_erase(&mod_set, NULL);
    sr_modinfo_free(&mod_info);
    if (cb_err_info) {
        /* return callback error if some was generated */
        sr_errinfo_merge(&err_info, cb_err_info);
        sr_errinfo_new(&err_info, SR_ERR_CALLBACK_FAILED, "User callback failed.");
    }
    if (err_info) {
        /* free any received output in case of an error */
        lyd_free_all(*output);
        *output = NULL;
    }
    return sr_api_ret(session, err_info);
}

/**
 * @brief libyang callback for full module traversal when searching for a notification.
 */
static LY_ERR
sr_event_notif_lysc_dfs_cb(struct lysc_node *node, void *data, ly_bool *dfs_continue)
{
    int *found = (int *)data;
    (void)dfs_continue;

    if (node->nodetype == LYS_NOTIF) {
        *found = 1;

        /* just stop the traversal */
        return LY_EEXIST;
    }

    return LY_SUCCESS;
}

/**
 * @brief Subscribe to a notification.
 *
 * @param[in] session Session subscription.
 * @param[in] ly_mod Notification module.
 * @param[in] xpath XPath to subscribe to.
 * @param[in] start_time Optional subscription start time.
 * @param[in] stop_time Optional subscription stop time.
 * @param[in] callback Callback.
 * @param[in] tree_callback Tree callback.
 * @param[in] private_data Arbitrary callback data.
 * @param[in] opts Subscription options.
 * @param[out] subscription Subscription structure.
 * @return err_code (SR_ERR_OK on success).
 */
static int
_sr_event_notif_subscribe(sr_session_ctx_t *session, const char *mod_name, const char *xpath, time_t start_time,
        time_t stop_time, sr_event_notif_cb callback, sr_event_notif_tree_cb tree_callback, void *private_data,
        sr_subscr_options_t opts, sr_subscription_ctx_t **subscription)
{
    sr_error_info_t *err_info = NULL, *tmp_err;
    struct ly_set *set;
    time_t cur_ts = time(NULL);
    const struct lys_module *ly_mod;
    sr_conn_ctx_t *conn;
    uint32_t i, sub_id;
    sr_mod_t *shm_mod;
    LY_ERR lyrc;
    int found;

    SR_CHECK_ARG_APIRET(!session || SR_IS_EVENT_SESS(session) || !mod_name || (start_time && (start_time > cur_ts)) ||
            (stop_time && (!start_time || (stop_time < start_time))) || (!callback && !tree_callback) || !subscription,
            session, err_info);

    /* is the module name valid? */
    ly_mod = ly_ctx_get_module_implemented(session->conn->ly_ctx, mod_name);
    if (!ly_mod) {
        sr_errinfo_new(&err_info, SR_ERR_NOT_FOUND, "Module \"%s\" was not found in sysrepo.", mod_name);
        return sr_api_ret(session, err_info);
    }

    /* check write perm */
    if ((err_info = sr_perm_check(mod_name, 1, NULL))) {
        return sr_api_ret(session, err_info);
    }

    if ((opts & SR_SUBSCR_CTX_REUSE) && !*subscription) {
        /* invalid option, remove */
        opts &= ~SR_SUBSCR_CTX_REUSE;
    }

    conn = session->conn;

    /* is the xpath/module valid? */
    found = 0;
    if (xpath) {
        lyrc = lys_find_xpath_atoms(conn->ly_ctx, NULL, xpath, 0, &set);
        if (lyrc) {
            sr_errinfo_new_ly(&err_info, ly_mod->ctx);
            return sr_api_ret(session, err_info);
        }

        /* there must be some notifications selected */
        for (i = 0; i < set->count; ++i) {
            if (set->snodes[i]->nodetype == LYS_NOTIF) {
                found = 1;
                break;
            }
        }
        ly_set_free(set, NULL);
    } else {
        lysc_module_dfs_full(ly_mod, sr_event_notif_lysc_dfs_cb, &found);
    }

    if (!found) {
        if (xpath) {
            sr_errinfo_new(&err_info, SR_ERR_INVAL_ARG, "XPath \"%s\" does not select any notifications.", xpath);
        } else {
            sr_errinfo_new(&err_info, SR_ERR_NOT_FOUND, "Module \"%s\" does not define any notifications.", ly_mod->name);
        }
        return sr_api_ret(session, err_info);
    }

    if (!(opts & SR_SUBSCR_CTX_REUSE)) {
        /* create a new subscription */
        if ((err_info = sr_subscr_new(conn, opts, subscription))) {
            return sr_api_ret(session, err_info);
        }
    }

    /* get new sub ID */
    sub_id = ATOMIC_INC_RELAXED(SR_CONN_MAIN_SHM(conn)->new_sub_id);
    if (sub_id == (uint32_t)(ATOMIC_T_MAX - 1)) {
        /* the value in the main SHM is actually ATOMIC_T_MAX and calling another INC would cause an overflow */
        ATOMIC_STORE_RELAXED(SR_CONN_MAIN_SHM(conn)->new_sub_id, 1);
    }

    /* find module */
    shm_mod = sr_shmmain_find_module(SR_CONN_MAIN_SHM(conn), ly_mod->name);
    SR_CHECK_INT_GOTO(!shm_mod, err_info, error1);

    /* add notification subscription into main SHM, suspended if replay was requested */
    if ((err_info = sr_shmext_notif_sub_add(conn, shm_mod, sub_id, (*subscription)->evpipe_num, start_time ? 1 : 0))) {
        goto error1;
    }

    /* add subscription into structure and create separate specific SHM segment */
    if ((err_info = sr_subscr_notif_sub_add(*subscription, sub_id, session, ly_mod->name, xpath, start_time, stop_time,
            callback, tree_callback, private_data, 0))) {
        goto error2;
    }

    if (start_time) {
        /* notify subscription there are already some events (replay needs to be performed) */
        if ((err_info = sr_shmsub_notify_evpipe((*subscription)->evpipe_num))) {
            goto error3;
        }
    }

    /* add the subscription into session */
    if ((err_info = sr_ptr_add(&session->ptr_lock, (void ***)&session->subscriptions, &session->subscription_count,
            *subscription))) {
        goto error3;
    }

    return sr_api_ret(session, NULL);

error3:
    sr_subscr_notif_sub_del(*subscription, sub_id, SR_LOCK_NONE);

error2:
    if ((tmp_err = sr_shmext_notif_sub_del(conn, shm_mod, sub_id))) {
        sr_errinfo_merge(&err_info, tmp_err);
    }

error1:
    if (!(opts & SR_SUBSCR_CTX_REUSE)) {
        _sr_unsubscribe(*subscription);
        *subscription = NULL;
    }

    return sr_api_ret(session, err_info);
}

API int
sr_event_notif_subscribe(sr_session_ctx_t *session, const char *module_name, const char *xpath, time_t start_time,
        time_t stop_time, sr_event_notif_cb callback, void *private_data, sr_subscr_options_t opts,
        sr_subscription_ctx_t **subscription)
{
    return _sr_event_notif_subscribe(session, module_name, xpath, start_time, stop_time, callback, NULL, private_data,
            opts, subscription);
}

API int
sr_event_notif_subscribe_tree(sr_session_ctx_t *session, const char *module_name, const char *xpath, time_t start_time,
        time_t stop_time, sr_event_notif_tree_cb callback, void *private_data, sr_subscr_options_t opts,
        sr_subscription_ctx_t **subscription)
{
    return _sr_event_notif_subscribe(session, module_name, xpath, start_time, stop_time, NULL, callback, private_data,
            opts, subscription);
}

API int
sr_event_notif_send(sr_session_ctx_t *session, const char *path, const sr_val_t *values, const size_t values_cnt)
{
    sr_error_info_t *err_info = NULL;
    struct lyd_node *notif_tree = NULL;
    char *val_str, buf[22];
    size_t i;
    int ret;

    SR_CHECK_ARG_APIRET(!session || !path, session, err_info);

    /* create the container */
    if ((err_info = sr_val_sr2ly(session->conn->ly_ctx, path, NULL, 0, 0, &notif_tree))) {
        goto cleanup;
    }

    /* transform values into a data tree */
    for (i = 0; i < values_cnt; ++i) {
        val_str = sr_val_sr2ly_str(session->conn->ly_ctx, &values[i], values[i].xpath, buf, 0);
        if ((err_info = sr_val_sr2ly(session->conn->ly_ctx, values[i].xpath, val_str, values[i].dflt, 0, &notif_tree))) {
            goto cleanup;
        }
    }

    /* API function */
    if ((ret = sr_event_notif_send_tree(session, notif_tree)) != SR_ERR_OK) {
        lyd_free_all(notif_tree);
        return ret;
    }

    /* success */

cleanup:
    lyd_free_all(notif_tree);
    return sr_api_ret(session, err_info);
}

API int
sr_event_notif_send_tree(sr_session_ctx_t *session, struct lyd_node *notif)
{
    sr_error_info_t *err_info = NULL, *tmp_err = NULL;
    struct sr_mod_info_s mod_info;
    struct ly_set mod_set = {0};
    struct lyd_node *notif_op;
    sr_dep_t *shm_deps;
    sr_mod_t *shm_mod;
    time_t notif_ts;
    uint16_t shm_dep_count;
    char *xpath = NULL;

    SR_CHECK_ARG_APIRET(!session || !notif, session, err_info);
    if (session->conn->ly_ctx != notif->schema->module->ctx) {
        sr_errinfo_new(&err_info, SR_ERR_INVAL_ARG, "Data trees must be created using the session connection libyang context.");
        return sr_api_ret(session, err_info);
    }

    SR_MODINFO_INIT(mod_info, session->conn, SR_DS_OPERATIONAL, SR_DS_RUNNING);

    /* remember when the notification was generated */
    notif_ts = time(NULL);

    /* check notif data tree */
    switch (notif->schema->nodetype) {
    case LYS_NOTIF:
        for (notif_op = notif; notif->parent; notif = lyd_parent(notif)) {}
        break;
    case LYS_CONTAINER:
    case LYS_LIST:
        /* find the notification */
        notif_op = notif;
        if ((err_info = sr_ly_find_last_parent(&notif_op, LYS_NOTIF))) {
            goto cleanup;
        }
        if (notif_op->schema->nodetype == LYS_NOTIF) {
            break;
        }
    /* fallthrough */
    default:
        sr_errinfo_new(&err_info, SR_ERR_INVAL_ARG, "Provided tree is not a valid notification invocation.");
        goto cleanup;
    }

    /* check write/read perm */
    shm_mod = sr_shmmain_find_module(SR_CONN_MAIN_SHM(session->conn), lyd_owner_module(notif)->name);
    SR_CHECK_INT_GOTO(!shm_mod, err_info, cleanup);
    if ((err_info = sr_perm_check(lyd_owner_module(notif)->name, ATOMIC_LOAD_RELAXED(shm_mod->replay_supp), NULL))) {
        goto cleanup;
    }

    if (notif != notif_op) {
        /* we need the OP module for checking parent existence */
        ly_set_add(&mod_set, (void *)lyd_owner_module(notif), 0, NULL);
        if ((err_info = sr_modinfo_add_modules(&mod_info, &mod_set, 0, SR_LOCK_READ, SR_MI_DATA_CACHE | SR_MI_PERM_NO,
                session->sid, session->orig_name, session->orig_data, NULL, SR_OPER_CB_TIMEOUT, 0))) {
            goto cleanup;
        }
        ly_set_clean(&mod_set, NULL);
    }

    /* collect all required modules for OP validation */
    xpath = lysc_path(notif_op->schema, LYSC_PATH_DATA, NULL, 0);
    SR_CHECK_MEM_GOTO(!xpath, err_info, cleanup);
    if ((err_info = sr_shmmod_collect_notif_deps(SR_CONN_MAIN_SHM(session->conn), lyd_owner_module(notif), xpath,
            &mod_set, &shm_deps, &shm_dep_count))) {
        goto cleanup;
    }
    if ((err_info = sr_modinfo_add_modules(&mod_info, &mod_set, 0, SR_LOCK_READ,
            SR_MI_MOD_DEPS | SR_MI_DATA_CACHE | SR_MI_PERM_NO, session->sid, session->orig_name, session->orig_data,
            NULL, SR_OPER_CB_TIMEOUT, 0))) {
        goto cleanup;
    }

    /* collect also any inst-id target modules */
    ly_set_clean(&mod_set, NULL);
    if ((err_info = sr_shmmod_collect_instid_deps_data(SR_CONN_MAIN_SHM(session->conn), shm_deps, shm_dep_count,
            session->conn->ly_ctx, notif, &mod_set))) {
        goto cleanup;
    }
    if ((err_info = sr_modinfo_add_modules(&mod_info, &mod_set, 0, SR_LOCK_READ,
            SR_MI_MOD_DEPS | SR_MI_DATA_CACHE | SR_MI_PERM_NO, session->sid, session->orig_name, session->orig_data,
            NULL, SR_OPER_CB_TIMEOUT, 0))) {
        goto cleanup;
    }

    /* validate the operation */
    if ((err_info = sr_modinfo_op_validate(&mod_info, notif_op, 0))) {
        goto cleanup;
    }

    /* MODULES UNLOCK */
    sr_shmmod_modinfo_unlock(&mod_info, session->sid);

    /* store the notification for a replay, we continue on failure */
    err_info = sr_replay_store(session, notif, notif_ts);

    /* NOTIF SUB READ LOCK */
    if ((tmp_err = sr_rwlock(&shm_mod->notif_lock, SR_SHMEXT_SUB_LOCK_TIMEOUT, SR_LOCK_READ, session->conn->cid, __func__,
            NULL, NULL))) {
        goto cleanup;
    }

    /* publish notif in an event, do not wait for subscribers */
    if ((tmp_err = sr_shmsub_notif_notify(session->conn, notif, notif_ts, session->orig_name, session->orig_data))) {
        goto cleanup_notifsub_unlock;
    }

    /* success */

cleanup_notifsub_unlock:
    /* NOTIF SUB READ UNLOCK */
    sr_rwunlock(&shm_mod->notif_lock, SR_SHMEXT_SUB_LOCK_TIMEOUT, SR_LOCK_READ, session->conn->cid, __func__);

cleanup:
    /* MODULES UNLOCK */
    sr_shmmod_modinfo_unlock(&mod_info, session->sid);

    free(xpath);
    ly_set_erase(&mod_set, NULL);
    sr_modinfo_free(&mod_info);
    if (tmp_err) {
        sr_errinfo_merge(&err_info, tmp_err);
    }
    return sr_api_ret(session, err_info);
}

API int
sr_event_notif_sub_get_info(sr_subscription_ctx_t *subscription, uint32_t sub_id, const char **module_name,
        const char **xpath, time_t *start_time, time_t *stop_time, uint32_t *filtered_out)
{
    sr_error_info_t *err_info = NULL;
    struct modsub_notifsub_s *notif_sub;

    SR_CHECK_ARG_APIRET(!subscription || !sub_id, NULL, err_info);

    /* SUBS READ LOCK */
    if ((err_info = sr_rwlock(&subscription->subs_lock, SR_SUBSCR_LOCK_TIMEOUT, SR_LOCK_READ, subscription->conn->cid,
            __func__, NULL, NULL))) {
        return sr_api_ret(NULL, err_info);
    }

    /* find the subscription in the subscription context */
    notif_sub = sr_subscr_notif_sub_find(subscription, sub_id, module_name);
    if (!notif_sub) {
        sr_errinfo_new(&err_info, SR_ERR_NOT_FOUND, "Notification subscription with ID \"%u\" not found.", sub_id);
        goto cleanup_unlock;
    }

    /* fill parameters */
    if (xpath) {
        *xpath = notif_sub->xpath;
    }
    if (start_time) {
        *start_time = notif_sub->start_time;
    }
    if (stop_time) {
        *stop_time = notif_sub->stop_time;
    }
    if (filtered_out) {
        *filtered_out = ATOMIC_LOAD_RELAXED(notif_sub->filtered_out);
    }

cleanup_unlock:
    /* SUBS READ UNLOCK */
    sr_rwunlock(&subscription->subs_lock, SR_SUBSCR_LOCK_TIMEOUT, SR_LOCK_READ, subscription->conn->cid, __func__);

    return sr_api_ret(NULL, err_info);
}

API int
sr_event_notif_sub_modify_xpath(sr_subscription_ctx_t *subscription, uint32_t sub_id, const char *xpath)
{
    sr_error_info_t *err_info = NULL;
    struct modsub_notifsub_s *notif_sub;
    sr_session_ctx_t *ev_sess = NULL;

    SR_CHECK_ARG_APIRET(!subscription || !sub_id, NULL, err_info);

    /* SUBS WRITE LOCK */
    if ((err_info = sr_rwlock(&subscription->subs_lock, SR_SUBSCR_LOCK_TIMEOUT, SR_LOCK_WRITE, subscription->conn->cid,
            __func__, NULL, NULL))) {
        return sr_api_ret(NULL, err_info);
    }

    /* find the subscription in the subscription context */
    notif_sub = sr_subscr_notif_sub_find(subscription, sub_id, NULL);
    if (!notif_sub) {
        sr_errinfo_new(&err_info, SR_ERR_NOT_FOUND, "Notification subscription with ID \"%u\" not found.", sub_id);
        goto cleanup_unlock;
    }

    /* if the xpath is the same, there is nothing to modify */
    if (!xpath && !notif_sub->xpath) {
        goto cleanup_unlock;
    } else if (xpath && notif_sub->xpath && !strcmp(xpath, notif_sub->xpath)) {
        goto cleanup_unlock;
    }

    /* update xpath */
    free(notif_sub->xpath);
    notif_sub->xpath = NULL;
    if (xpath) {
        notif_sub->xpath = strdup(xpath);
        SR_CHECK_MEM_GOTO(!notif_sub->xpath, err_info, cleanup_unlock);
    }

    /* create event session */
    if ((err_info = _sr_session_start(subscription->conn, SR_DS_OPERATIONAL, SR_SUB_EV_NOTIF, NULL, &ev_sess))) {
        goto cleanup_unlock;
    }

    /* send the special notification */
    if ((err_info = sr_notif_call_callback(ev_sess, notif_sub->cb, notif_sub->tree_cb, notif_sub->private_data,
            SR_EV_NOTIF_MODIFIED, sub_id, NULL, time(NULL)))) {
        goto cleanup_unlock;
    }

cleanup_unlock:
    /* SUBS WRITE UNLOCK */
    sr_rwunlock(&subscription->subs_lock, SR_SUBSCR_LOCK_TIMEOUT, SR_LOCK_WRITE, subscription->conn->cid, __func__);

    sr_session_stop(ev_sess);
    return sr_api_ret(NULL, err_info);
}

API int
sr_event_notif_sub_modify_stop_time(sr_subscription_ctx_t *subscription, uint32_t sub_id, time_t stop_time)
{
    sr_error_info_t *err_info = NULL;
    struct modsub_notifsub_s *notif_sub;
    sr_session_ctx_t *ev_sess = NULL;

    SR_CHECK_ARG_APIRET(!subscription || !sub_id, NULL, err_info);

    /* SUBS WRITE LOCK */
    if ((err_info = sr_rwlock(&subscription->subs_lock, SR_SUBSCR_LOCK_TIMEOUT, SR_LOCK_WRITE, subscription->conn->cid,
            __func__, NULL, NULL))) {
        return sr_api_ret(NULL, err_info);
    }

    /* find the subscription in the subscription context */
    notif_sub = sr_subscr_notif_sub_find(subscription, sub_id, NULL);
    if (!notif_sub) {
        sr_errinfo_new(&err_info, SR_ERR_NOT_FOUND, "Notification subscription with ID \"%u\" not found.", sub_id);
        goto cleanup_unlock;
    }

    /* check stop time validity */
    if (stop_time && !notif_sub->start_time && (stop_time < notif_sub->start_time)) {
        sr_errinfo_new(&err_info, SR_ERR_INVAL_ARG, "Stop time cannot be earlier than start time.");
        goto cleanup_unlock;
    }

    /* if the stop time is the same, there is nothing to modify */
    if (stop_time == notif_sub->stop_time) {
        goto cleanup_unlock;
    }

    /* update stop time */
    notif_sub->stop_time = stop_time;

    /* create event session */
    if ((err_info = _sr_session_start(subscription->conn, SR_DS_OPERATIONAL, SR_SUB_EV_NOTIF, NULL, &ev_sess))) {
        goto cleanup_unlock;
    }

    /* send the special notification */
    if ((err_info = sr_notif_call_callback(ev_sess, notif_sub->cb, notif_sub->tree_cb, notif_sub->private_data,
            SR_EV_NOTIF_MODIFIED, sub_id, NULL, time(NULL)))) {
        goto cleanup_unlock;
    }

cleanup_unlock:
    /* SUBS WRITE UNLOCK */
    sr_rwunlock(&subscription->subs_lock, SR_SUBSCR_LOCK_TIMEOUT, SR_LOCK_WRITE, subscription->conn->cid, __func__);

    sr_session_stop(ev_sess);
    return sr_api_ret(NULL, err_info);
}

/**
 * @brief Learn what kinds (config) of nodes are provided by an operational subscription
 * to determine its type.
 *
 * @param[in] ly_ctx libyang context to use.
 * @param[in] path Subscription path.
 * @param[out] sub_type Learned subscription type.
 * @return err_info, NULL on success.
 */
static sr_error_info_t *
sr_oper_sub_get_type(const struct ly_ctx *ly_ctx, const char *path, sr_mod_oper_sub_type_t *sub_type)
{
    sr_error_info_t *err_info = NULL;
    struct lysc_node *elem;
    struct ly_set *set = NULL;
    uint16_t i;

    if (lys_find_xpath(ly_ctx, NULL, path, 0, &set)) {
        sr_errinfo_new_ly(&err_info, ly_ctx);
        goto cleanup;
    } else if (!set->count) {
        sr_errinfo_new(&err_info, SR_ERR_INVAL_ARG, "XPath \"%s\" does not point to any nodes.", path);
        goto cleanup;
    }

    *sub_type = SR_OPER_SUB_NONE;
    for (i = 0; i < set->count; ++i) {
        LYSC_TREE_DFS_BEGIN(set->snodes[i], elem) {
            switch (elem->nodetype) {
            case LYS_CONTAINER:
            case LYS_LEAF:
            case LYS_LEAFLIST:
            case LYS_LIST:
            case LYS_ANYXML:
            case LYS_ANYDATA:
                /* data node - check config */
                if ((elem->flags & LYS_CONFIG_MASK) == LYS_CONFIG_R) {
                    if (*sub_type == SR_OPER_SUB_CONFIG) {
                        *sub_type = SR_OPER_SUB_MIXED;
                    } else {
                        *sub_type = SR_OPER_SUB_STATE;
                    }
                } else {
                    assert((elem->flags & LYS_CONFIG_MASK) == LYS_CONFIG_W);
                    if (*sub_type == SR_OPER_SUB_STATE) {
                        *sub_type = SR_OPER_SUB_MIXED;
                    } else {
                        *sub_type = SR_OPER_SUB_CONFIG;
                    }
                }
                break;
            case LYS_CHOICE:
            case LYS_CASE:
                /* go into */
                break;
            default:
                /* should not be reachable */
                SR_ERRINFO_INT(&err_info);
                goto cleanup;
            }

            if ((*sub_type == SR_OPER_SUB_STATE) || (*sub_type == SR_OPER_SUB_MIXED)) {
                /* redundant to look recursively */
                break;
            }

            LYSC_TREE_DFS_END(set->snodes[i], elem);
        }

        if (*sub_type == SR_OPER_SUB_MIXED) {
            /* we found both config type nodes, nothing more to look for */
            break;
        }
    }

cleanup:
    ly_set_free(set, NULL);
    return err_info;
}

API int
sr_oper_get_items_subscribe(sr_session_ctx_t *session, const char *module_name, const char *path,
        sr_oper_get_items_cb callback, void *private_data, sr_subscr_options_t opts, sr_subscription_ctx_t **subscription)
{
    sr_error_info_t *err_info = NULL, *tmp_err;
    sr_conn_ctx_t *conn;
    const struct lys_module *ly_mod;
    sr_mod_oper_sub_type_t sub_type;
    uint32_t sub_id;
    sr_subscr_options_t sub_opts;
    sr_mod_t *shm_mod;

    SR_CHECK_ARG_APIRET(!session || SR_IS_EVENT_SESS(session) || !module_name || !path || !callback || !subscription,
            session, err_info);

    if ((opts & SR_SUBSCR_CTX_REUSE) && !*subscription) {
        /* invalid option, remove */
        opts &= ~SR_SUBSCR_CTX_REUSE;
    }

    conn = session->conn;
    /* only these options are relevant outside this function and will be stored */
    sub_opts = opts & SR_SUBSCR_OPER_MERGE;

    ly_mod = ly_ctx_get_module_implemented(conn->ly_ctx, module_name);
    if (!ly_mod) {
        sr_errinfo_new(&err_info, SR_ERR_INVAL_ARG, "Module \"%s\" was not found in sysrepo.", module_name);
        return sr_api_ret(session, err_info);
    }

    /* check write perm */
    if ((err_info = sr_perm_check(module_name, 1, NULL))) {
        return sr_api_ret(session, err_info);
    }

    /* find out what kinds of nodes are provided */
    if ((err_info = sr_oper_sub_get_type(conn->ly_ctx, path, &sub_type))) {
        return sr_api_ret(session, err_info);
    }

    if (!(opts & SR_SUBSCR_CTX_REUSE)) {
        /* create a new subscription */
        if ((err_info = sr_subscr_new(conn, opts, subscription))) {
            return sr_api_ret(session, err_info);
        }
    }

    /* get new sub ID */
    sub_id = ATOMIC_INC_RELAXED(SR_CONN_MAIN_SHM(conn)->new_sub_id);
    if (sub_id == (uint32_t)(ATOMIC_T_MAX - 1)) {
        /* the value in the main SHM is actually ATOMIC_T_MAX and calling another INC would cause an overflow */
        ATOMIC_STORE_RELAXED(SR_CONN_MAIN_SHM(conn)->new_sub_id, 1);
    }

    /* find module */
    shm_mod = sr_shmmain_find_module(SR_CONN_MAIN_SHM(conn), module_name);
    SR_CHECK_INT_GOTO(!shm_mod, err_info, error1);

    /* add oper subscription into main SHM */
    if ((err_info = sr_shmext_oper_sub_add(conn, shm_mod, sub_id, path, sub_type, sub_opts,
            (*subscription)->evpipe_num))) {
        goto error1;
    }

    /* add subscription into structure and create separate specific SHM segment */
    if ((err_info = sr_subscr_oper_sub_add(*subscription, sub_id, session, module_name, path, callback, private_data, 0))) {
        goto error2;
    }

    /* add the subscription into session */
    if ((err_info = sr_ptr_add(&session->ptr_lock, (void ***)&session->subscriptions, &session->subscription_count,
            *subscription))) {
        goto error3;
    }

    return sr_api_ret(session, err_info);

error3:
    sr_subscr_oper_sub_del(*subscription, sub_id, SR_LOCK_NONE);

error2:
    if ((tmp_err = sr_shmext_oper_sub_del(conn, shm_mod, sub_id))) {
        sr_errinfo_merge(&err_info, tmp_err);
    }

error1:
    if (!(opts & SR_SUBSCR_CTX_REUSE)) {
        _sr_unsubscribe(*subscription);
        *subscription = NULL;
    }
    return sr_api_ret(session, err_info);
}
