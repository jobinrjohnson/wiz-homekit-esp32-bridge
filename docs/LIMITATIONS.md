# Limitations & Workarounds

Things HomeKit (or HomeSpan) can't do natively, with the realistic workarounds —
plus the full list of HomeKit service types you *can* add.

## Energy monitoring

WiZ energy plugs answer `getPower` (instantaneous watts, in milliwatts), but
**HomeKit has no power/energy characteristic**, so the **stock Apple Home app
cannot display watts or kWh** — it only shows the on/off outlet.

**Workaround — Eve custom characteristics.** Eve defined unofficial UUIDs
(`E863F1…`) for power/energy that the **Eve app**, Controller for HomeKit, Home+,
etc. read and graph. HomeSpan can attach them via `CUSTOM_CHAR`:

| What | Eve UUID | Source |
|------|----------|--------|
| Consumption (W) | `E863F10D-079E-48FF-8F27-9C2605A29F52` | `getPower` (mW→W) |
| Total (kWh) | `E863F10C-079E-48FF-8F27-9C2605A29F52` | integrated on-device |

These show in third-party apps, **not** stock Home. A kWh total must be
integrated on the ESP32 (watts × time) and resets on reboot unless persisted to
NVS. True Apple-blessed energy reporting needs **Matter** (a different stack
HomeSpan doesn't implement).

## WiZ scenes

HomeKit has **no light-effect/scene selector** for a bulb. WiZ's built-in dynamic
scenes (`sceneId` 1–32) are exposed as **Switches** instead — tapping one sends
`setPilot {"sceneId":N,"speed":100}`. Momentary switches (fire then flip off)
work best and are automatable. To exit a scene, just set a normal color/brightness
(it overrides the scene). Common IDs: 1 Ocean, 4 Party, 5 Fireplace, 6 Cozy,
11 Warm White, 12 Daylight, 16 Relax, 29 Candlelight.

## In-Home-app WiFi setup (WAC)

The seamless "Home app hands WiFi to the accessory" flow (**WAC**) needs **MFi
hardware authentication** and isn't available to software stacks like HomeSpan.
WiFi is provisioned via the SoftAP captive portal instead — see
[WIFI-PROVISIONING.md](WIFI-PROVISIONING.md).

## Remote access & automations need a Home Hub

The bridge works **locally** without a hub, but **remote access** (away from
home), **automations**, and **multi-user/persistent sharing** require an Apple
**Home Hub** (HomePod / HomePod mini / Apple TV). See
[the hub section in TROUBLESHOOTING.md](TROUBLESHOOTING.md#do-i-need-a-home-hub).

## HomeKit service types you *can* add

Every one renders with native Home-app UI; each is a `Service::` in HomeSpan
(add like the reed switch — see [CUSTOMIZATION.md](CUSTOMIZATION.md)).

**Lighting/power:** `LightBulb`, `Outlet`, `Switch`, `StatelessProgrammableSwitch`
(button).
**Sensors:** `ContactSensor`, `MotionSensor`, `OccupancySensor`,
`TemperatureSensor`, `HumiditySensor`, `LightSensor`, `AirQualitySensor`,
`CarbonDioxideSensor`, `CarbonMonoxideSensor`, `SmokeSensor`, `LeakSensor`.
**Security/access:** `SecuritySystem`, `Doorbell`, `LockMechanism`.
**Doors/windows:** `GarageDoorOpener`, `WindowCovering`, `Door`, `Window`.
**Climate:** `Thermostat`, `HeaterCooler`, `Fan`, `HumidifierDehumidifier`,
`AirPurifier`.
**Water/misc:** `Valve`, `IrrigationSystem`, `Television`+`InputSource`,
`Battery`, `FilterMaintenance`.

**No HomeKit equivalent for:** power/energy (watts/kWh), light effect/scene
lists, generic numeric/text displays.
