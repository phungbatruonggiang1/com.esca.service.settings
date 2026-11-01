// LinkInfo out of systemd-networkd's Describe() JSON. Kept apart from the
// D-Bus half of networkd.cpp so it can be tested anywhere.

#include <arpa/inet.h>

#include "networkd.hpp"

namespace esca::settings {

using nlohmann::json;

namespace {

/// networkd writes addresses as arrays of bytes.
std::string ip_string(const json &bytes, int family)
{
    if (!bytes.is_array())
        return bytes.is_string() ? bytes.get<std::string>() : "";
    unsigned char raw[16] = {};
    const size_t n = std::min<size_t>(bytes.size(), 16);
    for (size_t i = 0; i < n; ++i)
        raw[i] = static_cast<unsigned char>(bytes[i].get<int>());
    char text[INET6_ADDRSTRLEN] = {};
    const int af = family == 10 || n == 16 ? AF_INET6 : AF_INET;
    return inet_ntop(af, raw, text, sizeof(text)) ? text : "";
}

} // namespace

json LinkInfo::to_json() const
{
    return { { "present", present },          { "name", name },
             { "type", type },                { "operState", oper_state },
             { "carrier", carrier_state == "carrier" || carrier_state == "enslaved" || carrier_state == "degraded-carrier" },
             { "mac", mac },                  { "addresses", addresses },
             { "gateway", gateway },          { "dns", dns },
             { "speedMbps", speed_mbps },     { "configSource", config_source } };
}

LinkInfo parse_link(const json &describe, const std::string &ifname)
{
    LinkInfo l;
    l.name = ifname;
    if (!describe.contains("Interfaces") || !describe["Interfaces"].is_array())
        return l;
    for (const auto &i : describe["Interfaces"]) {
        if (i.value("Name", "") != ifname)
            continue;
        l.present = true;
        l.index = i.value("Index", 0);
        l.type = i.value("Type", "");
        l.oper_state = i.value("OperationalState", "");
        l.carrier_state = i.value("CarrierState", "");
        if (i.contains("HardwareAddress") && i["HardwareAddress"].is_array()) {
            std::vector<uint8_t> mac;
            for (const auto &b : i["HardwareAddress"])
                mac.push_back(static_cast<uint8_t>(b.get<int>()));
            static const char hex[] = "0123456789ABCDEF";
            for (size_t k = 0; k < mac.size(); ++k) {
                if (k)
                    l.mac += ':';
                l.mac += hex[mac[k] >> 4];
                l.mac += hex[mac[k] & 15];
            }
        }
        for (const auto &a : i.value("Addresses", json::array())) {
            const int family = a.value("Family", 2);
            const std::string ip = ip_string(a.value("Address", json()), family);
            if (ip.empty() || ip.rfind("fe80:", 0) == 0)
                continue; // link-local IPv6 is noise for the user
            l.addresses.push_back(ip + "/" + std::to_string(a.value("PrefixLength", 0)));
            const std::string source = a.value("ConfigSource", "");
            if (family == 2 && l.config_source.empty())
                l.config_source = source.rfind("DHCP", 0) == 0 || source.rfind("dhcp", 0) == 0 ? "dhcp" : "static";
        }
        for (const auto &r : i.value("Routes", json::array())) {
            if (r.value("DestinationPrefixLength", -1) == 0 && r.contains("Gateway")) {
                const std::string gw = ip_string(r["Gateway"], r.value("Family", 2));
                if (!gw.empty() && gw != "0.0.0.0" && gw != "::") {
                    l.gateway = gw;
                    break;
                }
            }
        }
        for (const auto &d : i.value("DNS", json::array())) {
            const std::string ip = d.is_object() ? ip_string(d.value("Address", json()), d.value("Family", 2))
                                                 : (d.is_string() ? d.get<std::string>() : "");
            if (!ip.empty())
                l.dns.push_back(ip);
        }
        break;
    }
    return l;
}

} // namespace esca::settings
