/**
 * @file sr_helpers.h
 * @author Rastislav Szabo <raszabo@cisco.com>, Lukas Macko <lmacko@cisco.com>,
 *         Milan Lenco <milan.lenco@pantheon.tech>
 * @brief Sysrepo helper macros.
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

#ifndef SR_HELPERS_H_
#define SR_HELPERS_H_

/**
 * Time out for mutex and rwlock
 */
#define MUTEX_WAIT_TIME 10

/**
 * CHECK_NULL_ARG internal macro with return
 */
#define CHECK_NULL_ARG__INTERNAL(ARG) \
    if (NULL == ARG) { \
        SR_LOG_ERR("NULL value detected for %s argument of %s", #ARG, __func__); \
        return SR_ERR_INVAL_ARG; \
    } \

/**
 * CHECK_NULL_ARG internal macro without return
 */
#define CHECK_NULL_ARG_VOID__INTERNAL(ARG) \
    if (NULL == ARG) { \
        SR_LOG_ERR("NULL value detected for %s argument of %s", #ARG, __func__); \
        return; \
    } \

/**
 * CHECK_NULL_ARG internal macro with setting rc
 */
#define CHECK_NULL_ARG_NORET__INTERNAL(RC, ARG) \
    if (NULL == ARG) { \
        SR_LOG_ERR("NULL value detected for %s argument of %s", #ARG, __func__); \
        RC = SR_ERR_INVAL_ARG; \
    } \


/**
 * CHECK_NULL_ARG one argument macro with return
 */
#define CHECK_NULL_ARG(ARG) \
    do { \
        CHECK_NULL_ARG__INTERNAL(ARG) \
    } while(0)

/**
 * CHECK_NULL_ARG two arguments macro with return
 */
#define CHECK_NULL_ARG2(ARG1, ARG2) \
    do { \
        CHECK_NULL_ARG__INTERNAL(ARG1) \
        CHECK_NULL_ARG__INTERNAL(ARG2) \
    } while(0)

/**
 * CHECK_NULL_ARG three arguments macro with return
 */
#define CHECK_NULL_ARG3(ARG1, ARG2, ARG3) \
    do { \
        CHECK_NULL_ARG__INTERNAL(ARG1) \
        CHECK_NULL_ARG__INTERNAL(ARG2) \
        CHECK_NULL_ARG__INTERNAL(ARG3) \
    } while(0)


/**
 * CHECK_NULL_ARG four arguments macro with return
 */
#define CHECK_NULL_ARG4(ARG1, ARG2, ARG3, ARG4) \
    do { \
        CHECK_NULL_ARG__INTERNAL(ARG1) \
        CHECK_NULL_ARG__INTERNAL(ARG2) \
        CHECK_NULL_ARG__INTERNAL(ARG3) \
        CHECK_NULL_ARG__INTERNAL(ARG4) \
    } while(0)

/**
 * CHECK_NULL_ARG five arguments macro with return
 */
#define CHECK_NULL_ARG5(ARG1, ARG2, ARG3, ARG4, ARG5) \
    do { \
        CHECK_NULL_ARG__INTERNAL(ARG1) \
        CHECK_NULL_ARG__INTERNAL(ARG2) \
        CHECK_NULL_ARG__INTERNAL(ARG3) \
        CHECK_NULL_ARG__INTERNAL(ARG4) \
        CHECK_NULL_ARG__INTERNAL(ARG5) \
    } while(0)


/**
 * CHECK_NULL_ARG one argument macro without return
 */
#define CHECK_NULL_ARG_VOID(ARG) \
    do { \
        CHECK_NULL_ARG_VOID__INTERNAL(ARG) \
    } while(0)

/**
 * CHECK_NULL_ARG two arguments macro without return
 */
#define CHECK_NULL_ARG_VOID2(ARG1, ARG2) \
    do { \
        CHECK_NULL_ARG_VOID__INTERNAL(ARG1) \
        CHECK_NULL_ARG_VOID__INTERNAL(ARG2) \
    } while(0)

/**
 * CHECK_NULL_ARG three arguments macro without return
 */
#define CHECK_NULL_ARG_VOID3(ARG1, ARG2, ARG3) \
    do { \
        CHECK_NULL_ARG_VOID__INTERNAL(ARG1) \
        CHECK_NULL_ARG_VOID__INTERNAL(ARG2) \
        CHECK_NULL_ARG_VOID__INTERNAL(ARG3) \
    } while(0)


/**
 * CHECK_NULL_ARG four arguments macro without return
 */
#define CHECK_NULL_ARG_VOID4(ARG1, ARG2, ARG3, ARG4) \
    do { \
        CHECK_NULL_ARG_VOID__INTERNAL(ARG1) \
        CHECK_NULL_ARG_VOID__INTERNAL(ARG2) \
        CHECK_NULL_ARG_VOID__INTERNAL(ARG3) \
        CHECK_NULL_ARG_VOID__INTERNAL(ARG4) \
    } while(0)


/**
 * CHECK_NULL_ARG five arguments macro without return
 */
#define CHECK_NULL_ARG_VOID5(ARG1, ARG2, ARG3, ARG4, ARG5) \
    do { \
        CHECK_NULL_ARG_VOID__INTERNAL(ARG1) \
        CHECK_NULL_ARG_VOID__INTERNAL(ARG2) \
        CHECK_NULL_ARG_VOID__INTERNAL(ARG3) \
        CHECK_NULL_ARG_VOID__INTERNAL(ARG4) \
        CHECK_NULL_ARG_VOID__INTERNAL(ARG5) \
    } while(0)


/**
 * CHECK_NULL_ARG one argument macro with setting rc
 */
#define CHECK_NULL_ARG_NORET(RC, ARG) \
    do { \
        CHECK_NULL_ARG_NORET__INTERNAL(RC, ARG) \
    } while(0)

/**
 * CHECK_NULL_ARG two arguments macro with setting rc
 */
#define CHECK_NULL_ARG_NORET2(RC, ARG1, ARG2) \
    do { \
        CHECK_NULL_ARG_NORET__INTERNAL(RC, ARG1) \
        CHECK_NULL_ARG_NORET__INTERNAL(RC, ARG2) \
    } while(0)

/**
 * CHECK_NULL_ARG three arguments macro with setting rc
 */
#define CHECK_NULL_ARG_NORET3(RC, ARG1, ARG2, ARG3) \
    do { \
        CHECK_NULL_ARG_NORET__INTERNAL(RC, ARG1) \
        CHECK_NULL_ARG_NORET__INTERNAL(RC, ARG2) \
        CHECK_NULL_ARG_NORET__INTERNAL(RC, ARG3) \
    } while(0)

/**
 * CHECK_NULL_ARG four arguments macro with setting rc
 */
#define CHECK_NULL_ARG_NORET4(RC, ARG1, ARG2, ARG3, ARG4) \
    do { \
        CHECK_NULL_ARG_NORET__INTERNAL(RC, ARG1) \
        CHECK_NULL_ARG_NORET__INTERNAL(RC, ARG2) \
        CHECK_NULL_ARG_NORET__INTERNAL(RC, ARG3) \
        CHECK_NULL_ARG_NORET__INTERNAL(RC, ARG4) \
    } while(0)

/**
 * CHECK_NULL_ARG five arguments macro with setting rc
 */
#define CHECK_NULL_ARG_NORET5(RC, ARG1, ARG2, ARG3, ARG4, ARG5) \
    do { \
        CHECK_NULL_ARG_NORET__INTERNAL(RC, ARG1) \
        CHECK_NULL_ARG_NORET__INTERNAL(RC, ARG2) \
        CHECK_NULL_ARG_NORET__INTERNAL(RC, ARG3) \
        CHECK_NULL_ARG_NORET__INTERNAL(RC, ARG4) \
        CHECK_NULL_ARG_NORET__INTERNAL(RC, ARG5) \
    } while(0)


/**
 * Memory allocation check macro with return
 */
#define CHECK_NULL_NOMEM_RETURN(ARG) \
    do { \
        if (NULL == ARG) { \
            SR_LOG_ERR("Unable to allocate memory in %s", __func__); \
            return SR_ERR_NOMEM; \
        } \
    } while(0)

/**
 * Memory allocation check macro with setting error
 */
#define CHECK_NULL_NOMEM_ERROR(ARG, ERROR) \
    do { \
        if (NULL == ARG) { \
            SR_LOG_ERR("Unable to allocate memory in %s", __func__); \
            ERROR = SR_ERR_NOMEM; \
        } \
    } while(0)

/**
 * Memory allocation check macro with goto
 */
#define CHECK_NULL_NOMEM_GOTO(ARG, ERROR, LABEL) \
    do { \
        if (NULL == ARG) { \
            SR_LOG_ERR("Unable to allocate memory in %s", __func__); \
            ERROR = SR_ERR_NOMEM; \
            goto LABEL; \
        } \
    } while(0)


/**
 * Return code check macro with return and no variable arguments.
 */
#define CHECK_RC_MSG_RETURN(RC, MSG) \
    do { \
        if (SR_ERR_OK != RC) { \
            SR_LOG_ERR_MSG(MSG); \
            return RC; \
        } \
    } while(0)

/**
 * Return code check macro with return and variable arguments.
 */
#define CHECK_RC_LOG_RETURN(RC, MSG, ...) \
    do { \
        if (SR_ERR_OK != RC) { \
            SR_LOG_ERR(MSG, __VA_ARGS__); \
            return RC; \
        } \
    } while(0)

/**
 * Return code check macro with goto and no variable arguments.
 */
#define CHECK_RC_MSG_GOTO(RC, LABEL, MSG) \
    do { \
        if (SR_ERR_OK != RC) { \
            SR_LOG_ERR_MSG(MSG); \
            goto LABEL; \
        } \
    } while(0)

/**
 * Return code check macro with goto and variable arguments.
 */
#define CHECK_RC_LOG_GOTO(RC, LABEL, MSG, ...) \
    do { \
        if (SR_ERR_OK != RC) { \
            SR_LOG_ERR(MSG, __VA_ARGS__); \
            goto LABEL; \
        } \
    } while(0)


/**
 * Non-zero value check macro with return and no variable arguments.
 */
#define CHECK_ZERO_MSG_RETURN(RET, ERROR, MSG) \
    do { \
        if (0 != RET) { \
            SR_LOG_ERR_MSG(MSG); \
            return ERROR; \
        } \
    } while(0)

/**
 * Non-zero value check macro with return and variable arguments.
 */
#define CHECK_ZERO_LOG_RETURN(RET, ERROR, MSG, ...) \
    do { \
        if (0 != RET) { \
            SR_LOG_ERR(MSG, __VA_ARGS__); \
            return ERROR; \
        } \
    } while(0)

/**
 * Non-zero value check macro with goto and no variable arguments.
 */
#define CHECK_ZERO_MSG_GOTO(RET, RC, ERROR, LABEL, MSG) \
    do { \
        if (0 != RET) { \
            SR_LOG_ERR_MSG(MSG); \
            RC = ERROR; \
            goto LABEL; \
        } \
    } while(0)

/**
 * Non-zero value check macro with goto and variable arguments.
 */
#define CHECK_ZERO_LOG_GOTO(RET, RC, ERROR, LABEL, MSG, ...) \
    do { \
        if (0 != RET) { \
            SR_LOG_ERR(MSG, __VA_ARGS__); \
            RC = ERROR; \
            goto LABEL; \
        } \
    } while(0)

/**
 * Non-minus value check macro with return and no variable arguments.
 */
#define CHECK_NOT_MINUS1_MSG_RETURN(RET, ERROR, MSG) \
    do { \
        if (-1 == RET) { \
            SR_LOG_ERR_MSG(MSG); \
            return ERROR; \
        } \
    } while(0)

/**
 * Non-minus value check macro with return and variable arguments.
 */
#define CHECK_NOT_MINUS1_LOG_RETURN(RET, ERROR, MSG, ...) \
    do { \
        if (-1 == RET) { \
            SR_LOG_ERR(MSG, __VA_ARGS__); \
            return ERROR; \
        } \
    } while(0)

/**
 * Non-minus value check macro with goto and no variable arguments.
 */
#define CHECK_NOT_MINUS1_MSG_GOTO(RET, RC, ERROR, LABEL, MSG) \
    do { \
        if (-1 == RET) { \
            SR_LOG_ERR_MSG(MSG); \
            RC = ERROR; \
            goto LABEL; \
        } \
    } while(0)

/**
 * Non-minus value check macro with goto and variable arguments.
 */
#define CHECK_NOT_MINUS1_LOG_GOTO(RET, RC, ERROR, LABEL, MSG, ...) \
    do { \
        if (-1 == RET) { \
            SR_LOG_ERR(MSG, __VA_ARGS__); \
            RC = ERROR; \
            goto LABEL; \
        } \
    } while(0)

/**
 * NULL value checker - returns given error code.
 */
#define CHECK_NULL_RETURN(ARG, RC) \
    if (NULL == ARG) { \
        SR_LOG_ERR("NULL value detected for %s in %s", #ARG, __func__); \
        return RC; \
    } \

/**
 * Mutex lock check macro with return
 */
#if defined(HAVE_TIMED_LOCK)
    #define MUTEX_LOCK_TIMED_CHECK_RETURN(MUTEX) \
    do {                                     \
        struct timespec ts = {0};            \
        int ret = 0;                         \
        sr_clock_get_time(CLOCK_REALTIME, &ts);  \
        ts.tv_sec += MUTEX_WAIT_TIME;        \
        ret = pthread_mutex_timedlock(MUTEX, &ts); \
        if (0 != ret) {                            \
            SR_LOG_ERR("Mutex can not be locked %s", sr_strerror_safe(ret));\
            return SR_ERR_TIME_OUT;          \
        }                                    \
    } while(0)
#else
    #define MUTEX_LOCK_TIMED_CHECK_RETURN(MUTEX) pthread_mutex_lock(MUTEX)
#endif

/**
 * Mutex lock check macro with goto
 */
#if defined(HAVE_TIMED_LOCK)
    #define MUTEX_LOCK_TIMED_CHECK_GOTO(MUTEX, RC, LABEL) \
    do {                                     \
        struct timespec ts = {0};            \
        int ret = 0;                         \
        sr_clock_get_time(CLOCK_REALTIME, &ts);  \
        ts.tv_sec += MUTEX_WAIT_TIME;        \
        ret = pthread_mutex_timedlock(MUTEX, &ts); \
        if (0 != ret) {                            \
            SR_LOG_ERR("Mutex can not be locked %s", sr_strerror_safe(ret));\
            rc = SR_ERR_TIME_OUT;            \
            goto LABEL;                      \
        }                                    \
    } while(0)
#else
    #define MUTEX_LOCK_TIMED_CHECK_GOTO(MUTEX, RC, LABEL) pthread_mutex_lock(MUTEX)
#endif

/**
 * Rwlock write lock check macro with return
 */
#if defined(HAVE_TIMED_LOCK)
    #define RWLOCK_WRLOCK_TIMED_CHECK_RETURN(RWLOCK) \
    do {                                     \
        struct timespec ts = {0};            \
        int ret = 0;                         \
        sr_clock_get_time(CLOCK_REALTIME, &ts);  \
        ts.tv_sec += MUTEX_WAIT_TIME;        \
        ret = pthread_rwlock_timedwrlock(RWLOCK, &ts); \
        if (0 != ret) {                            \
            SR_LOG_ERR("rwlock can not be locked %s", sr_strerror_safe(ret));   \
            return SR_ERR_TIME_OUT;          \
        }                                    \
    } while(0)
#else
    #define RWLOCK_WRLOCK_TIMED_CHECK_RETURN(RWLOCK) pthread_rwlock_wrlock(RWLOCK)
#endif

/**
 * Rwlock read check macro with return
 */
#if defined(HAVE_TIMED_LOCK)
    #define RWLOCK_RDLOCK_TIMED_CHECK_RETURN(RWLOCK) \
    do {                                     \
        struct timespec ts = {0};            \
        int ret = 0;                         \
        sr_clock_get_time(CLOCK_REALTIME, &ts);  \
        ts.tv_sec += MUTEX_WAIT_TIME;        \
        ret = pthread_rwlock_timedrdlock(RWLOCK, &ts); \
        if (0 != ret) {                            \
            SR_LOG_ERR("rwlock can not be locked %s", sr_strerror_safe(ret));   \
            return SR_ERR_TIME_OUT;          \
        }                                    \
    } while(0)
#else
    #define RWLOCK_RDLOCK_TIMED_CHECK_RETURN(RWLOCK) pthread_rwlock_rdlock(RWLOCK)
#endif

/**
 * Rwlock write check macro with return
 */
#if defined(HAVE_TIMED_LOCK)
    #define RWLOCK_WRLOCK_TIMED_CHECK_GOTO(RWLOCK, RC, LABEL) \
    do {                                     \
        struct timespec ts = {0};            \
        int ret = 0;                         \
        sr_clock_get_time(CLOCK_REALTIME, &ts);  \
        ts.tv_sec += MUTEX_WAIT_TIME;        \
        ret = pthread_rwlock_timedwrlock(RWLOCK, &ts); \
        if (0 != ret) {                            \
            SR_LOG_ERR("rwlock can not be locked %s", sr_strerror_safe(ret));   \
            rc = SR_ERR_TIME_OUT;            \
            goto LABEL;                      \
        }                                    \
    } while(0)
#else
    #define RWLOCK_WRLOCK_TIMED_CHECK_GOTO(RWLOCK, RC, LABEL) pthread_rwlock_wrlock(RWLOCK)
#endif

/**
 * Rwlock read check macro with goto
 */
#if defined(HAVE_TIMED_LOCK)
    #define RWLOCK_RDLOCK_TIMED_CHECK_GOTO(RWLOCK, RC, LABEL) \
    do {                                     \
        struct timespec ts = {0};            \
        int ret = 0;                         \
        sr_clock_get_time(CLOCK_REALTIME, &ts);  \
        ts.tv_sec += MUTEX_WAIT_TIME;        \
        ret = pthread_rwlock_timedrdlock(RWLOCK, &ts); \
        if (0 != ret) {                            \
            SR_LOG_ERR("rwlock can not be locked %s", sr_strerror_safe(ret));   \
            rc = SR_ERR_TIME_OUT;            \
            goto LABEL;                      \
        }                                    \
    } while(0)
#else
    #define RWLOCK_RDLOCK_TIMED_CHECK_GOTO(RWLOCK, RC, LABEL) pthread_rwlock_rdlock(RWLOCK)
#endif

#endif /* SR_HELPERS_H_ */
