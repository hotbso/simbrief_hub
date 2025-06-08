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
#include <memory>
#include <future>
#include <chrono>

#include "sbh.h"

#include "XPLMPlugin.h"
#include "XPLMPlanes.h"
#include "XPLMDisplay.h"
#include "XPLMGraphics.h"
#include "XPLMDataAccess.h"
#include "XPLMUtilities.h"
#include "XPLMProcessing.h"
#include "XPLMMenus.h"
#include "XPWidgets.h"
#include "XPStandardWidgets.h"

static char xpdir[512];
static const char *psep;

static XPWidgetID getofp_widget, display_widget, getofp_btn, status_line;
static XPWidgetID conf_widget, pilot_id_input, conf_ok_btn;

struct WidgetCtx
{
    XPWidgetID widget;
    int in_vr;          // currently in vr
    int l, t, w, h;     // last geometry before bringing into vr
};

static WidgetCtx getofp_widget_ctx, conf_widget_ctx;

static XPLMDataRef vr_enabled_dr, acf_icao_dr;
static XPLMFlightLoopID flight_loop_id;

static int error_disabled;

static char pref_path[512];
static char pilot_id[20];

// A note on async processing:
// Everything is synchronously fired by the flightloop so we don't need mutexes

// these 2 variables are owned and written by the main (= flightloop) thread
static bool download_active;
static std::unique_ptr<OfpInfo> ofp_info;

// use of this variable is alternate
// If download_active:
//  true:  written by the download thread
//  false: read and written by the main thread
static std::unique_ptr<OfpInfo> ofp_info_new;

// variable under system control
static std::future<bool> download_future;

static void
SavePrefs()
{
    FILE *f = fopen(pref_path, "wb");
    if (NULL == f)
        return;

    fputs(pilot_id, f); putc('\n', f);
    fclose(f);
}


static void
LoadPrefs()
{
    FILE *f  = fopen(pref_path, "rb");
    if (NULL == f)
        return;

    fgets(pilot_id, sizeof(pilot_id), f);
    int len = strlen(pilot_id);
    if ('\n' == pilot_id[len - 1]) pilot_id[len - 1] = '\0';
    fclose(f);
}

static void
ShowWidget(WidgetCtx& ctx)
{
    if (XPIsWidgetVisible(ctx.widget))
        return;

    // force window into visible area of screen
    // we use modern windows under the hut so UI coordinates are in boxels

    int xl, yl, xr, yr;
    XPLMGetScreenBoundsGlobal(&xl, &yr, &xr, &yl);

    ctx.l = (ctx.l + ctx.w < xr) ? ctx.l : xr - ctx.w - 50;
    ctx.l = (ctx.l <= xl) ? 20 : ctx.l;

    ctx.t = (ctx.t + ctx.h < yr) ? ctx.t : (yr - ctx.h - 50);
    ctx.t = (ctx.t >= ctx.h) ? ctx.t : (yr / 2);

    LogMsg("ShowWidget: s: (%d, %d) -> (%d, %d), w: (%d, %d) -> (%d,%d)",
           xl, yl, xr, yr, ctx.l, ctx.t, ctx.l + ctx.w, ctx.t - ctx.h);

    XPSetWidgetGeometry(ctx.widget, ctx.l, ctx.t, ctx.l + ctx.w, ctx.t - ctx.h);
    XPShowWidget(ctx.widget);

    int in_vr = (NULL != vr_enabled_dr) && XPLMGetDatai(vr_enabled_dr);
    if (in_vr) {
        LogMsg("VR mode detected");
        XPLMWindowID window =  XPGetWidgetUnderlyingWindow(ctx.widget);
        XPLMSetWindowPositioningMode(window, xplm_WindowVR, -1);
        ctx.in_vr = 1;
    } else {
        if (ctx.in_vr) {
            LogMsg("widget now out of VR, map at (%d,%d)", ctx.l, ctx.t);
            XPLMWindowID window =  XPGetWidgetUnderlyingWindow(ctx.widget);
            XPLMSetWindowPositioningMode(window, xplm_WindowPositionFree, -1);

            // A resize is necessary so it shows up on the main screen again
            XPSetWidgetGeometry(ctx.widget, ctx.l, ctx.t, ctx.l + ctx.w, ctx.t - ctx.h);
            ctx.in_vr = 0;
        }
    }
}

