# WiZ Local UDP Protocol Reference

WiZ devices expose a local control API over **UDP on port `38899`** using small
JSON messages — no auth, no cloud, entirely on the LAN. This is the same protocol
used by Home Assistant's `pywizlight` and by this project's `discover_wiz.py`.

> Responses come back from the device's port `38899` to the **source port** of
> your request (so a fresh ephemeral socket per request only hears its own reply).

## Methods

### `getSystemConfig` — identify a device
Request:
```json
{"method":"getSystemConfig","params":{}}
```
Response (example, a tunable-white bulb):
```json
{"method":"getSystemConfig","env":"pro","result":{
  "mac":"a8bb50aabbcc","homeId":12345,"roomId":678,"rgn":"eu",
  "moduleName":"ESP25_SHTW_01","fwVersion":"1.37.0","groupId":0}}
```
`moduleName` is how the firmware classifies the device (see **Module naming**).

### `getPilot` — read current state
Request:
```json
{"method":"getPilot","params":{}}
```
Response (color mode):
```json
{"result":{"mac":"...","state":true,"sceneId":0,"r":255,"g":100,"b":0,"dimming":80}}
```
Response (tunable-white / CCT mode):
```json
{"result":{"state":true,"temp":2700,"dimming":60,"sceneId":0}}
```
A device is in **either** color mode (`r,g,b`) **or** CCT mode (`temp`).

### `setPilot` — control a device
```json
{"method":"setPilot","params":{"state":true,"dimming":75,"temp":3000}}
```

### `getPower` — energy monitoring (plugs that support it)
```json
{"method":"getPower","params":{}}  →  {"result":{"power": 12345}}   // milliwatts
```
Not exposed in the stock Home app (HomeKit has no power characteristic) — see
[LIMITATIONS.md](LIMITATIONS.md).

### `registration` — discovery probe
Broadcasting this makes every WiZ device reply:
```json
{"method":"registration","params":{"phoneMac":"AAAAAAAAAAAA","register":false,
  "phoneIp":"1.2.3.4","id":"1"}}
```

## `setPilot` parameters

| Param | Type | Range | Meaning |
|-------|------|-------|---------|
| `state` | bool | — | on/off |
| `dimming` | int | **10–100** | brightness % (values <10 invalid) |
| `temp` | int | 2200–6500 | white color temperature (K) → CCT mode |
| `r`,`g`,`b` | int | 0–255 | color channels → color mode |
| `c`,`w` | int | 0–255 | cool/warm white channels (some bulbs) |
| `sceneId` | int | 1–32 | built-in dynamic scene |
| `speed` | int | 10–200 | scene animation speed % |

The last mode written wins (`r,g,b` ↔ `temp` ↔ `sceneId` are mutually exclusive).

## Module naming

`moduleName` (e.g. `ESP01_SHRGB1C_31`) encodes capabilities. The firmware maps by
substring:

| Substring | Type | HomeKit |
|-----------|------|---------|
| `SOCKET` / `PLUG` / `SWITCH` | SOCKET | Outlet |
| `RGB` | RGB | Lightbulb (color + CCT) |
| `TW` | TW | Lightbulb (CCT) |
| `DW` | DW | Lightbulb (dimmable) |
| *(none of the above)* | DW | Lightbulb (dimmable) — safe default |

Examples seen in the wild: `ESP01_SHRGB1C_31` (color), `ESP25_SHTW_01` (tunable
white), `ESP01_SHDW1_31` (dimmable), `ESP25_SOCKET_01` (plug).

## Quick manual test (from a computer)

```bash
# read state
echo -n '{"method":"getPilot","params":{}}' | nc -u -w1 192.168.0.50 38899
# turn on at 50%
echo -n '{"method":"setPilot","params":{"state":true,"dimming":50}}' | nc -u -w1 192.168.0.50 38899
```
If these work from your computer but not the ESP32, it's network segmentation
(different subnet/VLAN or AP isolation), not the firmware.
