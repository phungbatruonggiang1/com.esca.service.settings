// The parts of esca-settingsd that decide things without a bus: IP
// configuration files, the Wi-Fi list, and reading networkd's state.

#include "check.hpp"
#include "ip_config.hpp"
#include "networkd.hpp"
#include "wifi_model.hpp"

using namespace esca::settings;
using nlohmann::json;

TEST("static IP: validated, written, read back")
{
    IpConfig c = ip_config_from_json(json { { "method", "static" }, { "address", " 192.168.1.50/24 " },
                                            { "gateway", "192.168.1.1" }, { "dns", { "8.8.8.8", "1.1.1.1" } } });
    CHECK(validate(c).empty());
    const std::string file = render_network_file("eth0", c, 100);
    CHECK(file.find("Name=eth0") != std::string::npos);
    CHECK(file.find("Address=192.168.1.50/24") != std::string::npos);
    CHECK(file.find("DHCP=") == std::string::npos);
    CHECK(parse_network_file(file) == c);
    CHECK(network_file_name("eth0") == "20-esca-eth0.network");
}

TEST("DHCP with own DNS: DHCP's DNS is not used")
{
    IpConfig c;
    c.dns = { "9.9.9.9" };
    const std::string file = render_network_file("wlan0", c, 600);
    CHECK(file.find("DHCP=yes") != std::string::npos);
    CHECK(file.find("UseDNS=no") != std::string::npos);
    CHECK(file.find("RouteMetric=600") != std::string::npos);
    CHECK(parse_network_file(file) == c);
}

TEST("bad static configurations are refused with a reason")
{
    auto bad = [](json j) { return !validate(ip_config_from_json(j)).empty(); };
    CHECK(bad({ { "method", "static" }, { "address", "192.168.1.50" } }));        // no prefix
    CHECK(bad({ { "method", "static" }, { "address", "192.168.1.500/24" } }));    // not an address
    CHECK(bad({ { "method", "static" }, { "address", "192.168.1.5/33" } }));      // prefix
    CHECK(bad({ { "method", "static" }, { "address", "10.0.0.2/8" }, { "gateway", "x" } }));
    CHECK(bad({ { "method", "dhcp" }, { "dns", { "dns.google" } } }));
    CHECK(!bad({ { "method", "static" }, { "address", "10.0.0.2/8" } }));      // no gateway is fine
    CHECK(!bad({ { "method", "dhcp" }, { "dns", { "2001:4860:4860::8888" } } }));
}

TEST("an unknown or hand-written file reads as DHCP")
{
    CHECK(parse_network_file("").method == IpConfig::Method::Dhcp);
    CHECK(parse_network_file("[Match]\nName=eth0\n[Network]\nDHCP=ipv4\n").method == IpConfig::Method::Dhcp);
}

TEST("signal strength in percent")
{
    CHECK(signal_percent(-95) == 0);
    CHECK(signal_percent(-90) == 0);
    CHECK(signal_percent(-60) == 50);
    CHECK(signal_percent(-30) == 100);
    CHECK(signal_percent(-10) == 100);
}

TEST("security from the BSS information elements")
{
    CHECK(security_of(false, {}, {}) == "open");
    CHECK(security_of(true, {}, {}) == "wep");
    CHECK(security_of(true, { "wpa-psk" }, {}) == "wpa2");
    CHECK(security_of(true, {}, { "wpa-psk" }) == "wpa2");
    CHECK(security_of(true, { "sae" }, {}) == "wpa3");
    CHECK(security_of(true, { "wpa-psk", "sae" }, {}) == "wpa2/wpa3");
    CHECK(security_of(true, { "wpa-eap" }, {}) == "enterprise");
}

TEST("one entry per name: the connected one, else the strongest; hidden ones dropped")
{
    std::vector<AccessPoint> aps = {
        { "Home", "AA:00:00:00:00:01", -70, 2412, "wpa2", true, false },
        { "Home", "AA:00:00:00:00:02", -50, 5180, "wpa2", false, false },
        { "Cafe", "BB:00:00:00:00:01", -40, 2437, "open", false, false },
        { "", "CC:00:00:00:00:01", -30, 2462, "wpa2", false, false },
        { "Office", "DD:00:00:00:00:01", -80, 2412, "wpa2", false, true },
        { "Office", "DD:00:00:00:00:02", -45, 2412, "wpa2", false, false },
    };
    const auto out = merge_by_ssid(aps);
    CHECK(out.size() == 3);
    CHECK(out[0].ssid == "Office" && out[0].connected && out[0].bssid == "DD:00:00:00:00:01");
    CHECK(out[1].ssid == "Home" && out[1].saved && out[1].bssid == "AA:00:00:00:00:02");
    CHECK(out[2].ssid == "Cafe");
}

TEST("passwords are checked against the network's security")
{
    CHECK(check_passphrase("open", "").empty());
    CHECK(!check_passphrase("wpa2", "short").empty());
    CHECK(check_passphrase("wpa2", "longenough").empty());
    CHECK(check_passphrase("wpa3", std::string(64, 'a')).empty());   // 64 hex digits: a raw key
    CHECK(!check_passphrase("wpa2", std::string(64, 'z')).empty());
    CHECK(!check_passphrase("enterprise", "whatever1").empty());
    CHECK(check_passphrase("wep", "abcde").empty());
}

TEST("wpa_supplicant states, as the app names them")
{
    CHECK(wifi_state_name("completed") == "connected");
    CHECK(wifi_state_name("4way_handshake") == "connecting");
    CHECK(wifi_state_name("scanning") == "scanning");
    CHECK(wifi_state_name("inactive") == "disconnected");
    CHECK(wifi_state_name("interface_disabled") == "disconnected");
}

TEST("a link out of networkd's Describe()")
{
    // Abridged from `busctl call org.freedesktop.network1 ... Describe`.
    const json describe = json::parse(R"({
      "Interfaces": [
        {"Index": 1, "Name": "lo", "Type": "loopback"},
        {"Index": 2, "Name": "eth0", "Type": "ether", "OperationalState": "routable",
         "CarrierState": "carrier", "HardwareAddress": [220, 166, 50, 1, 2, 3],
         "Addresses": [
           {"Family": 2, "Address": [192, 168, 1, 9], "PrefixLength": 24, "ConfigSource": "DHCPv4"},
           {"Family": 10, "Address": [254, 128, 0, 0, 0, 0, 0, 0, 1, 2, 3, 4, 5, 6, 7, 8], "PrefixLength": 64}
         ],
         "Routes": [
           {"Family": 2, "Destination": [192, 168, 1, 0], "DestinationPrefixLength": 24},
           {"Family": 2, "Destination": [0, 0, 0, 0], "DestinationPrefixLength": 0, "Gateway": [192, 168, 1, 1]}
         ],
         "DNS": [{"Family": 2, "Address": [192, 168, 1, 1]}]}
      ]})");
    const LinkInfo l = parse_link(describe, "eth0");
    CHECK(l.present && l.index == 2 && l.type == "ether");
    CHECK(l.mac == "DC:A6:32:01:02:03");
    CHECK(l.addresses.size() == 1 && l.addresses[0] == "192.168.1.9/24"); // fe80:: left out
    CHECK(l.gateway == "192.168.1.1");
    CHECK(l.dns.size() == 1 && l.dns[0] == "192.168.1.1");
    CHECK(l.config_source == "dhcp");
    CHECK(l.to_json()["carrier"] == true);
    CHECK(!parse_link(describe, "wlan0").present);
}

int main() { return check::run_all(); }
