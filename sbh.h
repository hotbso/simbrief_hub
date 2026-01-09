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
#include <memory>

#include "log_msg.h"

static constexpr const char* kSuccess ="Success";

#define F(f) std::string f
struct OfpInfo
{
    int stale{false};   // int!, is accessed by a integer accessor
    int seqno{0};       // incremented after each successfull fetch
    F(units);
    F(status);
    F(icao_airline);
    F(flight_number);
    F(aircraft_icao);
    F(max_passengers);
    F(fuel_plan_ramp);
    F(origin);
    F(origin_rwy);
    F(sid);
    F(destination);
    F(alternate);
    F(destination_rwy);
    F(ci);
    F(altitude);
    F(tropopause);
    F(isa_dev);
    F(wind_component);
    F(oew);
    F(pax_count);
    F(freight);
    F(payload);
    F(route);
    F(alt_route);
    F(time_generated);
    F(est_time_enroute);
    F(est_out);
    F(est_off);
    F(est_on);
    F(est_in);
    F(fuel_taxi);
    F(max_zfw);
    F(max_tow);
    F(dx_rmk);
    void Dump() const;
};

struct CdmInfo
{
    int seqno{0};       // incremented after each successfull fetch
    F(url);
    F(status);
    F(tobt);
    F(tsat);
    F(runway);
    F(sid);
    void Dump() const;
};
#undef F

extern bool OfpGetParse(const std::string& pilot_id, std::unique_ptr<OfpInfo>& ofp_info);
extern bool CdmInit(const std::string& cfg_path);
extern bool CdmGetParse(const std::string& icao, const std::string& callsign, std::unique_ptr<CdmInfo>& Cdm_info);
