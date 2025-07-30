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
#include <fstream>
#include "nlohmann/json.hpp"
using json = nlohmann::json;

#include <vector>
#include "sbh.h"
#include "http_get.h"

// https://github.com/rpuig2001/CDM
// https://github.com/vACDM/vacdm-server

enum CdmProtocol {
    kProtoInvalid,
    kProtoRRuig,
    kProtoVacdmV1
};

// single airport EKCH served by a server
struct Airport {
    std::string icao;
    std::string url;
    CdmProtocol proto;
};

// cdm server
class Server {
    const std::string name_;
    const std::string url_;
    const CdmProtocol proto_;
    bool retrieved_;

   public:
    std::map<const std::string, Airport> airports_;

    Server(std::string name, std::string url, CdmProtocol proto) : name_(name), url_(url), proto_(proto) {
        retrieved_ = false;
    }

    bool RetrieveAirports();
};

static std::vector<Server> servers;

void
CdmInfo::Dump() const
{
    if (status == "Success") {
#define L(field) LogMsg(#field ": %s", field.c_str())
        L(url);
        L(status);
        L(tobt);
        L(tsat);
        L(runway);
        L(sid);
    } else
        LogMsg("%s", status.c_str());
#undef L
}

// Load served airports
bool Server::RetrieveAirports() {
    if (retrieved_)
        return true;

    LogMsg("Loading airports for '%s' url: '%s'", name_.c_str(), url_.c_str());

    std::string api_url;
    if (proto_ == kProtoRRuig)
        api_url = url_ + "/CDM_feeds.json";
    else if (proto_ == kProtoVacdmV1)
        api_url = url_ + "/api/v1/airports";
    else
        std::runtime_error("Oh no, how could that happen: invalid protocol");

    std::string data;
    data.reserve(20 * 1024);
    bool res = HttpGet(api_url, data, 10);

    if (!res) {
        LogMsg("Can't retrieve from '%s'", api_url.c_str());
        return false;
    }

    int len = data.length();
    LogMsg("got data %d bytes", len);

    try {
        json data_obj = json::parse(data);

        if (proto_ == kProtoRRuig) {
            const auto& airport_obj = data_obj.at("airports");
            for (auto const& [icao, url_list] : airport_obj.items()) {
                airports_[icao] = Airport{icao, url_list[0], proto_};
            }
        } else if (proto_ == kProtoVacdmV1) {
            for (auto const& a : data_obj) {
                std::string icao = a.at("icao");
                airports_[icao] = Airport{icao, url_, proto_};
                LogMsg("  '%s'", icao.c_str());
            }
        }
    } catch (const std::exception& e) {
        LogMsg("Invalid airport data: '%s'", e.what());
        return false;
    }

    retrieved_ = true;
    return true;
}

// Find url for an airport
static std::pair<std::string, CdmProtocol>
FindUrl(const std::string& icao)
{
    // unlikely to change, cache successful queries
    static std::string icao_c, url_c;
    static CdmProtocol proto_c;

    if (icao == icao_c)
        return std::make_pair(url_c, proto_c);

    for (auto& s : servers) {
        if (!s.RetrieveAirports())
            continue;
        try {
            const auto& h = s.airports_.at(icao);
            icao_c = icao;
            url_c = h.url;
            proto_c = h.proto;
            return std::make_pair(url_c, proto_c);
        } catch (std::out_of_range& e) {
            // FALLTHROUGH
        }
    }

    return std::make_pair("", kProtoInvalid);
}

