# Architecture

How the firmware is put together and how data flows between Apple Home and your
WiZ devices.

## Big picture

```
            ┌──────────────────────────────────────────────────────┐
            │                      ESP32                            │
            │                                                       │
  Apple     │   HomeSpan (HAP server)        WiZ UDP client         │   WiZ
  Home  ◄───┼──►  Bridge accessory      ◄──►  (port 38899)     ◄────┼──►  bulbs /
  app       │     ├─ WiZLight  (update/poll)                        │     plugs
 (HAP/IP)   │     ├─ WiZLight                                       │  (UDP/JSON)
            │     └─ WiZOutlet                                      │
            └──────────────────────────────────────────────────────┘
```

- **Northbound**: HomeSpan implements the HomeKit Accessory Protocol (HAP) over
  your LAN. The iPhone/Home app talks to the ESP32 directly — no hub or cloud.
- **Southbound**: a single `WiFiUDP` socket on port `38899` speaks the WiZ
  local JSON protocol to every bulb/plug.

## Boot sequence (`setup()`)

1. **WiFi up first.** We connect with `WiFi.begin()` *before* HomeSpan so the
   network is available for UDP discovery while we build the accessory list.
2. **Bind UDP** socket on `38899` (`wizUDP.begin`).
3. **Configure HomeSpan** — `setWifiCredentials()` (so it reuses our connection
   instead of opening a provisioning AP), `setPairingCode()`, then
   `homeSpan.begin(Category::Bridge, ...)`.
4. **Create the bridge accessory** (the first `SpanAccessory`).
5. **Discover** WiZ devices and create one bridged `SpanAccessory` per device.

## Main loop (`loop()`)

```
homeSpan.poll();                 // service HAP traffic every iteration
└─ round-robin poll one device   // every POLL_STEP_MS (default 2.5s)
└─ re-scan network               // every REDISCOVER_MS (default 5 min)
```

- **Round-robin polling** keeps UDP blocking minimal: only one `getPilot`
  request (≤ `WIZ_TIMEOUT_MS`) happens per `POLL_STEP_MS`, so HomeSpan stays
  responsive even with many devices.
- **Re-discovery** picks up devices added after boot and registers them live via
  `homeSpan.updateDatabase()`.

## Control path (Home → WiZ)

When the Home app changes a characteristic, HomeSpan calls the service's
`update()`:

```
On(false)            -> setPilot {"state":false}
On(true)+Brightness  -> setPilot {"state":true,"dimming":N}
ColorTemperature     -> setPilot {"state":true,"dimming":N,"temp":KELVIN}
Hue/Saturation       -> setPilot {"state":true,"dimming":N,"r":R,"g":G,"b":B}
```

`ColorTemperature` is checked **before** `Hue/Saturation` because Home nudges
hue/sat whenever the temperature slider moves; this priority keeps a temperature
change from being misinterpreted as a color change.

## Sync path (WiZ → Home)

`pollState()` issues `getPilot` and writes any externally-changed values back
into the characteristics with `setVal()` (which notifies the Home app). This
reflects changes made from the WiZ app, a physical switch, or schedules.

## Key classes

| Symbol           | Role                                                            |
|------------------|----------------------------------------------------------------|
| `WiZPollable`    | Tiny interface so lights and outlets share one poll list.      |
| `WiZLight`       | `Service::LightBulb` + `WiZPollable`. Color/CCT/dim capable.    |
| `WiZOutlet`      | `Service::Outlet` + `WiZPollable`. On/off.                      |
| `wizRequest()`   | Send JSON to one IP, parse JSON reply (with timeout).           |
| `wizSetPilot()`  | Fire-and-forget control command.                               |
| `discoverAndAdd()`| Broadcast probe, collect IPs, build new accessories.          |
| `detectType()`   | Map `moduleName` → `RGB` / `TW` / `DW` / `SOCKET`.              |

## Value conversions

| Domain        | HomeKit            | WiZ                     | Conversion                         |
|---------------|--------------------|-------------------------|------------------------------------|
| Brightness    | 1–100 (%)          | `dimming` 10–100        | clamp to `[MIN_BRIGHTNESS, 100]`   |
| Color temp    | 153–454 mired      | `temp` 2200–6500 K      | `K = 1_000_000 / mired`            |
| Color         | Hue 0–360 / Sat %  | `r`,`g`,`b` 0–255       | `hsv2rgb()` / `rgb2hsv()` (V=100)  |

See [WIZ_PROTOCOL.md](WIZ_PROTOCOL.md) for the wire format.
