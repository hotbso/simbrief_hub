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

#include <cassert>
#include <string>
#include <fstream>

#define JSON_USE_IMPLICIT_CONVERSIONS 0
#include "nlohmann/json.hpp"
using json = nlohmann::json;

#include <vector>
#include <unordered_map>
#include "sbh.h"
#include "http_get.h"

// https://github.com/rpuig2001/CDM
// https://github.com/vACDM/vacdm-server

static constexpr int kMaxRetries = 3;
enum CdmProtocol {
    kProtoInvalid,
    kProtoRPuig,
    kProtoVacdmV1
};

// single airport
struct Airport {
    std::string icao;
    std::string url;
    CdmProtocol proto;
};

// cdm server
class Server {
    const std::string url_;
    const CdmProtocol proto_;
    bool retrieved_{false};
    int retries_left_{kMaxRetries};

   public:
    const std::string name_;

    Server(const Server&) = delete;
    Server& operator=(const Server&) = delete;
    std::unordered_map<std::string, Airport> airports_;

    Server(std::string name, std::string url, CdmProtocol proto)
        : url_(url), proto_(proto), name_(name) {
    }

    bool is_dead() const {
        return retries_left_ <= 0;
    }

    // retrieve airports from this server
    bool RetrieveAirports();
};

static std::vector<std::unique_ptr<Server>> servers;

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

// Get json from url or return null object
json GetJson(const std::string& url) {
    std::string data;
    data.reserve(20 * 1024);
    bool res = HttpGet(url, data, 10);

    if (!res) {
        LogMsg("Can't retrieve from '%s'", url.c_str());
        return json();
    }

    int len = data.length();
    LogMsg("got data %d bytes", len);

    try {
        json data_obj = json::parse(data);
        // LogMsgRaw(data_obj.dump(4));
        return data_obj;
    } catch (const std::exception& e) {
        LogMsg("Invalid json from '%s': %s", url.c_str(), e.what());
    }

    return json();
}

// Load served airports
// Attempts to retrieve the list of airports served by this server.
// Returns true on success, false on failure. Retries up to kMaxRetries times before marking the server as dead.
bool Server::RetrieveAirports() {
    if (retrieved_)
        return true;

    LogMsg("Loading airports for '%s' url: '%s'", name_.c_str(), url_.c_str());

    std::string api_url;
    if (proto_ == kProtoRPuig)
        api_url = url_ + "/CDM_feeds.json";
    else if (proto_ == kProtoVacdmV1)
        api_url = url_ + "/api/v1/airports";
    else
        throw std::runtime_error("Oh no, how could that happen: invalid protocol");

    json data_obj = GetJson(api_url);
    if (data_obj.is_null()) {
        LogMsg("Can't retrieve from '%s', retries left: %d", api_url.c_str(), --retries_left_);
        return false;
    }

    try {
        switch (proto_) {
            case kProtoRPuig: {
                const auto& airport_obj = data_obj.at("airports");
                for (auto const& [icao, url_list] : airport_obj.items()) {
                    airports_[icao] = Airport{icao, url_list[0].get<std::string>(), proto_};
                    LogMsg("  '%s'", icao.c_str());
                }
                break;
            }

            case kProtoVacdmV1: {
                for (auto const& a : data_obj) {
                    auto icao = a.at("icao").get<std::string>();
                    airports_[icao] = Airport{icao, url_, proto_};
                    LogMsg("  '%s'", icao.c_str());
                }
                break;
            }

            default:
                assert(0);
        }
    } catch (const std::exception& e) {
        LogMsg("Invalid airport data: '%s'", e.what());
        return false;
    }

    retrieved_ = true;
    return true;
}

