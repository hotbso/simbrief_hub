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

// This code loosely follows
// Google's style guide: https://google.github.io/styleguide/cppguide.html

#include <cstdlib>
#include <cstring>
#include <cstdio>
#include <ctime>
#include <fstream>
#include <memory>
#include <future>
#include <chrono>

#include "XPLMPlugin.h"
#include "XPLMGraphics.h"
#include "XPLMDataAccess.h"
#include "XPLMUtilities.h"
#include "XPLMProcessing.h"
#include "XPLMMenus.h"

#include "sbh.h"
#include "ui.h"

#include "version.h"

const char *log_msg_prefix = "sbh: ";

static constexpr float kCdmPollInterval = 90.0f;  // s
static constexpr float kCdmNoPoll = 100000.0f;    // never poll
static constexpr float kAirtimeForArrival = 300.0f;  // s, airtime > this means arrival after a flight

static XPLMMenuID sbh_menu;
static int fake_cdm_item;

static XPLMDataRef acf_icao_dr, total_running_time_sec_dr, num_engines_dr, eng_running_dr, gear_fnrml_dr;
static XPLMDataRef xpilot_status_dr, xpilot_callsign_dr;

static XPLMFlightLoopID flight_loop_id;

static int pref_fake_cdm;

bool error_disabled;

static std::string xp_dir, base_dir, pref_path;
std::string pilot_id;
static std::string cdm_airport, callsign;
static int cdm_seqno;
static bool fake_xpilot;    // faked by env var XPILOT_CALLSIGN=xxxx
static bool xpilot_connected;


static float now, air_time, cdm_next_poll_ts;

// A note on async processing:
// Everything is synchronously fired by the flightloop so we don't need mutexes

// these variables are owned and written by the main (= flightloop) thread
bool ofp_download_active;
static bool cdm_download_active;
std::unique_ptr<OfpInfo> ofp_info;
std::unique_ptr<CdmInfo> cdm_info;

// use of this variable is alternate
// If download_active:
//  true:  written by the download thread
//  false: read and written by the main thread
static std::unique_ptr<OfpInfo> ofp_info_new;
static std::unique_ptr<CdmInfo> cdm_info_new;

// variable under system control
static std::future<bool> ofp_download_future;
static std::future<bool> cdm_download_future;

// forwards
static void FetchCdm(void);

void SavePrefs() {
    std::ofstream f(pref_path);
    if (!f.is_open()) {
        LogMsg("Can't create '%s'", pref_path.c_str());
        return;
    }

    f << pilot_id << " " << pref_fake_cdm << "\n";
}

static void LoadPrefs() {
    std::ifstream f(pref_path);
    if (!f.is_open()) {
        LogMsg("Can't open '%s'", pref_path.c_str());
        return;
    }

    f >> pilot_id >> pref_fake_cdm;
}

// connected to xpilot, engine off, no airtime
bool CdmPollEnabled() {
    if (xpilot_status_dr == nullptr)
        return false;

    if (ofp_download_active)
        return false;

    if (!fake_xpilot) {
        int connected = XPLMGetDatai(xpilot_status_dr);
        if (connected && !xpilot_connected) {
            // catch the transition to connected and retrieve the callsign
            char buffer[20];
            int n = XPLMGetDatab(xpilot_callsign_dr, buffer, 0, sizeof(buffer) - 1);
            buffer[n] = '\0';
            callsign = buffer;
            LogMsg("xpilot is connected: '%s'", callsign.c_str());
        }

        xpilot_connected = connected;
        if (!xpilot_connected)
            return false;
    }

    // check engines
    int er[8];
    int n = 8;
    if (num_engines_dr)
        n = std::min(n, XPLMGetDatai(num_engines_dr));

    n = XPLMGetDatavi(eng_running_dr, er, 0, n);

    for (int i = 0; i < n; i++)
        if (er[i])
            return false;

    if (air_time > kAirtimeForArrival)  // arrival after a flight
        return false;

    return true;
}

