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

#define MIN(x, y) (((x) < (y)) ? (x) : (y))

#ifdef WIN
#define unlink(f) _unlink(f)
#endif

void
dump_ofp_info(ofp_info_t *ofp_info)
{
    if (0 == strcmp(ofp_info->status, "Success")) {
#define L(field) log_msg(#field ": %s", ofp_info->field)
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
        L(sb_path);
        L(time_generated);
    } else {
        log_msg(ofp_info->status);
    }
}

/* super simple xml extractor */
static int
get_element_text(char *xml, int start_ofs, int end_ofs, const char *tag, int *text_start, int *text_end)
{
    char stag[50], etag[50];
    sprintf(stag, "<%s>", tag);
    sprintf(etag, "</%s>", tag);

    char *s = strstr(xml + start_ofs, stag);
    if (NULL == s)
        return 0;

    s += strlen(stag);

    /* don't run over end_ofs */
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
get_element_text(ofp, 0, ofp_len, tag, &out_s, &out_e)

#define EXTRACT(tag, field) \
do { \
    int s, e; \
    if (get_element_text(ofp, out_s, out_e, tag, &s, &e)) { \
        strncpy(ofp_info->field, ofp + s, MIN((int)sizeof(ofp_info->field), e - s)); \
    } \
} while (0)

bool
OfpGetParse(const std::string&& pilot_id, ofp_info_t *ofp_info)
{
    memset(ofp_info, 0, sizeof(*ofp_info));

    std::string url = "https://www.simbrief.com/api/xml.fetcher.php?userid=" + pilot_id;
    // log_msg(url);

    std::string ofp_data;
    ofp_data.reserve(250 * 1024);
    bool res = HttpGet(url, ofp_data, 10);

    if (! res) {
        strcpy(ofp_info->status, "Network error");
        return false;
    }

    int ofp_len = ofp_data.length();
    log_msg("got ofp %d bytes", ofp_len);


    char *ofp = (char *)ofp_data.c_str();
    int out_s, out_e;

    if (POSITION("fetch")) {
        EXTRACT("status", status);
        if (strcmp(ofp_info->status, "Success")) {
            return false;
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
    }

    return true;
}

#ifdef TEST_SB_PARSE
// g++ --std=c++20 -Wall -DIBM=1 -DTEST_SB_PARSE -DLOCAL_DEBUGSTRING -I../SDK/CHeaders/XPLM  -O OfpGetParse.cpp http_get.c log_msg.cpp

#include <ctime>

char sbh_tmp_fn[] = "xml.tmp";
char pilot_id[20];

//
// call with
// sbfetch_test pilot_id
//
int
main(int argc, char** argv)
{
    if (argc < 2) {
        log_msg("missing argument");
        exit(1);
    }

    strncpy(pilot_id, argv[1], sizeof(pilot_id) - 1);

    ofp_info_t ofp_info;
    OfpGetParse(pilot_id, &ofp_info);
    dump_ofp_info(&ofp_info);
    time_t tg = atol(ofp_info.time_generated);
    log_msg("tg %ld", (long)tg);
    struct tm tm;
#ifdef IBM
    gmtime_s(&tm, &tg);
#else
    gmtime_r(&tg, &tm);
#endif
    char line[100];
    sprintf(line, "OFP generated at %4d-%02d-%02d %02d:%02d:%02d UTC",
                   tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday,
                   tm.tm_hour, tm.tm_min, tm.tm_sec);
    log_msg("'%s'", line);

    exit(0);
}
#endif
