/**
 * @file get_item_example.cpp
 * @author Mislav Novakovic <mislav.novakovic@sartura.hr>
 * @brief Example usage of get_item method
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

#include <iostream>
#include <memory>

#include "Session.h"

using namespace std;

int
main(int argc, char **argv)
{
    try {
        Logs::set_stderr(SR_LL_NONE);
        Logs::setCallback([](const sr_log_level_t logLevel, const std::string &message) {
            std::cerr << "sysrepo log (" << logLevel << "): " << message << std::endl;
        });

	S_Connection conn(new Connection("app1"));

        S_Session sess(new Session(conn));

        const char *xpath = "/ietf-interfaces:interfaces/interface[name='eth0']/enabled";

        auto value = sess->get_item(xpath);
        if (value == NULL)
            return 0;

        cout << endl << "Value on xpath: " << value->xpath() << " = "\
             << (value->data()->get_bool() ? "true" : "false") << endl << endl;
    } catch( const std::exception& e ) {
        cout << e.what() << endl;
    }

    return 0;
}