// fake cdm info from ofp info
static void FakeCdm() {
    if (ofp_info == nullptr)
        return;

    LogMsg("Faking CDM airport '%s'", ofp_info->origin.c_str());
    time_t out_time = atol(ofp_info->est_out.c_str());
    time_t off_time = atol(ofp_info->est_off.c_str());

    auto out_tm = *std::gmtime(&out_time);
    auto off_tm = *std::gmtime(&off_time);
    char out[20], off[20];
    strftime(out, sizeof(out), "%H%M", &out_tm);
    strftime(off, sizeof(off), "%H%M", &off_tm);

    cdm_info = std::make_unique<CdmInfo>();
    cdm_info->status = kSuccess;
    cdm_info->url = "faked from OFP";
    cdm_info->tobt = out;
    cdm_info->tsat = out;
    cdm_info->ctot = off;
    cdm_info->runway = ofp_info->origin_rwy;
    cdm_info->sid = ofp_info->sid;
    cdm_info->seqno = ++cdm_seqno;
}

//
// Check for download and activate the new ofp
// return true if download is still in progress
bool OfpCheckAsyncDownload() {
    if (ofp_download_active) {
        if (std::future_status::ready != ofp_download_future.wait_for(std::chrono::seconds::zero()))
            return true;

        ofp_download_active = false;
        [[maybe_unused]] bool res = ofp_download_future.get();
        ofp_info = std::move(ofp_info_new);

        LogMsg("OfpCheckAsyncDownload(): Download status: %s", ofp_info->status.c_str());
        if (ofp_info->status != kSuccess) {
            return false;  // no download active
        }

        ofp_info->altitude = std::to_string(atoi(ofp_info->altitude.c_str()) / 100); // -> FL
        cdm_airport = ofp_info->origin;

        if (pref_fake_cdm)
            FakeCdm();          // will be overwritten by real cdm data if available

        cdm_next_poll_ts = now;  // schedule immediate CDM polling after OFP download
        air_time = 0.0f;
    }

    return false;
}

//
// Check for download and activate the new cdm info
// return true if download is still in progress
bool CdmCheckAsyncDownload() {
    if (cdm_download_active) {
        if (std::future_status::ready != cdm_download_future.wait_for(std::chrono::seconds::zero()))
            return true;

        cdm_download_active = false;
        cdm_next_poll_ts = now + kCdmPollInterval;

        [[maybe_unused]] bool res = cdm_download_future.get();

        LogMsg("CdmCheckAsyncDownload(): Download status: %s", cdm_info_new->status.c_str());
        // do not overwrite a fake_cdm with a failed download
        if (pref_fake_cdm && cdm_info_new->status != kSuccess) {
            cdm_info_new = nullptr;  // discard failed real download
            return false;            // no download active
        }

#define F_EQ(f) (cdm_info->f == cdm_info_new->f)
        if (cdm_info && F_EQ(status) && F_EQ(tobt) && F_EQ(tsat) && F_EQ(runway) && F_EQ(sid)) {
            cdm_info_new = nullptr;  // unchanged, discard
            return false;            // no download active
        }
#undef F_EQ

        cdm_info = std::move(cdm_info_new);
        cdm_info->seqno = ++cdm_seqno;
    }

    return false;
}

void FetchOfp(void) {
    if (pilot_id.empty()) {
        LogMsg("pilot_id is not configured!");
        return;
    }

    if (ofp_download_active) {
        LogMsg("Download is already in progress, request ignored");
        return;
    }

    ofp_download_future = std::async(std::launch::async, []() { return OfpGetParse(pilot_id, ofp_info_new); });
    ofp_download_active = true;
}

static void FetchCdm() {
    if (error_disabled)
        return;

    if (cdm_download_active) {
        LogMsg("Download is already in progress, request ignored");
        return;
    }

    if (pilot_id.empty()) {
        LogMsg("pilot_id is not configured!");
        return;
    }

    cdm_download_future =
        std::async(std::launch::async, []() { return CdmGetParse(cdm_airport, callsign, cdm_info_new); });
    cdm_download_active = true;
}

static void MenuCb([[maybe_unused]] void* menu_ref, [[maybe_unused]] void* item_ref) {
    if (error_disabled)
        return;

    if (ui == nullptr) {
        LogMsg("Creating UI");
        CreateUi();
    }
}

