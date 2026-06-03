/*
 * WiZHomeKitBridge
 * -----------------
 * An ESP32 sketch (Arduino IDE) that auto-discovers every Philips WiZ device on
 * your local network and exposes each one to the Apple Home app through HomeSpan
 * (a native HomeKit/HAP implementation - no Hub/Homebridge required).
 *
 * Supported WiZ accessories (detected automatically from the device moduleName):
 *   - Color (RGB) bulbs / strips ...... HomeKit Lightbulb: On, Brightness, Hue, Saturation, ColorTemperature
 *   - Tunable-white (TW) bulbs ........ HomeKit Lightbulb: On, Brightness, ColorTemperature
 *   - Dimmable-white (DW) bulbs ....... HomeKit Lightbulb: On, Brightness
 *   - Smart plugs / sockets ........... HomeKit Outlet:    On
 *
 * ------------------------------------------------------------------------------
 * REQUIRED LIBRARIES (Arduino IDE -> Tools -> Manage Libraries...):
 *   1. "HomeSpan"     by Gregg E. Berman   (v1.9.0 or newer)
 *   2. "ArduinoJson"  by Benoit Blanchon   (v7.x)
 * REQUIRED BOARD PACKAGE:
 *   - "esp32" by Espressif Systems (Boards Manager). Select any ESP32 dev board.
 *
 * SETUP:
 *   1. Fill in WIFI_SSID and WIFI_PASS below.
 *   2. Flash to the ESP32 (115200 baud serial monitor to watch discovery).
 *   3. In the Apple Home app: Add Accessory -> "More options..." -> select
 *      "WiZ HomeKit Bridge". Pairing code (default HomeSpan code): 466-37-726.
 *      All discovered WiZ devices appear as one bridge.
 *
 * NOTES:
 *   - Make sure the ESP32 and the WiZ devices are on the SAME subnet/VLAN and
 *     that the WiZ devices already work in the official WiZ app.
 *   - Devices are discovered at boot and re-scanned periodically; newly added
 *     WiZ devices appear automatically (HomeSpan updateDatabase). Removed
 *     devices simply stop responding (state is left as-is until reboot).
 * ------------------------------------------------------------------------------
 */

#include "HomeSpan.h"
#include <WiFi.h>
#include <WiFiUdp.h>
#include <ArduinoJson.h>
#include <vector>
#include <math.h>

// ====================== USER CONFIG ==========================================
#define WIFI_SSID   "A404 Wifi"
#define WIFI_PASS   "Test@123"

#define BRIDGE_NAME "WiZ HomeKit Bridge"
#define PAIRING_CODE "46637726"          // 8 digits, shown as 466-37-726

#define WIZ_PORT          38899          // WiZ UDP control port
#define MIN_BRIGHTNESS    10             // WiZ dimming floor (valid range 10-100)
#define POLL_STEP_MS      2500           // poll one device every this many ms (round-robin)
#define REDISCOVER_MS     300000UL       // re-scan network for new devices (5 min)
#define WIZ_TIMEOUT_MS    300            // per-request UDP response timeout
// =============================================================================

WiFiUDP wizUDP;

// ---- HSV <-> RGB helpers ----------------------------------------------------
static void hsv2rgb(double H, double S, double V, int &R, int &G, int &B) {
  S /= 100.0; V /= 100.0;
  double C = V * S;
  double X = C * (1 - fabs(fmod(H / 60.0, 2) - 1));
  double m = V - C;
  double r, g, b;
  if      (H < 60)  { r = C; g = X; b = 0; }
  else if (H < 120) { r = X; g = C; b = 0; }
  else if (H < 180) { r = 0; g = C; b = X; }
  else if (H < 240) { r = 0; g = X; b = C; }
  else if (H < 300) { r = X; g = 0; b = C; }
  else              { r = C; g = 0; b = X; }
  R = (int)lround((r + m) * 255);
  G = (int)lround((g + m) * 255);
  B = (int)lround((b + m) * 255);
}

static void rgb2hsv(int R, int G, int B, double &H, double &S, double &V) {
  double r = R / 255.0, g = G / 255.0, b = B / 255.0;
  double mx = fmax(r, fmax(g, b)), mn = fmin(r, fmin(g, b));
  double d = mx - mn;
  V = mx * 100.0;
  S = (mx == 0) ? 0 : (d / mx) * 100.0;
  if      (d == 0)  H = 0;
  else if (mx == r) H = 60 * fmod((g - b) / d, 6);
  else if (mx == g) H = 60 * ((b - r) / d + 2);
  else              H = 60 * ((r - g) / d + 4);
  if (H < 0) H += 360;
}

