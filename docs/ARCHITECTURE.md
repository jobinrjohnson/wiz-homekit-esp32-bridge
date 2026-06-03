# Architecture

How the firmware is put together and how data flows between Apple Home and your
WiZ devices.

## Big picture

```
            ┌──────────────────────────────────────────────────────┐
            │                      ESP32                            │
            │                                                       │
  Apple     │   HomeSpan (HAP server)        WiZ UDP client         │   WiZ
  Home  ◄───┼──►  Bridge accessory      ◄──►  (BSD sockets,    ◄────┼──►  bulbs /
  app       │     ├─ WiZLight   (update/poll)  port 38899)          │     plugs
 (HAP/IP)   │     ├─ WiZOutlet                                      │  (UDP/JSON)
            │     └─ ReedSensor (GPIO 23)                           │
            └──────────────────────────────────────────────────────┘
```

- **Northbound:** HomeSpan implements the HomeKit Accessory Protocol over your
  LAN. The iPhone/Home app talks to the ESP32 directly — no hub or cloud.
- **Southbound:** WiZ devices are controlled over their local UDP/JSON API on
  port 38899, using **BSD sockets** (a direct port of `discover_wiz.py`).
- **Local I/O:** a reed switch on GPIO 23 is a HomeKit Contact Sensor.

## Boot sequence (`setup()`)

1. Configure HomeSpan: setup-AP name/password, `enableAutoStartAP()`, pairing
   code, log level, and the **WiFi-connected callback**.
2. `homeSpan.begin(Category::Bridges, …)`.
3. Create the static accessories with **fixed AIDs**: bridge = **AID 1**, reed
   sensor = **AID 2**.
4. WiFi is **not** hardcoded — HomeSpan connects with saved credentials, or
   starts the setup AP if none (see [WIFI-PROVISIONING.md](WIFI-PROVISIONING.md)).
5. Discovery does **not** run here — it's deferred to the WiFi callback, because
   the network isn't up yet during `setup()`.

## WiFi-connected callback (`onWiFiConnected`)

HomeSpan calls this **once WiFi is actually connected**. Only then is it safe to
do UDP. It runs discovery (`discoverAndAdd()`), and if any new device was added,
calls `homeSpan.updateDatabase()` to publish them.

## Main loop (`loop()`)

```
homeSpan.poll();                 // service HAP traffic every iteration
├─ heartbeat                     // print device count every 15s
├─ round-robin poll one device   // every POLL_STEP_MS (2.5s)
└─ background re-discovery        // quick first ~2 min, then every REDISCOVER_MS
```

- **Round-robin polling** keeps UDP blocking minimal: one `getPilot` per
  `POLL_STEP_MS`, so HAP stays responsive with many devices.
- **Background re-discovery** uses a lighter scan (2 s, single pass) so it doesn't
  stall HAP; it catches devices missed at boot or added later.

## Control path (Home → WiZ)

When the Home app changes a characteristic, HomeSpan calls the service's
`update()`:

```
On(false)            -> setPilot {"state":false}
On(true)+Brightness  -> setPilot {"state":true,"dimming":N}
ColorTemperature     -> setPilot {"state":true,"dimming":N,"temp":KELVIN}
Hue/Saturation       -> setPilot {"state":true,"dimming":N,"r":R,"g":G,"b":B}
```
`ColorTemperature` is checked **before** Hue/Sat (Home nudges hue/sat when the
temperature slider moves), so a temperature change isn't misread as a color
change.

## Sync path (WiZ → Home)

`pollState()` issues `getPilot` and writes externally-changed values back with
`setVal()` (reflects the WiZ app, a physical switch, or schedules).

## Reed switch

`ReedSensor::loop()` (called by HomeSpan) reads GPIO 23 with ~30 ms debounce and
updates `ContactSensorState` (0 = closed/contact, 1 = open).

## Stable identity

Each WiZ accessory's **AID is derived from its MAC** (`aidFromMac` = `100 + low 3
bytes`), so the same device always has the same HomeKit ID across reboots — Home
keeps its names/rooms/automations. See [STABLE-IDS.md](STABLE-IDS.md).

## Key symbols

| Symbol | Role |
|--------|------|
| `wizSendRaw()` / `wizQuery()` | fire-and-forget send / request-response over a fresh UDP socket |
| `wizSetPilot()` | build & send a `setPilot` control command |
| `WiZLight` / `WiZOutlet` | HomeSpan services (Lightbulb / Outlet) + polling |
| `ReedSensor` | HomeSpan ContactSensor on a GPIO |
| `wizDiscover()` | broadcast + unicast subnet sweep → responder IPs |
| `reportAndAdd()` | per responder: `getSystemConfig`/`getPilot`, classify, create |
| `aidFromMac()` / `detectType()` | stable AID / map moduleName → type |

## Value conversions

| Domain | HomeKit | WiZ | Conversion |
|--------|---------|-----|------------|
| Brightness | 1–100 (%) | `dimming` 10–100 | clamp to `[MIN_BRIGHTNESS, 100]` |
| Color temp | 153–454 mired | `temp` 2200–6500 K | `K = 1_000_000 / mired` |
| Color | Hue 0–360 / Sat % | `r,g,b` 0–255 | `hsv2rgb()` / `rgb2hsv()` (V=100) |

See [DISCOVERY.md](DISCOVERY.md) for the scan, [WIZ_PROTOCOL.md](WIZ_PROTOCOL.md)
for the wire format.