// call back for toggle cmd
static int ToggleUiCmdCb([[maybe_unused]] XPLMCommandRef cmdr, XPLMCommandPhase phase, [[maybe_unused]] void* ref) {
    if (error_disabled || xplm_CommandBegin != phase)
        return 0;

    LogMsg("toggle cmd called");

    if (ui)
        ui = nullptr;
    else {
        LogMsg("Creating UI");
        CreateUi();
    }

    return 0;
}

// flight loop for delayed actions
static float FlightLoopCb(float inElapsedSinceLastCall, [[maybe_unused]] float inElapsedTimeSinceLastFlightLoop,
                          [[maybe_unused]] int inCounter, [[maybe_unused]] void* inRefcon) {
    if (error_disabled)
        return 0.0f;

    now = XPLMGetDataf(total_running_time_sec_dr);
    OfpCheckAsyncDownload();
    CdmCheckAsyncDownload();

    if (XPLMGetDataf(gear_fnrml_dr) == 0.0f)
        air_time += inElapsedSinceLastCall;

    bool enab = CdmPollEnabled();
    if (enab)  // limit logging for now
        LogMsg("FlightLoopCB, now: %5.1f, cdm_next_poll_ts: %5.1f, air_time: %5.1f, enab: %d", now, cdm_next_poll_ts,
               air_time, enab);

    if (now > cdm_next_poll_ts && enab) {
        cdm_next_poll_ts = kCdmNoPoll;
        FetchCdm();
    }

    return 5.0f;
}

// Generic data accessor helper returning string data
static int GenericDataAcc(const std::string* data, void* values, int ofs, int n) {
    int len = data->length() + 1;  // we always offer a trailing 0
    if (values == nullptr)
        return len;

    if (n <= 0 || ofs < 0 || ofs >= len)
        return 0;

    n = std::min(n, len - ofs);
    memcpy(values, data->c_str() + ofs, n);
    return n;
}

// data accessor
// ref = offset of field (std::string) within OfpInfo
static int OfpDataAcc(void* ref, void* values, int ofs, int n) {
    if (ofp_info == nullptr || ofp_info->seqno == 0)  // not even stale data
        return 0;

    const std::string* data = reinterpret_cast<const std::string*>((char*)ofp_info.get() + (size_t)ref);
    return GenericDataAcc(data, values, ofs, n);
}

// int accessor
// ref = offset of field (int) within OfpInfo
static int OfpIntAcc(void* ref) {
    if (ofp_info == nullptr)
        return 0;

    int* data = reinterpret_cast<int*>((char*)ofp_info.get() + (size_t)ref);
    return *data;
}

// data accessor
// ref = offset of field (std::string) within CdmInfo
static int CdmDataAcc(void* ref, void* values, int ofs, int n) {
    if (cdm_info == nullptr || cdm_info->seqno == 0)  // not even stale data
        return 0;

    const std::string* data = reinterpret_cast<const std::string*>((char*)cdm_info.get() + (size_t)ref);
    return GenericDataAcc(data, values, ofs, n);
}

// int accessor
// ref = offset of field (int) within OfpInfo
static int CdmIntAcc(void* ref) {
    if (cdm_info == nullptr)
        return 0;

    int* data = reinterpret_cast<int*>((char*)cdm_info.get() + (size_t)ref);
    return *data;
}

/// ------------------------------------------------------ API --------------------------------------------
#define OFP_DATA_DREF(f)                                                                                              \
    XPLMRegisterDataAccessor("sbh/" #f, xplmType_Data, 0, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, \
                             OfpDataAcc, NULL, (void*)offsetof(OfpInfo, f), NULL)
#define CDM_DATA_DREF(f)                                                                                            \
    XPLMRegisterDataAccessor("sbh/cdm/" #f, xplmType_Data, 0, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, \
                             NULL, CdmDataAcc, NULL, (void*)offsetof(CdmInfo, f), NULL)

