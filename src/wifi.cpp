#include "wifi.hpp"

#include <filesystem>
#include <fstream>

#include "logger.hpp"
#include "rfkill.hpp"

namespace esca::settings {

using nlohmann::json;
using namespace std::chrono_literals;

namespace {

const sdbus::ServiceName kWpa { "fi.w1.wpa_supplicant1" };
const sdbus::ObjectPath kWpaPath { "/fi/w1/wpa_supplicant1" };
constexpr const char *kWpaIface = "fi.w1.wpa_supplicant1";
constexpr const char *kIface = "fi.w1.wpa_supplicant1.Interface";
constexpr const char *kBss = "fi.w1.wpa_supplicant1.BSS";
constexpr const char *kNetwork = "fi.w1.wpa_supplicant1.Network";

constexpr auto kConnectTimeout = 30s;

/// wpa_supplicant keeps string network options quoted, as in its file.
std::string unquote(const std::string &s)
{
    return s.size() >= 2 && s.front() == '"' && s.back() == '"' ? s.substr(1, s.size() - 2) : s;
}

} // namespace

Wifi::Wifi(sdbus::IConnection &connection, Loop &loop, Networkd &networkd, std::string ifname,
           std::string config_file, std::string country)
    : connection_(connection), loop_(loop), networkd_(networkd), ifname_(std::move(ifname)),
      config_file_(std::move(config_file)), country_(std::move(country))
{
}

void Wifi::start()
{
    // wpa_supplicant is D-Bus activated and may restart; attach again
    // whenever its name gets an owner.
    connection_.addMatch("type='signal',sender='org.freedesktop.DBus',member='NameOwnerChanged',"
                         "arg0='fi.w1.wpa_supplicant1'",
                         [this](sdbus::Message msg) {
                             std::string name, old_owner, new_owner;
                             msg >> name >> old_owner >> new_owner;
                             loop_.post([this, gone = new_owner.empty()] {
                                 if (gone)
                                     detach();
                                 else
                                     attach();
                             });
                         });
    attach();
}

void Wifi::ensure_config_file()
{
    if (std::filesystem::exists(config_file_))
        return;
    std::error_code ec;
    std::filesystem::create_directories(std::filesystem::path(config_file_).parent_path(), ec);
    std::ofstream out(config_file_);
    // update_config lets SaveConfig() keep what the user joins.
    out << "ctrl_interface=/run/wpa_supplicant\nupdate_config=1\n";
    if (!country_.empty())
        out << "country=" << country_ << "\n";
    std::filesystem::permissions(config_file_, std::filesystem::perms::owner_read | std::filesystem::perms::owner_write, ec);
}

void Wifi::attach()
{
    if (iface_)
        return;
    if (!std::filesystem::exists("/sys/class/net/" + ifname_)) {
        ESCA_LOG_WARN("no %s: Wi-Fi unavailable", ifname_.c_str());
        return;
    }
    ensure_config_file();
    try {
        wpa_ = sdbus::createProxy(connection_, kWpa, kWpaPath);
        sdbus::ObjectPath path;
        try {
            wpa_->callMethod("GetInterface").onInterface(kWpaIface).withArguments(ifname_).storeResultsTo(path);
        } catch (const sdbus::Error &) {
            Props args { { "Ifname", sdbus::Variant(ifname_) },
                         { "Driver", sdbus::Variant(std::string("nl80211")) },
                         { "ConfigFile", sdbus::Variant(config_file_) } };
            wpa_->callMethod("CreateInterface").onInterface(kWpaIface).withArguments(args).storeResultsTo(path);
        }
        iface_path_ = path;
        iface_ = sdbus::createProxy(connection_, kWpa, path);
        iface_->uponSignal("PropertiesChanged").onInterface(kIface).call([this](const Props &changed) {
            interface_changed(changed);
        });
        iface_->uponSignal("ScanDone").onInterface(kIface).call([this](bool) {
            if (networks_changed_)
                loop_.coalesce("wifi-networks", 300ms, networks_changed_);
        });
        iface_->uponSignal("BSSRemoved").onInterface(kIface).call([this](const sdbus::ObjectPath &) {
            if (networks_changed_)
                loop_.coalesce("wifi-networks", 1000ms, networks_changed_);
        });
        ESCA_LOG_INFO("Wi-Fi on %s (%s)", ifname_.c_str(), path.c_str());
        notify();
    } catch (const sdbus::Error &e) {
        ESCA_LOG_WARN("wpa_supplicant: %s", e.getMessage().c_str());
        iface_.reset();
    }
}

void Wifi::detach()
{
    if (pending_)
        finish("Failed", "wpa_supplicant stopped");
    iface_.reset();
    wpa_.reset();
    notify();
}

void Wifi::notify()
{
    if (changed_)
        loop_.coalesce("wifi-state", 200ms, changed_);
}

std::string Wifi::current_state()
{
    try {
        return iface_->getProperty("State").onInterface(kIface).get<std::string>();
    } catch (const sdbus::Error &) {
        return "disconnected";
    }
}

AccessPoint Wifi::read_bss(const sdbus::ObjectPath &path)
{
    AccessPoint ap;
    auto proxy = sdbus::createProxy(connection_, kWpa, path);
    const Props p = get_all(*proxy, kBss);
    ap.ssid = bytes_to_string(prop<std::vector<uint8_t>>(p, "SSID"));
    ap.bssid = mac_to_string(prop<std::vector<uint8_t>>(p, "BSSID"));
    ap.signal_dbm = prop<int16_t>(p, "Signal", -100);
    ap.frequency_mhz = prop<uint16_t>(p, "Frequency", 0);
    const Props rsn = prop<Props>(p, "RSN");
    const Props wpa = prop<Props>(p, "WPA");
    ap.security = security_of(prop<bool>(p, "Privacy"), prop<std::vector<std::string>>(rsn, "KeyMgmt"),
                              prop<std::vector<std::string>>(wpa, "KeyMgmt"));
    return ap;
}

std::map<std::string, sdbus::ObjectPath> Wifi::saved_networks()
{
    std::map<std::string, sdbus::ObjectPath> out;
    std::vector<sdbus::ObjectPath> paths;
    try {
        paths = iface_->getProperty("Networks").onInterface(kIface).get<std::vector<sdbus::ObjectPath>>();
    } catch (const sdbus::Error &) {
        return out;
    }
    for (const auto &path : paths) {
        auto proxy = sdbus::createProxy(connection_, kWpa, path);
        try {
            const Props props = proxy->getProperty("Properties").onInterface(kNetwork).get<Props>();
            out[unquote(prop<std::string>(props, "ssid"))] = path;
        } catch (const sdbus::Error &) {
        }
    }
    return out;
}

std::vector<AccessPoint> Wifi::access_points()
{
    std::vector<AccessPoint> aps;
    if (!iface_)
        return aps;
    std::vector<sdbus::ObjectPath> bsss;
    sdbus::ObjectPath current;
    try {
        bsss = iface_->getProperty("BSSs").onInterface(kIface).get<std::vector<sdbus::ObjectPath>>();
        current = iface_->getProperty("CurrentBSS").onInterface(kIface).get<sdbus::ObjectPath>();
    } catch (const sdbus::Error &e) {
        ESCA_LOG_WARN("reading scan results: %s", e.getMessage().c_str());
        return aps;
    }
    const auto saved = saved_networks();
    const bool connected = current_state() == "completed";
    for (const auto &path : bsss) {
        try {
            AccessPoint ap = read_bss(path);
            ap.saved = saved.count(ap.ssid) != 0;
            ap.connected = connected && path == current;
            aps.push_back(std::move(ap));
        } catch (const sdbus::Error &) {
            // gone between listing and reading
        }
    }
    return merge_by_ssid(std::move(aps));
}

json Wifi::networks()
{
    json out = json::array();
    for (const auto &ap : access_points())
        out.push_back(to_json(ap));
    return out;
}

json Wifi::state()
{
    json s = { { "present", present() || rfkill::present("wlan") },
               { "enabled", !rfkill::blocked("wlan") },
               { "interface", ifname_ },
               { "state", "disconnected" },
               { "connecting", pending_ ? pending_->ssid : "" } };
    if (iface_) {
        const std::string st = current_state();
        s["state"] = wifi_state_name(st);
        if (st == "completed") {
            try {
                const auto bss = iface_->getProperty("CurrentBSS").onInterface(kIface).get<sdbus::ObjectPath>();
                if (bss != "/") {
                    const AccessPoint ap = read_bss(bss);
                    s["network"] = to_json(ap);
                }
            } catch (const sdbus::Error &) {
            }
        }
    }
    s["link"] = networkd_.link(ifname_).to_json();
    s["ipConfig"] = to_json(networkd_.saved(ifname_));
    return s;
}

void Wifi::scan()
{
    if (!iface_)
        throw std::runtime_error("Wi-Fi is unavailable");
    Props args { { "Type", sdbus::Variant(std::string("active")) } };
    try {
        iface_->callMethod("Scan").onInterface(kIface).withArguments(args);
    } catch (const sdbus::Error &e) {
        // Already scanning: the result comes all the same.
        ESCA_LOG_DEBUG("scan: %s", e.getMessage().c_str());
    }
}

void Wifi::set_enabled(bool on)
{
    if (!rfkill::set_blocked("wlan", !on))
        throw std::runtime_error("cannot switch the Wi-Fi radio");
    ESCA_LOG_INFO("Wi-Fi %s", on ? "on" : "off");
    notify();
    if (on)
        loop_.after(2s, [this] {
            if (iface_)
                scan();
        });
}

void Wifi::connect(const std::string &ssid, const std::string &passphrase, bool hidden, Done done)
{
    if (!iface_)
        return done("Unavailable", "Wi-Fi is unavailable");
    if (rfkill::blocked("wlan"))
        return done("Unavailable", "Wi-Fi is off");
    if (pending_)
        return done("Busy", "already joining " + pending_->ssid);
    if (ssid.empty() || ssid.size() > 32)
        return done("InvalidArgument", "a network name is 1 to 32 bytes");

    std::string security = passphrase.empty() ? "open" : "wpa2/wpa3";
    for (const auto &ap : access_points())
        if (ap.ssid == ssid)
            security = ap.security;

    const auto saved = saved_networks();
    const auto existing = saved.find(ssid);
    Pending p;
    p.ssid = ssid;
    p.done = std::move(done);
    try {
        if (existing != saved.end() && passphrase.empty()) {
            p.network = existing->second; // join with what was saved
        } else {
            const std::string why = check_passphrase(security, passphrase);
            if (!why.empty())
                return p.done("InvalidArgument", why);
            if (existing != saved.end())
                iface_->callMethod("RemoveNetwork").onInterface(kIface).withArguments(existing->second);
            Props net { { "ssid", sdbus::Variant(ssid) } };
            if (hidden)
                net["scan_ssid"] = sdbus::Variant(int32_t(1));
            if (security == "open") {
                net["key_mgmt"] = sdbus::Variant(std::string("NONE"));
            } else if (security == "wep") {
                net["key_mgmt"] = sdbus::Variant(std::string("NONE"));
                net["wep_key0"] = sdbus::Variant(passphrase);
                net["wep_tx_keyidx"] = sdbus::Variant(int32_t(0));
            } else if (security == "wpa3") {
                net["key_mgmt"] = sdbus::Variant(std::string("SAE"));
                net["psk"] = sdbus::Variant(passphrase);
                net["ieee80211w"] = sdbus::Variant(int32_t(2));
            } else if (security == "wpa2/wpa3") {
                net["key_mgmt"] = sdbus::Variant(std::string("WPA-PSK SAE"));
                net["psk"] = sdbus::Variant(passphrase);
                net["ieee80211w"] = sdbus::Variant(int32_t(1));
            } else { // wpa2
                net["key_mgmt"] = sdbus::Variant(std::string("WPA-PSK"));
                net["psk"] = sdbus::Variant(passphrase);
            }
            iface_->callMethod("AddNetwork").onInterface(kIface).withArguments(net).storeResultsTo(p.network);
            p.added = true;
        }
        p.timer = loop_.after(kConnectTimeout, [this] { finish("Timeout", "no answer from the network"); });
        pending_ = std::move(p);
        iface_->callMethod("SelectNetwork").onInterface(kIface).withArguments(pending_->network);
        ESCA_LOG_INFO("joining \"%s\" (%s)", ssid.c_str(), security.c_str());
        notify();
    } catch (const sdbus::Error &e) {
        if (pending_)
            finish("Failed", e.getMessage());
        else
            p.done("Failed", e.getMessage());
    }
}

void Wifi::interface_changed(const Props &changed)
{
    const auto st = changed.find("State");
    if (st != changed.end() && st->second.containsValueOfType<std::string>() && pending_) {
        const std::string s = st->second.get<std::string>();
        if (s == "4way_handshake" || s == "group_handshake")
            pending_->handshake_seen = true;
        if (s == "completed") {
            finish("", "");
        } else if ((s == "disconnected" || s == "scanning") && pending_->handshake_seen) {
            // The key exchange started and was dropped: the password. Once
            // can be bad luck, twice is not.
            pending_->handshake_seen = false;
            if (++pending_->failures >= 2)
                finish("WrongPassword", "the password is not right");
        }
    }
    if (st != changed.end() || changed.count("CurrentBSS"))
        notify();
}

void Wifi::enable_all_networks()
{
    // SelectNetwork() disables every other saved network; give them back
    // so the board still falls back to them on its own.
    for (const auto &[ssid, path] : saved_networks()) {
        try {
            auto proxy = sdbus::createProxy(connection_, kWpa, path);
            proxy->setProperty("Enabled").onInterface(kNetwork).toValue(true);
        } catch (const sdbus::Error &) {
        }
    }
}

void Wifi::save_config()
{
    try {
        iface_->callMethod("SaveConfig").onInterface(kIface);
    } catch (const sdbus::Error &e) {
        ESCA_LOG_WARN("saving Wi-Fi networks: %s", e.getMessage().c_str());
    }
}

void Wifi::finish(const std::string &error, const std::string &message)
{
    if (!pending_)
        return;
    Pending p = std::move(*pending_);
    pending_.reset();
    loop_.cancel(p.timer);
    if (iface_) {
        try {
            if (!error.empty() && p.added)
                iface_->callMethod("RemoveNetwork").onInterface(kIface).withArguments(p.network);
            enable_all_networks();
            if (!error.empty())
                iface_->callMethod("Reassociate").onInterface(kIface);
            save_config();
        } catch (const sdbus::Error &e) {
            ESCA_LOG_WARN("after joining: %s", e.getMessage().c_str());
        }
    }
    if (error.empty())
        ESCA_LOG_INFO("joined \"%s\"", p.ssid.c_str());
    else
        ESCA_LOG_WARN("could not join \"%s\": %s", p.ssid.c_str(), message.c_str());
    p.done(error, message);
    notify();
    if (networks_changed_)
        loop_.coalesce("wifi-networks", 300ms, networks_changed_);
}

void Wifi::disconnect()
{
    if (!iface_)
        throw std::runtime_error("Wi-Fi is unavailable");
    iface_->callMethod("Disconnect").onInterface(kIface);
}

bool Wifi::forget(const std::string &ssid)
{
    if (!iface_)
        throw std::runtime_error("Wi-Fi is unavailable");
    const auto saved = saved_networks();
    const auto it = saved.find(ssid);
    if (it == saved.end())
        return false;
    iface_->callMethod("RemoveNetwork").onInterface(kIface).withArguments(it->second);
    save_config();
    ESCA_LOG_INFO("forgot \"%s\"", ssid.c_str());
    notify();
    if (networks_changed_)
        loop_.coalesce("wifi-networks", 300ms, networks_changed_);
    return true;
}

} // namespace esca::settings
