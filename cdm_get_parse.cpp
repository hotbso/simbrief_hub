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
    kProtoNool,
    kProtoVacdmV1
};

// cdm realm, vACDM, vatsimspain, ...
struct Realm {
    // keep these 3 in order
    std::string name;
    std::string server_url;
    CdmProtocol proto;

    json arpt_feeds;
};

// single host,e.g. EKCH running vACDM software
struct CdmHost {
    std::string icao;
    std::string server_url;
    CdmProtocol proto;
};

static std::vector<Realm> realm_servers;
static std::map<const std::string, CdmHost> cdm_host_servers;

void
CdmInfo::Dump() const
{
    if (status == "Success") {
#define L(field) LogMsg(#field ": %s", field.c_str())
        L(feed);
        L(status);
        L(tobt);
        L(tsat);
        L(runway);
        L(sid);
    } else
        LogMsg("%s", status.c_str());
#undef L
}

// Load airport feeds for a realm
static void
LoadRealm(Realm& realm)
{
    if (!realm.arpt_feeds.empty())
        return;

    LogMsg("Loading realm: '%s'", realm.name.c_str());

    std::string data;
    data.reserve(20 * 1024);
    bool res = HttpGet(realm.server_url, data, 10);

    if (!res) {
        LogMsg("Can't retrieve from '%s'", realm.server_url.c_str());
        return;
    }

    int len = data.length();
    LogMsg("got realm data %d bytes", len);
    realm.arpt_feeds = json::parse(data);
    //std::cout << realm.arpt_feeds.dump(4) << std::endl;
}

// Find feed for an airport
static std::pair<std::string, CdmProtocol>
FindFeed(const std::string& icao)
{
    // unlikely to change, cache successful queries
    static std::string icao_c, feed_c;
    static CdmProtocol proto_c;

    if (icao == icao_c)
        return std::make_pair(feed_c, proto_c);

    try {
        const auto& h = cdm_host_servers.at(icao);
        icao_c = icao;
        feed_c = h.server_url;
        proto_c = h.proto;
        return std::make_pair(feed_c, proto_c);
    } catch (std::out_of_range& e) {
        // FALLTHROUGH
    }

    for (auto& r : realm_servers) {
        LoadRealm(r);
        if (r.arpt_feeds.empty())
            continue;

        try {
            const auto& af_list = r.arpt_feeds.at("airports").at(icao);
            icao_c = icao;
            feed_c = af_list[0];
            proto_c = r.proto;
            return std::make_pair(feed_c, proto_c);
        } catch (json::out_of_range &e) {
            continue;
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
        for (const auto& h : cfg.at("hosts")) {
            const std::string name = h.at("name");
            const std::string protocol = h.at("protocol");
            const std::string url = h.at("url");
            LogMsg("host: '%s', protocol: '%s', url: '%s'", name.c_str(), protocol.c_str(), url.c_str());
            if (protocol != "vacdm_v1") {
                LogMsg("Sorry, only 'vacdm_v1' is currently supported for hosts");
                return false;
            }

            cdm_host_servers[name] = CdmHost{name, url, kProtoVacdmV1};
        }

        for (const auto& r : cfg.at("realms")) {
            const std::string name = r.at("name");
            const std::string protocol = r.at("protocol");
            const std::string url = r.at("url");
            LogMsg("realm: '%s', protocol: '%s', url: '%s'", name.c_str(), protocol.c_str(), url.c_str());
            if (protocol != "nool") {
                LogMsg("Sorry, only 'nool' is currently supported for rearms");
                return false;
            }

            realm_servers.push_back(Realm{name, url, kProtoNool});
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

    auto [feed, proto] = FindFeed(arpt_icao);
    if (feed.empty()) {
        LogMsg("Feed for %s not found", arpt_icao.c_str());
        return false;
    }

    cdm_info->feed = feed;
    std::string data;
    data.reserve(10 * 1024);

    auto GetData = [&]() ->bool {
        LogMsg("Feed for %s: %s", arpt_icao.c_str(), cdm_info->feed.c_str());
        bool res = HttpGet(cdm_info->feed, data, 10);

        if (!res) {
            LogMsg("Can't retrieve from '%s'", feed.c_str());
            return false;
        }

        int len = data.length();
        LogMsg("got flight data %d bytes", len);
        return true;
    };

    if (proto == kProtoVacdmV1) {
        cdm_info->feed += std::string("/api/v1/pilots/") + callsign;;
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
        } catch (const std::exception &e) {
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
            // FALLTHROUGH
        }
    } else if (proto == kProtoNool) {
        if (!GetData())
            return false;

        try {
            json flights = json::parse(data).at("flights");
            // LogMsgRaw(flights.dump(4));
            for (const auto& f : flights) {
                if (f.at("callsign") == callsign) {
                    LogMsgRaw(f.dump(4));
#define EXTRACT(fn) cdm_info->fn = f.at(#fn)
                    EXTRACT(tobt);
                    EXTRACT(tsat);
                    EXTRACT(runway);
                    EXTRACT(sid);
                    cdm_info->status = "Success";
                    LogMsg("CDM data for flight '%s' retrieved from '%s'", callsign.c_str(), feed.c_str());
                    cdm_info->Dump();
                    return true;
#undef EXTRACT
                }
            }
            LogMsg("flight '%s' not present on '%s'", callsign.c_str(), arpt_icao.c_str());
        } catch (const std::exception& e) {
            LogMsg("Exception: '%s'", e.what());
        }
    } else {
        LogMsg("Unsupported protocol");
    }

    cdm_info->status = "not found";
    return false;
}
