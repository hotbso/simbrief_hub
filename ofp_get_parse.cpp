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
#include <cstdio>
#include <cstring>
#include <string>

#define JSON_USE_IMPLICIT_CONVERSIONS 0
#include "nlohmann/json.hpp"
using json = nlohmann::json;

#include "sbh.h"
#include "http_get.h"

static int seqno;

void OfpInfo::Dump() const {
    if (status == "Success") {
#define L(field) LogMsg(#field ": %s", field.c_str())
        L(units);
        L(icao_airline);
        L(flight_number);
        L(aircraft_icao);
        L(origin);
        L(origin_rwy);
        L(sid);
        L(destination);
        L(alternate);
        L(ci);
        L(tropopause);
        L(isa_dev);
        L(wind_component);
        L(route);
        L(alt_route);
        L(max_passengers);
        L(fuel_plan_ramp);
        L(oew);
        L(pax_count);
        L(freight);
        L(payload);
        L(est_time_enroute);
        L(time_generated);
        L(est_out);
        L(est_off);
        L(est_on);
        L(est_in);
        L(fuel_taxi);
        L(max_zfw);
        L(max_tow);
        L(dx_rmk);
    } else
        LogMsg("%s", status.c_str());
#undef L
}

// extract string if defined, undefined fields are a null object {}
static void Extract(const json& field, std::string& str) {
    if (field.is_string())
        str = field.get<std::string>();
}

bool OfpGetParse(const std::string& pilot_id, std::unique_ptr<OfpInfo>& ofp_info) {
    std::string url = "https://www.simbrief.com/api/xml.fetcher.php?userid=" + pilot_id + "&json=1";
    // LogMsg("%s", url);

    ofp_info = std::make_unique<OfpInfo>();

    std::string json_str;
    json_str.reserve(300 * 1024);
    bool res = HttpGet(url, json_str, 10);

    if (!res) {
        ofp_info->status = "Network error";
        ofp_info->stale = true;
        return false;
    }

    LogMsg("got ofp json %d bytes", (int)json_str.length());
    json data_obj;
    try {
        data_obj = json::parse(json_str);
        //LogMsgRaw(data_obj.dump(4));
    } catch (const std::exception& e) {
        LogMsg("Invalid json from '%s': %s", url.c_str(), e.what());
        ofp_info->status = "Invalid JSON data";
        ofp_info->stale = true;
        return false;
    }

    // we only use mandatory fields, so exceptions are fatal
    try {
        ofp_info->status = data_obj.at("fetch").at("status").get<std::string>();
        if (ofp_info->status != "Success") {
            ofp_info->stale = true;
            return false;
        }

        const auto& params = data_obj.at("params");
        Extract(params.at("time_generated"), ofp_info->time_generated);
        Extract(params.at("units"), ofp_info->units);

        const auto& aircraft = data_obj.at("aircraft");
        Extract(aircraft.at("icaocode"), ofp_info->aircraft_icao);
        Extract(aircraft.at("max_passengers"), ofp_info->max_passengers);

        const auto& fuel = data_obj.at("fuel");
        Extract(fuel.at("plan_ramp"), ofp_info->fuel_plan_ramp);
        Extract(fuel.at("taxi"), ofp_info->fuel_taxi);
        const auto& origin = data_obj.at("origin");
        Extract(origin.at("icao_code"), ofp_info->origin);
        Extract(origin.at("plan_rwy"), ofp_info->origin_rwy);

        const auto& destination = data_obj.at("destination");
        Extract(destination.at("icao_code"), ofp_info->destination);
        Extract(destination.at("plan_rwy"), ofp_info->destination_rwy);

        const auto& general = data_obj.at("general");
        Extract(general.at("icao_airline"), ofp_info->icao_airline);
        Extract(general.at("flight_number"), ofp_info->flight_number);
        Extract(general.at("costindex"), ofp_info->ci);
        Extract(general.at("initial_altitude"), ofp_info->altitude);
        Extract(general.at("avg_tropopause"), ofp_info->tropopause);
        Extract(general.at("avg_wind_comp"), ofp_info->wind_component);
        Extract(general.at("avg_temp_dev"), ofp_info->isa_dev);
        Extract(general.at("route"), ofp_info->route);
        Extract(general.at("sid_ident"), ofp_info->sid);

        auto const& dx_rmk = general.at("dx_rmk");
        if (dx_rmk.is_string()) {
            ofp_info->dx_rmk = dx_rmk.get<std::string>();
        } else if (dx_rmk.is_array()) {
            // concatenate array entries with space
            for (const auto& item : dx_rmk) {
                if (item.is_string()) {
                    if (!ofp_info->dx_rmk.empty())
                        ofp_info->dx_rmk += " ";
                    ofp_info->dx_rmk += item.get<std::string>();
                }
            }
        }

        const auto& alternate = data_obj.at("alternate");
        Extract(alternate.at("icao_code"), ofp_info->alternate);
        Extract(alternate.at("route"), ofp_info->alt_route);

        const auto& weights = data_obj.at("weights");
        Extract(weights.at("oew"), ofp_info->oew);
        Extract(weights.at("pax_count"), ofp_info->pax_count);
        Extract(weights.at("freight_added"), ofp_info->freight);
        Extract(weights.at("payload"), ofp_info->payload);
        Extract(weights.at("max_zfw"), ofp_info->max_zfw);
        Extract(weights.at("max_tow"), ofp_info->max_tow);

        const auto& times = data_obj.at("times");
        Extract(times.at("est_time_enroute"), ofp_info->est_time_enroute);
        Extract(times.at("est_out"), ofp_info->est_out);
        Extract(times.at("est_off"), ofp_info->est_off);
        Extract(times.at("est_on"), ofp_info->est_on);
        Extract(times.at("est_in"), ofp_info->est_in);
    } catch (const std::exception& e) {
        LogMsg("error during JSON parsing: '%s'", e.what());
        ofp_info->status = "Invalid JSON data";
        ofp_info->stale = true;

        // for debugging, log the received json without userid
        data_obj["fetch"]["userid"] = "xxx";
        data_obj["user_id"] = "xxx";
        LogMsgRaw(data_obj.dump(4));
        return false;
    }

    ofp_info->stale = false;
    ofp_info->seqno = ++seqno;
    LogMsg("OfpGetParse() success, seqno %d", ofp_info->seqno);
    return true;
}

#ifdef TEST_OFP_PARSE
#include <ctime>

const char* log_msg_prefix = "ofp_get_parse_test: ";
//
// call with
// a.[out,exe] pilot_id
//
int main(int argc, char** argv) {
    if (argc < 2) {
        LogMsg("missing argument");
        exit(1);
    }

    const std::string pilot_id = argv[1];
    std::unique_ptr<OfpInfo> ofp_info;
    if (!OfpGetParse(pilot_id, ofp_info)) {
        LogMsg("OfpGetParse() failed");
        exit(1);
    }

    ofp_info->Dump();
    time_t tg = atol(ofp_info->time_generated.c_str());
    LogMsg("tg %ld", (long)tg);

    auto tm = *std::gmtime(&tg);
    char line[100];
    snprintf(line, sizeof(line), "OFP generated at %4d-%02d-%02d %02d:%02d:%02d UTC", tm.tm_year + 1900, tm.tm_mon + 1,
             tm.tm_mday, tm.tm_hour, tm.tm_min, tm.tm_sec);
    LogMsg("'%s'", line);

    exit(0);
}
#endif
