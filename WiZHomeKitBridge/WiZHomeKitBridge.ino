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
 *   1. Flash to the ESP32 (115200 baud serial monitor to watch progress).
 *   2. WiFi is provisioned from your phone (no hardcoded credentials):
 *      on the iPhone, join the "WiZBridge-Setup" WiFi network (password
 *      "wizsetup"); a captive-portal page opens - enter your home WiFi there.
 *      (To re-provision later, type 'X' in the serial monitor to erase WiFi.)
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
// WiFi is provisioned from your phone - there are NO hardcoded credentials.
// On first boot (or after erasing WiFi via the 'X' serial command), the ESP32
// starts the setup Access Point below. Join it from the iPhone and a captive-
// portal page lets you enter your home WiFi network + password.
#define SETUP_AP_SSID     "WiZBridge-Setup"  // setup network the phone joins
#define SETUP_AP_PASSWORD "wizsetup"         // password for that setup network

#define BRIDGE_NAME "WiZ HomeKit Bridge"
#define PAIRING_CODE "46637726"          // 8 digits, shown as 466-37-726

// --- Magnetic reed switch (door/window contact sensor) ----------------------
// Wire the reed switch between REED_PIN and GND. The internal pull-up is used,
// so when the magnet is present the reed closes and pulls the pin LOW.
// Pick an input-capable GPIO WITH a usable pull-up (avoid 34-39 which have none,
// and avoid strapping/flash pins 0,2,6-11,15). GPIO 23 is a safe default.
#define REED_PIN          23             // GPIO the reed switch is wired to
#define REED_NAME         "Door Sensor"  // name shown in the Home app
#define REED_ACTIVE_LOW   true           // true: magnet present -> pin LOW -> "closed"

#define WIZ_PORT          38899          // WiZ UDP control port (devices listen here)
#define MIN_BRIGHTNESS    10             // WiZ dimming floor (valid range 10-100)
#define POLL_STEP_MS      2500           // poll one device every this many ms (round-robin)
#define REDISCOVER_MS     300000UL       // re-scan network for new devices (5 min)
#define DISCOVER_TIMEOUT  6.0f           // total seconds to listen for discovery replies
#define DISCOVER_PASSES   3              // sweep the subnet this many times
// =============================================================================

// ---- Thread-safe handoff: WiZ task (core 0)  <->  HomeSpan (loop, core 1) -----
// The WiZ task does ALL blocking UDP (discovery + polling) so it can never starve
// homeSpan.poll(). It produces results into these queues; the main loop applies
// them to HomeKit. ALL HomeSpan mutations (setVal / new accessory / updateDatabase)
// stay on the main thread. Defined here, ABOVE the first function, so Arduino's
// auto-generated prototypes can see these types.
struct PollResult {              // one getPilot result, produced by the task
  String mac;
  bool   reachable = false;
  bool   state = false;
  int    dimming = -1, temp = -1, r = -1, g = -1, b = -1;   // -1 = field absent
};
struct DiscResult {             // one discovered device, produced by the task
  String    mac, type, module;
  IPAddress ip;
};
struct DeviceAddr {             // mac -> ip the task uses as poll targets
  String    mac;
  IPAddress ip;
};

volatile bool            wifiUp = false;  // set true once WiFi is connected
SemaphoreHandle_t        wizMux;          // guards the three vectors below
std::vector<PollResult>  pollQ;           // task -> loop
std::vector<DiscResult>  discQ;           // task -> loop
std::vector<DeviceAddr>  deviceAddrs;     // loop -> task (poll targets)

// ---- Pollable interface (so we can store lights & outlets together) ---------
struct WiZPollable {
  virtual void   setIp(IPAddress ip) = 0;
  virtual String macAddr() = 0;
  virtual void   applyPilot(const PollResult &p) = 0;   // applied on main thread
};
std::vector<WiZPollable *> pollables;     // main thread only
std::vector<String>        knownMacs;     // dedupe (main thread only)

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

// (WiZ task <-> HomeSpan shared types + the WiZPollable interface are defined
//  near the top of the file, right after USER CONFIG, so that Arduino's
//  auto-generated function prototypes can see them.)

// =============================================================================
// HomeKit Lightbulb backed by a WiZ bulb
// =============================================================================
struct WiZLight : Service::LightBulb, WiZPollable {
  IPAddress ip;
  String mac;
  bool hasColor, hasCCT;
  SpanCharacteristic *power;
  SpanCharacteristic *bright = nullptr;
  SpanCharacteristic *hue    = nullptr;
  SpanCharacteristic *sat    = nullptr;
  SpanCharacteristic *cct    = nullptr;

  void   setIp(IPAddress n) override { ip = n; }
  String macAddr() override { return mac; }

