//
//    Simbrief Hub: A central resource of simbrief data for other plugins
//
//    Copyright (C) 2025, 2026 Holger Teutsch
//
//      based on example code from imgui4xp by William Good
//      published under MIT license, see README.html for details
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
#include <vector>
#include <memory>
#include <print>

#include "XPLMDisplay.h"
#include "XPLMProcessing.h"

#include "imgui.h"
#include "imgui_stdlib.h"
#include "ImgWindow.h"

#include "sbh.h"
#include "ui.h"
#include "log_msg.h"
#include "version.h"

static constexpr int kWinWidth = 440;
static constexpr int kWinHeight = 450;
static constexpr int kWinPad = 75;
static constexpr float kFontSize = 12.0f;

std::unique_ptr<ImgWindow> ui;

// Our own class defining the UI
class Ui : public ImgWindow {
    XPLMFlightLoopID flt_id_ = nullptr;
    int ofp_seqno_ = 0;
    std::string out_, off_, tropo_, trip_time_, status_line_;
    ImVec4 field_color_ = ImColor(0.0f, 0.5f, 0.3f, 1.0f);

    // Main function: creates the window's UI
    void BuildInterface() override;

    // flight loop callback for delayed actions prohibited in drawloops
    static float FlightLoopCb(float inElapsedSinceLastCall, float inElapsedTimeSinceLastFlightLoop, int inCounter,
                              void* inRefcon);

   public:
    Ui(int left, int top, int right, int bot);
    ~Ui() override;
};

void CreateUi() {
    int sc_left, sc_top;
    XPLMGetScreenBoundsGlobal(&sc_left, &sc_top, nullptr, nullptr);

    int left = sc_left + kWinPad;
    int right = left + kWinWidth;
    int top = sc_top - kWinPad;
    int bottom = top - kWinHeight;
    ui = std::make_unique<Ui>(left, top, right, bottom);
}

void ImgWindowIni() {
    LogMsg("Initializing Imgui Window...");
    ImgWindow::sFontAtlas = std::make_shared<ImgFontAtlas>();

    // load from X-Plane's default font directory
    if (ImgWindow::sFontAtlas->AddFontFromFileTTF("./Resources/fonts/DejaVuSans.ttf", kFontSize) == nullptr) {
        LogMsg("Failed to load font DejaVuSans from file, falling back to default font");
    }

    LogMsg("Imgui Window initialized");
}

void ImgWindowFini() {
    ui = nullptr;  // just in case ...
    ImgWindow::sFontAtlas.reset();
}

///////////////////////////////////////////////////////////////////////////////////////////
Ui::Ui(int left, int top, int right, int bot)
    : ImgWindow(left, top, right, bot, xplm_WindowDecorationRoundRectangle, xplm_WindowLayerFloatingWindows) {
    // is currently not really supported, hopefully with 12.5
    ImGui::GetIO().IniFilename = nullptr;  // strdup((user_cfg_dir + "imgui.ini").c_str());

    // Create a flight loop id, but don't schedule it yet
    XPLMCreateFlightLoop_t loop_params = {
        sizeof(loop_params),                      // structSize
        xplm_FlightLoop_Phase_BeforeFlightModel,  // phase
        FlightLoopCb,                             // callbackFunc
        (void*)this,                              // refcon
    };

    flt_id_ = XPLMCreateFlightLoop(&loop_params);

    SetWindowTitle("simbrief_hub " VERSION);
    SetWindowResizingLimits(100, 100, 1024, 1024);
    SetVisible(true);
}

Ui::~Ui() {
    if (flt_id_)
        XPLMDestroyFlightLoop(flt_id_);
}

