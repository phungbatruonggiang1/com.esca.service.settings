// How an interface gets its address, and the systemd-networkd file that
// says so.
//
// The image ships 80-wired.network (DHCP on every Ethernet port). A user's
// choice is written to 20-esca-<interface>.network, which networkd prefers
// because it sorts first; going back to DHCP rewrites it with DHCP=yes.

#pragma once

#include <string>
#include <vector>

#include <nlohmann/json.hpp>

namespace esca::settings {

struct IpConfig {
    enum class Method { Dhcp, Static };
    Method method = Method::Dhcp;
    std::string address; ///< static only: "192.168.1.50/24"
    std::string gateway; ///< static only, optional
    std::vector<std::string> dns;

    bool operator==(const IpConfig &o) const
    {
        return method == o.method && address == o.address && gateway == o.gateway && dns == o.dns;
    }
};

/// {"method": "dhcp"|"static", "address": "a.b.c.d/n", "gateway": "...", "dns": [...]}
IpConfig ip_config_from_json(const nlohmann::json &j);
nlohmann::json to_json(const IpConfig &c);

/// "" if usable, otherwise what is wrong, in words for the user.
std::string validate(const IpConfig &c);

/// The .network file for `ifname`. `metric` orders the default routes when
/// both Ethernet and Wi-Fi are up: Ethernet wins.
std::string render_network_file(const std::string &ifname, const IpConfig &c, int metric);

/// Reads back what render_network_file wrote (DHCP when it is anything else).
IpConfig parse_network_file(const std::string &text);

/// "20-esca-eth0.network"
std::string network_file_name(const std::string &ifname);

} // namespace esca::settings
