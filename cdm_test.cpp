//
//    Simbrief Hub: A central resource of simbrief data for other plugins
//
//    Copyright (C) 2025 Holger Teutsch
//
//    This library is free software; you can redistribute it and/or
//    modify it under the terms of the GNU Lesser General Public
//    License as published by the Free Software Foundation; either
//    version 2.1 of the License, or (at your option) any later version.
//
//    This library is distributed in the hope that it will be useful,
//    but WITHOUT ANY WARRANTY; without even the implied warranty of
//    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
//    Lesser General Public License for more details.
//
//    You should have received a copy of the GNU Lesser General Public
//    License along with this library; if not, write to the Free Software
//    Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301
//    USA
//

#include <cstdlib>
#include <string>
#include <iostream>

#include "sbh.h"

const char *log_msg_prefix = "cdm_test: ";
static std::unique_ptr<CdmInfo> cdm_info;

static std::string callsign{"EWG74A"};

int
main(int argc, char **argv)
{
    if (argc < 3) {
        LogMsg("missing arguments: airport callsign");
        exit(1);
    }

    if (!CdmInit("cdm_cfg.default.json")) {
        LogMsg("CdmInit() failed, bye!");
        exit(1);
    }

    std::string airport = argv[1];
    std::string callsign = argv[2];

    CdmGetParse(airport, callsign, cdm_info);
    if (cdm_info != nullptr)
        cdm_info->Dump();

    while (true) {
        std::cout << "Enter  airport: ";
        std::cin >> airport;
        std::cout << "Enter callsign: ";
        std::cin >> callsign;
        CdmGetParse(airport, callsign, cdm_info);
        if (cdm_info != nullptr)
            cdm_info->Dump();
    }
    return 0;
}
