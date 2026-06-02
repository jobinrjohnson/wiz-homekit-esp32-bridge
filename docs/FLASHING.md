# Flashing Guide (Arduino IDE)

Step-by-step instructions to get the firmware onto an ESP32.

## 1. Install the Arduino IDE

Download the Arduino IDE (2.x recommended) from
<https://www.arduino.cc/en/software>.

## 2. Add the ESP32 board package

1. **File → Preferences** (Windows/Linux) or **Arduino IDE → Settings** (macOS).
2. In **Additional boards manager URLs**, add:
   ```
   https://raw.githubusercontent.com/espressif/arduino-esp32/gh-pages/package_esp32_index.json
   ```
3. **Tools → Board → Boards Manager…**, search **esp32**, install
   *"esp32 by Espressif Systems"*.

## 3. Install the libraries

**Tools → Manage Libraries…**, then install:

| Library      | Author            | Version |
|--------------|-------------------|---------|
| HomeSpan     | Gregg E. Berman   | ≥ 1.9.0 |
| ArduinoJson  | Benoit Blanchon   | 7.x     |

## 4. Open the sketch

Open `WiZHomeKitBridge/WiZHomeKitBridge.ino`. (Arduino requires the `.ino` file
to live in a folder of the same name — it already does.)

## 5. Configure WiFi

Edit the **USER CONFIG** block at the top:

```cpp
#define WIFI_SSID  "YOUR_WIFI_SSID"
#define WIFI_PASS  "YOUR_WIFI_PASSWORD"
```

> The ESP32 only supports **2.4 GHz** WiFi. Make sure your SSID is on the
> 2.4 GHz band, and on the **same subnet** as your WiZ devices.

## 6. Select board & port

- **Tools → Board → esp32** → pick your module (e.g. *"ESP32 Dev Module"*).
- **Tools → Port** → select the serial port that appears when the ESP32 is
  plugged in.
- Defaults for *ESP32 Dev Module* are fine. If upload fails, set
  **Upload Speed → 115200**.

## 7. Upload

Click **Upload** (→). If you see *"Connecting…….."* with no progress, hold the
**BOOT** button on the ESP32 while it tries to connect, then release.

## 8. Watch it boot

Open **Tools → Serial Monitor** at **115200 baud**. You should see:

```
[WiZHomeKitBridge] starting...
Connecting to WiFi....
WiFi connected. IP = 192.168.1.42
Scanning network for WiZ devices...
Discovery: 3 device(s) responded.
  + WiZ Color A1B2C3  ip=192.168.1.50  mac=...  module=ESP01_SHRGB1C_31  type=RGB
  + WiZ White  D4E5F6  ip=192.168.1.51  mac=...  module=ESP01_SHTW1C_31  type=TW
  + WiZ Plug   778899  ip=192.168.1.52  mac=...  module=ESP25_SOCKET_01  type=SOCKET
Added 3 WiZ accessory(ies).
```

## 9. Pair with Apple Home

See [PAIRING in the README](../README.md#pairing-with-apple-home). Default code:
**`466-37-726`**.

## HomeSpan serial commands

With the Serial Monitor open you can type single-letter HomeSpan commands. The
most useful:

| Key | Action                                            |
|-----|---------------------------------------------------|
| `?` | List all commands.                                |
| `s` | Print HomeKit status / pairing state.             |
| `W` | Re-launch WiFi setup.                              |
| `H` | **Delete HomeKit pairing** (then re-add in Home). |
| `F` | Factory reset (clears WiFi + pairing).            |
| `r` | Restart the device.                               |
