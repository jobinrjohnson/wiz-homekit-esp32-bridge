# WiFi Provisioning (from your phone)

There are **no hardcoded WiFi credentials** in the sketch. WiFi is set up after
flashing using HomeSpan's built-in **setup Access Point** (SoftAP) + captive
portal — all from the iPhone, no app required.

## First-time setup

1. Flash and power the ESP32. With no saved WiFi, it starts a setup network:
   - SSID: **`WiZBridge-Setup`**
   - Password: **`wizsetup`**
   (both configurable — see [CUSTOMIZATION.md](CUSTOMIZATION.md)).
2. On the iPhone: **Settings → Wi-Fi → join `WiZBridge-Setup`**.
3. iOS auto-opens a **captive-portal page**. (If it doesn't, open a browser to
   `http://192.168.4.1`.)
4. Choose your home WiFi, enter its password, and submit. You can also set the
   HomeKit pairing code here.
5. The ESP32 saves the credentials to NVS, reboots, connects to your home WiFi,
   and starts WiZ discovery. Watch the serial monitor for `WiFi up. IP=…`.

After this, **every boot connects straight to the saved network** — the setup AP
only appears when no credentials are stored.

## Re-provisioning / switching networks

The setup AP returns only when stored credentials are cleared. In the Serial
Monitor (115200, line-ending = Newline):

- **`A`** — start the setup AP immediately (even if creds are stored).
- **`X`** — delete stored WiFi credentials and reboot → AP starts on next boot.
- **`F`** / **`E`** — factory reset / erase all (also clears HomeKit pairing).

## Important gotcha after a re-flash

A normal sketch upload does **not** erase WiFi credentials — they're in the NVS
partition, which uploads leave untouched. So if the device was provisioned (or
previously used hardcoded creds), it will **reconnect to the old network** and
the setup AP won't appear. To force provisioning, type **`X`** (or `A`) in the
serial monitor, or enable **Tools → "Erase All Flash Before Sketch Upload"**.

## Why not provision inside the Apple Home app?

The seamless "Home app hands WiFi to the accessory" flow (**WAC** — Wireless
Accessory Configuration) requires **MFi hardware authentication**, which a
software HomeKit stack like HomeSpan cannot provide. The SoftAP captive portal is
the phone-based alternative. (A Bluetooth alternative — Espressif BLE
Provisioning with the *ESP BLE Provisioning* iOS app — is also possible but much
heavier; see [LIMITATIONS.md](LIMITATIONS.md).)

## Security notes

- The setup network is WPA2 (password `wizsetup`) so credentials aren't sent over
  an open link. Set `SETUP_AP_PASSWORD ""` for an open network if your iOS
  version is finicky about auto-opening the captive portal.
- Credentials are stored in the ESP32's NVS, not in the source code.
