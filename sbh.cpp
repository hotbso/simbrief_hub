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

#include "sbh.h"

#include "XPLMPlugin.h"
#include "XPLMGraphics.h"
#include "XPLMDataAccess.h"
#include "XPLMUtilities.h"
#include "XPLMProcessing.h"
#include "XPLMMenus.h"
#include "XPWidgets.h"
#include "XPStandardWidgets.h"

#include "widget_ctx.h"
#include "version.h"

const char *log_msg_prefix = "sbh: ";

static constexpr float kCdmPollInterval = 90.0f;  // s
static constexpr float kCdmNoPoll = 100000.0f;    // never poll
static constexpr float kAirtimeForArrival = 300.0f;  // s, airtime > this means arrival after a flight

static XPWidgetID main_widget, display_widget, getofp_btn, status_line;
static XPWidgetID conf_widget, pilot_id_input, conf_ok_btn;

static WidgetCtx main_widget_ctx, conf_widget_ctx;

static XPLMDataRef acf_icao_dr, total_running_time_sec_dr, num_engines_dr, eng_running_dr, gear_fnrml_dr;
static XPLMDataRef xpilot_status_dr, xpilot_callsign_dr;

static XPLMFlightLoopID flight_loop_id;

static int error_disabled;

static std::string xp_dir, base_dir, pref_path;
static std::string pilot_id, cdm_airport, callsign;
static int cdm_seqno;
static bool fake_xpilot;    // faked by env var XPILOT_CALLSIGN=xxxx

static float now, air_time, cdm_next_poll_ts;

// A note on async processing:
// Everything is synchronously fired by the flightloop so we don't need mutexes

// these variables are owned and written by the main (= flightloop) thread
static bool ofp_download_active;
static std::unique_ptr<OfpInfo> ofp_info;
static bool cdm_download_active;
static std::unique_ptr<CdmInfo> cdm_info;

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

static void
SavePrefs()
{
    std::ofstream f(pref_path);
    if (!f.is_open()) {
        LogMsg("Can't create '%s'", pref_path.c_str());
        return;
    }

    f << pilot_id << "\n";
}


static void
LoadPrefs()
{
    std::ifstream f(pref_path);
    if (! f.is_open()) {
        LogMsg("Can't open '%s'", pref_path.c_str());
        return;
    }

    std::getline(f, pilot_id);
    if (!pilot_id.empty() && pilot_id.back() == '\r')
        pilot_id.pop_back();
}

static int
ConfWidgetCb(XPWidgetMessage msg, XPWidgetID widget_id, intptr_t param1, intptr_t param2)
{
    if (msg == xpMessage_CloseButtonPushed) {
        conf_widget_ctx.Hide();
        return 1;
    }

    if (error_disabled)
        return 1;

    if ((widget_id == conf_ok_btn) && (msg == xpMsg_PushButtonPressed)) {
        char buffer[40];
        XPGetWidgetDescriptor(pilot_id_input, buffer, sizeof(buffer));
        pilot_id = buffer;
        SavePrefs();
        conf_widget_ctx.Hide();
        return 1;
    }

    return 0;
}

