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

#include <iostream>
#include <vector>
#include "sbh.h"
#include "http_get.h"

static const char *def_realm_server_cfg = R"({"realm_servers": ["https://app.vacdm.net/api/vdgs/nool", "https://aman.vatsimspain.es/CDM_feeds.json"]})";

// cdm realm, vACDM, vatsimspain, ...
struct Realm {
    std::string server_url;
    json arpt_feeds;
};

static std::vector<Realm> realm_servers;

// Load airport feeds for a realm
static void
LoadRealm(Realm& realm)
{
    if (!realm.arpt_feeds.empty())
        return;

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
static std::string
FindFeed(const std::string& icao)
{
    // unlikely to change, cache successful queries
    static std::string icao_c, feed_c;
    if (icao == icao_c)
        return feed_c;

    for (auto& r : realm_servers) {
        LoadRealm(r);
        if (r.arpt_feeds.empty())
            continue;

        try {
            const auto& af_list = r.arpt_feeds.at("airports").at(icao);
            icao_c = icao;
            feed_c = af_list[0];
            return feed_c;
        } catch (json::out_of_range &e) {
            //std::cerr << e.what() << std::endl;
            continue;
        }
    }

    return "";
}

bool
CdmInit()
{
    // TODO: possible overwrite with a file
    json realm_server_cfg = json::parse(def_realm_server_cfg);
    std::cout << realm_server_cfg.dump(4) << std::endl;
    for (const auto& s : realm_server_cfg.at("realm_servers")) {
        realm_servers.push_back(Realm{s});
    }

    return true;
}

bool
CdmGetParse(const std::string& icao, const std::string& callsign, std::unique_ptr<CdmInfo>& Cdm_info)
{
    const auto feed = FindFeed(icao);
    if (feed.empty()) {
        LogMsg("Feed for %s not found", icao.c_str());
        return false;
    }

    LogMsg("Feed for %s: %s", icao.c_str(), feed.c_str());

    //load flight data
    std::string data;
    data.reserve(10 * 1024);
    bool res = HttpGet(feed, data, 10);

    if (!res) {
        LogMsg("Can't retrieve from '%s'", feed.c_str());
        return false;
    }

    int len = data.length();
    LogMsg("got flight data %d bytes", len);
    try {
        json flights = json::parse(data).at("flights");
        for (const auto& f : flights) {
            if (f.at("callsign") == callsign) {
                std::cout << f.dump(4) << std::endl;
                return true;
            }
        }

        LogMsg("flight '%s' not present on '%s'", callsign.c_str(), icao.c_str());
    } catch (std::exception& e) {
        LogMsg("Exception: '%s'", e.what());
    }

    return false;
}
