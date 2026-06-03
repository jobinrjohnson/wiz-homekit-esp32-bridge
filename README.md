# WiZ → Apple Home Bridge (ESP32 / HomeSpan)

An ESP32 firmware that **auto-discovers every Philips WiZ device on your LAN** and
exposes each one to the **Apple Home app** natively via [HomeSpan](https://github.com/HomeSpan/HomeSpan).
No Homebridge, no hub, no cloud — the ESP32 *is* the HomeKit bridge.

## Supported accessories

| WiZ device              | Detected from `moduleName` | HomeKit type | Characteristics                                  |
|-------------------------|----------------------------|--------------|--------------------------------------------------|
| Color (RGB) bulb/strip  | contains `RGB`             | Lightbulb    | On, Brightness, Hue, Saturation, ColorTemperature |
| Tunable-white bulb      | contains `TW`              | Lightbulb    | On, Brightness, ColorTemperature                 |
| Dimmable-white bulb     | contains `DW` (default)    | Lightbulb    | On, Brightness                                   |
| Smart plug / socket     | contains `SOCKET`          | Outlet       | On                                               |

## Requirements

- An **ESP32** board (any dev module).
- Arduino IDE with the **esp32 board package** (Espressif Systems) installed via Boards Manager.
- Two libraries (Arduino IDE → *Tools → Manage Libraries…*):
  - **HomeSpan** by Gregg E. Berman (v1.9.0+)
  - **ArduinoJson** by Benoit Blanchon (v7.x)

## Setup

1. Open `WiZHomeKitBridge/WiZHomeKitBridge.ino` in the Arduino IDE.
2. Edit the **USER CONFIG** block:
   ```cpp
   #define WIFI_SSID  "YOUR_WIFI_SSID"
   #define WIFI_PASS  "YOUR_WIFI_PASSWORD"
   ```
3. Select your ESP32 board and port, then **Upload**.
4. Open the Serial Monitor at **115200 baud** to watch WiFi connect and devices
   get discovered.

## Pairing with Apple Home

1. Open the **Home** app → **+** → **Add Accessory**.
2. Tap **More options…** and select **WiZ HomeKit Bridge**.
3. Enter the setup code: **`466-37-726`** (the HomeSpan default; change it via
   `PAIRING_CODE` in the sketch).
4. All discovered WiZ devices appear under the single bridge.

## Documentation

Full guides live in [`docs/`](docs/):

- [Flashing guide](docs/FLASHING.md) — IDE, libraries, upload, pairing.
- [Architecture](docs/ARCHITECTURE.md) — how it all fits together.
- [WiZ protocol reference](docs/WIZ_PROTOCOL.md) — the UDP/JSON wire format.
- [Customization](docs/CUSTOMIZATION.md) — settings, scenes, fans, fixed IPs.
- [Troubleshooting](docs/TROUBLESHOOTING.md) — when something won't work.

## How it works

- **Discovery** — two methods are used automatically: (1) a UDP `registration`
  **broadcast** on port `38899`, and (2) an **active unicast sweep** that pings
  every host on the local subnet. The sweep needs no broadcast, so devices are
  found even on routers/mesh systems that filter broadcast (the usual cause of
  "no devices found"). Each responder is queried with `getSystemConfig` to read
  its `moduleName` and MAC, which determines the HomeKit type.
- **Control** — Home-app changes trigger `update()`, which sends a WiZ
  `setPilot` command (state / dimming / temp / r,g,b).
- **Sync** — a round-robin poller calls `getPilot` on one device every
  ~2.5 s and pushes external changes (e.g. the WiZ app or a physical switch)
  back into HomeKit.
- **Runtime additions** — the network is re-scanned every 5 minutes; brand-new
  WiZ devices are added live via `homeSpan.updateDatabase()`.

## Tuning (top of the `.ino`)

| Macro            | Meaning                                             |
|------------------|-----------------------------------------------------|
| `MIN_BRIGHTNESS` | WiZ dimming floor (WiZ valid range is 10–100).      |
| `POLL_STEP_MS`   | Gap between polling successive devices.              |
| `REDISCOVER_MS`  | How often to re-scan for newly added devices.       |
| `WIZ_TIMEOUT_MS` | Per-request UDP response timeout.                   |
| `PAIRING_CODE`   | 8-digit HomeKit setup code.                         |

## Troubleshooting

- **No devices found** — ESP32 and WiZ devices must be on the **same subnet /
  VLAN**, and the bulbs must already work in the official WiZ app. Some
  routers/APs block broadcast traffic (check "AP/client isolation").
- **Colors look off** — WiZ RGB bulbs separate color vs. tunable-white modes;
  picking a color sends RGB, the temperature slider sends Kelvin. This is
  expected HomeKit/WiZ behavior.
- **Re-pair after big changes** — if you remove devices and want them gone from
  Home immediately, reset the ESP32's HomeKit pairing (HomeSpan CLI `H` over
  serial) and re-add the bridge.
```
