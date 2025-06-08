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

#include "sbh.h"

static int seqno;

#define MIN(x, y) (((x) < (y)) ? (x) : (y))

#ifdef WIN
#define unlink(f) _unlink(f)
#endif

void
OfpInfo::Dump() const
{
    if (status == "Success") {
#define L(field) LogMsg(#field ": %s", field.c_str())
        L(units);
        L(icao_airline);
        L(flight_number);
        L(aircraft_icao);
        L(origin);
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
    } else
        LogMsg("%s", status.c_str());
#undef L
}

// super simple xml extractor
static int
get_element_text(char *xml, int start_ofs, int end_ofs, const char *tag, int *text_start, int *text_end)
{
    char stag[50], etag[50];
    snprintf(stag, sizeof(stag), "<%s>", tag);
    snprintf(etag, sizeof(etag), "</%s>", tag);

    char *s = strstr(xml + start_ofs, stag);
    if (NULL == s)
        return 0;

    s += strlen(stag);

    // don't run over end_ofs
    int c = xml[end_ofs];
    xml[end_ofs] = '\0';
    char *e = strstr(s, etag);
    xml[end_ofs] = c;

    if (NULL == e)
        return 0;

    *text_start = s - xml;
    *text_end = e - xml;
    return 1;
}

#define POSITION(tag) \
get_element_text(xml, 0, xml_len, tag, &out_s, &out_e)

#define EXTRACT(tag, field) \
do { \
    int s, e; \
    if (get_element_text(xml, out_s, out_e, tag, &s, &e)) { \
        ofp_info->field = std::string(xml + s, e - s); \
    } \
} while (0)

std::unique_ptr<OfpInfo>
OfpGetParse(const std::string& pilot_id)
{
    std::string url = "https://www.simbrief.com/api/xml.fetcher.php?userid=" + pilot_id;
    // LogMsg("%s", url);

    auto ofp_info = std::make_unique<OfpInfo>();

    std::string xml_data;
    xml_data.reserve(250 * 1024);
    bool res = HttpGet(url, xml_data, 10);

    if (! res) {
        ofp_info->status = "Network error";
        ofp_info->stale = true;
        return ofp_info;
    }

    int xml_len = xml_data.length();
    LogMsg("got ofp xml %d bytes", xml_len);

    char *xml = (char *)xml_data.c_str();
    int out_s, out_e;

    if (POSITION("fetch")) {
        EXTRACT("status", status);
        if (ofp_info->status != "Success") {
            return ofp_info;
        }
    }

    if (POSITION("params")) {
        EXTRACT("time_generated", time_generated);
        EXTRACT("units", units);
    }

    if (POSITION("aircraft")) {
        EXTRACT("icaocode", aircraft_icao);
        EXTRACT("max_passengers", max_passengers);
    }

    if (POSITION("fuel")) {
        EXTRACT("plan_ramp", fuel_plan_ramp);
    }

    if (POSITION("origin")) {
        EXTRACT("icao_code", origin);
        EXTRACT("plan_rwy", origin_rwy);
    }

    if (POSITION("destination")) {
        EXTRACT("icao_code", destination);
        EXTRACT("plan_rwy", destination_rwy);
     }

    if (POSITION("general")) {
        EXTRACT("icao_airline", icao_airline);
        EXTRACT("flight_number", flight_number);
        EXTRACT("costindex", ci);
        EXTRACT("initial_altitude", altitude);
        EXTRACT("avg_tropopause", tropopause);
        EXTRACT("avg_wind_comp", wind_component);
        EXTRACT("avg_temp_dev", isa_dev);
        EXTRACT("route", route);
    }

    if (POSITION("alternate")) {
        EXTRACT("icao_code", alternate);
        EXTRACT("route", alt_route);
    }

    if (POSITION("weights")) {
        EXTRACT("oew", oew);
        EXTRACT("pax_count", pax_count);
        EXTRACT("freight_added", freight);
        EXTRACT("payload", payload);
    }

    if (POSITION("times")) {
        EXTRACT("est_time_enroute", est_time_enroute);
        EXTRACT("est_out", est_out);
        EXTRACT("est_off", est_off);
        EXTRACT("est_on", est_on);
        EXTRACT("est_in", est_in);
    }

    ofp_info->stale = false;
    ofp_info->seqno = ++seqno;
    return ofp_info;
}

#ifdef TEST_SB_PARSE
// g++ --std=c++20 -Wall -DIBM=1 -DTEST_SB_PARSE -DLOCAL_DEBUGSTRING -I../SDK/CHeaders/XPLM  -O ofp_get_parse.cpp http_get.c LogMsg.cpp -lwinhttp
// g++ --std=c++20 -Wall -DLIN=1 -DTEST_SB_PARSE -DLOCAL_DEBUGSTRING -I../SDK/CHeaders/XPLM  -O ofp_get_parse.cpp http_get.c LogMsg.cpp -lcurl
#include <ctime>

//
// call with
// a.[out,exe] pilot_id
//
int
main(int argc, char** argv)
{
    if (argc < 2) {
        LogMsg("missing argument");
        exit(1);
    }

    const std::string pilot_id = argv[1];

    auto ofp_info = OfpGetParse(pilot_id);

    ofp_info->Dump();
    time_t tg = atol(ofp_info->time_generated.c_str());
    LogMsg("tg %ld", (long)tg);

    auto tm = *std::gmtime(&tg);
    char line[100];
    snprintf(line, sizeof(line), "OFP generated at %4d-%02d-%02d %02d:%02d:%02d UTC",
                   tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday,
                   tm.tm_hour, tm.tm_min, tm.tm_sec);
    LogMsg("'%s'", line);

    exit(0);
}
#endif
