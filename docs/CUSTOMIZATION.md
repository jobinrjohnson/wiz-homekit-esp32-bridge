# Customization & Extending

All knobs live in the **USER CONFIG** block at the top of
`WiZHomeKitBridge.ino`.

## Settings

| Macro | Default | Effect |
|-------|---------|--------|
| `SETUP_AP_SSID` | `"WiZBridge-Setup"` | name of the WiFi setup network |
| `SETUP_AP_PASSWORD` | `"wizsetup"` | password for the setup network (`""` = open) |
| `BRIDGE_NAME` | `"WiZ HomeKit Bridge"` | name shown in the Home app |
| `PAIRING_CODE` | `"46637726"` | 8-digit HomeKit setup code (shown 466-37-726) |
| `REED_PIN` | `23` | GPIO for the reed switch |
| `REED_NAME` | `"Door Sensor"` | reed sensor name in Home |
| `REED_ACTIVE_LOW` | `true` | `false` if your reed is normally-closed / inverted |
| `MIN_BRIGHTNESS` | `10` | WiZ dimming floor (WiZ valid range is 10–100) |
| `POLL_STEP_MS` | `2500` | gap between polling successive devices |
| `REDISCOVER_MS` | `300000` | background re-scan interval (5 min) |
| `DISCOVER_TIMEOUT` | `6.0` | seconds to listen during a boot scan |
| `DISCOVER_PASSES` | `3` | subnet sweeps per boot scan |

> ⚠️ `PAIRING_CODE` must be a valid HomeKit code — **not** trivial/sequential
> ones like `12345678`, `00000000`, `11111111` (HomeKit rejects those, and the
> bridge then won't offer to pair).

## Common changes

**Reed inverted (shows Open when closed):** flip `REED_ACTIVE_LOW`.

**Move the reed to another pin:** change `REED_PIN` (see
[HARDWARE.md](HARDWARE.md) for safe GPIOs).

**Quieter/looser discovery:** raise `POLL_STEP_MS` or `REDISCOVER_MS`; lower
`DISCOVER_PASSES` to 2 for a faster boot at slightly lower reliability.

**Rename accessories:** edit `friendlyName()` — default names are
`WiZ <Type> <last 6 of MAC>`. You can map specific MACs to room names, or just
rename in the Home app after pairing.

**Force fixed device IPs (skip discovery):** give devices DHCP reservations on
your router. The firmware keys identity on MAC, so a stable IP just means control
works immediately at boot before the first scan refreshes it.

## How a HomeKit service is declared (to add sensors)

The reed switch is the template. A service is a `struct` deriving a HomeSpan
`Service::`, creating its `Characteristic::`s in the constructor and updating them
in `loop()`:

```cpp
struct MyMotion : Service::MotionSensor {
  uint8_t pin;
  SpanCharacteristic *detected;
  MyMotion(uint8_t p) : Service::MotionSensor(), pin(p) {
    pinMode(pin, INPUT);
    detected = new Characteristic::MotionDetected(false);
  }
  void loop() override {
    bool m = digitalRead(pin);
    if (detected->getVal() != m) detected->setVal(m);
  }
};
```
Then add it as its own accessory in `setup()` with a fixed AID (e.g. 3):
```cpp
new SpanAccessory(3);
  new Service::AccessoryInformation();
    new Characteristic::Identify();
    new Characteristic::Name("Hallway Motion");
  new MyMotion(27);
```
See [LIMITATIONS.md](LIMITATIONS.md) for the full list of service types.

## Adding WiZ scenes (Switches)

HomeKit has no light-effect selector, so WiZ scenes are exposed as **Switches**
that send `setPilot {"sceneId":N}`. A momentary switch (fires the scene, then
flips back off) works well and is automatable. Common scene IDs: 1 Ocean,
4 Party, 5 Fireplace, 6 Cozy, 11 Warm White, 12 Daylight, 16 Relax,
29 Candlelight. See [LIMITATIONS.md](LIMITATIONS.md#wiz-scenes).

## Energy monitoring (plugs)

WiZ energy plugs answer `getPower` (milliwatts), but the **stock Home app has no
power/energy UI**. It can be surfaced to the **Eve app** and other third-party
HomeKit apps via Eve custom characteristics. See
[LIMITATIONS.md](LIMITATIONS.md#energy-monitoring).

## Color-temperature range

HomeKit `ColorTemperature` is in **mired**. The sketch uses `153–454`
(≈ 6500K–2200K). To restrict the slider, change the `setRange()` on the
`ColorTemperature` characteristic (`mired = 1_000_000 / kelvin`).
