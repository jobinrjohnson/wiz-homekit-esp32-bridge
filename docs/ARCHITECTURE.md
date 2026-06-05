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

## Concurrency: a dedicated WiZ task on core 0

This is the key design decision. **All blocking WiZ UDP** (the discovery scan and
per-device `getPilot` polling) runs in its own FreeRTOS task **pinned to core 0**,
created in `setup()` (`wizTaskFn`). `homeSpan.poll()` runs in `loop()` on core 1
and is **never blocked** by UDP timeouts or scans — which is what prevents the
"No Response" stalls (HomeKit drops a bridge whose `poll()` isn't serviced
frequently).

```
core 0  ── wizTaskFn ──►  discovery scan + round-robin getPilot   (blocking UDP)
                              │ produces results
                              ▼  (thread-safe queues, guarded by wizMux)
core 1  ── loop() ─────►  homeSpan.poll() + drainWizResults()      (HomeKit only)
```

**All HomeSpan mutations stay on core 1 / the main thread.** The task never calls
`setVal()` or creates accessories; it only fills queues:

- `pollQ` — `PollResult`s from polling → main loop applies via `applyPilot()`.
- `discQ` — `DiscResult`s from discovery → main loop creates new accessories /
  refreshes IPs, then calls `updateDatabase()` once per batch.
- `deviceAddrs` — the `mac→ip` poll-target list the loop fills and the task reads.

`onWiFiConnected` just sets `wifiUp`; the task waits on that flag, then does the
initial scan and settles into: quick re-scans (every 30 s for ~2 min) then every
`REDISCOVER_MS`, polling one device per `POLL_STEP_MS` in between.

## Main loop (`loop()`)

```
homeSpan.poll();        // HAP traffic - never blocked
drainWizResults();      // apply queued state updates / new devices (main thread)
heartbeat;              // device count every 15s
```

Control (`setPilot`) stays on the main thread inside `update()` — it's a quick
fire-and-forget `sendto`, so it doesn't need the task.

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