// ---- Low-level WiZ UDP request/response -------------------------------------
// Sends a JSON command to a specific device IP and parses the JSON reply.
static bool wizRequest(IPAddress ip, const char *json, JsonDocument &doc,
                       uint32_t timeout = WIZ_TIMEOUT_MS) {
  while (wizUDP.parsePacket() > 0) wizUDP.flush();   // drain stale packets
  wizUDP.beginPacket(ip, WIZ_PORT);
  wizUDP.write((const uint8_t *)json, strlen(json));
  wizUDP.endPacket();

  uint32_t start = millis();
  while (millis() - start < timeout) {
    int sz = wizUDP.parsePacket();
    if (sz > 0) {
      if (wizUDP.remoteIP() == ip) {
        char buf[1024];
        int n = wizUDP.read(buf, sizeof(buf) - 1);
        buf[n] = 0;
        doc.clear();
        if (!deserializeJson(doc, buf)) return true;
      } else {
        wizUDP.flush();
      }
    }
    delay(2);
  }
  return false;
}

// Fire-and-forget setPilot (control) command.
static void wizSetPilot(IPAddress ip, const String &params) {
  String json = String("{\"method\":\"setPilot\",\"params\":{") + params + "}}";
  wizUDP.beginPacket(ip, WIZ_PORT);
  wizUDP.write((const uint8_t *)json.c_str(), json.length());
  wizUDP.endPacket();
}

// ---- Pollable interface (so we can store lights & outlets together) ---------
struct WiZPollable {
  virtual void pollState() = 0;
};
std::vector<WiZPollable *> pollables;
std::vector<String>        knownMacs;     // dedupe across re-scans

// =============================================================================
// HomeKit Lightbulb backed by a WiZ bulb
// =============================================================================
struct WiZLight : Service::LightBulb, WiZPollable {
  IPAddress ip;
  bool hasColor, hasCCT;
  SpanCharacteristic *power;
  SpanCharacteristic *bright = nullptr;
  SpanCharacteristic *hue    = nullptr;
  SpanCharacteristic *sat    = nullptr;
  SpanCharacteristic *cct    = nullptr;

  WiZLight(IPAddress ip, bool hasColor, bool hasCCT)
      : Service::LightBulb(), ip(ip), hasColor(hasColor), hasCCT(hasCCT) {
    power  = new Characteristic::On(false);
    bright = (new Characteristic::Brightness(100))->setRange(1, 100, 1);
    if (hasColor) {
      hue = new Characteristic::Hue(0);
      sat = new Characteristic::Saturation(0);
    }
    if (hasCCT) {
      // HomeKit ColorTemperature is in mired (1e6/Kelvin).
      // WiZ supports ~2200K..6500K  ->  ~454..153 mired.
      cct = (new Characteristic::ColorTemperature(250))->setRange(153, 454);
    }
  }

  // Called by HomeSpan when the Home app changes something.
  boolean update() override {
    // Pure OFF -> just send state:false (keeps last color/temp on the bulb).
    if (power->updated() && power->getNewVal() == false) {
      wizSetPilot(ip, "\"state\":false");
      return true;
    }

    String p = "\"state\":true";

    if (bright) {
      int b = bright->getNewVal();
      b = constrain(b, MIN_BRIGHTNESS, 100);
      p += ",\"dimming\":" + String(b);
    }

    // ColorTemperature change takes priority (Home also nudges Hue/Sat when the
    // temperature slider moves, so check cct first).
    if (cct && cct->updated()) {
      int mired = cct->getNewVal();
      int kelvin = (mired > 0) ? (1000000 / mired) : 4000;
      kelvin = constrain(kelvin, 2200, 6500);
      p += ",\"temp\":" + String(kelvin);
    } else if (hue && (hue->updated() || sat->updated())) {
      double H = hue->getNewVal<double>();
      double S = sat->getNewVal<double>();
      int R, G, B;
      hsv2rgb(H, S, 100, R, G, B);   // brightness handled via dimming
      p += ",\"r\":" + String(R) + ",\"g\":" + String(G) + ",\"b\":" + String(B);
    }

    wizSetPilot(ip, p);
    return true;
  }