PLUGIN_API int XPluginStart(char* out_name, char* out_sig, char* out_desc) {
    LogMsg("startup " VERSION);

    // Always use Unix-native paths on the Mac!
    XPLMEnableFeature("XPLM_USE_NATIVE_PATHS", 1);
    XPLMEnableFeature("XPLM_USE_NATIVE_WIDGET_WINDOWS", 1);

    strcpy(out_name, "simbrief_hub " VERSION);
    strcpy(out_sig, "sbh-hotbso");
    strcpy(out_desc, "A central resource of simbrief data for other plugins");

    // set various path
    char buffer[2048];
    XPLMGetSystemPath(buffer);
    xp_dir = std::string(buffer);
    base_dir = xp_dir + "Resources/plugins/simbrief_hub/";
    pref_path = xp_dir + "Output/preferences/simbrief_hub.prf";

    if (!(CdmInit(base_dir + "cdm_cfg.json") || CdmInit(base_dir + "cdm_cfg.default.json"))) {
        LogMsg("Can't find cdm_cfg.json");
        return 0;
    }

    LoadPrefs();

    ImgWindowIni();

    // map standard datarefs
    acf_icao_dr = XPLMFindDataRef("sim/aircraft/view/acf_ICAO");
    gear_fnrml_dr = XPLMFindDataRef("sim/flightmodel/forces/fnrml_gear");
    total_running_time_sec_dr = XPLMFindDataRef("sim/time/total_running_time_sec");
    num_engines_dr = XPLMFindDataRef("sim/aircraft/engine/acf_num_engines");
    eng_running_dr = XPLMFindDataRef("sim/flightmodel/engine/ENGN_running");

    XPLMCommandRef cmdr = XPLMCreateCommand("sbh/toggle", "Toggle Simbrief Hub widget");
    XPLMRegisterCommandHandler(cmdr, ToggleUiCmdCb, 0, NULL);

    cmdr = XPLMCreateCommand("sbh/fetch", "Fetch ofp data and show in widget");
    XPLMRegisterCommandHandler(
        cmdr,
        [](XPLMCommandRef, XPLMCommandPhase phase, void*) -> int {
            if (xplm_CommandBegin != phase)
                return 0;

            LogMsg("fetch cmd called");
            FetchOfp();
            return 0;
        },
        0, NULL);

    XPLMCommandRef fake_cmdr = XPLMCreateCommand("sbh/toggle_fake_cdm", "Toggle fake CDM data");
    XPLMRegisterCommandHandler(
        fake_cmdr,
        [](XPLMCommandRef, XPLMCommandPhase phase, void*) -> int {
            if (xplm_CommandBegin != phase)
                return 0;

            pref_fake_cdm = !pref_fake_cdm;
            LogMsg("pref_fake_cdm set to %d", pref_fake_cdm);
            XPLMCheckMenuItem(sbh_menu, fake_cdm_item, pref_fake_cdm ? xplm_Menu_Checked : xplm_Menu_Unchecked);
            if (pref_fake_cdm)
                FakeCdm();
            else
                cdm_info = nullptr;

            return 0;
        },
        0, NULL);

    // build menu
    XPLMMenuID menu = XPLMFindPluginsMenu();
    int sub_menu = XPLMAppendMenuItem(menu, "Simbrief Hub", NULL, 1);
    sbh_menu = XPLMCreateMenu("Simbrief Hub", menu, sub_menu, MenuCb, NULL);
    XPLMAppendMenuItem(sbh_menu, "Show widget", NULL, 0);
    fake_cdm_item = XPLMAppendMenuItemWithCommand(sbh_menu, "Fake CDM", fake_cmdr);

    XPLMCreateFlightLoop_t create_flight_loop = {sizeof(XPLMCreateFlightLoop_t),
                                                 xplm_FlightLoop_Phase_BeforeFlightModel, FlightLoopCb, NULL};
    flight_loop_id = XPLMCreateFlightLoop(&create_flight_loop);

    // create own datarefs
    // XPLMStart must succeed beyond this point
    OFP_DATA_DREF(units);
    OFP_DATA_DREF(status);
    OFP_DATA_DREF(icao_airline);
    OFP_DATA_DREF(flight_number);
    OFP_DATA_DREF(aircraft_icao);
    OFP_DATA_DREF(max_passengers);
    OFP_DATA_DREF(fuel_plan_ramp);
    OFP_DATA_DREF(origin);
    OFP_DATA_DREF(origin_rwy);
    OFP_DATA_DREF(destination);
    OFP_DATA_DREF(alternate);
    OFP_DATA_DREF(destination_rwy);
    OFP_DATA_DREF(ci);
    OFP_DATA_DREF(altitude);
    OFP_DATA_DREF(tropopause);
    OFP_DATA_DREF(isa_dev);
    OFP_DATA_DREF(wind_component);
    OFP_DATA_DREF(oew);
    OFP_DATA_DREF(pax_count);
    OFP_DATA_DREF(freight);
    OFP_DATA_DREF(payload);
    OFP_DATA_DREF(route);
    OFP_DATA_DREF(alt_route);
    OFP_DATA_DREF(time_generated);
    OFP_DATA_DREF(est_time_enroute);
    OFP_DATA_DREF(est_out);
    OFP_DATA_DREF(est_off);
    OFP_DATA_DREF(est_on);
    OFP_DATA_DREF(est_in);
    OFP_DATA_DREF(fuel_taxi);
    OFP_DATA_DREF(max_zfw);
    OFP_DATA_DREF(max_tow);
    OFP_DATA_DREF(dx_rmk);

    XPLMRegisterDataAccessor("sbh/stale", xplmType_Int, 0, OfpIntAcc, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL,
                             NULL, NULL, NULL, (void*)offsetof(OfpInfo, stale), NULL);

    XPLMRegisterDataAccessor("sbh/seqno", xplmType_Int, 0, OfpIntAcc, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL,
                             NULL, NULL, NULL, (void*)offsetof(OfpInfo, seqno), NULL);

    CDM_DATA_DREF(url);
    CDM_DATA_DREF(status);
    CDM_DATA_DREF(tobt);
    CDM_DATA_DREF(tsat);
    CDM_DATA_DREF(ctot);
    CDM_DATA_DREF(runway);
    CDM_DATA_DREF(sid);

    XPLMRegisterDataAccessor("sbh/cdm/seqno", xplmType_Int, 0, CdmIntAcc, NULL, NULL, NULL, NULL, NULL, NULL, NULL,
                             NULL, NULL, NULL, NULL, (void*)offsetof(CdmInfo, seqno), NULL);

    const char* cs = getenv("XPILOT_CALLSIGN");
    if (cs) {
        fake_xpilot = true;
        callsign = cs;
        LogMsg("fake callsign set to '%s'", callsign.c_str());
    }

    XPLMScheduleFlightLoop(flight_loop_id, 1.0f, 1);
    return 1;
}
#undef OFP_DATA_DREF

