/**
 * @file application_changes_example.cpp
 * @author Mislav Novakovic <mislav.novakovic@sartura.hr>
 * @brief Example application that uses sysrepo as the configuration datastore. It
 * prints the changes made in running data store.
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

#include <unistd.h>
#include <iostream>
#include <signal.h>
#include <stdlib.h>
#include <stdio.h>

#include "Session.h"

#define MAX_LEN 100

using namespace std;

volatile int exit_application = 0;

void
print_value(Values *value)
{
    cout << value->get_xpath();
    cout << " ";

    switch (value->get_type()) {
    case SR_CONTAINER_T:
    case SR_CONTAINER_PRESENCE_T:
        cout << "(container)" << endl;
        break;
    case SR_LIST_T:
        cout << "(list instance)" << endl;
        break;
    case SR_STRING_T:
        cout << "= " << value->get_string() << endl;;
        break;
    case SR_BOOL_T:
	if (value->get_bool())
            cout << "= true" << endl;
	else
            cout << "= false" << endl;
        break;
    case SR_UINT8_T:
        cout << "= " << unsigned(value->get_uint8()) << endl;
        break;
    case SR_UINT16_T:
        cout << "= " << unsigned(value->get_uint16()) << endl;
        break;
    case SR_UINT32_T:
        cout << "= " << unsigned(value->get_uint32()) << endl;
        break;
    case SR_IDENTITYREF_T:
        cout << "= " << value->get_identityref() << endl;
        break;
    default:
        cout << "(unprintable)" << endl;
    }
    return;
}

static void
print_change(sr_change_oper_t op, Values *old_val, Values *new_val) {
    switch(op) {
    case SR_OP_CREATED:
        if (NULL != new_val) {
           printf("CREATED: ");
           print_value(new_val);
        }
        break;
    case SR_OP_DELETED:
        if (NULL != old_val) {
           printf("DELETED: ");
           print_value(old_val);
        }
	break;
    case SR_OP_MODIFIED:
        if (NULL != old_val && NULL != new_val) {
           printf("MODIFIED: ");
           printf("old value");
           print_value(old_val);
           printf("new value");
           print_value(new_val);
        }
	break;
    case SR_OP_MOVED:
        if (NULL != new_val) {
	    cout<<"MOVED: " << new_val->get_xpath() << " after " << old_val->get_xpath() << endl;
        }
	break;
    }
}

static void
print_current_config(Session *session, const char *module_name)
{
    char select_xpath[MAX_LEN];
    try {
        Values values;

        snprintf(select_xpath, MAX_LEN, "/%s:*//*", module_name);

        session->get_items(&select_xpath[0], &values);

        do {
            print_value(&values);
        } while (values.Next());

    } catch( const std::exception& e ) {
        cout << e.what() << endl;
    }
}

static int
module_change_cb(sr_session_ctx_t *session, const char *module_name, sr_notif_event_t event, void *private_ctx)
{
    char change_path[MAX_LEN];
    Values old_value;
    Values new_value;
    Iter_Change it;

    try {
        Session sess(session);

        printf("\n\n ========== CONFIG HAS CHANGED, CURRENT RUNNING CONFIG: ==========\n\n");

        print_current_config(&sess, module_name);

        printf("\n\n ========== CHANGES: =============================================\n\n");

        snprintf(change_path, MAX_LEN, "/%s:*", module_name);

        Subscribe subscribe(&sess);
        subscribe.get_changes_iter(&change_path[0], &it);

	while (true) {
            try {
                sr_change_oper_t oper = subscribe.get_change_next(&it, &old_value, &new_value);
                print_change(oper, &old_value, &new_value);
            } catch( const std::exception& e ) {
                break;
            }
        }
        printf("\n\n ========== END OF CHANGES =======================================\n\n");

    } catch( const std::exception& e ) {
        cout << e.what() << endl;
    }

    return SR_ERR_OK;
}

static void
sigint_handler(int signum)
{
    exit_application = 1;
}

int
main(int argc, char **argv)
{
    const char *module_name = "ietf-interfaces";
    try {
        if (argc > 1) {
            module_name = argv[1];
        } else {
            printf("\nYou can pass the module name to be subscribed as the first argument\n");
        }

        printf("Application will watch for changes in %s\n", module_name);
        /* connect to sysrepo */
        Connection conn("example_application");

        /* start session */
        Session sess(conn);

	/* subscribe for changes in running config */
        Subscribe subscribe(&sess);

	subscribe.module_change_subscribe(module_name, module_change_cb);

        /* read startup config */
        printf("\n\n ========== READING STARTUP CONFIG: ==========\n\n");
        print_current_config(&sess, module_name);

        cout << "\n\n ========== STARTUP CONFIG APPLIED AS RUNNING ==========\n" << endl;

        /* loop until ctrl-c is pressed / SIGINT is received */
        signal(SIGINT, sigint_handler);
        while (!exit_application) {
            sleep(1000);  /* or do some more useful work... */
        }

        printf("Application exit requested, exiting.\n");

    } catch( const std::exception& e ) {
        cout << e.what() << endl;
        return -1;
    }

    return 0;
}
