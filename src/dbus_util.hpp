// Small helpers for talking to other daemons' D-Bus APIs (wpa_supplicant,
// BlueZ, systemd-networkd), which describe their objects as a{sv}.

#pragma once

#include <map>
#include <string>
#include <vector>

#include <sdbus-c++/sdbus-c++.h>

namespace esca::settings {

using Props = std::map<std::string, sdbus::Variant>;
using ManagedObjects = std::map<sdbus::ObjectPath, std::map<std::string, Props>>;

inline Props get_all(sdbus::IProxy &proxy, const std::string &interface)
{
    Props props;
    proxy.callMethod("GetAll").onInterface("org.freedesktop.DBus.Properties").withArguments(interface).storeResultsTo(props);
    return props;
}

/// The value of `key` if present and of type T, otherwise `fallback`.
template <typename T>
T prop(const Props &props, const std::string &key, T fallback = T())
{
    const auto it = props.find(key);
    if (it == props.end() || !it->second.containsValueOfType<T>())
        return fallback;
    return it->second.get<T>();
}

inline std::string bytes_to_string(const std::vector<uint8_t> &b) { return std::string(b.begin(), b.end()); }

inline std::string mac_to_string(const std::vector<uint8_t> &b)
{
    static const char hex[] = "0123456789ABCDEF";
    std::string out;
    for (size_t i = 0; i < b.size(); ++i) {
        if (i)
            out += ':';
        out += hex[b[i] >> 4];
        out += hex[b[i] & 15];
    }
    return out;
}

} // namespace esca::settings