// Find url for an airport
// Returns a pair of (url, protocol) for the given ICAO code.
// If no matching airport is found, returns ("", kProtoInvalid).
static std::pair<std::string, CdmProtocol> FindUrl(const std::string& icao) {
    // unlikely to change, cache successful queries
    static std::string icao_c, url_c;
    static CdmProtocol proto_c;

    if (icao == icao_c)
        return std::make_pair(url_c, proto_c);

    for (auto& s : servers) {
        if (s->is_dead()) {
            LogMsg("Server '%s' is dead, skipping", s->name_.c_str());
            continue;
        }

        if (!s->RetrieveAirports())
            continue;

        auto it = s->airports_.find(icao);
        if (it == s->airports_.end())
            continue;

        const auto& h = it->second;
        icao_c = icao;
        url_c = h.url;
        proto_c = h.proto;
        return std::make_pair(url_c, proto_c);
    }

    return std::make_pair("", kProtoInvalid);
}

bool CdmInit(const std::string& cfg_path) {
    std::ifstream f(cfg_path);
    if (f.fail())
        return false;

    f.seekg(0, std::ios::end);
    size_t size = f.tellg();
    std::string content;
    content.resize(size);
    f.seekg(0);
    f.read(content.data(), size);
    LogMsgRaw(content.c_str());
    auto mm_pos = content.find("#&*!");
    if (mm_pos == std::string::npos) {
        LogMsg("Magic marker not found in '%s'", cfg_path.c_str());
        return false;
    }

    content.erase(0, mm_pos + 4);

    try {
        json cfg = json::parse(content);

        for (const auto& s : cfg.at("servers").get<json::array_t>()) {
            const auto& name = s.at("name").get<std::string>();
            if (!s.at("enabled").get<bool>()) {
                LogMsg("Server '%s' is disabled, skipping", name.c_str());
                continue;
            }
            const auto& protocol = s.at("protocol").get<std::string>();
            const auto& url = s.at("url").get<std::string>();
            LogMsg("server: '%s', protocol: '%s', url: '%s'", name.c_str(), protocol.c_str(), url.c_str());

            CdmProtocol proto;
            if (protocol == "rpuig")
                proto = kProtoRPuig;
            else if (protocol == "vacdm_v1")
                proto = kProtoVacdmV1;
            else {
                LogMsg("Sorry, only 'rpuig' or 'vacdm_v1' are currently supported");
                return false;
            }
            servers.push_back(std::make_unique<Server>(name, url, proto));
        }
    } catch (const std::exception& e) {
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
        cdm_info->status = "Feed for airport not found";
        return false;
    }

    cdm_info->url = url;

    switch (proto) {
        case kProtoVacdmV1: {
            cdm_info->url += std::string("/api/v1/pilots/") + callsign;
            json flight = GetJson(cdm_info->url);
            if (flight.is_null()) {
                cdm_info->status = "Failed to retrieve CDM data";
                return false;
            }

            try {
                const auto& vacdm = flight.at("vacdm").get<json>();
                cdm_info->tobt = ExtractHHMM(vacdm.at("tobt").get<std::string>());
                cdm_info->tsat = ExtractHHMM(vacdm.at("tsat").get<std::string>());
                const auto& clearance = flight.at("clearance").get<json>();
                cdm_info->runway = clearance.at("dep_rwy").get<std::string>();
                cdm_info->sid = clearance.at("sid").get<std::string>();
                cdm_info->status = kSuccess;
                return true;
            } catch (const json::out_of_range& e) {
                LogMsg("JSON key not found: '%s'", e.what());
                cdm_info->status = "Flight not found";
            } catch (const std::exception& e) {
                LogMsg("Exception: '%s'", e.what());
                cdm_info->status = e.what();
            }
            break;
        }

        case kProtoRPuig: {
            json arpt_obj = GetJson(cdm_info->url);
            if (arpt_obj.is_null()) {
                cdm_info->status = "Failed to retrieve CDM data";
                return false;
            }

            try {
                const auto& flights = arpt_obj.at("flights").get<json>();
                // LogMsgRaw(flights.dump(4));
                for (const auto& f : flights) {
                    if (f.at("callsign").get<std::string>() == callsign) {
                        // LogMsgRaw(f.dump(4));
#define EXTRACT(fn) cdm_info->fn = f.at(#fn).get<std::string>()
                        EXTRACT(tobt);
                        EXTRACT(tsat);
                        EXTRACT(runway);
                        EXTRACT(sid);
                        cdm_info->status = kSuccess;
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

    cdm_info->status = "Flight not found";
    return false;
}
