#include "wifi_model.hpp"

#include <algorithm>
#include <map>

namespace esca::settings {

int signal_percent(int dbm)
{
    if (dbm <= -90)
        return 0;
    if (dbm >= -30)
        return 100;
    return (dbm + 90) * 100 / 60;
}

std::string security_of(bool privacy, const std::vector<std::string> &rsn, const std::vector<std::string> &wpa)
{
    auto has = [](const std::vector<std::string> &v, const std::string &prefix) {
        return std::any_of(v.begin(), v.end(), [&](const std::string &s) { return s.rfind(prefix, 0) == 0; });
    };
    if (has(rsn, "wpa-eap") || has(rsn, "eap") || has(wpa, "wpa-eap"))
        return "enterprise";
    const bool sae = has(rsn, "sae");
    const bool psk = has(rsn, "wpa-psk") || has(wpa, "wpa-psk");
    if (sae && psk)
        return "wpa2/wpa3";
    if (sae)
        return "wpa3";
    if (psk)
        return "wpa2";
    return privacy ? "wep" : "open";
}

std::vector<AccessPoint> merge_by_ssid(std::vector<AccessPoint> aps)
{
    std::map<std::string, AccessPoint> best;
    for (auto &ap : aps) {
        if (ap.ssid.empty())
            continue;
        auto it = best.find(ap.ssid);
        if (it == best.end()) {
            best.emplace(ap.ssid, ap);
            continue;
        }
        AccessPoint &b = it->second;
        const bool connected = b.connected || ap.connected;
        const bool saved = b.saved || ap.saved;
        // Show the access point we are on; otherwise the strongest one.
        if (ap.connected || (!b.connected && ap.signal_dbm > b.signal_dbm))
            b = ap;
        b.connected = connected;
        b.saved = saved;
    }
    std::vector<AccessPoint> out;
    for (auto &[_, ap] : best)
        out.push_back(ap);
    std::sort(out.begin(), out.end(), [](const AccessPoint &a, const AccessPoint &b) {
        if (a.connected != b.connected)
            return a.connected;
        if (a.saved != b.saved)
            return a.saved;
        if (a.signal_dbm != b.signal_dbm)
            return a.signal_dbm > b.signal_dbm;
        return a.ssid < b.ssid;
    });
    return out;
}

std::string check_passphrase(const std::string &security, const std::string &p)
{
    if (security == "open")
        return "";
    if (security == "enterprise")
        return "enterprise (802.1X) networks are not supported yet";
    if (security == "wep")
        return (p.size() == 5 || p.size() == 13 || p.size() == 10 || p.size() == 26) ? ""
                                                                                    : "a WEP key is 5 or 13 characters";
    if (p.size() < 8 || p.size() > 63) {
        const bool hex64 = p.size() == 64 && std::all_of(p.begin(), p.end(), ::isxdigit);
        if (!hex64)
            return "the password must be 8 to 63 characters";
    }
    return "";
}

std::string wifi_state_name(const std::string &s)
{
    if (s == "completed")
        return "connected";
    if (s == "authenticating" || s == "associating" || s == "associated" || s == "4way_handshake"
        || s == "group_handshake")
        return "connecting";
    if (s == "scanning")
        return "scanning";
    return "disconnected";
}

nlohmann::json to_json(const AccessPoint &ap)
{
    return { { "ssid", ap.ssid },
             { "bssid", ap.bssid },
             { "signal", signal_percent(ap.signal_dbm) },
             { "signalDbm", ap.signal_dbm },
             { "frequency", ap.frequency_mhz },
             { "band", ap.frequency_mhz >= 5900 ? "6 GHz" : ap.frequency_mhz >= 4900 ? "5 GHz" : "2.4 GHz" },
             { "security", ap.security },
             { "saved", ap.saved },
             { "connected", ap.connected } };
}

} // namespace esca::settings