  // Round-robin pull of the real bulb state back into HomeKit.
  void pollState() override {
    JsonDocument doc;
    if (!wizRequest(ip, "{\"method\":\"getPilot\",\"params\":{}}", doc)) return;
    JsonVariant res = doc["result"];
    if (res.isNull()) return;

    if (!res["state"].isNull()) {
      bool st = res["state"];
      if (power->getVal<bool>() != st) power->setVal(st);
    }
    if (bright && !res["dimming"].isNull()) {
      int dm = res["dimming"];
      if (dm > 0 && bright->getVal() != dm) bright->setVal(dm);
    }
    if (cct && !res["temp"].isNull()) {
      int k = res["temp"];
      if (k > 0) {
        int m = constrain(1000000 / k, 153, 454);
        if (cct->getVal() != m) cct->setVal(m);
      }
    }
    if (hue && !res["r"].isNull()) {
      int R = res["r"] | 0, G = res["g"] | 0, B = res["b"] | 0;
      double H, S, V;
      rgb2hsv(R, G, B, H, S, V);
      hue->setVal(H);
      sat->setVal(S);
    }
  }
};

// =============================================================================
// HomeKit Outlet backed by a WiZ smart plug / socket
// =============================================================================
struct WiZOutlet : Service::Outlet, WiZPollable {
  IPAddress ip;
  SpanCharacteristic *power;

  WiZOutlet(IPAddress ip) : Service::Outlet(), ip(ip) {
    power = new Characteristic::On(false);
    new Characteristic::OutletInUse(true);
  }

  boolean update() override {
    wizSetPilot(ip, power->getNewVal() ? "\"state\":true" : "\"state\":false");
    return true;
  }

  void pollState() override {
    JsonDocument doc;
    if (!wizRequest(ip, "{\"method\":\"getPilot\",\"params\":{}}", doc)) return;
    JsonVariant res = doc["result"];
    if (res.isNull() || res["state"].isNull()) return;
    bool st = res["state"];
    if (power->getVal<bool>() != st) power->setVal(st);
  }
};

// =============================================================================
// Discovery + accessory creation
// =============================================================================

// Map a WiZ moduleName (e.g. "ESP01_SHRGB1C_31") to a capability class.
static String detectType(const String &moduleName) {
  String s = moduleName;
  s.toUpperCase();
  if (s.indexOf("SOCKET") >= 0) return "SOCKET";
  if (s.indexOf("RGB")    >= 0) return "RGB";
  if (s.indexOf("TW")     >= 0) return "TW";
  if (s.indexOf("DW")     >= 0) return "DW";
  return "DW";   // safe default: dimmable white
}

static String friendlyName(const String &type, const String &mac) {
  String suffix = mac;
  suffix.replace(":", "");
  if (suffix.length() > 6) suffix = suffix.substring(suffix.length() - 6);
  suffix.toUpperCase();
  String base = (type == "SOCKET") ? "WiZ Plug "
              : (type == "RGB")    ? "WiZ Color "
              : (type == "TW")     ? "WiZ White "
                                   : "WiZ Light ";
  return base + suffix;
}

// Query a single IP, figure out its type, and build the HomeKit accessory.
// Returns true if a NEW accessory was added.
static bool addDeviceAt(IPAddress ip) {
  Serial.printf("[add] %s -> getSystemConfig ...\n", ip.toString().c_str());
  JsonDocument doc;
  if (!wizRequest(ip, "{\"method\":\"getSystemConfig\",\"params\":{}}", doc, 800)) {
    Serial.printf("[add] %s -> NO reply to getSystemConfig\n",
                  ip.toString().c_str());
    return false;
  }
  JsonVariant res = doc["result"];
  if (res.isNull()) {
    Serial.printf("[add] %s -> reply had no 'result' field\n",
                  ip.toString().c_str());
    return false;
  }

  String mac        = res["mac"]        | "";
  String moduleName = res["moduleName"] | "";
  Serial.printf("[add] %s -> mac=%s module=%s\n",
                ip.toString().c_str(), mac.c_str(), moduleName.c_str());
  if (mac.length() == 0) {
    Serial.printf("[add] %s -> no MAC, skipping\n", ip.toString().c_str());
    return false;
  }

  for (auto &m : knownMacs)            // already added?
    if (m == mac) {
      Serial.printf("[add] %s -> mac %s already added, skipping\n",
                    ip.toString().c_str(), mac.c_str());
      return false;
    }

  String type = detectType(moduleName);
  String name = friendlyName(type, mac);

  Serial.printf("  + ADDED %s  ip=%s  mac=%s  module=%s  type=%s\n",
                name.c_str(), ip.toString().c_str(), mac.c_str(),
                moduleName.c_str(), type.c_str());

  new SpanAccessory();
    new Service::AccessoryInformation();
      new Characteristic::Identify();
      new Characteristic::Name(name.c_str());
      new Characteristic::Manufacturer("WiZ");
      new Characteristic::SerialNumber(mac.c_str());
      new Characteristic::Model(moduleName.length() ? moduleName.c_str() : "WiZ");
      new Characteristic::FirmwareRevision("1.0");

  if (type == "SOCKET") {
    pollables.push_back(new WiZOutlet(ip));
  } else {
    bool color = (type == "RGB");
    bool cct   = (type == "RGB" || type == "TW");
    pollables.push_back(new WiZLight(ip, color, cct));
  }

  knownMacs.push_back(mac);
  return true;
}

