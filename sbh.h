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

#include <stdio.h>
#include <stdarg.h>

#define XPLM200
#define XPLM210
#define XPLM300
#define XPLM301

typedef struct _ofp_info
{
    int valid;
    char units[10];
    char status[100];
    char icao_airline[6];
    char flight_number[10];
    char aircraft_icao[10];
    char max_passengers[10];
    char fuel_plan_ramp[10];
    char origin[10];
    char origin_rwy[6];
    char destination[10];
    char alternate[10];
    char destination_rwy[10];
    char ci[10];
    char altitude[10];
    char tropopause[10];
    char isa_dev[10];
    char wind_component[10];
    char oew[10];
    char pax_count[10];
    char freight[10];
    char payload[10];
    char route[1000];
    char alt_route[1000];
    char sb_path[200];
    char time_generated[11];
    char est_time_enroute[11];
} ofp_info_t;

/* tmpfile is unreliable on windows so we use this as filename */
extern char sbh_tmp_fn[];

extern int ofp_get_parse(const char *pilot_id, ofp_info_t *ofp_info);
extern void dump_ofp_info(ofp_info_t *ofp_info);
extern bool HttpGet(const char *url, FILE *f, int *retlen, int timeout);
extern void log_msg(const char *fmt, ...)  __attribute__ ((format (printf, 1, 2)));

