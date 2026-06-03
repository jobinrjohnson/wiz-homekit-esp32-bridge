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
#include <ArduinoJson.h>
#include <vector>
#include <algorithm>
#include <math.h>
#include <lwip/sockets.h>   // BSD sockets - direct port of discover_wiz.py
#include <fcntl.h>          // non-blocking socket for draining replies mid-sweep

// ====================== USER CONFIG ==========================================
#define WIFI_SSID   "A404 Wifi"
#define WIFI_PASS   "Test@123"

#define BRIDGE_NAME "WiZ HomeKit Bridge"
#define PAIRING_CODE "46637726"          // 8 digits, shown as 466-37-726

#define WIZ_PORT          38899          // WiZ UDP control port (devices listen here)
#define MIN_BRIGHTNESS    10             // WiZ dimming floor (valid range 10-100)
#define POLL_STEP_MS      2500           // poll one device every this many ms (round-robin)
#define REDISCOVER_MS     300000UL       // re-scan network for new devices (5 min)
#define DISCOVER_TIMEOUT  6.0f           // total seconds to listen for discovery replies
#define DISCOVER_PASSES   3              // sweep the subnet this many times
// =============================================================================

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

// ---- Low-level WiZ UDP (BSD sockets - port of discover_wiz.py) ---------------

// Build a sockaddr_in for ip:WIZ_PORT. `s_addr` is the network-order address;
// (uint32_t)IPAddress already gives exactly that on this platform.
static struct sockaddr_in wizAddr(uint32_t s_addr) {
  struct sockaddr_in d;
  memset(&d, 0, sizeof(d));
  d.sin_family = AF_INET;
  d.sin_port = htons(WIZ_PORT);
  d.sin_addr.s_addr = s_addr;
  return d;
}

// Fire-and-forget raw JSON to a device on the WiZ port (fresh socket each time).
static bool wizSendRaw(IPAddress ip, const char *json) {
  int sock = socket(AF_INET, SOCK_DGRAM, 0);
  if (sock < 0) return false;
  struct sockaddr_in d = wizAddr((uint32_t)ip);
  int r = sendto(sock, json, strlen(json), 0, (struct sockaddr *)&d, sizeof(d));
  close(sock);
  return r > 0;
}

// query() - exactly like discover_wiz.py: open a fresh ephemeral socket, send
// one method, wait for the reply, parse it. A new socket per call means we only
// ever hear OUR reply (no port-38899 broadcast noise to confuse matching).
static bool wizQuery(IPAddress ip, const char *method, JsonDocument &doc,
                     float timeoutSec = 1.5f) {
  int sock = socket(AF_INET, SOCK_DGRAM, 0);
  if (sock < 0) return false;

  struct timeval tv;
  tv.tv_sec  = (int)timeoutSec;
  tv.tv_usec = (int)((timeoutSec - (int)timeoutSec) * 1000000);
  setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

  char payload[96];
  int len = snprintf(payload, sizeof(payload),
                     "{\"method\":\"%s\",\"params\":{}}", method);
  struct sockaddr_in d = wizAddr((uint32_t)ip);
  sendto(sock, payload, len, 0, (struct sockaddr *)&d, sizeof(d));

  char buf[2048];
  int n = recvfrom(sock, buf, sizeof(buf) - 1, 0, NULL, NULL);
  close(sock);
  if (n <= 0) return false;
  buf[n] = 0;
  doc.clear();
  return deserializeJson(doc, buf) == DeserializationError::Ok;
}

