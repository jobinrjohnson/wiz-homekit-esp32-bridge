# Documentation

Detailed docs for the WiZ → Apple Home ESP32 bridge.

## Start here
1. [FLASHING.md](FLASHING.md) — install everything and get the firmware onto the ESP32.
2. [WIFI-PROVISIONING.md](WIFI-PROVISIONING.md) — set up WiFi from your phone, then pair in Home.
3. [HARDWARE.md](HARDWARE.md) — wire the reed switch (and add more sensors).

## Reference
| Doc | What's in it |
|-----|--------------|
| [ARCHITECTURE.md](ARCHITECTURE.md) | Boot/loop flow, control vs. sync paths, classes, value conversions. |
| [DISCOVERY.md](DISCOVERY.md) | How the subnet scan finds devices + the `discover_wiz.py` desktop tool. |
| [WIZ_PROTOCOL.md](WIZ_PROTOCOL.md) | The WiZ local UDP/JSON API (methods, params, module naming). |
| [STABLE-IDS.md](STABLE-IDS.md) | Why devices keep the same HomeKit identity across reboots. |
| [CUSTOMIZATION.md](CUSTOMIZATION.md) | Every tunable setting; how to extend. |
| [LIMITATIONS.md](LIMITATIONS.md) | Energy monitoring, WiZ scenes, full list of HomeKit service types. |
| [TROUBLESHOOTING.md](TROUBLESHOOTING.md) | Fixes for WiFi, discovery, pairing, sync, stability. |

## TL;DR of how it works
- HomeSpan runs a HomeKit **Bridge** on the ESP32. WiFi is provisioned from your
  phone (no hardcoded credentials).
- WiZ devices are found by **scanning every IP on the subnet** over UDP:38899
  (broadcast + unicast), then queried with `getSystemConfig` for their type.
- Each device gets a **permanent HomeKit ID derived from its MAC**, so the Home
  app's layout survives reboots.
- A **reed switch** on a GPIO is exposed as a Contact Sensor.
