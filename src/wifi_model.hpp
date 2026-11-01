// What the Wi-Fi page shows, independent of where it comes from: the
// networks in range, merged per name, and the rules for joining one.

#pragma once

#include <string>
#include <vector>

#include <nlohmann/json.hpp>

namespace esca::settings {

struct AccessPoint {
    std::string ssid;
    std::string bssid;
    int signal_dbm = -100;
    int frequency_mhz = 0;
    std::string security; ///< "open", "wep", "wpa2", "wpa3", "wpa2/wpa3", "enterprise"
    bool saved = false;
    bool connected = false;
};

/// 0..100, for the signal bars: -90 dBm and below is 0, -30 and above 100.
int signal_percent(int dbm);

/// From a BSS's Privacy flag and the key management of its RSN/WPA IEs.
std::string security_of(bool privacy, const std::vector<std::string> &rsn_key_mgmt,
                        const std::vector<std::string> &wpa_key_mgmt);

/// One entry per network name, the strongest of its access points; hidden
/// networks (no name) are left out. Connected first, then saved, then by
/// signal.
std::vector<AccessPoint> merge_by_ssid(std::vector<AccessPoint> aps);

/// Whether a passphrase can join a network with this security: "" if so,
/// otherwise why not.
std::string check_passphrase(const std::string &security, const std::string &passphrase);

/// wpa_supplicant's interface state, in the app's terms:
/// "disconnected", "scanning", "connecting", "connected".
std::string wifi_state_name(const std::string &wpa_state);

nlohmann::json to_json(const AccessPoint &ap);

} // namespace esca::settings
