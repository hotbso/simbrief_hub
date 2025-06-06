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

static float flight_loop_cb(float unused1, float unused2, int unused3, void *unused4);

static char xpdir[512];
static const char *psep;

#define MSG_GET_OFP (xpMsg_UserStart + 1)
static XPWidgetID getofp_widget, display_widget, getofp_btn, status_line;
static XPWidgetID conf_widget, pilot_id_input, conf_ok_btn;

typedef struct _widget_ctx
{
    XPWidgetID widget;
    int in_vr;          // currently in vr
    int l, t, w, h;     // last geometry before bringing into vr
} widget_ctx_t;

static widget_ctx_t getofp_widget_ctx, conf_widget_ctx;

static OfpInfo ofp_info;

static XPLMDataRef vr_enabled_dr, acf_icao_dr;

static XPLMCreateFlightLoop_t create_flight_loop =
{
    .structSize = sizeof(XPLMCreateFlightLoop_t),
    .phase = xplm_FlightLoop_Phase_BeforeFlightModel,
    .callbackFunc = flight_loop_cb
};
static XPLMFlightLoopID flight_loop_id;

static int error_disabled;

static char pref_path[512];
static char pilot_id[20];
static char msg_line_1[100], msg_line_2[100], msg_line_3[100];

static void
save_pref()
{
    FILE *f = fopen(pref_path, "wb");
    if (NULL == f)
        return;

    fputs(pilot_id, f); putc('\n', f);
    fclose(f);
}


static void
load_pref()
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
show_widget(widget_ctx_t *ctx)
{
    if (XPIsWidgetVisible(ctx->widget))
        return;

    // force window into visible area of screen
    // we use modern windows under the hut so UI coordinates are in boxels

    int xl, yl, xr, yr;
    XPLMGetScreenBoundsGlobal(&xl, &yr, &xr, &yl);

    ctx->l = (ctx->l + ctx->w < xr) ? ctx->l : xr - ctx->w - 50;
    ctx->l = (ctx->l <= xl) ? 20 : ctx->l;

    ctx->t = (ctx->t + ctx->h < yr) ? ctx->t : (yr - ctx->h - 50);
    ctx->t = (ctx->t >= ctx->h) ? ctx->t : (yr / 2);

    LogMsg("show_widget: s: (%d, %d) -> (%d, %d), w: (%d, %d) -> (%d,%d)",
           xl, yl, xr, yr, ctx->l, ctx->t, ctx->l + ctx->w, ctx->t - ctx->h);

    XPSetWidgetGeometry(ctx->widget, ctx->l, ctx->t, ctx->l + ctx->w, ctx->t - ctx->h);
    XPShowWidget(ctx->widget);

    int in_vr = (NULL != vr_enabled_dr) && XPLMGetDatai(vr_enabled_dr);
    if (in_vr) {
        LogMsg("VR mode detected");
        XPLMWindowID window =  XPGetWidgetUnderlyingWindow(ctx->widget);
        XPLMSetWindowPositioningMode(window, xplm_WindowVR, -1);
        ctx->in_vr = 1;
    } else {
        if (ctx->in_vr) {
            LogMsg("widget now out of VR, map at (%d,%d)", ctx->l, ctx->t);
            XPLMWindowID window =  XPGetWidgetUnderlyingWindow(ctx->widget);
            XPLMSetWindowPositioningMode(window, xplm_WindowPositionFree, -1);

            // A resize is necessary so it shows up on the main screen again
            XPSetWidgetGeometry(ctx->widget, ctx->l, ctx->t, ctx->l + ctx->w, ctx->t - ctx->h);
            ctx->in_vr = 0;
        }
    }
}


static int
conf_widget_cb(XPWidgetMessage msg, XPWidgetID widget_id, intptr_t param1, intptr_t param2)
{
    if (msg == xpMessage_CloseButtonPushed) {
        XPHideWidget(widget_id);
        return 1;
    }

    if (error_disabled)
        return 1;

    if ((widget_id == conf_ok_btn) && (msg == xpMsg_PushButtonPressed)) {
        XPGetWidgetDescriptor(pilot_id_input, pilot_id, sizeof(pilot_id));
        save_pref();
        XPHideWidget(conf_widget);
        return 1;
    }

    return 0;
}