static int
ConfWidgetCb(XPWidgetMessage msg, XPWidgetID widget_id, intptr_t param1, intptr_t param2)
{
    if (msg == xpMessage_CloseButtonPushed) {
        XPHideWidget(widget_id);
        return 1;
    }

    if (error_disabled)
        return 1;

    if ((widget_id == conf_ok_btn) && (msg == xpMsg_PushButtonPressed)) {
        XPGetWidgetDescriptor(pilot_id_input, pilot_id, sizeof(pilot_id));
        SavePrefs();
        XPHideWidget(conf_widget);
        return 1;
    }

    return 0;
}

//
// Check for download and activate the new ofp
// return true if download is still in progress
bool
CheckAsyncDownload()
{
    if (download_active) {
        if (std::future_status::ready != download_future.wait_for(std::chrono::seconds::zero()))
            return true;

        download_active = false;
        [[maybe_unused]] bool res = download_future.get();
        ofp_info = std::move(ofp_info_new);

        LogMsg("CheckAsyncDownload(): Download status: %s", ofp_info->status.c_str());
        if (ofp_info->status != "Success") {
            XPSetWidgetDescriptor(status_line, ofp_info->status.c_str());
            return false; // no download active
        }

        time_t tg = atol(ofp_info->time_generated.c_str());
        auto tm = *std::gmtime(&tg);

        char line[200];
        snprintf(line, sizeof(line),
                 "%s%s %s / OFP generated at %4d-%02d-%02d %02d:%02d:%02d UTC",
                 ofp_info->icao_airline.c_str(), ofp_info->flight_number.c_str(), ofp_info->aircraft_icao.c_str(),
                 tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday,
                 tm.tm_hour, tm.tm_min, tm.tm_sec);

        XPSetWidgetDescriptor(status_line, line);
        char buffer[100];
        snprintf(buffer, sizeof(buffer), "%d", atoi(ofp_info->altitude.c_str()) / 100);
        ofp_info->altitude = buffer;
    }

    return download_active;
}

static bool
AsyncOfpGetParse()
{
    return OfpGetParse(pilot_id, ofp_info_new);
}

