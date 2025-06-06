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

#include <string>

#define XPLM200
#define XPLM210
#define XPLM300
#define XPLM301

struct OfpInfo
{
    bool valid{false};
    std::string units;
    std::string status;
    std::string icao_airline;
    std::string flight_number;
    std::string aircraft_icao;
    std::string max_passengers;
    std::string fuel_plan_ramp;
    std::string origin;
    std::string origin_rwy;
    std::string destination;
    std::string alternate;
    std::string destination_rwy;
    std::string ci;
    std::string altitude;
    std::string tropopause;
    std::string isa_dev;
    std::string wind_component;
    std::string oew;
    std::string pax_count;
    std::string freight;
    std::string payload;
    std::string route;
    std::string alt_route;
    std::string sb_path;
    std::string time_generated;
    std::string est_time_enroute;
};

extern bool OfpGetParse(const std::string& pilot_id, OfpInfo& ofp_info);
extern void DumpOfpInfo(const OfpInfo& ofp_info);
extern bool HttpGet(const std::string& url, std::string& data, int timeout);
extern void LogMsg(const char *fmt, ...)  __attribute__ ((format (printf, 1, 2)));

