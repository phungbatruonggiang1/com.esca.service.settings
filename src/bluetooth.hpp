// Bluetooth, through BlueZ (org.bluez): the adapter, the devices it sees
// and knows, and the pairing agent.
//
// Pairing goes through an agent BlueZ calls back into: "is 123456 the code
// on the other device?", "what PIN?". Those questions become a
// PairingRequest signal for the Settings app, and its AnswerPairing() call
// answers BlueZ; the loop keeps serving meanwhile.

#pragma once

#include <functional>
#include <map>
#include <memory>
#include <string>
#include <variant>

#include <nlohmann/json.hpp>
#include <sdbus-c++/sdbus-c++.h>

#include "dbus_util.hpp"
#include "loop.hpp"

namespace esca::settings {

class Bluetooth {
public:
    using Done = std::function<void(const std::string &error, const std::string &message)>;

    Bluetooth(sdbus::IConnection &connection, Loop &loop, std::string agent_path);

    void start();
    bool present() const { return !adapter_path_.empty(); }

    nlohmann::json state();
    nlohmann::json devices();
    void set_powered(bool on);
    void set_discoverable(bool on);
    /// Stops on its own after a while, as a phone's does.
    void start_discovery();
    void stop_discovery();
    void pair(const std::string &address, Done done);
    void connect(const std::string &address, Done done);
    void disconnect(const std::string &address, Done done);
    /// Unpairs and forgets. False if no such device.
    bool remove(const std::string &address);
    /// The user's answer to a PairingRequest. False if nothing was asked
    /// about that device.
    bool answer(const std::string &address, bool accept, const std::string &value);

    void on_change(std::function<void()> f) { changed_ = std::move(f); }
    void on_devices(std::function<void()> f) { devices_changed_ = std::move(f); }
    void on_pairing_request(std::function<void(const nlohmann::json &)> f) { pairing_request_ = std::move(f); }

private:
    using ConfirmResult = std::shared_ptr<sdbus::Result<>>;
    using PinResult = std::shared_ptr<sdbus::Result<std::string>>;
    using PasskeyResult = std::shared_ptr<sdbus::Result<uint32_t>>;
    struct Question {
        std::variant<ConfirmResult, PinResult, PasskeyResult> result;
        uint64_t timer = 0;
    };

    void find_adapter();
    void register_agent();
    void announce_agent();
    sdbus::ObjectPath device_path(const std::string &address);
    Props device_props(const sdbus::ObjectPath &path);
    void ask(const sdbus::ObjectPath &device, const std::string &kind, const std::string &value, Question q);
    void reject(const sdbus::ObjectPath &device, const char *error);
    void notify_state();
    void notify_devices();

    sdbus::IConnection &connection_;
    Loop &loop_;
    std::string agent_path_;
    std::unique_ptr<sdbus::IProxy> root_;
    std::unique_ptr<sdbus::IObject> agent_;
    std::string adapter_path_;
    std::unique_ptr<sdbus::IProxy> adapter_;
    uint64_t discovery_timer_ = 0;
    std::map<std::string, Question> questions_; ///< by device object path

    std::function<void()> changed_;
    std::function<void()> devices_changed_;
    std::function<void(const nlohmann::json &)> pairing_request_;
};

} // namespace esca::settings