static void
FetchOfp(void)
{
    if (download_active) {
        LogMsg("Download is already in progress, request ignored");
        return;
    }

    download_future = std::async(std::launch::async, AsyncOfpGetParse);
    download_active = true;
    XPLMScheduleFlightLoop(flight_loop_id, 1.0f, 1);
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

static int
GetOfpWidgetCb(XPWidgetMessage msg, XPWidgetID widget_id, intptr_t param1, intptr_t param2)
{
    if (msg == xpMessage_CloseButtonPushed) {
        XPHideWidget(widget_id);
        return 1;
    }

    if (error_disabled)
        return 1;

    if ((widget_id == getofp_btn) && (msg == xpMsg_PushButtonPressed)) {
        XPSetWidgetDescriptor(status_line, "Fetching...");
        FetchOfp();
    }

    // draw the embedded custom widget
    if ((widget_id == display_widget) && (xpMsg_Draw == msg)) {
        if (ofp_info == nullptr)
            return 1;

        static float label_color[] = { 0.0, 0.0, 0.0 };
        static float xfer_color[] = { 0.0, 0.5, 0.0 };
        static float bg_color[] = { 0.0, 0.3, 0.3 };
        char str[80];
        int left, top, right, bottom;

        XPGetWidgetGeometry(display_widget, &left, &top, &right, &bottom);
        // LogMsg("display_widget start %d %d %d %d", left, top, right, bottom);

        int left_col[2] = { left + 5, left + 180 };
        int right_col[2] = { left_col[0] + 75, left_col[1] + 75 };
        int y = top - 5;

#define DL(COL, TXT) \
    if (COL == 0) y -= 15; \
    XPLMDrawString(label_color, left_col[COL], y, (char *)TXT, NULL, xplmFont_Proportional)

#define DX(COL, FIELD) \
    XPLMDrawString(xfer_color, right_col[COL], y, ofp_info->FIELD.c_str(), NULL, xplmFont_Basic)

#define DF(COL, FIELD) \
    XPLMDrawString(bg_color, right_col[COL], y, ofp_info->FIELD.c_str(), NULL, xplmFont_Basic)

#define DS(COL, STR) \
    XPLMDrawString(bg_color, right_col[COL], y, STR, NULL, xplmFont_Basic)

        // D(right_col, oew);
        DL(0, "Pax:"); DX(0, pax_count);
        DL(0, "Cargo:"); DX(0, freight);
        DL(0, "Fuel:"); DX(0, fuel_plan_ramp);
        // D(right_col, payload);

        y -= 30;

        // D(aircraft_icao);
        DL(0, "Departure:"); DS(0, (ofp_info->origin + "/" + ofp_info->origin_rwy).c_str());
        DL(0, "Destination:"); DS(0, (ofp_info->destination + "/" + ofp_info->destination_rwy).c_str());
        DL(0, "Route:");

        y = FormatRoute(bg_color, ofp_info->route, right_col[0], y);

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
        y = FormatRoute(bg_color, ofp_info->alt_route, right_col[0], y);
        y -= 15;

        int pleft, ptop, pright, pbottom;
        XPGetWidgetGeometry(getofp_widget, &pleft, &ptop, &pright, &pbottom);

        if (y != pbottom) {
            XPSetWidgetGeometry(getofp_widget, pleft, ptop, pright, y);
            getofp_widget_ctx.h = ptop - y;

            // widgets are internally managed relative to the left lower corner.
            // Hence if we resize a container we must shift all childs accordingly.
            int delta = y - pbottom;
            int nchild = XPCountChildWidgets(getofp_widget);
            for (int i = 0; i < nchild; i++) {
                int cleft, ctop, cright, cbottom;
                XPWidgetID cw = XPGetNthChildWidget(getofp_widget, i);
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
    if (getofp_widget)
        return;

    int left = 200;
    int top = 800;
    int width = 450;
    int height = 300;

    getofp_widget_ctx.l = left;
    getofp_widget_ctx.t = top;
    getofp_widget_ctx.w = width;
    getofp_widget_ctx.h = height;

    getofp_widget = XPCreateWidget(left, top, left + width, top - height,
                                 0, "Simbrief Hub " VERSION, 1, NULL, xpWidgetClass_MainWindow);
    getofp_widget_ctx.widget = getofp_widget;

    XPSetWidgetProperty(getofp_widget, xpProperty_MainWindowHasCloseBoxes, 1);
    XPAddWidgetCallback(getofp_widget, GetOfpWidgetCb);
    left += 5; top -= 25;

    int left1 = left + 10;
    getofp_btn = XPCreateWidget(left1, top, left1 + 60, top - 30,
                              1, "Fetch OFP", 0, getofp_widget, xpWidgetClass_Button);
    XPAddWidgetCallback(getofp_btn, GetOfpWidgetCb);

    top -= 25;
    status_line = XPCreateWidget(left1, top, left + width - 10, top - 20,
                              1, "", 0, getofp_widget, xpWidgetClass_Caption);

    top -= 20;
    display_widget = XPCreateCustomWidget(left + 10, top, left + width -20, top - height + 10,
                                           1, "", 0, getofp_widget, GetOfpWidgetCb);
}

static void
MenuCb(void *menu_ref, void *item_ref)
{
    // create gui
    if (item_ref == &getofp_widget) {
        ShowWidget(getofp_widget_ctx);
        return;
    }

    if (item_ref == &conf_widget) {
        if (NULL == conf_widget) {
            int left = 250;
            int top = 780;
            int width = 150;
            int height = 100;

            conf_widget_ctx.l = left;
            conf_widget_ctx.t = top;
            conf_widget_ctx.w = width;
            conf_widget_ctx.h = height;

            conf_widget = XPCreateWidget(left, top, left + width, top - height,
                                         0, "SBH / Configuration", 1, NULL, xpWidgetClass_MainWindow);
            conf_widget_ctx.widget = conf_widget;

            XPSetWidgetProperty(conf_widget, xpProperty_MainWindowHasCloseBoxes, 1);
            XPAddWidgetCallback(conf_widget, ConfWidgetCb);
            left += 5; top -= 25;
            XPCreateWidget(left, top, left + width - 2 * 5, top - 15,
                           1, "Pilot Id", 0, conf_widget, xpWidgetClass_Caption);

            int left1 = left + 60;
            pilot_id_input = XPCreateWidget(left1, top, left1 +  50, top - 15,
                                            1, pilot_id, 0, conf_widget, xpWidgetClass_TextField);
            XPSetWidgetProperty(pilot_id_input, xpProperty_TextFieldType, xpTextEntryField);
            XPSetWidgetProperty(pilot_id_input, xpProperty_MaxCharacters, sizeof(pilot_id) -1);

            top -= 30;
            conf_ok_btn = XPCreateWidget(left + 10, top, left + 140, top - 30,
                                      1, "OK", 0, conf_widget, xpWidgetClass_Button);
            XPAddWidgetCallback(conf_ok_btn, ConfWidgetCb);
        }

        XPSetWidgetDescriptor(pilot_id_input, pilot_id);
        ShowWidget(conf_widget_ctx);
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

    if (XPIsWidgetVisible(getofp_widget_ctx.widget)) {
        XPHideWidget(getofp_widget_ctx.widget);
        return 0;
    }

    ShowWidget(getofp_widget_ctx);
    return 0;
}

// flight loop for delayed actions
static float
FlightLoopCb([[maybe_unused]] float inElapsedSinceLastCall,
             [[maybe_unused]] float inElapsedTimeSinceLastFlightLoop, [[maybe_unused]] int inCounter,
             [[maybe_unused]] void *inRefcon)
{
    LogMsg("flight loop");
    if (CheckAsyncDownload())
        return 1.0f;

    return 0; // unschedule
}

// data accessor
// ref = offset of field (std::string) within OfpInfo
static int
DataAcc(XPLMDataRef ref, void *values, int ofs, int n)
{
    if (ofp_info == nullptr)
        return 0;

    if (ofp_info->seqno == 0)    // not even stale data
        return 0;

    std::string *data = static_cast<std::string *>((void *)((char *)ofp_info.get() + (uint64_t)ref));
    int len = data->length();
    if (values == nullptr)
        return len;

    if (n <= 0 || ofs < 0 || ofs >= len)
        return 0;

    n = std::min(n, len - ofs);
    memcpy(values, data->c_str() + ofs, n);
    return n;
}

// int accessor
// ref = pointer to int
static int
IntAcc(XPLMDataRef ref)
{
    if (ofp_info == nullptr)
        return 0;

    int *data = static_cast<int *>((void *)((char *)ofp_info.get() + (uint64_t)ref));
    return *data;
}

/// ------------------------------------------------------ API --------------------------------------------
#define DATA_DREF(f) \
    XPLMRegisterDataAccessor("sbh/" #f, xplmType_Data, 0, NULL, NULL, \
                             NULL, NULL, NULL, NULL, NULL, NULL, \
                             NULL, NULL, DataAcc, NULL, (void *)offsetof(OfpInfo, f), NULL)

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

    psep = XPLMGetDirectorySeparator();
    XPLMGetSystemPath(xpdir);

    // map standard datarefs
    vr_enabled_dr = XPLMFindDataRef("sim/graphics/VR/enabled");
    acf_icao_dr = XPLMFindDataRef("sim/aircraft/view/acf_ICAO");

    // load preferences
    XPLMGetPrefsPath(pref_path);
    XPLMExtractFileAndPath(pref_path);
    strcat(pref_path, psep);
    strcat(pref_path, "simbrief_hub.prf");
    LoadPrefs();

    XPLMMenuID menu = XPLMFindPluginsMenu();
    int sub_menu = XPLMAppendMenuItem(menu, "Simbrief Hub", NULL, 1);
    XPLMMenuID sbh_menu = XPLMCreateMenu("Simbrief Hub", menu, sub_menu, MenuCb, NULL);
    XPLMAppendMenuItem(sbh_menu, "Configure", &conf_widget, 0);
    XPLMAppendMenuItem(sbh_menu, "Show widget", &getofp_widget, 0);

    XPLMCommandRef cmdr = XPLMCreateCommand("sbh/toggle", "Toggle Simbrief Hub widget");
    XPLMRegisterCommandHandler(cmdr, ToggleCmdCb, 0, NULL);

    cmdr = XPLMCreateCommand("sbh/fetch", "Fetch ofp data and show in widget");
    XPLMRegisterCommandHandler(cmdr, FetchCmdCb, 0, NULL);

    XPLMCreateFlightLoop_t create_flight_loop =
        {sizeof(XPLMCreateFlightLoop_t), xplm_FlightLoop_Phase_BeforeFlightModel, FlightLoopCb, NULL};
    flight_loop_id = XPLMCreateFlightLoop(&create_flight_loop);

    DATA_DREF(units);
    DATA_DREF(status);
    DATA_DREF(icao_airline);
    DATA_DREF(flight_number);
    DATA_DREF(aircraft_icao);
    DATA_DREF(max_passengers);
    DATA_DREF(fuel_plan_ramp);
    DATA_DREF(origin);
    DATA_DREF(origin_rwy);
    DATA_DREF(destination);
    DATA_DREF(alternate);
    DATA_DREF(destination_rwy);
    DATA_DREF(ci);
    DATA_DREF(altitude);
    DATA_DREF(tropopause);
    DATA_DREF(isa_dev);
    DATA_DREF(wind_component);
    DATA_DREF(oew);
    DATA_DREF(pax_count);
    DATA_DREF(freight);
    DATA_DREF(payload);
    DATA_DREF(route);
    DATA_DREF(alt_route);
    DATA_DREF(time_generated);
    DATA_DREF(est_time_enroute);
    DATA_DREF(est_out);
    DATA_DREF(est_off);
    DATA_DREF(est_on);
    DATA_DREF(est_in);

    XPLMRegisterDataAccessor("sbh/stale", xplmType_Int, 0, IntAcc, NULL,
                             NULL, NULL, NULL, NULL, NULL, NULL,
                             NULL, NULL, NULL, NULL, (void *)offsetof(OfpInfo, stale), NULL);

    XPLMRegisterDataAccessor("sbh/seqno", xplmType_Int, 0, IntAcc, NULL,
                             NULL, NULL, NULL, NULL, NULL, NULL,
                             NULL, NULL, NULL, NULL, (void *)offsetof(OfpInfo, seqno), NULL);
    CreateWidget();
    return 1;
}
#undef DATA_DREF

PLUGIN_API void
XPluginStop(void)
{
}

PLUGIN_API void
XPluginDisable(void)
{
}

PLUGIN_API int
XPluginEnable(void)
{
    return 1;
}

PLUGIN_API void
XPluginReceiveMessage([[maybe_unused]] XPLMPluginID in_from, long in_msg, void *in_param)
{
    if (in_msg == XPLM_MSG_PLANE_LOADED && in_param == 0) {
        LogMsg("plane loaded");
        //FetchOfp();
    }
}
