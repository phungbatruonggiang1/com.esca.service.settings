// Wi-Fi, through wpa_supplicant's D-Bus API (fi.w1.wpa_supplicant1).
//
// The interface is created with a configuration file of its own
// (update_config=1), so the networks joined here are remembered across
// reboots; systemd-networkd gives the interface its address once it is
// associated. The radio is switched with rfkill.

#pragma once

#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>
#include <sdbus-c++/sdbus-c++.h>

#include "dbus_util.hpp"
#include "loop.hpp"
#include "networkd.hpp"
#include "wifi_model.hpp"

namespace esca::settings {

class Wifi {
public:
    /// error is "" on success, otherwise an error name: WrongPassword,
    /// Timeout, InvalidArgument, Unavailable, Busy, Failed.
    using Done = std::function<void(const std::string &error, const std::string &message)>;

    Wifi(sdbus::IConnection &connection, Loop &loop, Networkd &networkd, std::string ifname,
         std::string config_file, std::string country);

    /// Attaches to wpa_supplicant; retried whenever wpa_supplicant (re)appears.
    void start();

    bool present() const { return static_cast<bool>(iface_); }
    const std::string &ifname() const { return ifname_; }

    nlohmann::json state();
    nlohmann::json networks();
    void scan();
    void set_enabled(bool on);
    void connect(const std::string &ssid, const std::string &passphrase, bool hidden, Done done);
    void disconnect();
    /// False if no network of that name was saved.
    bool forget(const std::string &ssid);

    void on_change(std::function<void()> f) { changed_ = std::move(f); }
    void on_networks(std::function<void()> f) { networks_changed_ = std::move(f); }

private:
    struct Pending {
        std::string ssid;
        sdbus::ObjectPath network;
        bool added = false;
        bool handshake_seen = false;
        int failures = 0;
        uint64_t timer = 0;
        Done done;
    };

    void attach();
    void detach();
    void ensure_config_file();
    void interface_changed(const Props &changed);
    void finish(const std::string &error, const std::string &message);
    void enable_all_networks();
    void save_config();

    AccessPoint read_bss(const sdbus::ObjectPath &path);
    std::vector<AccessPoint> access_points();
    std::map<std::string, sdbus::ObjectPath> saved_networks();
    std::string current_state();

    void notify();

    sdbus::IConnection &connection_;
    Loop &loop_;
    Networkd &networkd_;
    std::string ifname_;
    std::string config_file_;
    std::string country_;

    std::unique_ptr<sdbus::IProxy> wpa_;
    std::unique_ptr<sdbus::IProxy> iface_;
    sdbus::ObjectPath iface_path_;
    std::optional<Pending> pending_;

    std::function<void()> changed_;
    std::function<void()> networks_changed_;
};

} // namespace esca::settings
