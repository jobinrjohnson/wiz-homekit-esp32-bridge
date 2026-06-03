# Discovery

How the bridge finds WiZ devices, and the matching desktop tool for debugging.

## The approach: scan every IP on the subnet

WiZ devices listen for UDP/JSON on port **38899** and reply to whoever asks.
Discovery uses **BSD sockets** (the firmware is a direct port of
`discover_wiz.py`) and probes the whole subnet, rather than relying on broadcast
alone — many consumer routers/mesh systems silently drop broadcast, which is the
usual cause of "no devices found."

Each round (`wizDiscover`) does:

1. **Broadcast** a `getPilot` (and a `registration` probe) to `255.255.255.255`
   and the subnet broadcast address.
2. **Unicast** a `getPilot` to **every host** on the subnet (`.1 … .254` for a
   /24).
3. **Collect** every IP that replies (only real WiZ devices answer on 38899).

Then for each responder (`reportAndAdd`):
- `getSystemConfig` → `mac`, `moduleName`, firmware.
- `getPilot` → current state (for the log).
- `detectType(moduleName)` → RGB / TW / DW / SOCKET.
- Create the HomeKit accessory (deduped by MAC).

## ESP32-specific reliability tricks

A desktop has big socket buffers; the ESP32 doesn't, so the firmware adds:

- **Non-blocking drain during the sweep.** Replies are read *while* probes are
  still going out, so the tiny lwIP UDP receive queue can't overflow and drop
  answers while several bulbs reply at once.
- **Multiple passes** (`DISCOVER_PASSES`, default 3 at boot). A device whose probe
  is dropped during ARP resolution or TX-buffer pressure gets another chance.
- **Fresh ephemeral socket per request** in `wizQuery`. Binding a shared socket to
  38899 picks up the constant WiZ "sync" broadcasts as noise that breaks
  request/response matching; a per-call ephemeral socket only ever hears its own
  reply.

These are why discovery is reliable on the MCU even though the logic mirrors the
simple desktop script.

## When discovery runs

- **At boot**, from the WiFi-connected callback (6 s total, 3 passes).
- **Periodically** in `loop()`: quick re-scans (every 30 s) for the first ~2
  minutes to catch boot misses fast, then every `REDISCOVER_MS` (5 min) for
  devices added later. Background scans are lighter (2 s, single pass) so they
  don't stall HomeKit. New devices are published via `updateDatabase()`.

## Reading the serial log

```
Local IP : 192.168.0.14
Subnet   : 192.168.0.0  (254 host addresses)
Probing  : broadcast + unicast getPilot to every host ...
  reply from 192.168.0.8
  (pass 1/3: 4 device(s) so far)
  (pass 2/3: 5 device(s) so far)
Discovered 5 WiZ device(s).
```
- `Local IP`/`Subnet` — compare with a bulb's IP in the WiZ app. Different
  subnet ⇒ the ESP32 can't reach the bulbs (network problem, not firmware).
- `reply from …` — which devices answered, and on which pass.

## `discover_wiz.py` — the desktop tool

A standalone Python script (no dependencies) that runs the **same logic** from
your computer. It's the definitive way to tell whether a problem is the firmware
or the network.

```bash
python3 discover_wiz.py                 # scan your /24
python3 discover_wiz.py --subnet 192.168.0.0/24
python3 discover_wiz.py --timeout 5     # listen longer
```

It prints each device's MAC, module, type, capabilities, HomeKit mapping, and
current state. Interpreting the result:

- **Mac finds your devices, ESP32 doesn't** → the ESP32 is on a different
  subnet/segment than your computer. Compare the `Local IP`/`Subnet` lines.
- **Mac finds nothing either** → the bulbs aren't reachable on this network
  (different VLAN, client isolation, or cloud-only) — no local bridge can work
  until that's fixed.

See [WIZ_PROTOCOL.md](WIZ_PROTOCOL.md) for the message formats and a one-liner
`nc` test.