void Ui::BuildInterface() {
    if (ofp_info && ofp_info->seqno > ofp_seqno_) {
        ofp_seqno_ = ofp_info->seqno;

        if (ofp_info->status != kSuccess) {
            status_line_ = ofp_info->status;
        } else {
            time_t tg = atol(ofp_info->time_generated.c_str());
            auto tm = *std::gmtime(&tg);

            status_line_ = std::format("{}{} {} / OFP generated at {:04}-{:02}-{:02} {:02}:{:02}:{:02} UTC, seqno: {}",
                                      ofp_info->icao_airline, ofp_info->flight_number, ofp_info->aircraft_icao,
                                      tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday, tm.tm_hour, tm.tm_min, tm.tm_sec,
                                      ofp_info->seqno);

            time_t out_time = atol(ofp_info->est_out.c_str());
            time_t off_time = atol(ofp_info->est_off.c_str());

            auto out_tm = *std::gmtime(&out_time);
            auto off_tm = *std::gmtime(&off_time);
            char out[20], off[20];
            strftime(out, sizeof(out), "%H:%M", &out_tm);
            strftime(off, sizeof(off), "%H:%M", &off_tm);
            out_ = out;
            off_ = off;

            int tropopause = atoi(ofp_info->tropopause.c_str());
            tropopause = (tropopause + 500) / 1000 * 1000;  // round to nearest 1000
            tropo_ = std::to_string(tropopause);

            if (ofp_info->est_time_enroute[0]) {
                int ttmin = (atoi(ofp_info->est_time_enroute.c_str()) + 30) / 60;
                trip_time_ = std::format("{:02d}{:02d}", ttmin / 60, ttmin % 60);
            } else
                trip_time_ = "<unknown>";
        }
    }

    if (ImGui::TreeNode("Settings")) {
        ImGui::Spacing();
        ImGui::Separator();
        ImGui::AlignTextToFramePadding();
        ImGui::TextUnformatted("Pilot ID:");
        ImGui::SameLine();
        ImGui::InputText("##pilot_id", &pilot_id);
        ImGui::Spacing();
        ImGui::Separator();
        ImGui::TreePop();
    }

    if (pilot_id.empty()) {
        ImGui::TextUnformatted("Pilot ID is not configured!");
        return;
    }

    ImGui::Spacing();

    if (ofp_download_active) {
        ImGui::Spacing();
        ImGui::TextUnformatted("OFP download in progress ...");
        return;
    }

    ImGui::Separator();
    if (ImGui::Button("Fetch OFP")) {
        LogMsg("Fetch OFP button pressed");
        FetchOfp();
    }
    ImGui::Spacing();
    ImGui::Separator();
    ImGui::TextUnformatted(status_line_.c_str());

    //--------------------------------------------------
    ImGui::Spacing();
    ImGui::Separator();

    if (ofp_info && ofp_info->status == kSuccess) {
        float col_0 = ImGui::GetCursorPosX();
        float left_col[2]{
            col_0,
            col_0 + 0.6f * kFontSize * 25.0f,
        };

        float right_col[2]{
            left_col[0] + 0.6f * kFontSize * 12.0f,
            left_col[1] + 0.6f * kFontSize * 12.0f,
        };

        auto DF = [&](int col, const std::string& label, const std::string& value) {
            if (col == 1)
                ImGui::SameLine();
            ImGui::SetCursorPosX(left_col[col]);
            ImGui::TextUnformatted(label.c_str());
            ImGui::SameLine();
            ImGui::SetCursorPosX(right_col[col]);
            ImGui::TextColored(field_color_, "%s", value.c_str());
        };

        auto DF_pm = [&](int col, const std::string& label, std::string value) {
            if (col == 1)
                ImGui::SameLine();
            int ivalue = atoi(value.c_str());
            ImGui::SetCursorPosX(left_col[col]);
            ImGui::TextUnformatted(label.c_str());
            ImGui::SameLine();
            ImGui::SetCursorPosX(right_col[col]);
            if (ivalue < 0)
                ImGui::TextColored(field_color_, "M%03d", -ivalue);
            else
                ImGui::TextColored(field_color_, "P%03d", ivalue);
        };

        auto FormatRoute = [&](std::string_view route, float right_col) {
            ImGui::SameLine();
            if (route.empty()) {
                ImGui::SetCursorPosX(right_col);
                ImGui::TextColored(field_color_, "<empty>");
                return;
            }

            static constexpr size_t kRouteBrk = 50;
            while (route.length() > kRouteBrk) {
                size_t space_pos = route.find_last_of(' ', kRouteBrk);
                if (space_pos == std::string_view::npos) {
                    LogMsg("Can't format route!");
                    break;
                }

                auto fragment = route.substr(0, space_pos);
                ImGui::SetCursorPosX(right_col);
                ImGui::TextColored(field_color_, "%.*s", static_cast<int>(fragment.length()), fragment.data());

                route.remove_prefix(space_pos + 1);
            }

            ImGui::SetCursorPosX(right_col);
            ImGui::TextColored(field_color_, "%.*s", static_cast<int>(route.length()), route.data());
        };

        DF(0, "Pax:", ofp_info->pax_count);
        DF(0, "Cargo:", ofp_info->freight);
        DF(0, "Fuel:", ofp_info->fuel_plan_ramp);

        ImGui::Spacing();
        DF(0, "Out:", out_);
        DF(1, "Off:", off_);
        ImGui::Spacing();
        ImGui::Spacing();

        DF(0, "Departure:", ofp_info->origin + "/" + ofp_info->origin_rwy);
        DF(0, "Destination:", ofp_info->destination + "/" + ofp_info->destination_rwy);
        ImGui::TextUnformatted("Route:");
        FormatRoute(ofp_info->route, right_col[0]);

        DF(0, "Trip Time:", trip_time_);

        DF(0, "CI:", ofp_info->ci);
        DF(1, "TROPO:", tropo_);

        DF(0, "CRZ FL:", ofp_info->altitude);
        DF_pm(1, "ISA:", ofp_info->isa_dev);

        DF_pm(0, "WC:", ofp_info->wind_component);

        ImGui::Spacing();
        DF(0, "Alternate:", ofp_info->alternate);
        ImGui::TextUnformatted("Alt Route:");
        FormatRoute(ofp_info->alt_route, right_col[0]);

        DF(0, "DX Remarks:", ofp_info->dx_rmk);

        ImGui::Spacing();
        ImGui::Spacing();
        ImGui::Separator();
        if (cdm_info) {
            DF(0, "CDM Status:", cdm_info->status);
            DF(0, "CDM Url:", cdm_info->url);

            if (cdm_info->status == kSuccess) {
                DF(0, "TOBT:", cdm_info->tobt);
                DF(1, "TSAT:", cdm_info->tsat);
                DF(0, "CTOT:", cdm_info->ctot);
                DF(0, "Runway:", cdm_info->runway);
                DF(1, "SID:", cdm_info->sid);
            }
        }
    }
}

// Delayed actions that require FlightLoop context
float Ui::FlightLoopCb(float, float, int, void* inRefcon) {
    LogMsg("FlightLoopCb called with inRefcon=%p", inRefcon);

    // Ui& ui = *reinterpret_cast<Ui*>(inRefcon);

    return 0.0f;
}
