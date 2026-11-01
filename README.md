# ESCA Settings Service

`esca-settingsd` puts the board on a network and pairs its Bluetooth
devices, for the Settings application. It runs as root and wraps three
system daemons behind one small D-Bus API.

    Settings app ─D-Bus─► esca-settingsd ─┬─► wpa_supplicant   (Wi-Fi)
                                          ├─► systemd-networkd (addresses)
                                          └─► BlueZ            (Bluetooth)

## D-Bus: `vn.sparc.esca.Settings1`

Object `/vn/sparc/esca/Settings1`, three interfaces:

| Interface | Methods | Signals |
| --- | --- | --- |
| `.Wifi` | `GetState`, `GetNetworks`, `SetEnabled`, `Scan`, `Connect(ssid, passphrase, hidden)`, `Disconnect`, `Forget`, `SetIpConfig` | `Changed`, `NetworksChanged` |
| `.Ethernet` | `GetState`, `SetIpConfig` | `Changed` |
| `.Bluetooth` | `GetState`, `GetDevices`, `SetPowered`, `SetDiscoverable`, `StartDiscovery`, `StopDiscovery`, `Pair`, `Connect`, `Disconnect`, `Remove`, `AnswerPairing` | `Changed`, `DevicesChanged`, `PairingRequest` |

Errors are named `vn.sparc.esca.Settings1.Error.<Name>`, for example
`WrongPassword` or `Timeout`, so the application can say what went wrong.
The bus policy admits root and the `settings` group.

## How each part works

- **Wi-Fi.** `Connect` adds and selects a network, then watches the
  supplicant's state: `completed` is success, two dropped handshakes are a
  wrong password, 30 s is a timeout. Every other saved network is enabled
  again afterwards and the configuration is saved
  (`/etc/wpa_supplicant/wpa_supplicant-wlan0.conf`).
- **Addresses.** DHCP or a static address for each link is written as
  `/etc/systemd/network/20-esca-<if>.network`, then networkd reloads and
  reconfigures the link. Ethernet routes take metric 100, Wi-Fi 600.
- **Bluetooth.** One agent, `KeyboardDisplay`. A pairing question
  (confirm a code, enter a PIN) becomes a `PairingRequest` signal and the
  application answers with `AnswerPairing`. Discovery stops by itself after
  30 s.
- **Radio switches** go through `/dev/rfkill`.

Everything runs on one thread with its own poll loop.

## Build and test

    cmake -B build && cmake --build build
    ./build/esca-settings-test

The tests cover what needs no bus: IP configuration, `.network` files,
link parsing, signal and security of scan results.

`/etc/esca/settings.conf`, the systemd unit and the bus policy are in
`data/`. Built by the `esca-settings` recipe in meta-sparc-esca.
