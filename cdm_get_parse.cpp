//
//    Simbrief Hub: A central resource of simbrief data for other plugins
//
//    Copyright (C) 2025, 2026 Holger Teutsch
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
// https://viff-system.network/docs
// https://github.com/vACDM/vacdm-server

static constexpr int kMaxRetries = 3;

// cdm server abstract base class
class CdmServer {
   protected:
    const std::string name_;
    const std::string url_;
    int retries_left_{kMaxRetries};

   public:
    CdmServer(const CdmServer&) = delete;
    CdmServer& operator=(const CdmServer&) = delete;
    CdmServer(const std::string &name, const std::string& url) : name_(name), url_(url) {}
    virtual ~CdmServer() = default;

    const std::string& name() const {
        return name_;
    }

    bool is_dead() const{
        return retries_left_ <= 0;
    }

    virtual bool CdmGetParse(const std::string& arpt_icao, const std::string& callsign, CdmInfo& cdm_info) = 0;
};

static std::vector<std::unique_ptr<CdmServer>> cdm_servers;

// simple cache for the last successful request as the same flight is likely to be requested again and again
static struct Cache {
    std::string arpt_icao;
    std::string callsign;
    int idx;
} cache;


// cdm server for R. Puig's CDM legacy protocol
class CdmServer_rpuig: public CdmServer {
    bool retrieved_{false};

    // icao -> url
    std::unordered_map<std::string, std::string> arpt_urls_;

    // retrieve airports from this server
    bool RetrieveAirports();

   public:
    CdmServer_rpuig(const CdmServer_rpuig&) = delete;
    CdmServer_rpuig& operator=(const CdmServer_rpuig&) = delete;

    CdmServer_rpuig(const std::string& name, const std::string& url) : CdmServer(name, url) {}

    bool CdmGetParse(const std::string& arpt_icao, const std::string& callsign, CdmInfo& cdm_info) override;
};

// cdm server for R. Puig's vIFF system
class CdmServer_viff: public CdmServer {
   public:
    CdmServer_viff(const CdmServer_viff&) = delete;
    CdmServer_viff& operator=(const CdmServer_viff&) = delete;

    CdmServer_viff(const std::string& name, const std::string& url) : CdmServer(name, url) {}

    bool CdmGetParse(const std::string& arpt_icao, const std::string& callsign, CdmInfo& cdm_info) override;
};

// cdm server for the vacdm legacy protocol
class CdmServer_vacdm: public CdmServer {
    bool retrieved_{false};

    // icao -> url
    std::unordered_map<std::string, std::string> arpt_urls_;

    // retrieve airports from this server
    bool RetrieveAirports();

   public:
    CdmServer_vacdm(const CdmServer_vacdm&) = delete;
    CdmServer_vacdm& operator=(const CdmServer_vacdm&) = delete;

    CdmServer_vacdm(const std::string& name, const std::string& url) : CdmServer(name, url) {}

    bool CdmGetParse(const std::string& arpt_icao, const std::string& callsign, CdmInfo& cdm_info) override;
};

