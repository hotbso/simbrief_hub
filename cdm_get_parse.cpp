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

// cdm realm, vACDM, vatsimspain, ...
struct Realm {
    std::string name;
    std::string server_url;
    json arpt_feeds;
};

static std::vector<Realm> realm_servers;

void
CdmInfo::Dump() const
{
    if (status == "Success") {
#define L(field) LogMsg(#field ": %s", field.c_str())
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
CdmInit(const std::string& cfg_path)
{
    std::ifstream f(cfg_path);
    if (f.fail())
        return false;

    try {
        json cfg = json::parse(f);
        //std::cout << cfg.dump(4) << std::endl;
        for (const auto& r : cfg.at("realms")) {
            const std::string name = r.at("name");
            const std::string protocol = r.at("protocol");
            const std::string url = r.at("url");
            LogMsg("realm: '%s', protocol: '%s', url: '%s'", name.c_str(), protocol.c_str(), url.c_str());
            if (protocol != "nool") {
                LogMsg("Sorry, only 'nool' is currently supported");
                return false;
            }

            realm_servers.push_back(Realm{name, url});
        }
    } catch (std::exception& e) {
        LogMsg("Exception: '%s'", e.what());
        return false;
    }
    return true;
}

// get and parse cdm data for airport/flight
// *** runs in an async ***
bool
CdmGetParse(const std::string& arpt_icao, const std::string& callsign, std::unique_ptr<CdmInfo>& cdm_info)
{
    cdm_info = std::make_unique<CdmInfo>();

    const auto feed = FindFeed(arpt_icao);
    if (feed.empty()) {
        LogMsg("Feed for %s not found", arpt_icao.c_str());
        return false;
    }

    LogMsg("Feed for %s: %s", arpt_icao.c_str(), feed.c_str());

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
        std::cout << flights.dump(4) << std::endl;
        for (const auto& f : flights) {
            if (f.at("callsign") == callsign) {
                std::cout << f.dump(4) << std::endl;
#define EXTRACT(fn) cdm_info->fn = f.at(#fn)
                EXTRACT(tobt);
                EXTRACT(tsat);
                EXTRACT(runway);
                EXTRACT(sid);
                cdm_info->status = "Success";
                return true;
#undef EXTRACT
            }
        }
        LogMsg("flight '%s' not present on '%s'", callsign.c_str(), arpt_icao.c_str());
    } catch (std::exception& e) {
        LogMsg("Exception: '%s'", e.what());
    }

    cdm_info->status = "not found";
    return false;
}
