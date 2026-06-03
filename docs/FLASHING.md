# Flashing Guide (Arduino IDE)

Get the firmware onto an ESP32. WiFi is configured **after** flashing, from your
phone — there are no credentials to edit in the sketch.

## 1. Install the Arduino IDE
Download from <https://www.arduino.cc/en/software> (2.x recommended).

## 2. Add the ESP32 board package
1. **File → Preferences** (Win/Linux) or **Arduino IDE → Settings** (macOS).
2. In **Additional boards manager URLs**, add:
   ```
   https://raw.githubusercontent.com/espressif/arduino-esp32/gh-pages/package_esp32_index.json
   ```
3. **Tools → Board → Boards Manager…** → search **esp32** → install
   *"esp32 by Espressif Systems"*.

## 3. Install the libraries
**Tools → Manage Libraries…**, install:

| Library      | Author            | Version |
|--------------|-------------------|---------|
| HomeSpan     | Gregg E. Berman   | ≥ 2.0   |
| ArduinoJson  | Benoit Blanchon   | 7.x     |

## 4. Open the sketch
Open `WiZHomeKitBridge/WiZHomeKitBridge.ino`. You normally **don't need to edit
anything** — WiFi is provisioned from your phone. (Optional knobs like the reed
GPIO and setup-AP name are in the `USER CONFIG` block; see
[CUSTOMIZATION.md](CUSTOMIZATION.md).)

## 5. Select board & port
- **Tools → Board → esp32 →** your module (e.g. *"ESP32 Dev Module"*).
- **Tools → Port →** the port that appears when the ESP32 is plugged in.
- If upload fails: set **Upload Speed → 115200**, and hold **BOOT** during
  "Connecting…".

## 6. Upload, then watch the serial monitor
Open **Tools → Serial Monitor** at **115200 baud**. On a fresh device you'll see
it start the WiFi setup Access Point — continue in
[WIFI-PROVISIONING.md](WIFI-PROVISIONING.md).

Once WiFi is up and devices are found, the log looks like:
```
WiFi up. IP=192.168.0.14  subnet=255.255.255.0
Local IP : 192.168.0.14
Subnet   : 192.168.0.0  (254 host addresses)
Probing  : broadcast + unicast getPilot to every host ...
  reply from 192.168.0.8
  (pass 1/3: 5 device(s) so far)
Discovered 5 WiZ device(s).
Device @ 192.168.0.8
  Module      : ESP25_SHTW_01
  Type        : TW
  HomeKit as  : Lightbulb (white)
  -> added to HomeKit (new)
[WiZ] bridged devices: 5
```

## 7. Pair with Apple Home
Home app → **Add Accessory** → **More options…** → **WiZ HomeKit Bridge** →
code **466-37-726**.

## HomeSpan serial commands
With the Serial Monitor open (line-ending = **Newline**), type a letter + Enter:

| Key | Action |
|-----|--------|
| `?` | List all commands |
| `A` | Start the WiFi setup Access Point now |
| `W` | Re-run WiFi setup |
| `X` | **Delete stored WiFi credentials** (then it re-provisions) |
| `H` | **Delete HomeKit pairing** (then re-add the bridge in Home) |
| `F` | Factory reset (WiFi + pairing) |
| `E` | Erase ALL stored data |
| `R` | Restart |

> A plain re-upload does **not** clear WiFi or pairing — those live in NVS. To
> wipe them, use `X`/`H`/`F`/`E`, or enable **Tools → "Erase All Flash Before
> Sketch Upload"** before uploading.