// return success == 1
static int
fetch_ofp(void)
{
    msg_line_1[0] = msg_line_2[0] = msg_line_3[0] = '\0';

    ofp_info.valid = false;

    OfpGetParse(pilot_id, ofp_info);
    DumpOfpInfo(ofp_info);

    if (ofp_info.status != "Success") {
        XPSetWidgetDescriptor(status_line, ofp_info.status.c_str());
        return 0; // error
    }

    time_t tg = atol(ofp_info.time_generated.c_str());
    struct tm tm;
#ifdef WINDOWS
    gmtime_s(&tm, &tg);
#else
    gmtime_r(&tg, &tm);
#endif
    char line[200];
    // strftime does not work for whatever reasons
    snprintf(line, sizeof(line),
             "%s%s %s / OFP generated at %4d-%02d-%02d %02d:%02d:%02d UTC",
             ofp_info.icao_airline.c_str(), ofp_info.flight_number.c_str(), ofp_info.aircraft_icao.c_str(),
             tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday,
             tm.tm_hour, tm.tm_min, tm.tm_sec);

    XPSetWidgetDescriptor(status_line, line);
    ofp_info.valid = true;
    char buffer[100];
    snprintf(buffer, sizeof(buffer), "%d", atoi(ofp_info.altitude.c_str()) / 100);
    ofp_info.altitude = buffer;
    return 1;
}