PLUGIN_API void XPluginStop(void) {
    // As an async can not be cancelled we have to wait
    // and collect the status. Otherwise X Plane won't shut down.
    while (OfpCheckAsyncDownload() || CdmCheckAsyncDownload()) {
        LogMsg("... waiting for async download to finish");
        std::this_thread::sleep_for(std::chrono::seconds(2));
    }

    ui = nullptr;
    ImgWindowFini();
}

PLUGIN_API void XPluginDisable(void) {
    SavePrefs();
}

PLUGIN_API int XPluginEnable(void) {
    if (error_disabled)
        return 0;

    return 1;
}

PLUGIN_API void XPluginReceiveMessage([[maybe_unused]] XPLMPluginID in_from, long in_msg, void* in_param) {
    if (in_msg == XPLM_MSG_PLANE_LOADED && in_param == 0) {
        LogMsg("plane loaded");

        static bool init_done;
        if (!init_done) {
            init_done = true;
            xpilot_status_dr = XPLMFindDataRef("xpilot/login/status");
            xpilot_callsign_dr = XPLMFindDataRef("xpilot/login/callsign");
            LogMsg("%s", xpilot_status_dr ? "xPilot is installed" : "xPilot is not installed, CDM disabled");
        }

        FetchOfp();
    }
}
