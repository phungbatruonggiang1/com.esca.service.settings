#include "networkd.hpp"

#include <filesystem>
#include <fstream>
#include <sstream>
#include <stdexcept>

#include "logger.hpp"

namespace esca::settings {

using nlohmann::json;

namespace {

const sdbus::ServiceName kNetworkd { "org.freedesktop.network1" };
const sdbus::ObjectPath kManagerPath { "/org/freedesktop/network1" };
constexpr const char *kManager = "org.freedesktop.network1.Manager";

int read_speed(const std::string &ifname)
{
    std::ifstream in("/sys/class/net/" + ifname + "/speed");
    int speed = 0;
    return (in >> speed) && speed > 0 ? speed : 0;
}

} // namespace

Networkd::Networkd(sdbus::IConnection &connection, std::string config_dir)
    : connection_(connection), dir_(std::move(config_dir)),
      manager_(sdbus::createProxy(connection, kNetworkd, kManagerPath))
{
    connection_.addMatch("type='signal',sender='org.freedesktop.network1',"
                         "interface='org.freedesktop.DBus.Properties',member='PropertiesChanged'",
                         [this](sdbus::Message) {
                             if (changed_)
                                 changed_();
                         });
}

LinkInfo Networkd::link(const std::string &ifname)
{
    std::string text;
    try {
        manager_->callMethod("Describe").onInterface(kManager).storeResultsTo(text);
    } catch (const sdbus::Error &e) {
        ESCA_LOG_WARN("networkd Describe failed: %s", e.getMessage().c_str());
        LinkInfo none;
        none.name = ifname;
        return none;
    }
    LinkInfo l = parse_link(json::parse(text, nullptr, false), ifname);
    if (l.present && l.type == "ether")
        l.speed_mbps = read_speed(ifname);
    return l;
}

IpConfig Networkd::saved(const std::string &ifname) const
{
    std::ifstream in(dir_ + "/" + network_file_name(ifname));
    if (!in)
        return IpConfig {};
    std::stringstream text;
    text << in.rdbuf();
    return parse_network_file(text.str());
}

void Networkd::apply(const std::string &ifname, const IpConfig &config, int metric)
{
    const std::string why = validate(config);
    if (!why.empty())
        throw std::invalid_argument(why);
    std::error_code ec;
    std::filesystem::create_directories(dir_, ec);
    const std::string path = dir_ + "/" + network_file_name(ifname);
    {
        std::ofstream out(path + ".tmp", std::ios::trunc);
        out << render_network_file(ifname, config, metric);
        if (!out)
            throw std::runtime_error("cannot write " + path);
    }
    std::filesystem::rename(path + ".tmp", path, ec);
    if (ec)
        throw std::runtime_error("cannot write " + path + ": " + ec.message());

    manager_->callMethod("Reload").onInterface(kManager);
    const LinkInfo l = link(ifname);
    if (l.present)
        manager_->callMethod("ReconfigureLink").onInterface(kManager).withArguments(l.index);
    ESCA_LOG_INFO("%s: %s", ifname.c_str(), config.method == IpConfig::Method::Dhcp ? "DHCP" : config.address.c_str());
}

void Networkd::ensure(const std::string &ifname, const IpConfig &config, int metric)
{
    if (std::filesystem::exists(dir_ + "/" + network_file_name(ifname)))
        return;
    try {
        apply(ifname, config, metric);
    } catch (const std::exception &e) {
        ESCA_LOG_WARN("%s: %s", ifname.c_str(), e.what());
    }
}

} // namespace esca::settings
