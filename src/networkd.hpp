// What systemd-networkd knows about an interface, and how to change how it
// gets its address.
//
// State comes from org.freedesktop.network1.Manager.Describe(), a JSON
// snapshot of every link; changes are written as .network files followed
// by Reload() and ReconfigureLink().

#pragma once

#include <functional>
#include <memory>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>
#include <sdbus-c++/sdbus-c++.h>

#include "ip_config.hpp"

namespace esca::settings {

struct LinkInfo {
    bool present = false;
    int index = 0;
    std::string name;
    std::string type;          ///< "ether", "wlan"...
    std::string oper_state;    ///< "routable", "degraded", "no-carrier", "off"...
    std::string carrier_state; ///< "carrier", "no-carrier"...
    std::string mac;
    std::vector<std::string> addresses; ///< "192.168.1.9/24"
    std::string gateway;
    std::vector<std::string> dns;
    int speed_mbps = 0; ///< Ethernet only; 0 when unknown
    std::string config_source; ///< "dhcp" | "static" | ""

    nlohmann::json to_json() const;
};

/// One link out of Describe()'s JSON; present = false if it is not there.
LinkInfo parse_link(const nlohmann::json &describe, const std::string &ifname);

class Networkd {
public:
    explicit Networkd(sdbus::IConnection &connection, std::string config_dir = "/etc/systemd/network");

    LinkInfo link(const std::string &ifname);
    /// What the Settings app last chose for this interface (DHCP if nothing).
    IpConfig saved(const std::string &ifname) const;
    /// Writes the .network file and has networkd apply it now. Throws
    /// std::runtime_error on failure.
    void apply(const std::string &ifname, const IpConfig &config, int metric);
    /// Writes the file only if there is none yet.
    void ensure(const std::string &ifname, const IpConfig &config, int metric);

    /// Called (on the bus thread) whenever networkd reports a link change.
    void on_change(std::function<void()> callback) { changed_ = std::move(callback); }

private:
    sdbus::IConnection &connection_;
    std::string dir_;
    std::unique_ptr<sdbus::IProxy> manager_;
    std::function<void()> changed_;
};

} // namespace esca::settings
