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
        ofp_info->time_generated = params.at("time_generated").get<std::string>();
        ofp_info->units = params.at("units").get<std::string>();

        const auto& aircraft = data_obj.at("aircraft");
        ofp_info->aircraft_icao = aircraft.at("icaocode").get<std::string>();
        ofp_info->max_passengers = aircraft.at("max_passengers").get<std::string>();

        const auto& fuel = data_obj.at("fuel");
        ofp_info->fuel_plan_ramp = fuel.at("plan_ramp").get<std::string>();
        ofp_info->fuel_taxi = fuel.at("taxi").get<std::string>();

        const auto& origin = data_obj.at("origin");
        ofp_info->origin = origin.at("icao_code").get<std::string>();
        ofp_info->origin_rwy = origin.at("plan_rwy").get<std::string>();

        const auto& destination = data_obj.at("destination");
        ofp_info->destination = destination.at("icao_code").get<std::string>();
        ofp_info->destination_rwy = destination.at("plan_rwy").get<std::string>();

        const auto& general = data_obj.at("general");
        ofp_info->icao_airline = general.at("icao_airline").get<std::string>();
        ofp_info->flight_number = general.at("flight_number").get<std::string>();
        ofp_info->ci = general.at("costindex").get<std::string>();
        ofp_info->altitude = general.at("initial_altitude").get<std::string>();
        ofp_info->tropopause = general.at("avg_tropopause").get<std::string>();
        ofp_info->wind_component = general.at("avg_wind_comp").get<std::string>();
        ofp_info->isa_dev = general.at("avg_temp_dev").get<std::string>();
        ofp_info->route = general.at("route").get<std::string>();
        ofp_info->sid = general.at("sid_ident").get<std::string>();
        ofp_info->dx_rmk = general.at("dx_rmk").get<std::string>();

        const auto& alternate = data_obj.at("alternate");
        ofp_info->alternate = alternate.at("icao_code").get<std::string>();
        ofp_info->alt_route = alternate.at("route").get<std::string>();

        const auto& weights = data_obj.at("weights");
        ofp_info->oew = weights.at("oew").get<std::string>();
        ofp_info->pax_count = weights.at("pax_count").get<std::string>();
        ofp_info->freight = weights.at("freight_added").get<std::string>();
        ofp_info->payload = weights.at("payload").get<std::string>();
        ofp_info->max_zfw = weights.at("max_zfw").get<std::string>();
        ofp_info->max_tow = weights.at("max_tow").get<std::string>();

        const auto& times = data_obj.at("times");
        ofp_info->est_time_enroute = times.at("est_time_enroute").get<std::string>();
        ofp_info->est_out = times.at("est_out").get<std::string>();
        ofp_info->est_off = times.at("est_off").get<std::string>();
        ofp_info->est_on = times.at("est_on").get<std::string>();
        ofp_info->est_in = times.at("est_in").get<std::string>();
    } catch (const json::out_of_range& e) {
        LogMsg("JSON key not found: '%s'", e.what());
        ofp_info->status = "Invalid JSON data";
        ofp_info->stale = true;
        return false;
    }

    ofp_info->stale = false;
    ofp_info->seqno = ++seqno;
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
