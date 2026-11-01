#include "rfkill.hpp"

#include <fcntl.h>
#include <linux/rfkill.h>
#include <unistd.h>

#include <filesystem>
#include <fstream>

namespace esca::settings::rfkill {

namespace {

std::string read_line(const std::filesystem::path &p)
{
    std::ifstream in(p);
    std::string s;
    std::getline(in, s);
    return s;
}

template <typename F>
bool any_of_type(const std::string &type, F &&f)
{
    std::error_code ec;
    for (const auto &e : std::filesystem::directory_iterator("/sys/class/rfkill", ec))
        if (read_line(e.path() / "type") == type && f(e.path()))
            return true;
    return false;
}

} // namespace

bool present(const std::string &type)
{
    return any_of_type(type, [](const std::filesystem::path &) { return true; });
}

bool blocked(const std::string &type)
{
    return any_of_type(type, [](const std::filesystem::path &p) { return read_line(p / "soft") == "1"; });
}

bool set_blocked(const std::string &type, bool block)
{
    rfkill_event ev {};
    ev.op = RFKILL_OP_CHANGE_ALL;
    ev.soft = block ? 1 : 0;
    if (type == "wlan")
        ev.type = RFKILL_TYPE_WLAN;
    else if (type == "bluetooth")
        ev.type = RFKILL_TYPE_BLUETOOTH;
    else
        return false;
    const int fd = open("/dev/rfkill", O_WRONLY | O_CLOEXEC);
    if (fd < 0)
        return false;
    const bool ok = write(fd, &ev, sizeof(ev)) == static_cast<ssize_t>(sizeof(ev));
    close(fd);
    return ok;
}

} // namespace esca::settings::rfkill