// Record a responder IP in `found` if not already present. Returns true if new.
static bool noteFound(std::vector<IPAddress> &found, IPAddress ip) {
  for (auto &f : found) if (f == ip) return false;
  found.push_back(ip);
  return true;
}

// Drain every UDP packet currently waiting and log it. Any responder is a live
// WiZ device (only they listen on 38899). Returns how many packets it read.
static int drainAndLog(std::vector<IPAddress> &found) {
  int count = 0;
  int sz;
  while ((sz = wizUDP.parsePacket()) > 0) {
    IPAddress remote = wizUDP.remoteIP();
    uint16_t  rport  = wizUDP.remotePort();
    char buf[700];
    int n = wizUDP.read(buf, sizeof(buf) - 1);
    if (n < 0) n = 0;
    buf[n] = 0;
    bool isNew = noteFound(found, remote);
    Serial.printf("[scan] REPLY from %s:%u (%d bytes)%s -> %s\n",
                  remote.toString().c_str(), rport, n,
                  isNew ? " [NEW DEVICE]" : "", buf);
    count++;
  }
  return count;
}

// Listen for replies for `windowMs`, logging each one.
static void listenAndLog(std::vector<IPAddress> &found, uint32_t windowMs) {
  uint32_t start = millis();
  while (millis() - start < windowMs) {
    if (drainAndLog(found) == 0) delay(2);
  }
}

// Discovery: enumerate every IP on the local subnet, send a getPilot to each
// (this probes whether a device exists AND that UDP:38899 is open), and collect
// the IPs that reply. No broadcast, so it works through broadcast-filtering APs.
// PASSES > 1 because the first unicast to an un-ARP'd host is often dropped while
// ARP resolves; a second pass reaches devices the first one missed.
static void collectViaScan(std::vector<IPAddress> &found) {
  const char *probe = "{\"method\":\"getPilot\",\"params\":{}}";
  const int   PASSES = 2;

  IPAddress ip   = WiFi.localIP();
  IPAddress mask = WiFi.subnetMask();
  IPAddress net;
  for (int i = 0; i < 4; i++) net[i] = ip[i] & mask[i];

  uint32_t hostBits = 0;
  for (int i = 0; i < 4; i++)
    hostBits += __builtin_popcount((uint8_t)(~mask[i] & 0xFF));
  uint32_t hostCount = (hostBits >= 20) ? 0 : ((1UL << hostBits) - 1);
  if (hostCount < 1 || hostCount > 1022) hostCount = 254;   // default to a /24

  uint32_t netAddr = ((uint32_t)net[0] << 24) | ((uint32_t)net[1] << 16) |
                     ((uint32_t)net[2] << 8) | net[3];

  Serial.printf("[scan] my IP=%s mask=%s -> network %s, probing %u hosts, %d passes\n",
                ip.toString().c_str(), mask.toString().c_str(),
                net.toString().c_str(), hostCount, PASSES);

  for (int pass = 1; pass <= PASSES; pass++) {
    int sentOk = 0, sentFail = 0;
    for (uint32_t h = 1; h <= hostCount; h++) {
      uint32_t a = netAddr + h;
      IPAddress target((a >> 24) & 0xFF, (a >> 16) & 0xFF,
                       (a >> 8) & 0xFF, a & 0xFF);
      if (target == ip) continue;                  // skip ourselves

      int ok = 0;
      if (wizUDP.beginPacket(target, WIZ_PORT)) {
        wizUDP.write((const uint8_t *)probe, strlen(probe));
        ok = wizUDP.endPacket();                   // 1 = queued, 0 = send failed
      }
      if (ok) sentOk++; else sentFail++;

      delay(3);                                    // pace + let ARP/replies flow
      if (h % 16 == 0) drainAndLog(found);         // collect replies as we go
    }
    Serial.printf("[scan] pass %d/%d: probes sent ok=%d failed=%d, responders so far=%d\n",
                  pass, PASSES, sentOk, sentFail, (int)found.size());
    listenAndLog(found, 1500);                     // listen after each pass
  }

  Serial.printf("[scan] finished: %d WiZ responder(s)\n", (int)found.size());
}

