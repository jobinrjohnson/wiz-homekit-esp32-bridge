# Troubleshooting

Work top-to-bottom; the Serial Monitor (115200 baud) is your best diagnostic.
HomeSpan's own lines (`In Get/Put Characteristics…`, `WiFi Connected!`) are normal
HAP chatter; your firmware's lines start with `Local IP`, `Probing`, `reply
from`, `Device @`, `[WiZ]`, `[reed]`.

## The setup AP doesn't appear after re-flashing
Expected — a sketch upload does **not** erase stored WiFi credentials (they're in
NVS). The device reconnects to the old network. To force provisioning: type
**`X`** (delete WiFi) or **`A`** (start AP now) in the serial monitor, or enable
**Tools → "Erase All Flash Before Sketch Upload"**. See
[WIFI-PROVISIONING.md](WIFI-PROVISIONING.md).

## Can't join / configure the setup network
- The setup AP is `WiZBridge-Setup` / password `wizsetup`.
- If the captive portal doesn't auto-open, browse to `http://192.168.4.1`.
- Some iOS versions prefer an open network — set `SETUP_AP_PASSWORD ""`.

## No WiZ devices discovered
Serial shows `Discovered 0 WiZ device(s).`
1. **Same subnet** — compare the `Local IP`/`Subnet` lines with a bulb's IP in the
   WiZ app. Different subnet/VLAN ⇒ unreachable.
2. **Confirm reachability from a computer** on the same WiFi:
   ```bash
   python3 discover_wiz.py
   # or: echo -n '{"method":"getPilot","params":{}}' | nc -u -w1 <bulb_ip> 38899
   ```
   If the computer finds them but the ESP32 doesn't → the ESP32 is on a different
   segment. If neither finds them → network issue (VLAN/client isolation).
3. ESP32 is **2.4 GHz only** — make sure it joined the right band.

## Finds some devices but not all
Usually UDP reply loss; the firmware already does 3 passes + non-blocking drain.
- Watch the `(pass N/3: X device(s) so far)` lines — if the count climbs across
  passes, it's timing; raise `DISCOVER_PASSES` or `DISCOVER_TIMEOUT`.
- If a device's IP **never** appears, it isn't answering `getPilot` (powered off,
  different subnet, or not a controllable WiZ device like a WiZmote remote).
- A device missed at boot is picked up by the background re-scan (within ~30 s
  early on, then every 5 min).

## Bridge doesn't appear in the Home app
- iPhone must be on the **same WiFi** as the ESP32; toggle iPhone WiFi to force an
  mDNS re-scan.
- In **Add Accessory**, tap **More options…** — the bridge shows there.
- **Pairing code must be valid** — trivial codes like `12345678` are rejected by
  HomeKit and the bridge won't advertise for pairing. Use `46637726` (default).

## Pairing was removed but won't re-show
The device may still think it's paired. Type **`H`** (delete HomeKit pairing) in
the serial monitor, then Add Accessory again.

## Have to re-add the bridge after every reboot
This is what the **MAC-based stable AIDs** fix — make sure you're on the current
firmware. After upgrading from an older (implicit-AID) version, re-add the bridge
**once**; it's stable thereafter. See [STABLE-IDS.md](STABLE-IDS.md).

## A device shows the wrong type (e.g. plug as a bulb)
Check the `Module`/`Raw config` line in the boot log against the table in
[WIZ_PROTOCOL.md](WIZ_PROTOCOL.md#module-naming). If `moduleName` was empty, the
`getSystemConfig` query failed that round; it self-corrects on the next scan. If
it's a genuinely new module string, add a substring to `detectType()`.

## Wrong device controlled / names mixed up
Symptom of unstable IDs from an old firmware. Update, then re-add the bridge once.

## Reed switch shows Open when closed (or vice-versa)
Flip `REED_ACTIVE_LOW`. If it never changes, check wiring (one leg to `REED_PIN`,
one to GND) and that the GPIO has a pull-up (avoid 34–39). See
[HARDWARE.md](HARDWARE.md).

## State doesn't update from the WiZ app / wall switch
Sync is polled (`POLL_STEP_MS`, ~2.5 s per device, longer with many devices).
Expect a short delay. If a device never updates, confirm it answers `getPilot`.

## Random reboots
Use a good USB cable/supply — brown-out during WiFi TX is the usual cause.

## Do I need a Home Hub?
**For local control on the same WiFi: no.** A Home Hub (HomePod / HomePod mini /
Apple TV) is required for **remote access** (away from home), **automations**, and
robust **multi-user** sharing. Without a hub, the bridge still works fully while
your iPhone is on the home network.

## Reset cheat-sheet (serial monitor)
| Key | Clears |
|-----|--------|
| `X` | WiFi credentials |
| `H` | HomeKit pairing |
| `A` | (re)start setup AP now |
| `F` | factory reset (WiFi + pairing) |
| `E` | erase ALL stored data |
| `R` | restart |
