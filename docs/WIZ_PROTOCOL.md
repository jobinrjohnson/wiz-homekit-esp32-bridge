# WiZ Local UDP Protocol Reference

WiZ devices expose a local control API over **UDP on port `38899`** using small
JSON messages. No authentication, no cloud — it works entirely on the LAN. This
is the same protocol used by Home Assistant's `pywizlight`.

> All examples below are the raw UDP payloads. Responses come back from the
> device's port `38899` to the source port of your request.

## Methods

### `getSystemConfig` — identify a device

Request:
```json
{"method":"getSystemConfig","params":{}}
```
Response:
```json
{
  "method": "getSystemConfig",
  "env": "pro",
  "result": {
    "mac": "a8bb50xxxxxx",
    "homeId": 123456,
    "roomId": 789,
    "moduleName": "ESP01_SHRGB1C_31",
    "fwVersion": "1.25.0",
    "groupId": 0
  }
}
```
`moduleName` is how we classify the device (see **Module naming** below).

### `getPilot` — read current state

Request:
```json
{"method":"getPilot","params":{}}
```
Response (color mode):
```json
{
  "method": "getPilot",
  "env": "pro",
  "result": {
    "mac": "a8bb50xxxxxx",
    "rssi": -55,
    "state": true,
    "sceneId": 0,
    "r": 255, "g": 100, "b": 0,
    "dimming": 80
  }
}
```
Response (tunable-white / CCT mode):
```json
{ "result": { "state": true, "temp": 2700, "dimming": 60, "sceneId": 0 } }
```

A device is in **either** color mode (`r`,`g`,`b` present) **or** CCT mode
(`temp` present) — not both at once.

### `setPilot` — control a device

Request:
```json
{"method":"setPilot","params":{"state":true,"dimming":75,"temp":3000}}
```
Response:
```json
{"method":"setPilot","env":"pro","result":{"success":true}}
```

### `registration` — discovery probe

Broadcasting this makes every WiZ device reply (which is how we discover them):
```json
{"method":"registration","params":{
  "phoneMac":"AAAAAAAAAAAA","register":false,
  "phoneIp":"1.2.3.4","id":"1"}}
```
Each device responds with its `mac`; the responder's IP is taken from the UDP
source address.

## `setPilot` parameters

| Param      | Type    | Range        | Meaning                                            |
|------------|---------|--------------|----------------------------------------------------|
| `state`    | bool    | —            | On/off.                                            |
| `dimming`  | int     | **10–100**   | Brightness %. Values <10 are invalid.              |
| `temp`     | int     | 2200–6500    | White color temperature in Kelvin.                 |
| `r`,`g`,`b`| int     | 0–255        | Color channels (sets color mode).                  |
| `c`        | int     | 0–255        | Cool-white channel (some bulbs).                   |
| `w`        | int     | 0–255        | Warm-white channel (some bulbs).                   |
| `sceneId`  | int     | 1–32         | Built-in dynamic scene (e.g. 1=Ocean, 6=Party).    |
| `speed`    | int     | 10–200       | Scene animation speed %.                            |

Setting `r,g,b` switches the bulb to **color mode**; setting `temp` switches it
to **CCT mode**; setting `sceneId` starts a dynamic scene. The last one written
wins.

## Module naming

`moduleName` encodes the hardware capabilities, e.g. `ESP01_SHRGB1C_31`:

| Substring | Capability                | This firmware maps to        |
|-----------|---------------------------|------------------------------|
| `SOCKET`  | Smart plug / socket       | `Outlet`                     |
| `RGB`     | Full color + tunable white| `LightBulb` color + CCT      |
| `TW`      | Tunable white only        | `LightBulb` CCT              |
| `DW`      | Dimmable warm white only  | `LightBulb` dim              |
| `FANDIM`  | Fan + dimmable light      | *not handled (treated as DW)*|

Examples seen in the wild:
- `ESP01_SHRGB1C_31`, `ESP03_SHRGB1C_01`, `ESP14_SHRGB_01` → color bulbs
- `ESP01_SHTW1C_31`, `ESP56_SHTW3_01` → tunable white
- `ESP01_SHDW1_31` → dimmable white
- `ESP25_SOCKET_01`, `ESP10_SOCKET_06` → plugs

Because we match by substring, unknown/new module strings fall back to
dimmable-white, which is safe (On + Brightness always work).

## Quick manual test (from a computer)

You can poke a device directly with `nc` (netcat) to confirm it's reachable:

```bash
# turn a bulb on at 50% (replace with the bulb's IP)
echo -n '{"method":"setPilot","params":{"state":true,"dimming":50}}' \
  | nc -u -w1 192.168.1.50 38899

# read its state
echo -n '{"method":"getPilot","params":{}}' \
  | nc -u -w1 192.168.1.50 38899
```

If these work from your computer but not from the ESP32, the issue is network
segmentation (different subnet/VLAN or AP client isolation), not the firmware.
