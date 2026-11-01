// The kernel's radio switches (rfkill): Wi-Fi off means its radio is
// soft-blocked, as a laptop's airplane-mode key does. Bluetooth is switched
// through BlueZ's Powered property instead, which drives rfkill itself.

#pragma once

#include <string>

namespace esca::settings::rfkill {

/// Whether any radio of this type ("wlan", "bluetooth") is soft-blocked.
bool blocked(const std::string &type);
/// Whether any radio of this type exists at all.
bool present(const std::string &type);
/// Soft-blocks or unblocks every radio of this type. False on failure.
bool set_blocked(const std::string &type, bool block);

} // namespace esca::settings::rfkill
