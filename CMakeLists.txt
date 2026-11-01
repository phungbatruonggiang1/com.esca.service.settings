cmake_minimum_required(VERSION 3.16)
project(esca-settings VERSION 0.1.0 LANGUAGES CXX)

# esca-settingsd: Wi-Fi, Ethernet and Bluetooth for the Settings app, on
# D-Bus as vn.sparc.esca.Settings1.

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

include(GNUInstallDirs)
find_package(PkgConfig REQUIRED)
find_package(nlohmann_json 3 REQUIRED)
pkg_check_modules(SDBUSCPP REQUIRED IMPORTED_TARGET sdbus-c++)

# What decides things without a bus, shared by the daemon and the tests.
add_library(esca-settings-core STATIC
    src/ip_config.cpp
    src/link_info.cpp
    src/wifi_model.cpp
    src/logger.cpp)
target_include_directories(esca-settings-core PUBLIC src)
target_link_libraries(esca-settings-core PUBLIC nlohmann_json::nlohmann_json PkgConfig::SDBUSCPP)

add_executable(esca-settingsd
    src/main.cpp
    src/loop.cpp
    src/rfkill.cpp
    src/networkd.cpp
    src/wifi.cpp
    src/bluetooth.cpp
    src/service.cpp)
target_link_libraries(esca-settingsd PRIVATE esca-settings-core PkgConfig::SDBUSCPP)

add_executable(esca-settings-test test/settings_test.cpp)
target_include_directories(esca-settings-test PRIVATE test)
target_link_libraries(esca-settings-test PRIVATE esca-settings-core)

install(TARGETS esca-settingsd RUNTIME DESTINATION ${CMAKE_INSTALL_SBINDIR})
install(TARGETS esca-settings-test RUNTIME DESTINATION ${CMAKE_INSTALL_LIBEXECDIR}/esca-settings)
install(FILES data/vn.sparc.esca.Settings1.conf DESTINATION ${CMAKE_INSTALL_DATADIR}/dbus-1/system.d)
install(FILES data/esca-settingsd.service DESTINATION ${CMAKE_INSTALL_LIBDIR}/systemd/system)
install(FILES data/settings.conf DESTINATION ${CMAKE_INSTALL_SYSCONFDIR}/esca)