// Sweep the subnet, then create accessories for any new devices that replied.
// Returns the number of newly added accessories.
static int discoverAndAdd() {
  std::vector<IPAddress> found;
  while (wizUDP.parsePacket() > 0) wizUDP.flush();   // clear stale RX

  collectViaScan(found);

  if (found.empty()) {
    Serial.println("[scan] zero replies from the whole subnet. Either no WiZ "
                   "device is reachable, or 38899 traffic is blocked.");
  }

  int added = 0;
  for (auto &ip : found)
    if (addDeviceAt(ip)) added++;
  Serial.printf("[scan] added %d new accessory(ies) this round.\n", added);
  return added;
}

// =============================================================================
bool wifiUp = false;   // set true once HomeSpan reports WiFi connected

// Called by HomeSpan ONCE the WiFi connection is fully established. This is the
// only safe place to do UDP work - doing it in setup() runs before HomeSpan has
// actually brought the network up, so the scan would hit a dead network.
void onWiFiConnected() {
  wifiUp = true;
  wizUDP.begin(WIZ_PORT);   // bind UDP now that the network is alive

  Serial.printf("WiFi up. IP=%s  subnet=%s\n",
                WiFi.localIP().toString().c_str(),
                WiFi.subnetMask().toString().c_str());

  Serial.println("Scanning subnet for WiZ devices (getPilot to every IP)...");
  int n = discoverAndAdd();
  Serial.printf("Total WiZ accessories: %d\n", (int)knownMacs.size());

  if (n > 0)
    homeSpan.updateDatabase();   // publish the freshly-found accessories
}

void setup() {
  Serial.begin(115200);
  delay(200);
  Serial.println("\n[WiZHomeKitBridge] starting...");

  // Let HomeSpan own WiFi. Provide credentials so it connects automatically and
  // skips its provisioning AP. Discovery is triggered from onWiFiConnected().
  homeSpan.setWifiCredentials(WIFI_SSID, WIFI_PASS);
  homeSpan.setPairingCode(PAIRING_CODE);
  homeSpan.setLogLevel(1);
  homeSpan.setWifiCallback(onWiFiConnected);
  homeSpan.begin(Category::Bridges, BRIDGE_NAME, "wiz-bridge");

  // The bridge accessory itself (always the first accessory).
  new SpanAccessory();
    new Service::AccessoryInformation();
      new Characteristic::Identify();
      new Characteristic::Name(BRIDGE_NAME);
      new Characteristic::Manufacturer("WiZ");
      new Characteristic::Model("ESP32-WiZ-Bridge");
      new Characteristic::FirmwareRevision("1.0");
}

void loop() {
  homeSpan.poll();

  // Heartbeat: print the current bridged-device count every 15s so the result
  // of discovery is always visible at the tail of the serial log.
  static uint32_t lastBeat = 0;
  if (millis() - lastBeat > 15000) {
    lastBeat = millis();
    Serial.printf("[WiZ] bridged devices: %d\n", (int)knownMacs.size());
  }

  // Round-robin state polling: one device per POLL_STEP_MS to avoid blocking.
  static uint32_t lastPoll = 0;
  static size_t   idx = 0;
  if (!pollables.empty() && millis() - lastPoll > POLL_STEP_MS) {
    lastPoll = millis();
    if (idx >= pollables.size()) idx = 0;
    pollables[idx]->pollState();
    idx++;
  }

  // Periodic re-discovery to pick up newly added WiZ devices at runtime.
  static uint32_t lastScan = 0;
  if (wifiUp && millis() - lastScan > REDISCOVER_MS) {
    lastScan = millis();
    int added = discoverAndAdd();
    if (added > 0) {
      Serial.printf("Re-scan added %d new accessory(ies); updating HomeKit DB.\n", added);
      homeSpan.updateDatabase();   // re-announce the bridge with new accessories
    }
  }
}
