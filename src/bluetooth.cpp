#include "bluetooth.hpp"

#include <algorithm>

#include "logger.hpp"

namespace esca::settings {

using nlohmann::json;
using namespace std::chrono_literals;

namespace {

const sdbus::ServiceName kBluez { "org.bluez" };
constexpr const char *kAdapter = "org.bluez.Adapter1";
constexpr const char *kDevice = "org.bluez.Device1";
constexpr const char *kAgent = "org.bluez.Agent1";
constexpr const char *kAgentManager = "org.bluez.AgentManager1";

constexpr auto kDiscoveryTime = 30s;
constexpr auto kAnswerTime = 60s;

template <typename R>
std::shared_ptr<R> share(R &&r)
{
    return std::make_shared<R>(std::move(r));
}

sdbus::Error bluez_error(const char *name, const std::string &message)
{
    return sdbus::Error(sdbus::Error::Name { std::string("org.bluez.Error.") + name }, message);
}

/// BlueZ's error names, in the app's: the reason a call failed.
std::string short_error(const sdbus::Error &e)
{
    const std::string n = e.getName();
    if (n.find("AuthenticationFailed") != std::string::npos || n.find("AuthenticationRejected") != std::string::npos
        || n.find("AuthenticationCanceled") != std::string::npos)
        return "Rejected";
    if (n.find("Timeout") != std::string::npos || n.find("NoReply") != std::string::npos)
        return "Timeout";
    if (n.find("InProgress") != std::string::npos)
        return "Busy";
    return "Failed";
}

json device_json(const Props &p)
{
    json d = { { "address", prop<std::string>(p, "Address") },
               { "name", prop<std::string>(p, "Alias", prop<std::string>(p, "Address")) },
               { "named", p.count("Name") != 0 },
               { "icon", prop<std::string>(p, "Icon") },
               { "paired", prop<bool>(p, "Paired") || prop<bool>(p, "Bonded") },
               { "connected", prop<bool>(p, "Connected") },
               { "trusted", prop<bool>(p, "Trusted") },
               { "rssi", nullptr } };
    if (p.count("RSSI"))
        d["rssi"] = prop<int16_t>(p, "RSSI");
    return d;
}

} // namespace

Bluetooth::Bluetooth(sdbus::IConnection &connection, Loop &loop, std::string agent_path)
    : connection_(connection), loop_(loop), agent_path_(std::move(agent_path))
{
}

void Bluetooth::start()
{
    root_ = sdbus::createProxy(connection_, kBluez, sdbus::ObjectPath("/"));
    connection_.addMatch("type='signal',sender='org.bluez',interface='org.freedesktop.DBus.Properties',"
                         "member='PropertiesChanged'",
                         [this](sdbus::Message msg) {
                             std::string iface;
                             msg >> iface;
                             if (iface == kAdapter)
                                 notify_state();
                             else if (iface == kDevice)
                                 notify_devices();
                         });
    connection_.addMatch("type='signal',sender='org.bluez',interface='org.freedesktop.DBus.ObjectManager'",
                         [this](sdbus::Message) {
                             // The adapter can appear late (the UART is
                             // attached after boot) and devices come and go.
                             if (adapter_path_.empty())
                                 loop_.post([this] { find_adapter(); });
                             notify_devices();
                         });
    connection_.addMatch("type='signal',sender='org.freedesktop.DBus',member='NameOwnerChanged',arg0='org.bluez'",
                         [this](sdbus::Message msg) {
                             std::string name, old_owner, new_owner;
                             msg >> name >> old_owner >> new_owner;
                             loop_.post([this, back = !new_owner.empty()] {
                                 adapter_.reset();
                                 adapter_path_.clear();
                                 if (back)
                                     find_adapter();
                                 notify_state();
                             });
                         });
    register_agent();
    find_adapter();
}

void Bluetooth::register_agent()
{
    if (agent_)
        return;
    agent_ = sdbus::createObject(connection_, sdbus::ObjectPath(agent_path_));
    agent_->addVTable(
        sdbus::registerMethod("Release").implementedAs([] {}),
        sdbus::registerMethod("RequestPinCode")
            .implementedAs([this](sdbus::Result<std::string> &&r, sdbus::ObjectPath device) {
                ask(device, "pin", "", Question { share(std::move(r)) });
            }),
        sdbus::registerMethod("DisplayPinCode").implementedAs([this](sdbus::ObjectPath device, std::string pin) {
            if (pairing_request_)
                pairing_request_(json { { "address", prop<std::string>(device_props(device), "Address") },
                                        { "kind", "display-pin" },
                                        { "value", pin } });
        }),
        sdbus::registerMethod("RequestPasskey")
            .implementedAs([this](sdbus::Result<uint32_t> &&r, sdbus::ObjectPath device) {
                ask(device, "passkey", "", Question { share(std::move(r)) });
            }),
        sdbus::registerMethod("DisplayPasskey")
            .implementedAs([this](sdbus::ObjectPath device, uint32_t passkey, uint16_t) {
                char code[8];
                std::snprintf(code, sizeof(code), "%06u", passkey);
                if (pairing_request_)
                    pairing_request_(json { { "address", prop<std::string>(device_props(device), "Address") },
                                            { "kind", "display-passkey" },
                                            { "value", code } });
            }),
        sdbus::registerMethod("RequestConfirmation")
            .implementedAs([this](sdbus::Result<> &&r, sdbus::ObjectPath device, uint32_t passkey) {
                char code[8];
                std::snprintf(code, sizeof(code), "%06u", passkey);
                ask(device, "confirm", code, Question { share(std::move(r)) });
            }),
        // Pairing was started from the Settings app, so the user already
        // said yes to the device itself and to its services.
        sdbus::registerMethod("RequestAuthorization").implementedAs([](sdbus::ObjectPath) {}),
        sdbus::registerMethod("AuthorizeService").implementedAs([](sdbus::ObjectPath, std::string) {}),
        sdbus::registerMethod("Cancel").implementedAs([this] {
            for (auto &[path, q] : questions_)
                loop_.cancel(q.timer);
            questions_.clear();
            if (pairing_request_)
                pairing_request_(json { { "kind", "cancel" } });
        }))
        .forInterface(kAgent);
}

void Bluetooth::announce_agent()
{
    try {
        auto manager = sdbus::createProxy(connection_, kBluez, sdbus::ObjectPath("/org/bluez"));
        manager->callMethod("RegisterAgent").onInterface(kAgentManager).withArguments(sdbus::ObjectPath(agent_path_),
                                                                                       std::string("KeyboardDisplay"));
        manager->callMethod("RequestDefaultAgent").onInterface(kAgentManager).withArguments(sdbus::ObjectPath(agent_path_));
    } catch (const sdbus::Error &e) {
        ESCA_LOG_WARN("Bluetooth agent: %s", e.getMessage().c_str());
    }
}

void Bluetooth::find_adapter()
{
    ManagedObjects objects;
    try {
        root_->callMethod("GetManagedObjects").onInterface("org.freedesktop.DBus.ObjectManager").storeResultsTo(objects);
    } catch (const sdbus::Error &e) {
        ESCA_LOG_WARN("BlueZ: %s", e.getMessage().c_str());
        return;
    }
    for (const auto &[path, ifaces] : objects) {
        if (ifaces.count(kAdapter)) {
            adapter_path_ = path;
            adapter_ = sdbus::createProxy(connection_, kBluez, path);
            ESCA_LOG_INFO("Bluetooth adapter %s", path.c_str());
            // An agent registered before bluetoothd restarted is gone.
            announce_agent();
            notify_state();
            return;
        }
    }
}

void Bluetooth::notify_state()
{
    if (changed_)
        loop_.coalesce("bt-state", 200ms, changed_);
}

void Bluetooth::notify_devices()
{
    // RSSI updates arrive several times a second while discovering.
    if (devices_changed_)
        loop_.coalesce("bt-devices", 500ms, devices_changed_);
}

json Bluetooth::state()
{
    json s = { { "present", present() } };
    if (!adapter_)
        return s;
    try {
        const Props p = get_all(*adapter_, kAdapter);
        s["address"] = prop<std::string>(p, "Address");
        s["name"] = prop<std::string>(p, "Alias", prop<std::string>(p, "Name"));
        s["powered"] = prop<bool>(p, "Powered");
        s["discoverable"] = prop<bool>(p, "Discoverable");
        s["discovering"] = prop<bool>(p, "Discovering");
    } catch (const sdbus::Error &e) {
        s["error"] = e.getMessage();
    }
    return s;
}

json Bluetooth::devices()
{
    json out = json::array();
    if (!adapter_)
        return out;
    ManagedObjects objects;
    root_->callMethod("GetManagedObjects").onInterface("org.freedesktop.DBus.ObjectManager").storeResultsTo(objects);
    for (const auto &[path, ifaces] : objects) {
        const auto d = ifaces.find(kDevice);
        if (d == ifaces.end() || prop<sdbus::ObjectPath>(d->second, "Adapter") != adapter_path_)
            continue;
        out.push_back(device_json(d->second));
    }
    std::sort(out.begin(), out.end(), [](const json &a, const json &b) {
        if (a["connected"] != b["connected"])
            return a["connected"].get<bool>();
        if (a["paired"] != b["paired"])
            return a["paired"].get<bool>();
        const int ra = a["rssi"].is_number() ? a["rssi"].get<int>() : -200;
        const int rb = b["rssi"].is_number() ? b["rssi"].get<int>() : -200;
        if (ra != rb)
            return ra > rb;
        return a["name"].get<std::string>() < b["name"].get<std::string>();
    });
    return out;
}

void Bluetooth::set_powered(bool on)
{
    if (!adapter_)
        throw std::runtime_error("no Bluetooth adapter");
    adapter_->setProperty("Powered").onInterface(kAdapter).toValue(on);
    ESCA_LOG_INFO("Bluetooth %s", on ? "on" : "off");
}

void Bluetooth::set_discoverable(bool on)
{
    if (!adapter_)
        throw std::runtime_error("no Bluetooth adapter");
    adapter_->setProperty("Discoverable").onInterface(kAdapter).toValue(on);
}

void Bluetooth::start_discovery()
{
    if (!adapter_)
        throw std::runtime_error("no Bluetooth adapter");
    try {
        adapter_->callMethod("StartDiscovery").onInterface(kAdapter);
    } catch (const sdbus::Error &e) {
        if (std::string(e.getName()).find("InProgress") == std::string::npos)
            throw;
    }
    loop_.cancel(discovery_timer_);
    discovery_timer_ = loop_.after(kDiscoveryTime, [this] {
        discovery_timer_ = 0;
        try {
            stop_discovery();
        } catch (const std::exception &) {
        }
    });
}

void Bluetooth::stop_discovery()
{
    loop_.cancel(discovery_timer_);
    discovery_timer_ = 0;
    if (!adapter_)
        return;
    try {
        adapter_->callMethod("StopDiscovery").onInterface(kAdapter);
    } catch (const sdbus::Error &) {
        // not discovering: nothing to stop
    }
}

sdbus::ObjectPath Bluetooth::device_path(const std::string &address)
{
    // BlueZ names device objects after the address: dev_AA_BB_CC_DD_EE_FF.
    std::string a = address;
    std::transform(a.begin(), a.end(), a.begin(), [](char c) { return c == ':' ? '_' : static_cast<char>(std::toupper(c)); });
    return sdbus::ObjectPath(adapter_path_ + "/dev_" + a);
}

Props Bluetooth::device_props(const sdbus::ObjectPath &path)
{
    try {
        auto proxy = sdbus::createProxy(connection_, kBluez, path);
        return get_all(*proxy, kDevice);
    } catch (const sdbus::Error &) {
        return {};
    }
}

void Bluetooth::pair(const std::string &address, Done done)
{
    if (!adapter_)
        return done("Unavailable", "no Bluetooth adapter");
    const auto path = device_path(address);
    if (device_props(path).empty())
        return done("NotFound", "no such device; scan first");
    auto proxy = std::shared_ptr<sdbus::IProxy>(sdbus::createProxy(connection_, kBluez, path));
    ESCA_LOG_INFO("pairing %s", address.c_str());
    proxy->callMethodAsync("Pair").onInterface(kDevice).withTimeout(90s).uponReplyInvoke(
        [this, proxy, address, done](std::optional<sdbus::Error> error) {
            if (error && std::string(error->getName()).find("AlreadyExists") == std::string::npos) {
                ESCA_LOG_WARN("pairing %s: %s", address.c_str(), error->getMessage().c_str());
                return done(short_error(*error), error->getMessage());
            }
            // Trusted: it may reconnect by itself later without asking.
            try {
                proxy->setProperty("Trusted").onInterface(kDevice).toValue(true);
            } catch (const sdbus::Error &) {
            }
            connect(address, done);
        });
}

void Bluetooth::connect(const std::string &address, Done done)
{
    if (!adapter_)
        return done("Unavailable", "no Bluetooth adapter");
    auto proxy = std::shared_ptr<sdbus::IProxy>(sdbus::createProxy(connection_, kBluez, device_path(address)));
    proxy->callMethodAsync("Connect").onInterface(kDevice).withTimeout(45s).uponReplyInvoke(
        [proxy, address, done](std::optional<sdbus::Error> error) {
            if (error) {
                // Paired but nothing to connect to (a phone without a
                // profile we offer) is not a failure of pairing.
                ESCA_LOG_WARN("connecting %s: %s", address.c_str(), error->getMessage().c_str());
                return done(short_error(*error), error->getMessage());
            }
            ESCA_LOG_INFO("connected %s", address.c_str());
            done("", "");
        });
}

void Bluetooth::disconnect(const std::string &address, Done done)
{
    if (!adapter_)
        return done("Unavailable", "no Bluetooth adapter");
    auto proxy = std::shared_ptr<sdbus::IProxy>(sdbus::createProxy(connection_, kBluez, device_path(address)));
    proxy->callMethodAsync("Disconnect").onInterface(kDevice).withTimeout(20s).uponReplyInvoke(
        [proxy, done](std::optional<sdbus::Error> error) {
            if (error)
                return done(short_error(*error), error->getMessage());
            done("", "");
        });
}

bool Bluetooth::remove(const std::string &address)
{
    if (!adapter_)
        throw std::runtime_error("no Bluetooth adapter");
    const auto path = device_path(address);
    if (device_props(path).empty())
        return false;
    adapter_->callMethod("RemoveDevice").onInterface(kAdapter).withArguments(path);
    ESCA_LOG_INFO("forgot %s", address.c_str());
    return true;
}

void Bluetooth::ask(const sdbus::ObjectPath &device, const std::string &kind, const std::string &value, Question q)
{
    reject(device, "Canceled"); // a new question replaces an old one
    const Props p = device_props(device);
    q.timer = loop_.after(kAnswerTime, [this, device] { reject(device, "Canceled"); });
    questions_[device] = std::move(q);
    if (pairing_request_)
        pairing_request_(json { { "address", prop<std::string>(p, "Address") },
                                { "name", prop<std::string>(p, "Alias") },
                                { "kind", kind },
                                { "value", value } });
}

void Bluetooth::reject(const sdbus::ObjectPath &device, const char *error)
{
    const auto it = questions_.find(device);
    if (it == questions_.end())
        return;
    loop_.cancel(it->second.timer);
    std::visit([&](auto &r) { r->returnError(bluez_error(error, "declined")); }, it->second.result);
    questions_.erase(it);
}

bool Bluetooth::answer(const std::string &address, bool accept, const std::string &value)
{
    const auto path = device_path(address);
    const auto it = questions_.find(path);
    if (it == questions_.end())
        return false;
    if (!accept) {
        reject(path, "Rejected");
        return true;
    }
    loop_.cancel(it->second.timer);
    Question q = std::move(it->second);
    questions_.erase(it);
    if (auto *c = std::get_if<ConfirmResult>(&q.result)) {
        (*c)->returnResults();
    } else if (auto *pin = std::get_if<PinResult>(&q.result)) {
        (*pin)->returnResults(value);
    } else if (auto *pk = std::get_if<PasskeyResult>(&q.result)) {
        try {
            (*pk)->returnResults(static_cast<uint32_t>(std::stoul(value)));
        } catch (const std::exception &) {
            (*pk)->returnError(bluez_error("Rejected", "not a number"));
        }
    }
    return true;
}

} // namespace esca::settings