static int
format_route(float *bg_color, const std::string& route, int right_col, int y)
{
    auto wroute = std::make_unique_for_overwrite<char []>(route.length() + 1);
    char *rptr = wroute.get();
    strcpy(rptr, route.c_str());

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
getofp_widget_cb(XPWidgetMessage msg, XPWidgetID widget_id, intptr_t param1, intptr_t param2)
{
    if (msg == xpMessage_CloseButtonPushed) {
        XPHideWidget(widget_id);
        return 1;
    }

    if (error_disabled)
        return 1;

    if ((widget_id == getofp_btn) && (msg == xpMsg_PushButtonPressed)) {
        XPSetWidgetDescriptor(status_line, "Fetching...");
        // Send message to myself to get a draw cycle (draws button as selected)
        XPSendMessageToWidget(status_line, MSG_GET_OFP, xpMode_UpChain, 0, 0);
        return 1;
    }

    // self sent message: fetch OFP (lengthy)
    if ((widget_id == getofp_widget) && (MSG_GET_OFP == msg)) {
        (void)fetch_ofp();
        return 1;
    }

    // draw the embedded custom widget
    if ((widget_id == display_widget) && (xpMsg_Draw == msg)) {
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
    XPLMDrawString(xfer_color, right_col[COL], y, ofp_info.FIELD.c_str(), NULL, xplmFont_Basic)

#define DF(COL, FIELD) \
    XPLMDrawString(bg_color, right_col[COL], y, ofp_info.FIELD.c_str(), NULL, xplmFont_Basic)

#define DS(COL, STR) \
    XPLMDrawString(bg_color, right_col[COL], y, STR, NULL, xplmFont_Basic)

        // D(right_col, oew);
        DL(0, "Pax:"); DX(0, pax_count);
        DL(0, "Cargo:"); DX(0, freight);
        DL(0, "Fuel:"); DX(0, fuel_plan_ramp);
        // D(right_col, payload);

        y -= 30;

        // D(aircraft_icao);
        DL(0, "Departure:"); DS(0, (ofp_info.origin + "/" + ofp_info.origin_rwy).c_str());
        DL(0, "Destination:"); DS(0, (ofp_info.destination + "/" + ofp_info.destination_rwy).c_str());
        DL(0, "Route:");

        y = format_route(bg_color, ofp_info.route.c_str(), right_col[0], y);

        DL(0, "Trip time");
        if (ofp_info.est_time_enroute[0]) {
            int ttmin = (atoi(ofp_info.est_time_enroute.c_str()) + 30) / 60;
            snprintf(str, sizeof(str), "%02d%02d", ttmin / 60, ttmin % 60);
            DS(0, str);
        }

        int tropopause = atoi(ofp_info.tropopause.c_str());
        snprintf(str, sizeof(str), "%d", (tropopause + 500)/1000 * 1000);
        DL(0, "CI:"); DF(0, ci); DL(1, "TROPO:"); DS(1, str);

        int isa_dev = atoi(ofp_info.isa_dev.c_str());
        if (isa_dev < 0)
            snprintf(str, sizeof(str), "M%03d", -isa_dev);
        else
            snprintf(str, sizeof(str), "P%03d", isa_dev);

        DL(0, "CRZ FL:"); DF(0, altitude); DL(1, "ISA:"); DS(1, str);


        int wind_component = atoi(ofp_info.wind_component.c_str());
        if (wind_component < 0)
            snprintf(str, sizeof(str), "M%03d", -wind_component);
        else
            snprintf(str, sizeof(str), "P%03d", wind_component);
        DL(0, "WC:"); DS(0, str);

        y -= 5;

        DL(0, "Alternate:"); DF(0, alternate);
        DL(0, "Alt Route:");
        y = format_route(bg_color, ofp_info.alt_route.c_str(), right_col[0], y);
        y -= 5;

        if (msg_line_1[0]) {
            y -= 15;
            XPLMDrawString(bg_color, left_col[0], y, msg_line_1, NULL, xplmFont_Proportional);
        }

        if (msg_line_2[0]) {
            y -= 15;
            XPLMDrawString(bg_color, left_col[0], y, msg_line_2, NULL, xplmFont_Proportional);
        }

        if (msg_line_3[0]) {
            y -= 15;
            XPLMDrawString(bg_color, left_col[0], y, msg_line_3, NULL, xplmFont_Proportional);
        }

        // adjust height of window
        y -= 10;

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
create_widget()
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
    XPAddWidgetCallback(getofp_widget, getofp_widget_cb);
    left += 5; top -= 25;

    int left1 = left + 10;
    getofp_btn = XPCreateWidget(left1, top, left1 + 60, top - 30,
                              1, "Fetch OFP", 0, getofp_widget, xpWidgetClass_Button);
    XPAddWidgetCallback(getofp_btn, getofp_widget_cb);

    top -= 25;
    status_line = XPCreateWidget(left1, top, left + width - 10, top - 20,
                              1, "", 0, getofp_widget, xpWidgetClass_Caption);

    top -= 20;
    display_widget = XPCreateCustomWidget(left + 10, top, left + width -20, top - height + 10,
                                           1, "", 0, getofp_widget, getofp_widget_cb);
}

static void
menu_cb(void *menu_ref, void *item_ref)
{
    // create gui
    if (item_ref == &getofp_widget) {
        create_widget();
        show_widget(&getofp_widget_ctx);
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
            XPAddWidgetCallback(conf_widget, conf_widget_cb);
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
            XPAddWidgetCallback(conf_ok_btn, conf_widget_cb);
        }

        XPSetWidgetDescriptor(pilot_id_input, pilot_id);
        show_widget(&conf_widget_ctx);
        return;
    }
}

// call back for fetch cmd
static int
fetch_cmd_cb(XPLMCommandRef cmdr, XPLMCommandPhase phase, [[maybe_unused]] void *ref)
{
    if (xplm_CommandBegin != phase)
        return 0;

    LogMsg("fetch cmd called");
    create_widget();
    fetch_ofp();
    show_widget(&getofp_widget_ctx);
    return 0;
}


// call back for toggle cmd
static int
toggle_cmd_cb(XPLMCommandRef cmdr, XPLMCommandPhase phase, [[maybe_unused]] void *ref)
{
    if (xplm_CommandBegin != phase)
        return 0;

    LogMsg("toggle cmd called");
    create_widget();

    if (XPIsWidgetVisible(getofp_widget_ctx.widget)) {
        XPHideWidget(getofp_widget_ctx.widget);
        return 0;
    }

    show_widget(&getofp_widget_ctx);
    return 0;
}

// flight loop for delayed actions
static float
flight_loop_cb([[maybe_unused]] float unused1, [[maybe_unused]] float unused2,
               [[maybe_unused]]int unused3, [[maybe_unused]]void *unused4)
{
    LogMsg("flight loop");
    return 0; // unschedule
}

/// ------------------------------------------------------ API --------------------------------------------
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
    load_pref();

    XPLMMenuID menu = XPLMFindPluginsMenu();
    int sub_menu = XPLMAppendMenuItem(menu, "Simbrief Hub", NULL, 1);
    XPLMMenuID sbh_menu = XPLMCreateMenu("Simbrief Hub", menu, sub_menu, menu_cb, NULL);
    XPLMAppendMenuItem(sbh_menu, "Configure", &conf_widget, 0);
    XPLMAppendMenuItem(sbh_menu, "Show widget", &getofp_widget, 0);

    XPLMCommandRef cmdr = XPLMCreateCommand("sbh/toggle", "Toggle Simbrief Hub widget");
    XPLMRegisterCommandHandler(cmdr, toggle_cmd_cb, 0, NULL);

    cmdr = XPLMCreateCommand("sbh/fetch", "Fetch ofp data and show in widget");
    XPLMRegisterCommandHandler(cmdr, fetch_cmd_cb, 0, NULL);
    flight_loop_id = XPLMCreateFlightLoop(&create_flight_loop);
    return 1;
}


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
    switch (in_msg) {
        case XPLM_MSG_PLANE_LOADED:
            if (in_param == 0) {
                LogMsg("plane loaded");
            }
        break;
    }
}