// connected to xpilot, engine off, no airtime
bool CdmPollEnabled() {
    static bool xpilot_connected;

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

//
// Check for download and activate the new ofp
// return true if download is still in progress
bool
OfpCheckAsyncDownload()
{
    if (ofp_download_active) {
        if (std::future_status::ready != ofp_download_future.wait_for(std::chrono::seconds::zero()))
            return true;

        ofp_download_active = false;
        [[maybe_unused]] bool res = ofp_download_future.get();
        ofp_info = std::move(ofp_info_new);

        LogMsg("OfpCheckAsyncDownload(): Download status: %s", ofp_info->status.c_str());
        if (ofp_info->status != "Success") {
            XPSetWidgetDescriptor(status_line, ofp_info->status.c_str());
            return false; // no download active
        }

        time_t tg = atol(ofp_info->time_generated.c_str());
        auto tm = *std::gmtime(&tg);

        char line[200];
        snprintf(line, sizeof(line),
                 "%s%s %s / OFP generated at %4d-%02d-%02d %02d:%02d:%02d UTC",
                 ofp_info->iata_airline.c_str(), ofp_info->flight_number.c_str(), ofp_info->aircraft_icao.c_str(),
                 tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday,
                 tm.tm_hour, tm.tm_min, tm.tm_sec);

        XPSetWidgetDescriptor(status_line, line);
        char buffer[100];
        snprintf(buffer, sizeof(buffer), "%d", atoi(ofp_info->altitude.c_str()) / 100);
        ofp_info->altitude = buffer;
        cdm_airport = ofp_info->origin;

        cdm_next_poll_ts = now;          // schedule immediate CDM polling after OFP download
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
#define F_EQ(f) (cdm_info->f == cdm_info_new->f)
        if (cdm_info &&
            F_EQ(status) && F_EQ(tobt) && F_EQ(tsat) && F_EQ(runway) && F_EQ(sid)) {
            cdm_info_new = nullptr;  // unchanged, discard
            return false;            // no download active
        }
#undef F_EQ

        cdm_info = std::move(cdm_info_new);
        cdm_info->seqno = ++cdm_seqno;
    }

    return false;
}

static void FetchOfp(void) {
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

static int
FormatRoute(float *bg_color, std::string& route, int right_col, int y)
{
    char *rptr = (char *)route.c_str();

    // break route to this # of chars
#define ROUTE_BRK 50
    while (1) {
        int len = strlen(rptr);
        if (len <= ROUTE_BRK)
            break;

        // find last blank < line length
        char c = rptr[ROUTE_BRK];
        rptr[ROUTE_BRK] = '\0';
        char *cptr = strrchr(rptr, ' ');
        rptr[ROUTE_BRK] = c;

        if (NULL == cptr) {
            LogMsg("Can't format route!");
            break;
        }

        // write that fragment
        c = *cptr;
        *cptr = '\0';
        XPLMDrawString(bg_color, right_col, y, rptr, NULL, xplmFont_Basic);
        y -= 15;
        *cptr = c;
        rptr = cptr + 1;    // behind the blank
    }

    XPLMDrawString(bg_color, right_col, y, rptr, NULL, xplmFont_Basic);
    return y;
}

static int MainWidgetCb(XPWidgetMessage msg, XPWidgetID widget_id, intptr_t param1, intptr_t param2) {
    if (msg == xpMessage_CloseButtonPushed) {
        main_widget_ctx.Hide();
        return 1;
    }

    if (error_disabled)
        return 1;

    if ((widget_id == getofp_btn) && (msg == xpMsg_PushButtonPressed)) {
        if (pilot_id.empty())
            return 1;
        XPSetWidgetDescriptor(status_line, "Fetching...");
        FetchOfp();
    }

    // draw the embedded custom widget
    if ((widget_id == display_widget) && (xpMsg_Draw == msg)) {
        if (ofp_info == nullptr)
            return 1;

        static float label_color[] = { 0.0, 0.0, 0.0 };
        static float f_color[] = { 0.0, 0.5, 0.3 };
        char str[80];
        int left, top, right, bottom;

        XPGetWidgetGeometry(display_widget, &left, &top, &right, &bottom);
        // LogMsg("display_widget start %d %d %d %d", left, top, right, bottom);

        int left_col[2] = { left + 5, left + 180 };
        int right_col[2] = { left_col[0] + 75, left_col[1] + 75 };
        int y = top - 5;

#define DL(COL, TXT) \
    if (COL == 0) y -= 15; \
    XPLMDrawString(label_color, left_col[COL], y, TXT, NULL, xplmFont_Proportional)

#define DF(COL, FIELD) \
    XPLMDrawString(f_color, right_col[COL], y, ofp_info->FIELD.c_str(), NULL, xplmFont_Basic)

#define DS(COL, STR) \
    XPLMDrawString(f_color, right_col[COL], y, STR, NULL, xplmFont_Basic)

        DL(0, "Pax:"); DF(0, pax_count);
        DL(0, "Cargo:"); DF(0, freight);
        DL(0, "Fuel:"); DF(0, fuel_plan_ramp);
        y -= 10;

        time_t out_time = atol(ofp_info->est_out.c_str());
        time_t off_time = atol(ofp_info->est_off.c_str());

        auto out_tm = *std::gmtime(&out_time);
        auto off_tm = *std::gmtime(&off_time);
        char out[20], off[20];
        strftime(out, sizeof(out), "%H:%M", &out_tm);
        strftime(off, sizeof(off), "%H:%M", &off_tm);
        std::string tmp_str = std::string("Out: ") + out + "  Off: " + off;
        DL(0, tmp_str.c_str());

        y -= 20;

        // D(aircraft_icao);
        DL(0, "Departure:"); DS(0, (ofp_info->origin + "/" + ofp_info->origin_rwy).c_str());
        DL(0, "Destination:"); DS(0, (ofp_info->destination + "/" + ofp_info->destination_rwy).c_str());
        DL(0, "Route:");

        y = FormatRoute(f_color, ofp_info->route, right_col[0], y);

        DL(0, "Trip time");
        if (ofp_info->est_time_enroute[0]) {
            int ttmin = (atoi(ofp_info->est_time_enroute.c_str()) + 30) / 60;
            snprintf(str, sizeof(str), "%02d%02d", ttmin / 60, ttmin % 60);
            DS(0, str);
        }

        int tropopause = atoi(ofp_info->tropopause.c_str());
        snprintf(str, sizeof(str), "%d", (tropopause + 500)/1000 * 1000);
        DL(0, "CI:"); DF(0, ci); DL(1, "TROPO:"); DS(1, str);

        int isa_dev = atoi(ofp_info->isa_dev.c_str());
        if (isa_dev < 0)
            snprintf(str, sizeof(str), "M%03d", -isa_dev);
        else
            snprintf(str, sizeof(str), "P%03d", isa_dev);

        DL(0, "CRZ FL:"); DF(0, altitude); DL(1, "ISA:"); DS(1, str);


        int wind_component = atoi(ofp_info->wind_component.c_str());
        if (wind_component < 0)
            snprintf(str, sizeof(str), "M%03d", -wind_component);
        else
            snprintf(str, sizeof(str), "P%03d", wind_component);
        DL(0, "WC:"); DS(0, str);

        y -= 5;

        DL(0, "Alternate:"); DF(0, alternate);
        DL(0, "Alt Route:");
        y = FormatRoute(f_color, ofp_info->alt_route, right_col[0], y);

#undef DF
#define DF(COL, FIELD) \
    XPLMDrawString(f_color, right_col[COL], y, cdm_info->FIELD.c_str(), NULL, xplmFont_Basic)

        if (cdm_info != nullptr) {
            y -= 10;
            DL(0, "CDM Status:"); DF(0, status);
            DL(0, "Url:"); DF(0, url);
            if (cdm_info->status == kSuccess) {
                y -= 2;
                DL(0, "TOBT:"); DF(0, tobt);
                DL(1, "TSAT:"); DF(1, tsat);
                DL(0, "Runway:"); DF(0, runway);
                DL(1, "SID:"); DF(1, sid);
            }
        }

        y -= 15;

        int pleft, ptop, pright, pbottom;
        XPGetWidgetGeometry(main_widget, &pleft, &ptop, &pright, &pbottom);

        if (y != pbottom) {
            XPSetWidgetGeometry(main_widget, pleft, ptop, pright, y);
            main_widget_ctx.h = ptop - y;

            // widgets are internally managed relative to the left lower corner.
            // Hence if we resize a container we must shift all childs accordingly.
            int delta = y - pbottom;
            int nchild = XPCountChildWidgets(main_widget);
            for (int i = 0; i < nchild; i++) {
                int cleft, ctop, cright, cbottom;
                XPWidgetID cw = XPGetNthChildWidget(main_widget, i);
                XPGetWidgetGeometry(cw, &cleft, &ctop, &cright, &cbottom);
                XPSetWidgetGeometry(cw, cleft, ctop - delta, cright, cbottom - delta);
            }
        }

		return 1;
	}

    return 0;
}

static void
CreateWidget()
{
    if (main_widget)
        return;

    int left = 200;
    int top = 800;
    int width = 450;
    int height = 300;

    main_widget = XPCreateWidget(left, top, left + width, top - height,
                                 0, "Simbrief Hub " VERSION, 1, NULL, xpWidgetClass_MainWindow);
    main_widget_ctx.Set(main_widget, left, top, width, height);

    XPSetWidgetProperty(main_widget, xpProperty_MainWindowHasCloseBoxes, 1);
    XPAddWidgetCallback(main_widget, MainWidgetCb);
    left += 5; top -= 25;

    int left1 = left + 10;
    getofp_btn = XPCreateWidget(left1, top, left1 + 60, top - 30,
                                1, "Fetch OFP", 0, main_widget, xpWidgetClass_Button);
    XPAddWidgetCallback(getofp_btn, MainWidgetCb);

    top -= 25;
    status_line = XPCreateWidget(left1, top, left + width - 10, top - 20,
                                 1, "", 0, main_widget, xpWidgetClass_Caption);

    top -= 20;
    display_widget = XPCreateCustomWidget(left + 10, top, left + width -20, top - height + 10,
                                          1, "", 0, main_widget, MainWidgetCb);
}

static void
MenuCb(void *menu_ref, void *item_ref)
{
    // create gui
    if (item_ref == &main_widget) {
        main_widget_ctx.Show();
        return;
    }

    if (item_ref == &conf_widget) {
        if (NULL == conf_widget) {
            int left = 250;
            int top = 780;
            int width = 150;
            int height = 100;

            conf_widget = XPCreateWidget(left, top, left + width, top - height,
                                         0, "SBH / Configuration", 1, NULL, xpWidgetClass_MainWindow);
            conf_widget_ctx.Set(conf_widget, left, top, width, height);

            XPSetWidgetProperty(conf_widget, xpProperty_MainWindowHasCloseBoxes, 1);
            XPAddWidgetCallback(conf_widget, ConfWidgetCb);
            left += 5; top -= 25;
            XPCreateWidget(left, top, left + width - 2 * 5, top - 15,
                           1, "Pilot Id", 0, conf_widget, xpWidgetClass_Caption);

            int left1 = left + 60;
            pilot_id_input = XPCreateWidget(left1, top, left1 +  50, top - 15,
                                            1, pilot_id.c_str(), 0, conf_widget, xpWidgetClass_TextField);
            XPSetWidgetProperty(pilot_id_input, xpProperty_TextFieldType, xpTextEntryField);
            XPSetWidgetProperty(pilot_id_input, xpProperty_MaxCharacters, 20);

            top -= 30;
            conf_ok_btn = XPCreateWidget(left + 10, top, left + 140, top - 30,
                                      1, "OK", 0, conf_widget, xpWidgetClass_Button);
            XPAddWidgetCallback(conf_ok_btn, ConfWidgetCb);
        }

        XPSetWidgetDescriptor(pilot_id_input, pilot_id.c_str());
        conf_widget_ctx.Show();
        return;
    }
}

// call back for fetch cmd
static int
FetchCmdCb(XPLMCommandRef cmdr, XPLMCommandPhase phase, [[maybe_unused]] void *ref)
{
    if (xplm_CommandBegin != phase)
        return 0;

    LogMsg("fetch cmd called");
    FetchOfp();
    return 0;
}


// call back for toggle cmd
static int
ToggleCmdCb(XPLMCommandRef cmdr, XPLMCommandPhase phase, [[maybe_unused]] void *ref)
{
    if (xplm_CommandBegin != phase)
        return 0;

    LogMsg("toggle cmd called");

    if (XPIsWidgetVisible(main_widget_ctx.widget)) {
        main_widget_ctx.Hide();
        return 0;
    }

    main_widget_ctx.Show();
    return 0;
}

// flight loop for delayed actions
static float FlightLoopCb(float inElapsedSinceLastCall,
                          [[maybe_unused]] float inElapsedTimeSinceLastFlightLoop, [[maybe_unused]] int inCounter,
                          [[maybe_unused]] void *inRefcon) {
    now = XPLMGetDataf(total_running_time_sec_dr);
    OfpCheckAsyncDownload();
    CdmCheckAsyncDownload();

    if (XPLMGetDataf(gear_fnrml_dr) == 0.0f)
        air_time += inElapsedSinceLastCall;

    bool enab = CdmPollEnabled();
    if (enab)   // limit logging for now
        LogMsg("FlightLoopCB, now: %5.1f, cdm_next_poll_ts: %5.1f, air_time: %5.1f, enab: %d", now, cdm_next_poll_ts, air_time, enab);

    if (now > cdm_next_poll_ts && enab) {
        cdm_next_poll_ts = kCdmNoPoll;
        FetchCdm();
    }

    return 5.0f;
}

// Generic data accessor helper returning string data
static int GenericDataAcc(const std::string *data, void *values, int ofs, int n) {
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
static int OfpDataAcc(XPLMDataRef ref, void *values, int ofs, int n) {
    if (ofp_info == nullptr || ofp_info->seqno == 0)  // not even stale data
        return 0;

    const std::string *data = reinterpret_cast<const std::string *>((char *)ofp_info.get() + (size_t)ref);
    return GenericDataAcc(data, values, ofs, n);
}

// int accessor
// ref = offset of field (int) within OfpInfo
static int OfpIntAcc(XPLMDataRef ref) {
    if (ofp_info == nullptr)
        return 0;

    int *data = reinterpret_cast<int *>((char *)ofp_info.get() + (size_t)ref);
    return *data;
}

// data accessor
// ref = offset of field (std::string) within CdmInfo
static int CdmDataAcc(XPLMDataRef ref, void *values, int ofs, int n) {
    if (cdm_info == nullptr || cdm_info->seqno == 0)  // not even stale data
        return 0;

    const std::string *data = reinterpret_cast<const std::string *>((char *)cdm_info.get() + (size_t)ref);
    return GenericDataAcc(data, values, ofs, n);
}

// int accessor
// ref = offset of field (int) within OfpInfo
static int CdmIntAcc(XPLMDataRef ref) {
    if (cdm_info == nullptr)
        return 0;

    int *data = reinterpret_cast<int *>((char *)cdm_info.get() + (size_t)ref);
    return *data;
}

/// ------------------------------------------------------ API --------------------------------------------
#define OFP_DATA_DREF(f) \
    XPLMRegisterDataAccessor("sbh/" #f, xplmType_Data, 0, NULL, NULL, \
                             NULL, NULL, NULL, NULL, NULL, NULL, \
                             NULL, NULL, OfpDataAcc, NULL, (void *)offsetof(OfpInfo, f), NULL)
#define CDM_DATA_DREF(f) \
    XPLMRegisterDataAccessor("sbh/cdm/" #f, xplmType_Data, 0, NULL, NULL, \
                             NULL, NULL, NULL, NULL, NULL, NULL, \
                             NULL, NULL, CdmDataAcc, NULL, (void *)offsetof(CdmInfo, f), NULL)

PLUGIN_API int
XPluginStart(char *out_name, char *out_sig, char *out_desc)
{
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

    if (! (CdmInit(base_dir + "cdm_cfg.json") || CdmInit(base_dir + "cdm_cfg.default.json"))) {
        LogMsg("Can't find cdm_cfg.json");
        return 0;
    }

    LoadPrefs();

    // map standard datarefs
    acf_icao_dr = XPLMFindDataRef("sim/aircraft/view/acf_ICAO");
    gear_fnrml_dr = XPLMFindDataRef("sim/flightmodel/forces/fnrml_gear");
    total_running_time_sec_dr = XPLMFindDataRef("sim/time/total_running_time_sec");
    num_engines_dr = XPLMFindDataRef("sim/aircraft/engine/acf_num_engines");
    eng_running_dr = XPLMFindDataRef("sim/flightmodel/engine/ENGN_running");

    // build menu
    XPLMMenuID menu = XPLMFindPluginsMenu();
    int sub_menu = XPLMAppendMenuItem(menu, "Simbrief Hub", NULL, 1);
    XPLMMenuID sbh_menu = XPLMCreateMenu("Simbrief Hub", menu, sub_menu, MenuCb, NULL);
    XPLMAppendMenuItem(sbh_menu, "Configure", &conf_widget, 0);
    XPLMAppendMenuItem(sbh_menu, "Show widget", &main_widget, 0);

    XPLMCommandRef cmdr = XPLMCreateCommand("sbh/toggle", "Toggle Simbrief Hub widget");
    XPLMRegisterCommandHandler(cmdr, ToggleCmdCb, 0, NULL);

    cmdr = XPLMCreateCommand("sbh/fetch", "Fetch ofp data and show in widget");
    XPLMRegisterCommandHandler(cmdr, FetchCmdCb, 0, NULL);

    XPLMCreateFlightLoop_t create_flight_loop =
        {sizeof(XPLMCreateFlightLoop_t), xplm_FlightLoop_Phase_BeforeFlightModel, FlightLoopCb, NULL};
    flight_loop_id = XPLMCreateFlightLoop(&create_flight_loop);

    // create own datarefs
    // XPLMStart must succeed beyond this point
    OFP_DATA_DREF(units);
    OFP_DATA_DREF(status);
    OFP_DATA_DREF(iata_airline);
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

    XPLMRegisterDataAccessor("sbh/stale", xplmType_Int, 0, OfpIntAcc, NULL,
                             NULL, NULL, NULL, NULL, NULL, NULL,
                             NULL, NULL, NULL, NULL, (void *)offsetof(OfpInfo, stale), NULL);

    XPLMRegisterDataAccessor("sbh/seqno", xplmType_Int, 0, OfpIntAcc, NULL,
                             NULL, NULL, NULL, NULL, NULL, NULL,
                             NULL, NULL, NULL, NULL, (void *)offsetof(OfpInfo, seqno), NULL);

    CDM_DATA_DREF(url);
    CDM_DATA_DREF(status);
    CDM_DATA_DREF(tobt);
    CDM_DATA_DREF(tsat);
    CDM_DATA_DREF(runway);
    CDM_DATA_DREF(sid);

    XPLMRegisterDataAccessor("sbh/cdm/seqno", xplmType_Int, 0, CdmIntAcc, NULL,
                             NULL, NULL, NULL, NULL, NULL, NULL,
                             NULL, NULL, NULL, NULL, (void *)offsetof(CdmInfo, seqno), NULL);

    CreateWidget();
    if (pilot_id.empty()) {
        XPSetWidgetDescriptor(status_line, "Pilot ID is not configured!");
    }

    const char *cs = getenv("XPILOT_CALLSIGN");
    if (cs) {
        fake_xpilot = true;
        callsign = cs;
        LogMsg("fake callsign set to '%s'", callsign.c_str());
    }

    XPLMScheduleFlightLoop(flight_loop_id, 1.0f, 1);
    return 1;
}
#undef OFP_DATA_DREF

PLUGIN_API void
XPluginStop(void)
{
    // As an async can not be cancelled we have to wait
    // and collect the status. Otherwise X Plane won't shut down.
    while (OfpCheckAsyncDownload() || CdmCheckAsyncDownload()) {
        LogMsg("... waiting for async download to finish");
        std::this_thread::sleep_for(std::chrono::seconds(2));
    }
}

PLUGIN_API void
XPluginDisable(void)
{
}

PLUGIN_API int XPluginEnable(void) {
    return 1;
}

PLUGIN_API void XPluginReceiveMessage([[maybe_unused]] XPLMPluginID in_from, long in_msg, void *in_param) {
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
