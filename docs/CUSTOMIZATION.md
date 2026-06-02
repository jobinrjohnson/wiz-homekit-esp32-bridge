# Customization & Extending

How to tweak behavior and add support for more WiZ features.

## Tunable settings

All in the **USER CONFIG** block at the top of the `.ino`:

| Macro            | Default     | Effect                                              |
|------------------|-------------|-----------------------------------------------------|
| `BRIDGE_NAME`    | "WiZ HomeKit Bridge" | Name shown in the Home app.                |
| `PAIRING_CODE`   | "46637726"  | 8-digit HomeKit setup code (shown as 466-37-726).   |
| `MIN_BRIGHTNESS` | `10`        | Lowest `dimming` sent to WiZ (WiZ floor is 10).     |
| `POLL_STEP_MS`   | `2500`      | Delay between polling successive devices.           |
| `REDISCOVER_MS`  | `300000`    | How often to re-scan for new devices (ms).          |
| `WIZ_TIMEOUT_MS` | `300`       | UDP response timeout per request.                   |

With many devices, increase `POLL_STEP_MS` to reduce UDP traffic, or decrease it
for snappier external-change sync.

## Forcing devices instead of (or in addition to) discovery

If broadcast discovery is unreliable on your network, you can hard-code device
IPs. Add this in `setup()` after `discoverAndAdd()`:

```cpp
addDeviceAt(IPAddress(192,168,1,50));
addDeviceAt(IPAddress(192,168,1,51));
```

`addDeviceAt()` still queries the device for its type and de-dupes by MAC, so
it's safe to call even if discovery already found it.

> Tip: give each WiZ device a **DHCP reservation** in your router so its IP
> never changes.

## Renaming accessories

Edit `friendlyName()`. By default names are `WiZ <Type> <last 6 of MAC>`. You
could map specific MACs to room-friendly names:

```cpp
static String friendlyName(const String &type, const String &mac) {
  if (mac == "a8bb50ABCDEF") return "Living Room Lamp";
  if (mac == "a8bb50123456") return "Kitchen Plug";
  ...
}
```

(You can also just rename them in the Home app after pairing.)

## Adding WiZ Scenes

WiZ supports built-in dynamic scenes via `sceneId` (1–32). To expose, say, a
"Party mode" you could add a stateless `Service::Switch` whose `update()` sends:

```cpp
wizSetPilot(ip, "\"sceneId\":6,\"speed\":100");   // 6 = Party
```

Common scene IDs: 1 Ocean, 2 Romance, 4 Sunset, 6 Party, 9 Cozy, 11 Forest,
12 Pastel, 13 Wake-up, 14 Bedtime, 16 Warm white, 18 Daylight, 28 Candlelight.

## Adding fan support (`FANDIM` modules)

WiZ ceiling-fan modules report `FANDIM` in `moduleName` and accept extra params
(`fanState`, `fanSpeed`, `fanRevrs`). To support them, add a branch in
`addDeviceAt()` that creates a `Service::Fan` alongside the `LightBulb` and maps
`Characteristic::RotationSpeed` / `Active` to `setPilot` `fanSpeed`/`fanState`.

## Changing the color-temperature range

HomeKit `ColorTemperature` is in **mired**. The sketch uses `153–454`
(≈ 6500K–2200K). To restrict the slider to, say, 2700K–5000K:

```cpp
cct = (new Characteristic::ColorTemperature(250))->setRange(200, 370);
```
(`mired = 1_000_000 / kelvin`.)

## Removing devices from HomeKit

This firmware adds devices but does not auto-remove unreachable ones (to avoid
flapping when a device is briefly offline). To clear stale accessories, reset
the HomeKit pairing (`H` over serial) and re-pair, or reboot after the device is
permanently gone.
