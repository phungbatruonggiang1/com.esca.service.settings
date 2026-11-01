#include "service.hpp"

#include <fstream>
#include <stdexcept>

#include "logger.hpp"

namespace esca::settings {

using nlohmann::json;

namespace {

constexpr const char *kWifi = "vn.sparc.esca.Settings1.Wifi";
constexpr const char *kEthernet = "vn.sparc.esca.Settings1.Ethernet";
constexpr const char *kBluetooth = "vn.sparc.esca.Settings1.Bluetooth";

// Ethernet first when both have a default route.
constexpr int kEthernetMetric = 100;
constexpr int kWifiMetric = 600;

sdbus::Error error(const std::string &name, const std::string &message)
{
    return sdbus::Error(sdbus::Error::Name { std::string("vn.sparc.esca.Settings1.Error.") + name }, message);
}

/// Handler failures as typed D-Bus errors instead of sdbus's generic one.
template <typename F>
auto guarded(F &&f) -> decltype(f())
{
    try {
        return f();
    } catch (const sdbus::Error &e) {
        throw error("Failed", e.getMessage());
    } catch (const std::invalid_argument &e) {
        throw error("InvalidArgument", e.what());
    } catch (const std::exception &e) {
        throw error("Unavailable", e.what());
    }
}

template <typename R>
std::shared_ptr<R> share(R &&r)
{
    return std::make_shared<R>(std::move(r));
}

/// A Done callback that answers a pending method call.
std::function<void(const std::string &, const std::string &)> reply_to(std::shared_ptr<sdbus::Result<>> r)
{
    return [r](const std::string &err, const std::string &msg) {
        if (err.empty())
            r->returnResults();
        else
            r->returnError(error(err, msg));
    };
}

IpConfig parse_ip_config(const std::string &text)
{
    const json j = json::parse(text, nullptr, false);
    if (!j.is_object())
        throw std::invalid_argument("the IP configuration is not a JSON object");
    return ip_config_from_json(j);
}

std::string trim(const std::string &s)
{
    const size_t a = s.find_first_not_of(" \t\r");
    const size_t b = s.find_last_not_of(" \t\r");
    return a == std::string::npos ? std::string() : s.substr(a, b - a + 1);
}

} // namespace

bool Config::load(const std::string &path, std::string &err)
{
    std::ifstream in(path);
    if (!in)
        return true; // defaults
    std::string line;
    int n = 0;
    while (std::getline(in, line)) {
        ++n;
        line = trim(line.substr(0, line.find('#')));
        if (line.empty())
            continue;
        const size_t eq = line.find('=');
        if (eq == std::string::npos) {
            err = path + ":" + std::to_string(n) + ": expected key = value";
            return false;
        }
        const std::string key = trim(line.substr(0, eq)), value = trim(line.substr(eq + 1));
        if (key == "wifi_interface") wifi_interface = value;
        else if (key == "ethernet_interface") ethernet_interface = value;
        else if (key == "wifi_country") wifi_country = value;
        else if (key == "wpa_config") wpa_config = value;
        else if (key == "network_dir") network_dir = value;
        else if (key == "log_level") log_level = value;
        else ESCA_LOG_WARN("%s:%d: unknown key \"%s\"", path.c_str(), n, key.c_str());
    }
    return true;
}

SettingsService::SettingsService(sdbus::IConnection &connection, Loop &loop, Config config)
    : connection_(connection), loop_(loop), config_(std::move(config)),
      networkd_(connection, config_.network_dir),
      wifi_(connection, loop, networkd_, config_.wifi_interface, config_.wpa_config, config_.wifi_country),
      bluetooth_(connection, loop, std::string(kPath) + "/BluetoothAgent")
{
}

void SettingsService::start()
{
    register_object();

    // Wi-Fi has no .network file in the image: give it DHCP, once.
    networkd_.ensure(config_.wifi_interface, IpConfig {}, kWifiMetric);

    auto emit = [this](const char *iface, const char *signal) {
        return [this, iface, signal] { object_->emitSignal(signal).onInterface(iface); };
    };
    wifi_.on_change(emit(kWifi, "Changed"));
    wifi_.on_networks(emit(kWifi, "NetworksChanged"));
    bluetooth_.on_change(emit(kBluetooth, "Changed"));
    bluetooth_.on_devices(emit(kBluetooth, "DevicesChanged"));
    bluetooth_.on_pairing_request([this](const json &request) {
        object_->emitSignal("PairingRequest").onInterface(kBluetooth).withArguments(request.dump());
    });
    // Addresses and carrier come from networkd, for both pages.
    networkd_.on_change([this, wifi = emit(kWifi, "Changed"), eth = emit(kEthernet, "Changed")] {
        loop_.coalesce("net-wifi", std::chrono::milliseconds(300), wifi);
        loop_.coalesce("net-eth", std::chrono::milliseconds(300), eth);
    });

    wifi_.start();
    bluetooth_.start();
}

void SettingsService::register_object()
{
    object_ = sdbus::createObject(connection_, sdbus::ObjectPath(kPath));

    // --- Wi-Fi ----------------------------------------------------------------
    object_->addVTable(
        sdbus::registerMethod("GetState").withOutputParamNames("state").implementedAs([this] {
            return guarded([&] { return wifi_.state().dump(); });
        }),
        sdbus::registerMethod("GetNetworks").withOutputParamNames("networks").implementedAs([this] {
            return guarded([&] { return wifi_.networks().dump(); });
        }),
        sdbus::registerMethod("SetEnabled").withInputParamNames("enabled").implementedAs([this](bool on) {
            guarded([&] { wifi_.set_enabled(on); });
        }),
        sdbus::registerMethod("Scan").implementedAs([this] { guarded([&] { wifi_.scan(); }); }),
        sdbus::registerMethod("Connect")
            .withInputParamNames("ssid", "passphrase", "hidden")
            .implementedAs([this](sdbus::Result<> &&r, std::string ssid, std::string passphrase, bool hidden) {
                wifi_.connect(ssid, passphrase, hidden, reply_to(share(std::move(r))));
            }),
        sdbus::registerMethod("Disconnect").implementedAs([this] { guarded([&] { wifi_.disconnect(); }); }),
        sdbus::registerMethod("Forget").withInputParamNames("ssid").withOutputParamNames("forgotten")
            .implementedAs([this](std::string ssid) { return guarded([&] { return wifi_.forget(ssid); }); }),
        sdbus::registerMethod("SetIpConfig").withInputParamNames("config").implementedAs([this](std::string text) {
            guarded([&] { networkd_.apply(config_.wifi_interface, parse_ip_config(text), kWifiMetric); });
        }),
        sdbus::registerSignal("Changed"),
        sdbus::registerSignal("NetworksChanged"))
        .forInterface(kWifi);

    // --- Ethernet -----------------------------------------------------------------
    object_->addVTable(
        sdbus::registerMethod("GetState").withOutputParamNames("state").implementedAs([this] {
            return guarded([&] {
                const LinkInfo l = networkd_.link(config_.ethernet_interface);
                json s = { { "present", l.present }, { "interface", config_.ethernet_interface }, { "link", l.to_json() },
                           { "ipConfig", to_json(networkd_.saved(config_.ethernet_interface)) } };
                return s.dump();
            });
        }),
        sdbus::registerMethod("SetIpConfig").withInputParamNames("config").implementedAs([this](std::string text) {
            guarded([&] { networkd_.apply(config_.ethernet_interface, parse_ip_config(text), kEthernetMetric); });
        }),
        sdbus::registerSignal("Changed"))
        .forInterface(kEthernet);

    // --- Bluetooth -----------------------------------------------------------------
    object_->addVTable(
        sdbus::registerMethod("GetState").withOutputParamNames("state").implementedAs([this] {
            return guarded([&] { return bluetooth_.state().dump(); });
        }),
        sdbus::registerMethod("GetDevices").withOutputParamNames("devices").implementedAs([this] {
            return guarded([&] { return bluetooth_.devices().dump(); });
        }),
        sdbus::registerMethod("SetPowered").withInputParamNames("powered").implementedAs([this](bool on) {
            guarded([&] { bluetooth_.set_powered(on); });
        }),
        sdbus::registerMethod("SetDiscoverable").withInputParamNames("discoverable").implementedAs([this](bool on) {
            guarded([&] { bluetooth_.set_discoverable(on); });
        }),
        sdbus::registerMethod("StartDiscovery").implementedAs([this] { guarded([&] { bluetooth_.start_discovery(); }); }),
        sdbus::registerMethod("StopDiscovery").implementedAs([this] { guarded([&] { bluetooth_.stop_discovery(); }); }),
        sdbus::registerMethod("Pair").withInputParamNames("address").implementedAs(
            [this](sdbus::Result<> &&r, std::string address) { bluetooth_.pair(address, reply_to(share(std::move(r)))); }),
        sdbus::registerMethod("Connect").withInputParamNames("address").implementedAs(
            [this](sdbus::Result<> &&r, std::string address) { bluetooth_.connect(address, reply_to(share(std::move(r)))); }),
        sdbus::registerMethod("Disconnect").withInputParamNames("address").implementedAs(
            [this](sdbus::Result<> &&r, std::string address) {
                bluetooth_.disconnect(address, reply_to(share(std::move(r))));
            }),
        sdbus::registerMethod("Remove").withInputParamNames("address").withOutputParamNames("removed")
            .implementedAs([this](std::string address) { return guarded([&] { return bluetooth_.remove(address); }); }),
        sdbus::registerMethod("AnswerPairing").withInputParamNames("address", "accept", "value")
            .withOutputParamNames("answered")
            .implementedAs([this](std::string address, bool accept, std::string value) {
                return guarded([&] { return bluetooth_.answer(address, accept, value); });
            }),
        sdbus::registerSignal("Changed"),
        sdbus::registerSignal("DevicesChanged"),
        sdbus::registerSignal("PairingRequest").withParameters<std::string>("request"))
        .forInterface(kBluetooth);
}

} // namespace esca::settings
