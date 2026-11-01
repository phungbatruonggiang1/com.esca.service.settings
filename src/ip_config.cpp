#include "ip_config.hpp"

#include <arpa/inet.h>

#include <sstream>

namespace esca::settings {

using nlohmann::json;

namespace {

bool is_ipv4(const std::string &s)
{
    in_addr a {};
    return inet_pton(AF_INET, s.c_str(), &a) == 1;
}

bool is_ip(const std::string &s)
{
    in6_addr a {};
    return is_ipv4(s) || inet_pton(AF_INET6, s.c_str(), &a) == 1;
}

std::string trim(const std::string &s)
{
    const size_t a = s.find_first_not_of(" \t\r");
    const size_t b = s.find_last_not_of(" \t\r");
    return a == std::string::npos ? std::string() : s.substr(a, b - a + 1);
}

} // namespace

IpConfig ip_config_from_json(const json &j)
{
    IpConfig c;
    c.method = j.value("method", "dhcp") == "static" ? IpConfig::Method::Static : IpConfig::Method::Dhcp;
    c.address = trim(j.value("address", ""));
    c.gateway = trim(j.value("gateway", ""));
    if (j.contains("dns") && j["dns"].is_array())
        for (const auto &d : j["dns"])
            if (d.is_string() && !trim(d.get<std::string>()).empty())
                c.dns.push_back(trim(d.get<std::string>()));
    return c;
}

json to_json(const IpConfig &c)
{
    return { { "method", c.method == IpConfig::Method::Static ? "static" : "dhcp" },
             { "address", c.address },
             { "gateway", c.gateway },
             { "dns", c.dns } };
}

std::string validate(const IpConfig &c)
{
    for (const auto &d : c.dns)
        if (!is_ip(d))
            return "\"" + d + "\" is not an IP address";
    if (c.method == IpConfig::Method::Dhcp)
        return "";
    const size_t slash = c.address.find('/');
    if (slash == std::string::npos)
        return "the address needs a prefix length, e.g. 192.168.1.50/24";
    const std::string ip = c.address.substr(0, slash);
    int prefix = -1;
    try {
        size_t used = 0;
        prefix = std::stoi(c.address.substr(slash + 1), &used);
        if (used != c.address.size() - slash - 1)
            prefix = -1;
    } catch (const std::exception &) {
    }
    if (!is_ipv4(ip) || prefix < 1 || prefix > 32)
        return "\"" + c.address + "\" is not an IPv4 address with a prefix length";
    if (!c.gateway.empty() && !is_ipv4(c.gateway))
        return "\"" + c.gateway + "\" is not an IPv4 gateway";
    return "";
}

std::string network_file_name(const std::string &ifname) { return "20-esca-" + ifname + ".network"; }

std::string render_network_file(const std::string &ifname, const IpConfig &c, int metric)
{
    std::ostringstream o;
    o << "# Written by esca-settingsd from the Settings app; edits are replaced.\n"
      << "[Match]\nName=" << ifname << "\n\n[Network]\n";
    if (c.method == IpConfig::Method::Dhcp) {
        o << "DHCP=yes\n";
    } else {
        o << "Address=" << c.address << "\n";
        if (!c.gateway.empty())
            o << "Gateway=" << c.gateway << "\n";
    }
    for (const auto &d : c.dns)
        o << "DNS=" << d << "\n";
    if (c.method == IpConfig::Method::Dhcp) {
        o << "\n[DHCP]\nRouteMetric=" << metric << "\nUseMTU=yes\nClientIdentifier=mac\n";
        // Our own DNS servers replace the ones DHCP offers.
        if (!c.dns.empty())
            o << "UseDNS=no\n";
    } else if (!c.gateway.empty()) {
        o << "\n[Route]\nGateway=" << c.gateway << "\nMetric=" << metric << "\n";
    }
    return o.str();
}

IpConfig parse_network_file(const std::string &text)
{
    IpConfig c;
    std::istringstream in(text);
    std::string line, section;
    bool dhcp = false;
    while (std::getline(in, line)) {
        line = trim(line);
        if (line.empty() || line[0] == '#')
            continue;
        if (line.front() == '[') {
            section = line;
            continue;
        }
        const size_t eq = line.find('=');
        if (eq == std::string::npos)
            continue;
        const std::string key = trim(line.substr(0, eq)), value = trim(line.substr(eq + 1));
        if (section != "[Network]")
            continue;
        if (key == "DHCP")
            dhcp = value == "yes" || value == "ipv4" || value == "true";
        else if (key == "Address")
            c.address = value;
        else if (key == "Gateway")
            c.gateway = value;
        else if (key == "DNS")
            c.dns.push_back(value);
    }
    c.method = (!dhcp && !c.address.empty()) ? IpConfig::Method::Static : IpConfig::Method::Dhcp;
    if (c.method == IpConfig::Method::Dhcp) {
        c.address.clear();
        c.gateway.clear();
    }
    return c;
}

} // namespace esca::settings