//
// some helpers
//
void CdmInfo::Dump() const {
#define L(field) LogMsg(#field ": %s", field.c_str())
    L(status);
    L(url);

    if (status == kSuccess) {
        L(tobt);
        L(tsat);
        L(runway);
        L(sid);
    }
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
    if (len == 0) {
        LogMsg("Empty response from '%s'", url.c_str());
        return json();
    }

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

// extract HHMM from something like "2025-07-28T09:45:06.694Z"
static std::string ExtractHHMM(const std::string& time) {
    if (time == "1969-12-31T23:59:59.999Z" || time.length() < 16)
        return "";

    return time.substr(11, 2) + time.substr(14, 2);
}

//
// RPuig implementation
//

// Load served airports
// Attempts to retrieve the list of airports served by this server.
// Returns true on success, false on failure. Retries up to kMaxRetries times before marking the server as dead.
bool CdmServer_rpuig::RetrieveAirports() {
    if (retrieved_)
        return true;

    LogMsg("Loading airports for '%s' url: '%s'", name_.c_str(), url_.c_str());

    const std::string api_url = url_ + "/CDM_feeds.json";

    json data_obj = GetJson(api_url);
    if (data_obj.is_null()) {
        LogMsg("Can't retrieve from '%s', retries left: %d", api_url.c_str(), --retries_left_);
        return false;
    }

    try {
        const auto& airport_obj = data_obj.at("airports");
        for (auto const& [icao, url_list] : airport_obj.items()) {
            arpt_urls_[icao] = url_list[0].get<std::string>();
            LogMsg("  '%s'", icao.c_str());
        }
    } catch (const std::exception& e) {
        LogMsg("Invalid airport data: '%s'", e.what());
        return false;
    }

    retrieved_ = true;
    return true;
}

// get and parse cdm data for airport/flight
bool CdmServer_rpuig::CdmGetParse(const std::string& arpt_icao, const std::string& callsign, CdmInfo& cdm_info) {
    if (is_dead())
        return false;

    if (!RetrieveAirports())
        return false;

    const auto it = arpt_urls_.find(arpt_icao);
    if (it == arpt_urls_.end())
        return false;

    cdm_info.url = it->second;

    json arpt_obj = GetJson(cdm_info.url);
    if (arpt_obj.is_null()) {
        cdm_info.status = "Failed to retrieve CDM data";
        return false;
    }

    try {
        const auto& flights = arpt_obj.at("flights").get<json>();
        // LogMsgRaw(flights.dump(4));
        for (const auto& f : flights) {
            if (f.at("callsign").get<std::string>() == callsign) {
                // LogMsgRaw(f.dump(4));
#define EXTRACT(fn) cdm_info.fn = f.at(#fn).get<std::string>()
                EXTRACT(tobt);
                EXTRACT(tsat);
                EXTRACT(runway);
                EXTRACT(sid);
                cdm_info.status = kSuccess;
                LogMsg("CDM data for flight '%s' retrieved from '%s'", callsign.c_str(), cdm_info.url.c_str());
                return true;
#undef EXTRACT
            }
        }
        LogMsg("flight '%s' not present on '%s'", callsign.c_str(), arpt_icao.c_str());
    } catch (const std::exception& e) {
        LogMsg("Exception: '%s'", e.what());
    }

    cdm_info.status = "Flight not found";
    return false;
}

//
// vIFF implementation
//
// get and parse cdm data for airport/flight
bool CdmServer_viff::CdmGetParse(const std::string& arpt_icao, const std::string& callsign, CdmInfo& cdm_info) {
    if (is_dead())
        return false;

    cdm_info.url = url_ + "/ifps/callsign?callsign=" + callsign;

    json flight_obj = GetJson(cdm_info.url);
    if (flight_obj.is_null()) {
        cdm_info.status = "Failed to retrieve CDM data";
        LogMsg("flight '%s' not present on vIFF server'%s'", callsign.c_str(), name().c_str());
        return false;
    }

    try {
        const auto& dep = flight_obj.at("departure").get<std::string>();
        if (dep != arpt_icao) {
            cdm_info.status = "Flight not departing from this airport";
            LogMsg("flight '%s' departs from '%s', not from '%s'", callsign.c_str(), dep.c_str(), arpt_icao.c_str());
            return false;
        }

        const auto& cdm_obj = flight_obj.at("cdmData").get<json>();
        LogMsgRaw(cdm_obj.dump(4));

        cdm_info.tobt = cdm_obj.at("tobt").get<std::string>().substr(0, 4);
        cdm_info.tsat = cdm_obj.at("tsat").get<std::string>().substr(0, 4);

        // "depInfo": "27L/TOLTA1F"
        std::string dep_info = cdm_obj.at("depInfo").get<std::string>();
        if (cdm_info.tobt.empty() && cdm_info.tsat.empty() && dep_info.empty()) {
            cdm_info.status = "Empty CDM data";
            LogMsg("CDM data for flight '%s' on vIFF server'%s' found but is empty", callsign.c_str(), name().c_str());
            return false;
        }

        auto i = dep_info.find("/");
        if (i != std::string::npos) {
            cdm_info.runway = dep_info.substr(0, i);
            cdm_info.sid = dep_info.substr(i + 1);
        }

        cdm_info.status = kSuccess;
        LogMsg("CDM data for flight '%s' retrieved from '%s'", callsign.c_str(), cdm_info.url.c_str());
        return true;
    } catch (const std::exception& e) {
        LogMsg("Exception: '%s'", e.what());
    }

    cdm_info.status = "Flight not found";
    return false;
}


//
// vacdm implementation
//
bool CdmServer_vacdm::RetrieveAirports() {
    if (retrieved_)
        return true;

    LogMsg("Loading airports for '%s' url: '%s'", name_.c_str(), url_.c_str());

    std::string api_url = url_ + "/api/v1/airports";

    json data_obj = GetJson(api_url);
    if (data_obj.is_null()) {
        LogMsg("Can't retrieve from '%s', retries left: %d", api_url.c_str(), --retries_left_);
        return false;
    }

    try {
        for (auto const& a : data_obj) {
            auto icao = a.at("icao").get<std::string>();
            arpt_urls_[icao] = url_;
            LogMsg("  '%s'", icao.c_str());
        }
    } catch (const std::exception& e) {
        LogMsg("Invalid airport data: '%s'", e.what());
        return false;
    }

    retrieved_ = true;
    return true;
}

// get and parse cdm data for airport/flight
bool CdmServer_vacdm::CdmGetParse(const std::string& arpt_icao, const std::string& callsign, CdmInfo& cdm_info) {
    if (is_dead())
        return false;

    if (!RetrieveAirports())
        return false;

    const auto it = arpt_urls_.find(arpt_icao);
    if (it == arpt_urls_.end())
        return false;

    cdm_info.url = it->second;

    cdm_info.url += std::string("/api/v1/pilots/") + callsign;
    json flight = GetJson(cdm_info.url);
    if (flight.is_null()) {
        cdm_info.status = "Failed to retrieve CDM data";
        return false;
    }

    try {
        const auto& vacdm = flight.at("vacdm").get<json>();
        cdm_info.tobt = ExtractHHMM(vacdm.at("tobt").get<std::string>());
        cdm_info.tsat = ExtractHHMM(vacdm.at("tsat").get<std::string>());
        const auto& clearance = flight.at("clearance").get<json>();
        cdm_info.runway = clearance.at("dep_rwy").get<std::string>();
        cdm_info.sid = clearance.at("sid").get<std::string>();
        cdm_info.status = kSuccess;
        return true;
    } catch (const json::out_of_range& e) {
        LogMsg("JSON key not found: '%s'", e.what());
        cdm_info.status = "Flight not found";
    } catch (const std::exception& e) {
        LogMsg("Exception: '%s'", e.what());
        cdm_info.status = e.what();
    }

    LogMsg("flight '%s' not present on '%s'", callsign.c_str(), arpt_icao.c_str());

    cdm_info.status = "Flight not found";
    return false;
}

//
// Global entry points
//
bool CdmInit(const std::string& cfg_path) {
    cache.idx = -1;

    std::ifstream f(cfg_path);
    if (f.fail())
        return false;

    // read whole file into string
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
                LogMsg("CdmServer '%s' is disabled, skipping", name.c_str());
                continue;
            }
            const auto& protocol = s.at("protocol").get<std::string>();
            const auto& url = s.at("url").get<std::string>();
            LogMsg("server: '%s', protocol: '%s', url: '%s'", name.c_str(), protocol.c_str(), url.c_str());

            if (protocol == "rpuig")
                cdm_servers.push_back(std::make_unique<CdmServer_rpuig>(name, url));
            else if (protocol == "viff")
                cdm_servers.push_back(std::make_unique<CdmServer_viff>(name, url));
            else if (protocol == "vacdm_v1")
                cdm_servers.push_back(std::make_unique<CdmServer_vacdm>(name, url));
            else {
                LogMsg("Sorry, only 'rpuig', 'viff' or 'vacdm_v1' are currently supported");
                return false;
            }
        }
    } catch (const std::exception& e) {
        LogMsg("Exception: '%s'", e.what());
        return false;
    }
    return true;
}

// get and parse cdm data for airport/flight
// *** runs in an async ***
bool CdmGetParse(const std::string& arpt_icao, const std::string& callsign, std::unique_ptr<CdmInfo>& cdm_info) {
    cdm_info = std::make_unique<CdmInfo>();

    if (cache.idx >= 0 && cache.arpt_icao == arpt_icao && cache.callsign == callsign) {
        LogMsg("Cache hit for '%s' '%s' on server '%s'", arpt_icao.c_str(), callsign.c_str(), cdm_servers[cache.idx]->name().c_str());
        return cdm_servers[cache.idx]->CdmGetParse(arpt_icao, callsign, *cdm_info);
    }

    for (auto i = 0; i < (int)cdm_servers.size(); i++) {
        auto& s = cdm_servers[i];
        if (s->is_dead()) {
            LogMsg("CdmServer '%s' is dead, skipping", s->name().c_str());
            continue;
        }

        if (s->CdmGetParse(arpt_icao, callsign, *cdm_info)) {
            cache.idx = i;
            cache.arpt_icao = arpt_icao;
            cache.callsign = callsign;
            return true;
        }
    }

    cdm_info->status = "Flight not found";
    cache.idx = -1;
    return false;
}
