# WiZ → Apple Home Bridge (ESP32 / HomeSpan)

An ESP32 firmware that **auto-discovers every Philips WiZ device on your LAN** and
exposes each one to the **Apple Home app** natively via
[HomeSpan](https://github.com/HomeSpan/HomeSpan) — no Homebridge, no hub, no
cloud. The ESP32 *is* the HomeKit bridge. It also exposes a local **magnetic reed
switch** (door/window contact sensor) wired to a GPIO.

## Features

- **Zero-config WiZ discovery** — scans the whole subnet over UDP (works even on
  routers that block broadcast).
- **All WiZ light/plug types** — color, tunable-white, dimmable, and smart plugs.
- **Phone-based WiFi setup** — no hardcoded credentials; provision from your
  iPhone via a setup Wi-Fi network.
- **Reboot-stable** — each device gets a permanent HomeKit ID derived from its
  MAC, so names/rooms/automations survive restarts.
- **Self-healing** — re-scans periodically; devices missed at boot or added later
  appear automatically.
- **Local hardware** — a reed switch on a GPIO shows up as a HomeKit Contact
  Sensor.

## Supported accessories

| WiZ device              | Detected from `moduleName` | HomeKit type | Characteristics                                  |
|-------------------------|----------------------------|--------------|--------------------------------------------------|
| Color (RGB) bulb/strip  | contains `RGB`             | Lightbulb    | On, Brightness, Hue, Saturation, ColorTemperature |
| Tunable-white bulb      | contains `TW`              | Lightbulb    | On, Brightness, ColorTemperature                 |
| Dimmable-white bulb     | contains `DW` (default)    | Lightbulb    | On, Brightness                                   |
| Smart plug / socket     | `SOCKET`/`PLUG`/`SWITCH`   | Outlet       | On                                               |
| Magnetic reed switch    | local GPIO                 | ContactSensor| Open / Closed                                    |

## Quick start

1. **Install** the Arduino IDE, the **esp32** board package, and the **HomeSpan**
   + **ArduinoJson** libraries → see [docs/FLASHING.md](docs/FLASHING.md).
2. **Flash** `WiZHomeKitBridge/WiZHomeKitBridge.ino` to your ESP32.
3. **Provision WiFi from your phone** — join the `WiZBridge-Setup` network and
   enter your home WiFi in the captive-portal page → see
   [docs/WIFI-PROVISIONING.md](docs/WIFI-PROVISIONING.md).
4. **Pair** in the Home app: Add Accessory → "More options…" → "WiZ HomeKit
   Bridge", code **466-37-726**. All WiZ devices appear under one bridge.

## Documentation

Everything lives in [`docs/`](docs/):

| Doc | What's in it |
|-----|--------------|
| [FLASHING.md](docs/FLASHING.md) | IDE, board package, libraries, upload, serial commands |
| [WIFI-PROVISIONING.md](docs/WIFI-PROVISIONING.md) | Phone-based WiFi setup (SoftAP), re-provisioning |
| [HARDWARE.md](docs/HARDWARE.md) | Reed-switch wiring, GPIO guidance, adding more sensors |
| [ARCHITECTURE.md](docs/ARCHITECTURE.md) | How the firmware is structured, data flow |
| [DISCOVERY.md](docs/DISCOVERY.md) | The subnet-scan discovery + the `discover_wiz.py` tool |
| [WIZ_PROTOCOL.md](docs/WIZ_PROTOCOL.md) | WiZ local UDP/JSON protocol reference |
| [STABLE-IDS.md](docs/STABLE-IDS.md) | Why/how device IDs stay stable across reboots |
| [CUSTOMIZATION.md](docs/CUSTOMIZATION.md) | All tunable settings and how to extend |
| [LIMITATIONS.md](docs/LIMITATIONS.md) | Energy monitoring, WiZ scenes, HomeKit service types |
| [TROUBLESHOOTING.md](docs/TROUBLESHOOTING.md) | When something won't work |

## Repository layout

```
homespan/
├── README.md
├── discover_wiz.py                 # desktop tool: find WiZ devices + capabilities
├── docs/                           # all documentation
└── WiZHomeKitBridge/
    └── WiZHomeKitBridge.ino        # the ESP32 firmware
```

## Requirements

- An **ESP32** dev board (2.4 GHz WiFi only).
- Arduino IDE + **esp32** board package (Espressif).
- Libraries: **HomeSpan** (≥ 2.0) and **ArduinoJson** (v7.x).
- WiZ devices on the **same subnet** as the ESP32 (they must already work in the
  official WiZ app).
