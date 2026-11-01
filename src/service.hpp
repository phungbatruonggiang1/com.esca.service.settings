// esca-settingsd on the bus: vn.sparc.esca.Settings1.
//
// One object, /vn/sparc/esca/Settings1, one interface per settings page:
//
//   vn.sparc.esca.Settings1.Wifi
//     GetState() -> s, GetNetworks() -> s           JSON snapshots
//     SetEnabled(b), Scan(), Disconnect(), Forget(s ssid) -> b
//     Connect(s ssid, s passphrase, b hidden)       returns once joined, or
//                                                   fails: WrongPassword, Timeout...
//     SetIpConfig(s json)
//     signals Changed(), NetworksChanged()
//   vn.sparc.esca.Settings1.Ethernet
//     GetState() -> s, SetIpConfig(s json); signal Changed()
//   vn.sparc.esca.Settings1.Bluetooth
//     GetState() -> s, GetDevices() -> s, SetPowered(b), SetDiscoverable(b),
//     StartDiscovery(), StopDiscovery(), Pair(s), Connect(s), Disconnect(s),
//     Remove(s) -> b, AnswerPairing(s address, b accept, s value)
//     signals Changed(), DevicesChanged(), PairingRequest(s json)
//
// Errors are vn.sparc.esca.Settings1.Error.<Name>. Snapshots are JSON so
// the pages can grow fields without changing D-Bus signatures; signals
// carry no state, the app reads what it shows.

#pragma once

#include <memory>
#include <string>

#include <sdbus-c++/sdbus-c++.h>

#include "bluetooth.hpp"
#include "loop.hpp"
#include "networkd.hpp"
#include "wifi.hpp"

namespace esca::settings {

struct Config {
    std::string wifi_interface = "wlan0";
    std::string ethernet_interface = "eth0";
    /// Regulatory domain: which Wi-Fi channels and powers are allowed.
    std::string wifi_country = "VN";
    std::string wpa_config = "/etc/wpa_supplicant/wpa_supplicant-wlan0.conf";
    std::string network_dir = "/etc/systemd/network";
    std::string log_level = "INFO";
    bool session_bus = false;

    bool load(const std::string &path, std::string &error);
};

class SettingsService {
public:
    static constexpr const char *kBusName = "vn.sparc.esca.Settings1";
    static constexpr const char *kPath = "/vn/sparc/esca/Settings1";

    SettingsService(sdbus::IConnection &connection, Loop &loop, Config config);
    void start();

private:
    void register_object();

    sdbus::IConnection &connection_;
    Loop &loop_;
    Config config_;
    Networkd networkd_;
    Wifi wifi_;
    Bluetooth bluetooth_;
    std::unique_ptr<sdbus::IObject> object_;
};

} // namespace esca::settings
