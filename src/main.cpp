// esca-settingsd: what the Settings app changes on the board -- Wi-Fi,
// Ethernet, Bluetooth -- done as root on its behalf.

#include <csignal>
#include <cstdio>
#include <string>

#include <sdbus-c++/sdbus-c++.h>

#include "logger.hpp"
#include "loop.hpp"
#include "service.hpp"

namespace {

esca::settings::Loop *g_loop = nullptr;

void handle_signal(int)
{
    if (g_loop)
        g_loop->quit();
}

int usage()
{
    std::fprintf(stderr,
                 "usage: esca-settingsd [options]\n"
                 "  --config FILE      settings (default /etc/esca/settings.conf)\n"
                 "  --log-level LEVEL  DEBUG, INFO, WARN, ERROR\n"
                 "  --session          use the session bus (development)\n");
    return 2;
}

} // namespace

int main(int argc, char **argv)
{
    using namespace esca::settings;

    std::string config_file = "/etc/esca/settings.conf";
    for (int i = 1; i + 1 < argc; ++i)
        if (std::string(argv[i]) == "--config")
            config_file = argv[i + 1];
    Config config;
    std::string error;
    if (!config.load(config_file, error)) {
        std::fprintf(stderr, "esca-settingsd: %s\n", error.c_str());
        return 1;
    }
    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        if (arg == "--config" && i + 1 < argc) ++i;
        else if (arg == "--log-level" && i + 1 < argc) config.log_level = argv[++i];
        else if (arg == "--session") config.session_bus = true;
        else return usage();
    }
    if (!log::set_threshold(config.log_level))
        ESCA_LOG_WARN("unknown log level \"%s\"", config.log_level.c_str());

    try {
        const sdbus::ServiceName name { SettingsService::kBusName };
        auto connection = config.session_bus ? sdbus::createSessionBusConnection(name)
                                             : sdbus::createSystemBusConnection(name);
        Loop loop(*connection);
        g_loop = &loop;
        std::signal(SIGINT, handle_signal);
        std::signal(SIGTERM, handle_signal);

        SettingsService service(*connection, loop, config);
        service.start();
        ESCA_LOG_INFO("ready on %s", SettingsService::kBusName);
        loop.run();
        ESCA_LOG_INFO("shutting down");
        return 0;
    } catch (const std::exception &e) {
        std::fprintf(stderr, "esca-settingsd: fatal: %s\n", e.what());
        return 1;
    }
}