// Fire-and-forget setPilot (control) command.
static void wizSetPilot(IPAddress ip, const String &params) {
  String json = String("{\"method\":\"setPilot\",\"params\":{") + params + "}}";
  wizSendRaw(ip, json.c_str());
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
    if (!wizQuery(ip, "getPilot", doc)) return;
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
    if (!wizQuery(ip, "getPilot", doc)) return;
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
  if (s.indexOf("PLUG")   >= 0) return "SOCKET";
  if (s.indexOf("SWITCH") >= 0) return "SOCKET";
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

// classify() - port of discover_wiz.py: type + capability string + HomeKit type.
static void classify(const String &type, const char *&caps, const char *&hk) {
  if      (type == "SOCKET") { caps = "on/off";                                          hk = "Outlet"; }
  else if (type == "RGB")    { caps = "on/off, brightness, color (RGB), tunable white";  hk = "Lightbulb (color)"; }
  else if (type == "TW")     { caps = "on/off, brightness, tunable white (CCT)";         hk = "Lightbulb (white)"; }
  else                       { caps = "on/off, brightness";                              hk = "Lightbulb (dimmable)"; }
}

// Create the HomeKit accessory for a device (deduped by MAC). Returns true if new.
static bool createAccessory(IPAddress ip, const String &mac,
                            const String &moduleName, const String &type) {
  for (auto &m : knownMacs)
    if (m == mac) return false;            // already added

  String name = friendlyName(type, mac);

  new SpanAccessory();
    new Service::AccessoryInformation();
      new Characteristic::Identify();
      new Characteristic::Name(name.c_str());
      new Characteristic::Manufacturer("WiZ");
      new Characteristic::SerialNumber(mac.c_str());
      new Characteristic::Model(moduleName.c_str());
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

// discover() - port of discover_wiz.py: one ephemeral broadcast-capable socket;
// send registration+getPilot to the broadcast addresses AND a unicast getPilot to
// every host; then collect the IPs that reply for `timeoutSec`.
static std::vector<IPAddress> wizDiscover(float timeoutSec) {
  std::vector<IPAddress> found;

  int sock = socket(AF_INET, SOCK_DGRAM, 0);
  if (sock < 0) { Serial.println("socket() failed"); return found; }
  int yes = 1;
  setsockopt(sock, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));
  setsockopt(sock, SOL_SOCKET, SO_BROADCAST, &yes, sizeof(yes));   // like Python

  struct sockaddr_in local;
  memset(&local, 0, sizeof(local));
  local.sin_family = AF_INET;
  local.sin_addr.s_addr = INADDR_ANY;
  local.sin_port = 0;                       // ephemeral local port (bind(("",0)))
  bind(sock, (struct sockaddr *)&local, sizeof(local));

  fcntl(sock, F_SETFL, O_NONBLOCK);         // non-blocking: drain replies anytime

  const char *getPilot = "{\"method\":\"getPilot\",\"params\":{}}";
  const char *registration =
      "{\"method\":\"registration\",\"params\":{"
      "\"phoneMac\":\"AAAAAAAAAAAA\",\"register\":false,"
      "\"phoneIp\":\"1.2.3.4\",\"id\":\"1\"}}";

  IPAddress myip = WiFi.localIP();
  IPAddress mask = WiFi.subnetMask();
  uint32_t myL   = ((uint32_t)myip[0]<<24)|((uint32_t)myip[1]<<16)|((uint32_t)myip[2]<<8)|myip[3];
  uint32_t maskL = ((uint32_t)mask[0]<<24)|((uint32_t)mask[1]<<16)|((uint32_t)mask[2]<<8)|mask[3];
  uint32_t netL  = myL & maskL;
  uint32_t bcL   = netL | ~maskL;
  uint32_t hostN = bcL - netL - 1;
  if (hostN < 1 || hostN > 1022) hostN = 254;          // default /24

  IPAddress net((netL>>24)&0xFF,(netL>>16)&0xFF,(netL>>8)&0xFF,netL&0xFF);
  Serial.printf("Local IP : %s\n", myip.toString().c_str());
  Serial.printf("Subnet   : %s  (%u host addresses)\n", net.toString().c_str(), hostN);
  Serial.println("Probing  : broadcast + unicast getPilot to every host ...");

  uint32_t myS = (uint32_t)myip;

  auto sendTo = [&](const char *msg, uint32_t s_addr) {
    struct sockaddr_in d = wizAddr(s_addr);
    sendto(sock, msg, strlen(msg), 0, (struct sockaddr *)&d, sizeof(d));
  };

  // Drain every reply currently queued (non-blocking). Called constantly so the
  // small lwIP UDP receive buffer never overflows during the probe burst.
  auto drain = [&]() {
    char buf[2048];
    struct sockaddr_in from; socklen_t fl;
    while (true) {
      fl = sizeof(from);
      int n = recvfrom(sock, buf, sizeof(buf) - 1, 0, (struct sockaddr *)&from, &fl);
      if (n <= 0) break;                     // EWOULDBLOCK -> nothing left
      uint32_t fs = from.sin_addr.s_addr;
      if (fs == myS) continue;
      bool seen = false;
      for (auto &f : found) if ((uint32_t)f == fs) { seen = true; break; }
      if (!seen) {
        IPAddress fip(fs);
        found.push_back(fip);
        Serial.printf("  reply from %s\n", fip.toString().c_str());
      }
    }
  };

  // Multiple passes: a device whose probe is dropped (ARP miss / TX overflow) on
  // one pass gets another chance on the next.
  const int PASSES = DISCOVER_PASSES;
  uint32_t perPassMs = (uint32_t)(timeoutSec * 1000) / PASSES;

  for (int pass = 0; pass < PASSES; pass++) {
    // broadcast (255.255.255.255 and the subnet broadcast)
    for (uint32_t b : {(uint32_t)0xFFFFFFFF, bcL}) {
      sendTo(registration, htonl(b));
      sendTo(getPilot, htonl(b));
    }
    drain();

    // unicast sweep, draining frequently so replies aren't dropped mid-burst
    for (uint32_t h = netL + 1; h < bcL; h++) {
      if (h == myL) continue;
      sendTo(getPilot, htonl(h));
      if ((h & 0x03) == 0) { drain(); delay(1); }   // drain + pace every 4 sends
    }

    // listen window for this pass
    uint32_t endt = millis() + perPassMs;
    while ((int32_t)(endt - millis()) > 0) { drain(); delay(5); }
    Serial.printf("  (pass %d/%d: %d device(s) so far)\n",
                  pass + 1, PASSES, (int)found.size());
  }

  close(sock);
  return found;
}

// report() - port of discover_wiz.py: query getSystemConfig + getPilot, print the
// capabilities, and create the HomeKit accessory.
static void reportAndAdd(IPAddress ip) {
  JsonDocument cfg, pilot;
  bool haveCfg   = wizQuery(ip, "getSystemConfig", cfg);
  bool havePilot = wizQuery(ip, "getPilot", pilot);

  Serial.println("----------------------------------------------------------------");
  Serial.printf("Device @ %s\n", ip.toString().c_str());
  if (!haveCfg || cfg["result"].isNull()) {
    Serial.println("  ! getSystemConfig: no/invalid reply");
    return;
  }

  JsonVariant r = cfg["result"];
  String module = r["moduleName"] | "";
  String mac    = r["mac"]        | "";
  String fw     = r["fwVersion"]  | "";
  String type   = detectType(module);
  const char *caps, *hk;
  classify(type, caps, hk);

  Serial.printf("  MAC         : %s\n", mac.c_str());
  Serial.printf("  Module      : %s\n", module.c_str());
  Serial.printf("  Firmware    : %s\n", fw.c_str());
  Serial.printf("  Type        : %s\n", type.c_str());
  Serial.printf("  Capabilities: %s\n", caps);
  Serial.printf("  HomeKit as  : %s\n", hk);
  Serial.print  ("  Raw config  : ");
  serializeJson(r, Serial);                 // full getSystemConfig for debugging
  Serial.println();

  if (havePilot && !pilot["result"].isNull()) {
    JsonVariant p = pilot["result"];
    Serial.printf("  Now         : state=%s", (p["state"] | false) ? "true" : "false");
    if (!p["dimming"].isNull()) Serial.printf(", dimming=%d%%", (int)p["dimming"]);
    if (!p["temp"].isNull())    Serial.printf(", temp=%dK", (int)p["temp"]);
    if (!p["r"].isNull())       Serial.printf(", rgb=(%d,%d,%d)",
                                              (int)p["r"], (int)p["g"], (int)p["b"]);
    if (!p["sceneId"].isNull()) Serial.printf(", sceneId=%d", (int)p["sceneId"]);
    Serial.println();
  }

  if (mac.length() && module.length()) {
    if (createAccessory(ip, mac, module, type))
      Serial.println("  -> added to HomeKit");
    else
      Serial.println("  -> already known");
  }
}

// main() discovery flow - port of discover_wiz.py main(). Returns # new devices.
static int discoverAndAdd() {
  size_t before = knownMacs.size();

  std::vector<IPAddress> found = wizDiscover(DISCOVER_TIMEOUT);

  Serial.println();
  Serial.printf("Discovered %d WiZ device(s).\n", (int)found.size());
  std::sort(found.begin(), found.end(),
            [](const IPAddress &a, const IPAddress &b) {
              return ntohl((uint32_t)a) < ntohl((uint32_t)b);
            });
  for (auto &ip : found) reportAndAdd(ip);
  Serial.println("----------------------------------------------------------------");

  return (int)(knownMacs.size() - before);
}

// =============================================================================
bool wifiUp = false;   // set true once HomeSpan reports WiFi connected

// Called by HomeSpan ONCE the WiFi connection is fully established. This is the
// only safe place to do UDP work - doing it in setup() runs before HomeSpan has
// actually brought the network up, so the scan would hit a dead network.
void onWiFiConnected() {
  wifiUp = true;

  Serial.printf("WiFi up. IP=%s  subnet=%s\n",
                WiFi.localIP().toString().c_str(),
                WiFi.subnetMask().toString().c_str());

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
  homeSpan.setLogLevel(0);   // silence HomeSpan HAP chatter so our logs are readable
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

}
