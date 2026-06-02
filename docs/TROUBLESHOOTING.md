# Troubleshooting

Work top-to-bottom; the Serial Monitor (115200 baud) is your best diagnostic.

## WiFi won't connect

Serial sticks at `Connecting to WiFi....`.

- ESP32 is **2.4 GHz only** — confirm the SSID is broadcast on 2.4 GHz.
- Double-check `WIFI_SSID` / `WIFI_PASS` (case-sensitive).
- Special characters in the password are fine in the `#define`, but avoid a
  literal `"` — escape it as `\"`.
- Move the ESP32 closer to the AP for first boot.

## No WiZ devices discovered

Serial shows `Discovery: 0 device(s) responded.`

1. **Confirm the devices work in the WiZ app** and are powered.
2. **Same subnet** — the ESP32 IP (printed at boot) and the bulbs must share the
   first three octets (e.g. both `192.168.1.x`). Guest networks and separate IoT
   VLANs will break discovery.
3. **AP/client isolation** — many routers have a "client isolation" or "AP
   isolation" toggle that blocks device-to-device traffic. Turn it off (at least
   for the IoT network).
4. **Broadcast blocked** — some mesh systems drop subnet broadcasts. Work around
   it by hard-coding IPs with `addDeviceAt()` (see
   [CUSTOMIZATION.md](CUSTOMIZATION.md#forcing-devices-instead-of-or-in-addition-to-discovery)).
5. **Verify reachability from a computer** on the same network:
   ```bash
   echo -n '{"method":"getPilot","params":{}}' | nc -u -w1 <BULB_IP> 38899
   ```
   If that returns nothing either, it's a network problem, not the firmware.

## Bridge doesn't appear in the Home app

- Make sure the iPhone is on the **same WiFi** as the ESP32.
- The bridge advertises over mDNS/Bonjour — some networks block multicast.
  Toggling the iPhone's WiFi off/on often forces a re-scan.
- In **Add Accessory**, tap **More options…**; the bridge shows there even
  without a QR code.

## Pairing fails / "Accessory not found"

- Use the exact code **466-37-726** (or whatever `PAIRING_CODE` you set).
- If the device was paired before, it may already be "owned." Reset the pairing:
  open Serial Monitor and press **`H`**, then re-add in Home.
- Only one HomeKit home can own the bridge at a time.

## A device shows the wrong type (e.g. color bulb as plain light)

- Check the `module=...` string printed at boot against the table in
  [WIZ_PROTOCOL.md](WIZ_PROTOCOL.md#module-naming).
- If it's a genuinely new module name, add a matching substring in
  `detectType()`.

## State doesn't update when I use the WiZ app / wall switch

- Sync is polled (`POLL_STEP_MS`, default 2.5s per device), so expect a short
  delay, longer with many devices.
- If a device never updates, it may not be responding to `getPilot` — confirm
  with the `nc` test above.

## Colors look slightly off / brightness jumps at the bottom

- WiZ separates **color mode** and **white (CCT) mode**; the Home app's color
  wheel sends RGB, the temperature slider sends Kelvin. Switching between them is
  expected behavior.
- Brightness below ~10% is clamped to WiZ's minimum (`MIN_BRIGHTNESS`).

## Random reboots / instability

- Use a quality USB cable and a port that supplies enough current; some ESP32
  boards brown-out on weak USB power during WiFi transmit.
- Lots of devices + very small `POLL_STEP_MS` increases UDP load — raise it.

## Reset everything

Open the Serial Monitor and press:

- **`H`** — delete HomeKit pairing only.
- **`F`** — full factory reset (clears stored WiFi + pairing).
- **`r`** — restart.