bool
CdmInit(const std::string& cfg_path)
{
    std::ifstream f(cfg_path);
    if (f.fail())
        return false;

    try {
        json cfg = json::parse(f);
        //std::cout << cfg.dump(4) << std::endl;

        for (const auto& s : cfg.at("servers")) {
            const std::string name = s.at("name");
            const std::string protocol = s.at("protocol");
            const std::string url = s.at("url");
            LogMsg("realm: '%s', protocol: '%s', url: '%s'", name.c_str(), protocol.c_str(), url.c_str());

            CdmProtocol proto;
            if (protocol == "rruig")
                proto = kProtoRRuig;
            else if (protocol == "vacdm_v1")
                proto = kProtoVacdmV1;
            else {
                LogMsg("Sorry, only 'rruig' or 'vacdm_v1' are currently supported");
                return false;
            }
            servers.push_back(Server(name, url, proto));
        }
    } catch (std::exception& e) {
        LogMsg("Exception: '%s'", e.what());
        return false;
    }
    return true;
}

// extract HHMM from something like "2025-07-28T09:45:06.694Z"
static std::string ExtractHHMM(const std::string& time) {
    if (time == "1969-12-31T23:59:59.999Z" || time.length() < 16)
        return "";

    return time.substr(11, 2) + time.substr(14, 2);
}

// get and parse cdm data for airport/flight
// *** runs in an async ***
bool CdmGetParse(const std::string& arpt_icao, const std::string& callsign, std::unique_ptr<CdmInfo>& cdm_info) {
    cdm_info = std::make_unique<CdmInfo>();

    auto [url, proto] = FindUrl(arpt_icao);
    if (url.empty()) {
        LogMsg("Feed for %s not found", arpt_icao.c_str());
        return false;
    }

    cdm_info->url = url;
    std::string data;
    data.reserve(10 * 1024);

    auto GetData = [&]() -> bool {
        LogMsg("Url for %s: %s", arpt_icao.c_str(), cdm_info->url.c_str());
        bool res = HttpGet(cdm_info->url, data, 10);

        if (!res) {
            LogMsg("Can't retrieve from '%s'", url.c_str());
            return false;
        }

        int len = data.length();
        LogMsg("got flight data %d bytes", len);
        return true;
    };
    switch (proto) {
        case kProtoVacdmV1: {
            cdm_info->url += std::string("/api/v1/pilots/") + callsign;
            if (!GetData())
                return false;

            json flight, vacdm;
            try {
                flight = json::parse(data);
                // LogMsgRaw(flight.dump(4));
                vacdm = flight.at("vacdm");
            } catch (const json::out_of_range& e) {
                LogMsg("flight '%s' not present on '%s'", callsign.c_str(), arpt_icao.c_str());
                return false;
            } catch (const std::exception& e) {
                LogMsg("Exception: '%s'", e.what());
                return false;
            }

            try {
                cdm_info->tobt = ExtractHHMM(vacdm.at("tobt"));
                cdm_info->tsat = ExtractHHMM(vacdm.at("tsat"));
                json clearance = flight.at("clearance");
                cdm_info->runway = clearance.at("dep_rwy");
                cdm_info->sid = clearance.at("sid");
                cdm_info->status = "Success";
                return true;
            } catch (const std::exception& e) {
                LogMsg("Exception: '%s'", e.what());
            }
            break;
        }

        case kProtoRRuig: {
            if (!GetData())
                return false;

            try {
                json flights = json::parse(data).at("flights");
                // LogMsgRaw(flights.dump(4));
                for (const auto& f : flights) {
                    if (f.at("callsign") == callsign) {
                        // LogMsgRaw(f.dump(4));
#define EXTRACT(fn) cdm_info->fn = f.at(#fn)
                        EXTRACT(tobt);
                        EXTRACT(tsat);
                        EXTRACT(runway);
                        EXTRACT(sid);
                        cdm_info->status = "Success";
                        LogMsg("CDM data for flight '%s' retrieved from '%s'", callsign.c_str(), url.c_str());
                        return true;
#undef EXTRACT
                    }
                }
                LogMsg("flight '%s' not present on '%s'", callsign.c_str(), arpt_icao.c_str());
            } catch (const std::exception& e) {
                LogMsg("Exception: '%s'", e.what());
            }
            break;
        }

        default:
            LogMsg("Unsupported protocol");
    }

    cdm_info->status = "not found";
    return false;
}