  WiZLight(IPAddress ip, const String &mac, bool hasColor, bool hasCCT)
      : Service::LightBulb(), ip(ip), mac(mac), hasColor(hasColor), hasCCT(hasCCT) {
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

  // Apply a getPilot result fetched by the WiZ task. Runs on the MAIN thread.
  void applyPilot(const PollResult &p) override {
    if (!p.reachable) return;
    if (power->getVal<bool>() != p.state) power->setVal(p.state);
    if (bright && p.dimming > 0 && bright->getVal() != p.dimming)
      bright->setVal(p.dimming);
    if (cct && p.temp > 0) {
      int m = constrain(1000000 / p.temp, 153, 454);
      if (cct->getVal() != m) cct->setVal(m);
    }
    if (hue && p.r >= 0) {
      double H, S, V;
      rgb2hsv(p.r, p.g, p.b, H, S, V);
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
  String mac;
  SpanCharacteristic *power;

  void   setIp(IPAddress n) override { ip = n; }
  String macAddr() override { return mac; }

  WiZOutlet(IPAddress ip, const String &mac) : Service::Outlet(), ip(ip), mac(mac) {
    power = new Characteristic::On(false);
    new Characteristic::OutletInUse(true);
  }

  boolean update() override {
    wizSetPilot(ip, power->getNewVal() ? "\"state\":true" : "\"state\":false");
    return true;
  }

  void applyPilot(const PollResult &p) override {
    if (!p.reachable) return;
    if (power->getVal<bool>() != p.state) power->setVal(p.state);
  }
};

// =============================================================================
// HomeKit Contact Sensor backed by a magnetic reed switch on a GPIO
// =============================================================================
struct ReedSensor : Service::ContactSensor {
  uint8_t pin;
  bool    activeLow;
  SpanCharacteristic *contact;
  int      stableRaw, lastRaw;
  uint32_t lastEdge = 0;

  ReedSensor(uint8_t pin, bool activeLow = true)
      : Service::ContactSensor(), pin(pin), activeLow(activeLow) {
    pinMode(pin, INPUT_PULLUP);
    stableRaw = lastRaw = digitalRead(pin);
    contact = new Characteristic::ContactSensorState(stateFrom(stableRaw));
  }

  // HomeKit ContactSensorState: 0 = contact detected (closed), 1 = open.
  uint8_t stateFrom(int raw) {
    bool closed = activeLow ? (raw == LOW) : (raw == HIGH);
    return closed ? 0 : 1;
  }

  // Polled automatically by HomeSpan; debounced ~30ms.
  void loop() override {
    int raw = digitalRead(pin);
    if (raw != lastRaw) { lastRaw = raw; lastEdge = millis(); }
    if (raw != stableRaw && millis() - lastEdge > 30) {
      stableRaw = raw;
      uint8_t s = stateFrom(raw);
      if (contact->getVal() != s) {
        contact->setVal(s);
        Serial.printf("[reed] pin %d -> %s\n", pin,
                      s == 0 ? "CLOSED (contact detected)" : "OPEN");
      }
    }
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

// Deterministic AID from MAC: low 3 bytes (24-bit) + offset above the static
// accessories (bridge=1, reed=2). Same device -> same AID on every boot, with no
// persistence needed, so the Home app's accessory mapping never shifts.
static uint32_t aidFromMac(const String &mac) {
  String m = mac; m.replace(":", "");
  if (m.length() < 6) return 0;
  uint32_t low = strtoul(m.substring(m.length() - 6).c_str(), nullptr, 16);
  return 100 + low;
}

// Create the HomeKit accessory for a device (deduped by MAC). Returns true if new.
static bool createAccessory(IPAddress ip, const String &mac,
                            const String &moduleName, const String &type) {
  for (auto &m : knownMacs)
    if (m == mac) return false;            // already added

  String name = friendlyName(type, mac);

  new SpanAccessory(aidFromMac(mac));      // FIXED AID derived from MAC
    new Service::AccessoryInformation();
      new Characteristic::Identify();
      new Characteristic::Name(name.c_str());
      new Characteristic::Manufacturer("WiZ");
      new Characteristic::SerialNumber(mac.c_str());
      new Characteristic::Model(moduleName.c_str());
      new Characteristic::FirmwareRevision("1.0");

  if (type == "SOCKET") {
    pollables.push_back(new WiZOutlet(ip, mac));
  } else {
    bool color = (type == "RGB");
    bool cct   = (type == "RGB" || type == "TW");
    pollables.push_back(new WiZLight(ip, mac, color, cct));
  }
  knownMacs.push_back(mac);

  xSemaphoreTake(wizMux, portMAX_DELAY);   // register as a poll target for the task
  deviceAddrs.push_back({mac, ip});
  xSemaphoreGive(wizMux);
  return true;
}

// discover() - port of discover_wiz.py: one ephemeral broadcast-capable socket;
// send registration+getPilot to the broadcast addresses AND a unicast getPilot to
// every host; then collect the IPs that reply for `timeoutSec`.
static std::vector<IPAddress> wizDiscover(float timeoutSec, int passes) {
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
  const int PASSES = passes;
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

// Parse a getPilot "result" object into a PollResult.
static void parsePilot(JsonVariant res, PollResult &pr) {
  if (res.isNull()) return;
  pr.reachable = true;
  pr.state = res["state"] | false;
  if (!res["dimming"].isNull()) pr.dimming = res["dimming"];
  if (!res["temp"].isNull())    pr.temp    = res["temp"];
  if (!res["r"].isNull()) { pr.r = res["r"] | 0; pr.g = res["g"] | 0; pr.b = res["b"] | 0; }
}

// [TASK] Query one responder (getSystemConfig + getPilot) and append the device
// + its initial state to the round's local batches. Runs on the WiZ task.
static void taskReport(IPAddress ip, std::vector<DiscResult> &dOut,
                       std::vector<PollResult> &pOut) {
  JsonDocument cfg, pilot;
  bool haveCfg   = wizQuery(ip, "getSystemConfig", cfg, 1.0f);
  bool havePilot = wizQuery(ip, "getPilot", pilot, 1.0f);
  if (!haveCfg || cfg["result"].isNull()) return;

  JsonVariant r = cfg["result"];
  String module = r["moduleName"] | "";
  String mac    = r["mac"]        | "";
  if (!mac.length() || !module.length()) return;
  String type = detectType(module);

  Serial.printf("Device @ %s  mac=%s  module=%s  type=%s\n",
                ip.toString().c_str(), mac.c_str(), module.c_str(), type.c_str());

  dOut.push_back({mac, type, module, ip});
  PollResult pr; pr.mac = mac;
  if (havePilot) parsePilot(pilot["result"], pr);
  pOut.push_back(pr);
}

// [TASK] One discovery round: sweep, query each responder, push the whole batch
// to the main thread in a single locked handoff (one updateDatabase per round).
static void taskDiscoverRound(float timeoutSec, int passes) {
  std::vector<IPAddress> found = wizDiscover(timeoutSec, passes);
  Serial.printf("Discovered %d WiZ device(s).\n", (int)found.size());
  std::sort(found.begin(), found.end(), [](const IPAddress &a, const IPAddress &b) {
    return ntohl((uint32_t)a) < ntohl((uint32_t)b);
  });

  std::vector<DiscResult> dList;
  std::vector<PollResult> pList;
  for (auto &ip : found) taskReport(ip, dList, pList);

  if (!dList.empty() || !pList.empty()) {
    xSemaphoreTake(wizMux, portMAX_DELAY);
    for (auto &d : dList) discQ.push_back(d);
    for (auto &p : pList) pollQ.push_back(p);
    xSemaphoreGive(wizMux);
  }
}

// [TASK] Poll one device's current state and hand it to the main thread.
static void pollOne(const DeviceAddr &d) {
  JsonDocument doc;
  PollResult pr; pr.mac = d.mac;
  if (wizQuery(d.ip, "getPilot", doc, 0.6f)) parsePilot(doc["result"], pr);
  if (pr.reachable) {
    xSemaphoreTake(wizMux, portMAX_DELAY);
    pollQ.push_back(pr);
    xSemaphoreGive(wizMux);
  }
}

// [TASK] Entry point - pinned to core 0. ALL blocking UDP lives here so the main
// loop's homeSpan.poll() (core 1) is never starved.
static void wizTaskFn(void *) {
  while (!wifiUp) vTaskDelay(pdMS_TO_TICKS(200));
  vTaskDelay(pdMS_TO_TICKS(500));

  taskDiscoverRound(DISCOVER_TIMEOUT, DISCOVER_PASSES);   // initial scan

  uint32_t lastScan = millis();
  int      quick = 4;            // first 4 re-scans are 30s apart, then REDISCOVER_MS
  size_t   idx = 0;

  for (;;) {
    uint32_t interval = (quick > 0) ? 30000UL : REDISCOVER_MS;
    if (millis() - lastScan > interval) {
      lastScan = millis();
      if (quick > 0) quick--;
      taskDiscoverRound(2.0f, 1);                          // light background scan
    }

    DeviceAddr d; bool have = false;
    xSemaphoreTake(wizMux, portMAX_DELAY);
    if (!deviceAddrs.empty()) {
      if (idx >= deviceAddrs.size()) idx = 0;
      d = deviceAddrs[idx++];
      have = true;
    }
    xSemaphoreGive(wizMux);
    if (have) pollOne(d);

    vTaskDelay(pdMS_TO_TICKS(POLL_STEP_MS));
  }
}

// [MAIN] find a service by MAC (only the main thread touches `pollables`).
static WiZPollable *pollableForMac(const String &mac) {
  for (auto *p : pollables) if (p->macAddr() == mac) return p;
  return nullptr;
}

// [MAIN] Apply the task's queued results to HomeKit. MUST run on the main thread.
static void drainWizResults() {
  // poll results -> setVal
  std::vector<PollResult> pr;
  xSemaphoreTake(wizMux, portMAX_DELAY); pr.swap(pollQ); xSemaphoreGive(wizMux);
  for (auto &p : pr) {
    WiZPollable *s = pollableForMac(p.mac);
    if (s) s->applyPilot(p);
  }

  // discovery results -> refresh IP (known) or create accessory (new)
  std::vector<DiscResult> dr;
  xSemaphoreTake(wizMux, portMAX_DELAY); dr.swap(discQ); xSemaphoreGive(wizMux);
  bool need = false;
  for (auto &d : dr) {
    bool known = false;
    for (auto &m : knownMacs) if (m == d.mac) { known = true; break; }
    if (known) {
      if (WiZPollable *s = pollableForMac(d.mac)) s->setIp(d.ip);
      xSemaphoreTake(wizMux, portMAX_DELAY);
      for (auto &da : deviceAddrs) if (da.mac == d.mac) { da.ip = d.ip; break; }
      xSemaphoreGive(wizMux);
    } else if (createAccessory(d.ip, d.mac, d.module, d.type)) {
      need = true;
      Serial.printf("[WiZ] added %s (%s)\n", d.mac.c_str(), d.type.c_str());
    }
  }
  if (need) homeSpan.updateDatabase();
}

// =============================================================================
// Called by HomeSpan once WiFi connects. Just flips `wifiUp` - the WiZ task is
// waiting on it and then drives all discovery/polling off the main thread.
void onWiFiConnected() {
  wifiUp = true;
  Serial.printf("WiFi up. IP=%s  subnet=%s\n",
                WiFi.localIP().toString().c_str(),
                WiFi.subnetMask().toString().c_str());
}

void setup() {
  Serial.begin(115200);
  delay(200);
  Serial.println("\n[WiZHomeKitBridge] starting...");

  // HomeSpan owns WiFi. No hardcoded credentials: if none are stored, it auto-
  // starts the setup Access Point so WiFi can be provisioned from the phone.
  // Discovery is triggered from onWiFiConnected() once a connection is up.
  homeSpan.setApSSID(SETUP_AP_SSID);
  homeSpan.setApPassword(SETUP_AP_PASSWORD);
  homeSpan.enableAutoStartAP();            // start setup AP when no WiFi saved
  homeSpan.setPairingCode(PAIRING_CODE);
  homeSpan.setLogLevel(0);   // silence HomeSpan HAP chatter so our logs are readable
  homeSpan.setWifiCallback(onWiFiConnected);
  homeSpan.begin(Category::Bridges, BRIDGE_NAME, "wiz-bridge");

  // The bridge accessory itself (always the first accessory) - fixed AID 1.
  new SpanAccessory(1);
    new Service::AccessoryInformation();
      new Characteristic::Identify();
      new Characteristic::Name(BRIDGE_NAME);
      new Characteristic::Manufacturer("WiZ");
      new Characteristic::Model("ESP32-WiZ-Bridge");
      new Characteristic::FirmwareRevision("1.0");

  // Local hardware: magnetic reed switch -> HomeKit Contact Sensor - fixed AID 2.
  new SpanAccessory(2);
    new Service::AccessoryInformation();
      new Characteristic::Identify();
      new Characteristic::Name(REED_NAME);
      new Characteristic::Manufacturer("DIY");
      new Characteristic::SerialNumber("reed-1");
      new Characteristic::Model("ReedSwitch");
      new Characteristic::FirmwareRevision("1.0");
    new ReedSensor(REED_PIN, REED_ACTIVE_LOW);

  // Start the WiZ I/O task on core 0. ALL blocking UDP (discovery + polling) runs
  // there, so homeSpan.poll() on the main loop (core 1) is never blocked - this is
  // what prevents the "No Response" stalls.
  wizMux = xSemaphoreCreateMutex();
  xTaskCreatePinnedToCore(wizTaskFn, "wizTask", 10240, NULL, 1, NULL, 0);
}

void loop() {
  homeSpan.poll();          // never blocked - all WiZ UDP is on the core-0 task

  // Apply whatever the WiZ task has produced (state updates / new devices).
  drainWizResults();

  // Heartbeat so the device count is always visible at the tail of the log.
  static uint32_t lastBeat = 0;
  if (millis() - lastBeat > 15000) {
    lastBeat = millis();
    Serial.printf("[WiZ] bridged devices: %d\n", (int)knownMacs.size());
  }
}
