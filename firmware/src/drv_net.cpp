// WiFi + web config + NTP time for the elevator display
//
// ───────── Three rules, carried over from esp32Fire ─────────
// 1. LOSING THE NETWORK MUST BREAK NOTHING. Router down, MQTT broker off — the
//    display still listens to CAN, still shows the floor, still sends calls
//    from the panel. Nothing on the display path (lift.cpp, drv_can.cpp)
//    calls into this file.
// 2. NEVER SLOW DOWN THE CAN PATH. One web request per loop pass; the TWAI
//    driver holds 256 frames meanwhile.
// 3. ANYTHING THAT TOUCHES THE LIFT'S BUS needs someone standing at the panel:
//    enabling commands and enabling ACK mode — NOT on the web page. Remote
//    commands (web + Home Assistant calls) can be switched here, behind the
//    technician code: they only pass when sending commands is ON at the panel.
#include <WiFi.h>
#include <WebServer.h>
#include <Update.h>
#include <ArduinoOTA.h>
#include <esp_timer.h>
#include "ui.h"
#include <Preferences.h>
#include "esp_sntp.h"
#include "drivers.h"
#include "store.h"
#include "lift.h"
#include "web_debug.h"

static Preferences nvsNet;
static WebServer   web(80);

// Network settings live in their own NVS namespace, NOT in struct Settings:
// adding fields there means bumping CFG_MAGIC (wiping user settings), and the
// WiFi password does not belong in the settings block.
static bool     s_on        = false;
static char     s_ssid[33]  = {0};
static char     s_pass[65]  = {0};
static bool     s_apMode    = false;
static uint32_t s_lastTry   = 0;
static uint8_t  s_tries     = 0;
static bool     s_webUp     = false;

#define AP_SSID "LIFT-DISPLAY"
#define AP_PASS "liftdisplay"

static void netLoad() {
  nvsNet.begin("net", false);
  s_on = nvsNet.getBool("on", false);
  if (nvsNet.isKey("ssid")) nvsNet.getString("ssid", s_ssid, sizeof(s_ssid));
  if (nvsNet.isKey("pass")) nvsNet.getString("pass", s_pass, sizeof(s_pass));
  nvsNet.end();
}
static void netSave() {
  nvsNet.begin("net", false);
  nvsNet.putBool("on", s_on);
  nvsNet.putString("ssid", s_ssid);
  nvsNet.putString("pass", s_pass);
  nvsNet.end();
}

// ═════════════════════════ Helpers ═════════════════════════
static void jsonEsc(String& out, const char* s) {
  out += '"';
  for (const char* p = s; *p; p++) {
    const char c = *p;
    if (c == '"' || c == '\\') { out += '\\'; out += c; }
    else if ((uint8_t)c < 0x20) out += ' ';
    else out += c;
  }
  out += '"';
}

// Cut strings on UTF-8 CHARACTER boundaries — SSIDs may carry multi-byte
// characters, and half a character renders as a box.
static void copyUtf8(char* dst, size_t cap, const String& src) {
  size_t n = 0;
  const char* s = src.c_str();
  while (s[n]) {
    const uint8_t c = (uint8_t)s[n];
    const size_t len = c < 0x80 ? 1 : (c & 0xE0) == 0xC0 ? 2 : (c & 0xF0) == 0xE0 ? 3 : 4;
    if (n + len >= cap) break;
    n += len;
  }
  memcpy(dst, s, n);
  dst[n] = 0;
}

static String hex64(uint64_t v) {
  char b[20];
  snprintf(b, sizeof(b), "%08lX%08lX", (unsigned long)(v >> 32), (unsigned long)(v & 0xFFFFFFFF));
  return String(b);
}

// ═════════════════════════ Technician code lock ═════════════════════════
// A 4-digit code is 10 000 options; a script on the LAN tries them all in
// seconds. So: 5 wrong → locked 60 s, each further wrong adds another 60 s.
static uint32_t s_tok = 0, s_tokUntil = 0;
static uint8_t  s_authFails = 0;
static uint32_t s_lockUntil = 0;
static char     s_newSsid[33] = {0}, s_newPass[65] = {0};
static uint8_t  s_netAction = 0;          // 1 = join a new network — done in Net::loop(), outside the request handler

static bool authOk() {
  if (s_tok && timeReached(millis(), s_tokUntil)) s_tok = 0;   // expired: gone for good, not "valid again" in 24.8 days
  if (!s_tok) return false;
  if ((uint32_t)strtoul(web.arg("t").c_str(), nullptr, 16) != s_tok) return false;
  s_tokUntil = millis() + 15UL * 60 * 1000;            // valid while in use
  return true;
}
static bool needAuth() {
  if (authOk()) return true;
  web.send(401, "application/json", "{\"err\":\"login required\"}");
  return false;
}

static void hLogin() {
  const uint32_t now = millis();
  if (s_lockUntil && timeReached(now, s_lockUntil)) s_lockUntil = 0;   // lock over: forget it
  if (s_lockUntil) {
    web.send(429, "application/json", "{\"err\":\"locked, try again later\"}");
    return;
  }
  if ((uint16_t)web.arg("pin").toInt() != Store::cfg().pin) {
    s_authFails++;
    if (s_authFails >= 5) s_lockUntil = now + 60000UL * (s_authFails - 4);
    web.send(403, "application/json", "{\"err\":\"wrong code\"}");
    return;
  }
  s_authFails = 0;
  s_tok = esp_random() | 1;
  s_tokUntil = now + 15UL * 60 * 1000;
  Store::logAdd(EV_LOGIN, 0, 0, SRC_WEB);
  char b[48];
  snprintf(b, sizeof(b), "{\"t\":\"%08lX\"}", (unsigned long)s_tok);
  web.send(200, "application/json", b);
}

// ═════════════════════════ Status (public, read only) ═════════════════════════
uint32_t loopMaxUs();                     // main.cpp

static void hState() {
  const LiftState& s = Lift::st();
  const LiftMode m = Lift::mode();
  Can::Stats cs; Can::stats(cs);
  CmdResult why = RES_OK;
  const bool ok = Lift::cmdAllowed(SRC_WEB, &why);
  String o;
  o.reserve(1100);
  o += "{\"name\":"; jsonEsc(o, Store::cfg().liftName);
  o += ",\"floor\":"; jsonEsc(o, s.linkOk ? s.label : "--");
  o += ",\"idx\":"; o += s.floorIdx;
  o += ",\"dir\":\""; o += s.dir == 1 ? "up" : s.dir == 2 ? "down" : "stop"; o += '"';
  o += ",\"moving\":"; o += s.moving ? "true" : "false";
  o += ",\"door\":"; o += s.door ? "true" : "false";
  o += ",\"mode\":"; jsonEsc(o, Lift::modeText(m));
  o += ",\"modeId\":\""; o += Lift::modeId(m); o += '"';
  o += ",\"fault\":"; o += s.fault;
  o += ",\"load\":"; o += s.load;
  o += ",\"link\":"; o += s.linkOk ? "true" : "false";
  o += ",\"sim\":"; o += Lift::simOn() ? "true" : "false";
  o += ",\"kbps\":"; o += Can::bitrate();
  o += ",\"rate\":"; jsonEsc(o, canRateLabel(Can::bitrate()));
  o += ",\"proto\":"; jsonEsc(o, Lift::protoText());
  {
    const Weather::Now w = Weather::now();
    if (w.valid) { char b[48]; snprintf(b, sizeof(b), ",\"wx\":{\"t\":%.1f,\"code\":%d,\"day\":%d}", w.tempC, w.code, w.isDay ? 1 : 0); o += b; }
  }
  o += ",\"can\":"; jsonEsc(o, Can::stateText());
  o += ",\"fps\":"; o += cs.fps;
  o += ",\"rx\":"; o += cs.rx;
  o += ",\"err\":"; o += cs.busErr;
  o += ",\"miss\":"; o += cs.rxMissed;
  o += ",\"mqtt\":"; o += Mqtt::connected() ? "true" : "false";
  o += ",\"cmdOk\":"; o += ok ? "true" : "false";
  o += ",\"why\":"; jsonEsc(o, Lift::resultText(why));
  o += ",\"last\":"; jsonEsc(o, Lift::lastCmdText());
  o += ",\"car\":\""; o += hex64(s.callCar);
  o += "\",\"up\":\""; o += hex64(s.callUp);
  o += "\",\"dn\":\""; o += hex64(s.callDn);
  o += "\",\"floors\":[";
  for (uint8_t i = 0; i < Store::cfg().floors; i++) {
    if (i) o += ',';
    jsonEsc(o, Store::label(i));
  }
  o += "],\"heap\":"; o += ESP.getFreeHeap();
  o += ",\"loopMs\":"; o += loopMaxUs() / 1000;
  o += ",\"up_s\":"; o += (unsigned long)(esp_timer_get_time() / 1000000ULL);   // does not wrap at 49.7 days
  o += ",\"time\":"; jsonEsc(o, Clock::stamp());
  o += '}';
  web.send(200, "application/json", o);
}

static void hLog() {
  const uint16_t n = Store::logCount() < 40 ? Store::logCount() : 40;
  String o;
  o.reserve(4096);
  o = "[";
  char ts[48], tx[80];
  for (uint16_t i = 0; i < n; i++) {
    const Event& e = Store::logAt(i);
    if (i) o += ',';
    o += '['; jsonEsc(o, Store::logStamp(e, ts, sizeof(ts)));
    o += ','; jsonEsc(o, Store::logText(e, tx, sizeof(tx))); o += ']';
  }
  o += ']';
  web.send(200, "application/json", o);
}

// Print into a String, so a response can be built in RAM and sent in one go
struct StrPrint : public Print {
  String& s;
  explicit StrPrint(String& o) : s(o) {}
  size_t write(uint8_t c) override { s += (char)c; return 1; }
};

static void hCsv() {
  String o;
  o.reserve(16384);
  StrPrint p(o);
  Store::exportCsv(p);
  web.sendHeader("Content-Disposition", "attachment; filename=elevator-log.csv");
  web.send(200, "text/csv; charset=utf-8", o);
}

// ═════════════════════════ Commands from the web ═════════════════════════
// Raw frame from the debug page — the same locks as a call, plus the code.
// Replaying a frame captured from a call panel is how an unknown command gets
// tested; it is also exactly the kind of thing that must never happen by accident.
static uint32_t s_rawTxAt = 0;
static void hTx() {
  if (!needAuth()) return;
  const Settings& c = Store::cfg();
  const bool sim = Lift::simOn();
  const char* why = nullptr;
  if (!c.allowRemote) why = "remote commands are OFF (web /cfg or panel: MENU > HA · MQTT)";
  else if (!sim && !c.txEnable) why = "sending commands is OFF (panel: MENU > CAN BUS)";
  else if (!sim && Can::modeActive() != CAN_NORMAL)
    why = Can::arming() ? "ACK mode is still listening for clean frames" : "CAN is LISTEN-ONLY (panel: MENU > CAN BUS > ENABLE ACK)";
  else if (s_rawTxAt && millis() - s_rawTxAt < 100) why = "too fast";
  CanFrame f = {};
  f.id = (uint32_t)strtoul(web.arg("id").c_str(), nullptr, 16);
  f.ext = web.arg("ext") == "1" || f.id > 0x7FF;
  f.id &= f.ext ? 0x1FFFFFFFUL : 0x7FFUL;
  const String d = web.arg("data");
  for (uint8_t i = 0; i < 8 && (int)(i * 2 + 1) < (int)d.length(); i++) {
    char h[3] = {d[i * 2], d[i * 2 + 1], 0};
    f.d[i] = (uint8_t)strtoul(h, nullptr, 16);
    f.dlc = i + 1;
  }
  if (!why && !Can::send(f)) why = "the driver refused the frame (TX queue full / bus-off)";
  if (!why) { s_rawTxAt = millis(); Store::logAdd(EV_RAW_TX, 0, (int16_t)(f.id & 0x7FFF), SRC_WEB); }
  String o = "{\"ok\":"; o += why ? "false" : "true";
  o += ",\"msg\":"; jsonEsc(o, why ? why : "sent");
  o += '}';
  web.send(why ? 409 : 200, "application/json", o);
}

// ═════════════════════════ Firmware update over the network ═════════════════════════
// POST /api/update?t=<token> with the firmware.bin as a multipart file. The
// image goes into the OTHER app slot; only a complete, verified image is made
// bootable, so a broken upload leaves the running firmware untouched.
// The main loop is busy for the ~20 s of the upload — CAN frames are dropped
// meanwhile and the display restarts at the end anyway.
static bool     s_otaAuth = false, s_otaOk = false;
static char     s_otaErr[64] = "";
static uint32_t s_rebootAt = 0;

static void hUpdateUpload() {
  HTTPUpload& u = web.upload();
  if (u.status == UPLOAD_FILE_START) {
    s_otaAuth = authOk();
    s_otaOk = false;
    s_otaErr[0] = 0;
    if (!s_otaAuth) return;
    Serial.printf("OTA  : receiving %s\n", u.filename.c_str());
    if (!Update.begin(UPDATE_SIZE_UNKNOWN, U_FLASH)) snprintf(s_otaErr, sizeof(s_otaErr), "%s", Update.errorString());
  } else if (u.status == UPLOAD_FILE_WRITE) {
    feedLoopWDT();                              // a slow upload must not trip the loop watchdog
    if (!s_otaAuth || s_otaErr[0]) return;
    if (Update.write(u.buf, u.currentSize) != u.currentSize) snprintf(s_otaErr, sizeof(s_otaErr), "%s", Update.errorString());
  } else if (u.status == UPLOAD_FILE_END) {
    if (!s_otaAuth) return;
    if (s_otaErr[0]) { Update.abort(); return; }
    if (Update.end(true)) {
      s_otaOk = true;
      Serial.printf("OTA  : %u bytes written and verified\n", (unsigned)u.totalSize);
    } else snprintf(s_otaErr, sizeof(s_otaErr), "%s", Update.errorString());
  } else if (u.status == UPLOAD_FILE_ABORTED) {
    Update.abort();
    snprintf(s_otaErr, sizeof(s_otaErr), "upload aborted");
  }
}

static void hUpdateDone() {
  if (!s_otaAuth) { web.send(401, "application/json", "{\"err\":\"login required\"}"); return; }
  if (!s_otaOk) {
    String o = "{\"ok\":false,\"msg\":"; jsonEsc(o, s_otaErr[0] ? s_otaErr : "no firmware received"); o += '}';
    web.send(500, "application/json", o);
    return;
  }
  Store::logAdd(EV_OTA, 0, 0, SRC_WEB);
  web.send(200, "application/json", "{\"ok\":true,\"msg\":\"firmware updated - restarting\"}");
  s_rebootAt = millis() + 1500;              // let the answer reach the browser first
}

// ArduinoOTA (espota): `pio run -e lift_ota -t upload`, password = technician code
static bool s_espota = false;
static void espotaBegin() {
  static char pw[8];
  snprintf(pw, sizeof(pw), "%04u", Store::cfg().pin);
  ArduinoOTA.setHostname("lift-display");
  ArduinoOTA.setPassword(pw);
  ArduinoOTA.onStart([]() { Serial.println("OTA  : espota upload started"); });
  ArduinoOTA.onEnd([]() { Store::logAdd(EV_OTA, 1, 0, SRC_WEB); Store::flush(); Serial.println("OTA  : espota done, restarting"); });
  ArduinoOTA.onError([](ota_error_t e) { Serial.printf("OTA  : espota error %u\n", (unsigned)e); });
  ArduinoOTA.onProgress([](unsigned int, unsigned int) { feedLoopWDT(); });
  ArduinoOTA.begin();
  s_espota = true;
  Serial.println("OTA  : espota ready (host lift-display, password = technician code)");
}

// Read-only bit-timing measurement — listens on the RX pin for ~300 ms
static void hProbe() {
  Can::Probe p;
  if (!Can::probe(p)) { web.send(500, "application/json", "{\"error\":\"probe failed to start\"}"); return; }
  char b[700];
  int k = snprintf(b, sizeof(b), "{\"pulses\":%u,\"highPct\":%u,\"minUs\":%.1f,\"bitUs\":%.3f,\"rate\":%lu,"
           "\"fit\":%.3f,\"nearest\":%u,\"nearestLabel\":\"%s\",\"nearestOk\":%s,"
           "\"slow\":%s,\"gapUs\":%u,\"edgesPerSec\":%u,\"maxRun\":%u,\"uart\":%lu,\"raw\":\"",
           p.pulses, p.highPct, p.minTicks / 10.0f, p.bitUs, (unsigned long)p.rateBps, p.fit,
           p.nearestKbps, canRateLabel(p.nearestKbps), p.nearestOk ? "true" : "false",
           p.slow ? "true" : "false", p.gapUs, p.edgesPerSec, p.maxRun, (unsigned long)p.uartBaud);
  for (uint8_t i = 0; i < p.rawN && k < (int)sizeof(b) - 16; i++)
    k += snprintf(b + k, sizeof(b) - k, "%s%c%lu", i ? " " : "", (p.rawHi >> i) & 1 ? 'H' : 'L', (unsigned long)p.rawUs[i]);
  snprintf(b + k, sizeof(b) - k, "\"}");
  web.send(200, "application/json", b);
}

// Screenshot of the panel as a BMP (public, read only): what the display shows
// right now, read back from the panel's own memory. At most one every 2 s —
// reading 480×320 pixels back holds the main loop for about a second.
// ?show=<screen> first switches the panel to that screen (display only, no
// setting changes; it returns to the indicator when idle) — for documentation.
static void hScreen() {
  if (web.hasArg("show")) {
    static const char* const N[SCR_COUNT] = {"main", "menu", "call", "pin", "can", "sniff", "frame",
      "map", "floors", "floorlist", "net", "wifiscan", "mqtt", "display", "audio", "log", "about",
      "clock", "keyboard"};
    for (uint8_t i = 0; i < SCR_COUNT; i++)
      if (web.arg("show") == N[i]) { UI::show((Screen)i); web.send(200, "application/json", "{\"ok\":true}"); return; }
    web.send(400, "application/json", "{\"err\":\"unknown screen\"}");
    return;
  }
  static uint32_t lastAt = 0;
  if (lastAt && millis() - lastAt < 2000) { web.send(429, "application/json", "{\"err\":\"one screenshot every 2 s\"}"); return; }
  lastAt = millis();
  const uint32_t rowB = LCD_W * 2, img = rowB * LCD_H, hdr = 14 + 40 + 12;
  uint8_t h[66] = {0};
  auto le32 = [&](int at, uint32_t v) { for (int i = 0; i < 4; i++) h[at + i] = (uint8_t)(v >> (8 * i)); };
  h[0] = 'B'; h[1] = 'M'; le32(2, hdr + img); le32(10, hdr);
  le32(14, 40); le32(18, LCD_W); le32(22, (uint32_t)(-(int32_t)LCD_H));   // negative height: top-down rows
  h[26] = 1; h[28] = 16; le32(30, 3); le32(34, img); le32(38, 2835); le32(42, 2835);  // 16 bpp, BI_BITFIELDS
  le32(54, 0xF800); le32(58, 0x07E0); le32(62, 0x001F);                 // RGB565 masks
  const bool swap = !web.hasArg("swap") || web.arg("swap") != "0";
  WiFiClient c = web.client();
  web.setContentLength(hdr + img);
  web.send(200, "image/bmp", "");
  if (c.write(h, hdr) != hdr) return;
  static uint16_t row[LCD_W];
  for (int y = 0; y < LCD_H; y++) {
    if ((y & 31) == 0) feedLoopWDT();
    Display::readRow(y, row);
    if (swap) for (int x = 0; x < LCD_W; x++) row[x] = (uint16_t)((row[x] >> 8) | (row[x] << 8));
    if (c.write((uint8_t*)row, rowB) != rowB) break;    // the client went away: stop at once
  }
}

// Raw capture of the RX pin (public, read only): ?ms=5..100. Answers the level at
// the start and every pulse in 0.1 µs — the /debug page draws and decodes it.
static void hRaw() {
  const uint32_t ms = web.hasArg("ms") ? (uint32_t)constrain(web.arg("ms").toInt(), 5, 100) : 50;
  const uint16_t MAXE = 2400;                              // ≤ 2400 numbers keeps the answer ≤ ~20 KB
  uint32_t* e = (uint32_t*)malloc(MAXE * sizeof(uint32_t));
  if (!e) { web.send(503, "application/json", "{\"err\":\"no memory\"}"); return; }
  uint8_t lvl0 = 1;
  uint32_t cpu = 240;
  const uint16_t n = Can::rawCapture(e, MAXE, ms, lvl0, cpu);
  String o;
  o.reserve(96 + n * 8);
  char b[96];
  snprintf(b, sizeof(b), "{\"ms\":%lu,\"edges\":%u,\"full\":%s,\"lvl0\":%u,\"d\":[",
           (unsigned long)ms, n, n >= MAXE ? "true" : "false", lvl0);
  o += b;
  uint32_t prev = 0;
  for (uint16_t i = 0; i < n; i++) {
    const uint32_t d = (e[i] - prev) * 10 / cpu;          // 0.1 µs; d[0] = start → first edge
    prev = e[i];
    if (i) o += ',';
    o += d;
  }
  o += "]}";
  free(e);
  web.send(200, "application/json", o);
}

// What the CAN controller says about its errors (public, read only): the error code
// capture register sampled at each new bus error since the last rate change.
static const char* eccSeg(uint8_t s) {
  switch (s) {
    case 0x03: return "start of frame";      case 0x02: return "ID bits 10-3";
    case 0x06: return "ID bits 2-0";          case 0x04: return "SRR bit";
    case 0x05: return "IDE bit";              case 0x07: return "ext ID 17-13";
    case 0x0F: return "ext ID 12-5";          case 0x0E: return "ext ID 4-0";
    case 0x0C: return "RTR bit";              case 0x0D: return "reserved bit 1";
    case 0x09: return "reserved bit 0";       case 0x0B: return "DLC";
    case 0x0A: return "data field";           case 0x08: return "CRC";
    case 0x18: return "CRC delimiter";        case 0x19: return "ACK slot";
    case 0x1B: return "ACK delimiter";        case 0x1A: return "end of frame";
    case 0x12: return "intermission";         case 0x11: return "active error flag";
    case 0x16: return "passive error flag";   case 0x13: return "tolerate dominant bits";
    case 0x17: return "error delimiter";      case 0x1C: return "overload flag";
    default:   return "?";
  }
}
static void hErrors() {
  static const char* const TYPE[4] = {"bit", "form", "stuff", "other"};
  static uint32_t c[256];
  const uint32_t n = Can::eccCounts(c);
  String o = "{\"kbps\":\""; o += canRateLabel(Can::bitrate()); o += "\",\"samples\":"; o += n;
  o += ",\"codes\":[";
  for (int k = 0; k < 12; k++) {
    int best = -1;
    for (int i = 0; i < 256; i++) if (c[i] && (best < 0 || c[i] > c[best])) best = i;
    if (best < 0) break;
    char b[128];
    snprintf(b, sizeof(b), "%s{\"ecc\":%d,\"type\":\"%s\",\"rx\":%d,\"seg\":\"%s\",\"n\":%lu}", k ? "," : "",
             best, TYPE[(best >> 6) & 3], (best >> 5) & 1, eccSeg(best & 0x1F), (unsigned long)c[best]);
    o += b;
    c[best] = 0;
  }
  o += "]}";
  web.send(200, "application/json", o);
}

static void hAutobaud() {
  if (!needAuth()) return;
  if (Lift::simOn()) { web.send(409, "application/json", "{\"ok\":false,\"msg\":\"stop the simulation first\"}"); return; }
  Can::autoBaudStart();
  web.send(200, "application/json", "{\"ok\":true,\"msg\":\"detecting the bit rate (listen-only, about 6 s)\"}");
}

static void hCmd() {
  if (!needAuth()) return;
  const String c = web.arg("c");
  const int f = web.arg("f").toInt();
  uint8_t cmd = CMD_COUNT;
  if (c == "car") cmd = CMD_CAR;
  else if (c == "up") cmd = CMD_HALL_UP;
  else if (c == "dn") cmd = CMD_HALL_DN;
  else if (c == "open") cmd = CMD_DOOR_OPEN;
  else if (c == "close") cmd = CMD_DOOR_CLOSE;
  else if (c == "hold") cmd = CMD_DOOR_HOLD;
  const CmdResult r = Lift::command(cmd, f, SRC_WEB);
  String o = "{\"ok\":"; o += r == RES_OK ? "true" : "false";
  o += ",\"msg\":"; jsonEsc(o, Lift::resultText(r)); o += '}';
  web.send(r == RES_OK ? 200 : 409, "application/json", o);
}

// ═════════════════════════ Settings ═════════════════════════
static void cfgJson(String& o) {
  const Settings& c = Store::cfg();
  o += "{\"name\":"; jsonEsc(o, c.liftName);
  o += ",\"floors\":"; o += c.floors;
  o += ",\"main\":"; o += c.mainFloor;
  o += ",\"base\":"; o += c.basements;
  o += ",\"ground\":"; o += c.groundStyle;
  o += ",\"timeout\":"; o += c.timeoutSec;
  o += ",\"kbps\":"; o += c.canKbps;
  o += ",\"mode\":"; o += c.canMode;
  o += ",\"tx\":"; o += c.txEnable ? 1 : 0;
  o += ",\"remote\":"; o += c.allowRemote ? 1 : 0;
  o += ",\"bright\":"; o += c.brightness;
  o += ",\"dimSec\":"; o += c.dimSec;
  o += ",\"dimPct\":"; o += c.dimPct;
  o += ",\"vol\":"; o += c.volume;
  o += ",\"chime\":"; o += c.chime ? 1 : 0;
  o += ",\"kiosk\":"; o += c.kiosk ? 1 : 0;
  o += ",\"touchOff\":"; o += c.touchOff ? 1 : 0;
  {
    const Weather::Cfg w = Weather::cfg();
    const Weather::Now n = Weather::now();
    char city[32]; Weather::city(city, sizeof(city));
    char b[160];
    snprintf(b, sizeof(b), ",\"wxOn\":%d,\"wxLat\":%.4f,\"wxLon\":%.4f,\"wxCity\":\"%s\",\"wxNow\":\"%s\"",
             w.on ? 1 : 0, w.lat, w.lon, city, n.valid ? String(String(n.tempC, 1) + " C, code " + n.code).c_str() : "-");
    o += b;
  }
  o += ",\"led\":"; o += c.statusLed ? 1 : 0;
  o += ",\"audio\":"; o += c.audioApi ? 1 : 0;
  o += ",\"pinDefault\":"; o += c.pin == 1234 ? 1 : 0;
  o += ",\"mqon\":"; o += Mqtt::on() ? 1 : 0;
  o += ",\"mqhost\":"; jsonEsc(o, Mqtt::host());
  o += ",\"mqport\":"; o += Mqtt::port();
  o += ",\"mquser\":"; jsonEsc(o, Mqtt::user());
  o += ",\"mqpass\":"; o += Mqtt::hasPass() ? 1 : 0;
  o += ",\"mqbase\":"; jsonEsc(o, Mqtt::baseTopic());
  o += ",\"mqerr\":"; jsonEsc(o, Mqtt::lastError());
  o += ",\"ssid\":"; jsonEsc(o, s_ssid);
  o += ",\"labels\":[";
  for (uint8_t i = 0; i < c.floors; i++) { if (i) o += ','; jsonEsc(o, Store::label(i)); }
  o += "]}";
}

static void hCfgGet() {
  if (!needAuth()) return;
  String o; o.reserve(900);
  cfgJson(o);
  web.send(200, "application/json", o);
}

// Every value is CLAMPED here and the value ACTUALLY accepted is sent back —
// the page holds no table of limits, and editing its source cannot widen them.
static void hSet() {
  if (!needAuth()) return;
  Settings& c = Store::cfg();
  bool rediscover = false;
  for (int i = 0; i < web.args(); i++) {
    const String k = web.argName(i), v = web.arg(i);
    const long n = v.toInt();
    if (k == "name")         { copyUtf8(c.liftName, NAME_LEN, v); rediscover = true; }
    else if (k == "floors")  { c.floors = (uint8_t)constrain(n, FLOORS_MIN, MAX_FLOORS);
                               if (c.mainFloor >= c.floors) c.mainFloor = 0; rediscover = true; }
    else if (k == "main")    c.mainFloor = (uint8_t)constrain(n, 0, c.floors - 1);
    else if (k == "base")    c.basements = (uint8_t)constrain(n, 0, 9);
    else if (k == "ground")  c.groundStyle = (uint8_t)constrain(n, 0, 3);
    else if (k == "auto")    { Store::autoLabels(); rediscover = true; }
    else if (k == "timeout") c.timeoutSec = (uint8_t)constrain(n, TIMEOUT_MIN, TIMEOUT_MAX);
    else if (k == "remote")  { c.allowRemote = n != 0; rediscover = true; }   // HA call/door entities follow
    else if (k == "kbps") {
      if (n > 0 && n <= 65535 && Can::rateOk((uint16_t)n)) { c.canKbps = (uint16_t)n; Can::setBitrate(c.canKbps); }
    }
    else if (k == "bright")  { c.brightness = (uint8_t)constrain(n, BRIGHT_MIN, BRIGHT_MAX); Display::setBrightness(c.brightness); }
    else if (k == "dimSec")  { c.dimSec = (uint16_t)constrain(n, 0, DIMSEC_MAX); Display::setSleepTimeout(c.dimSec * 1000UL); }
    else if (k == "dimPct")  { c.dimPct = (uint8_t)constrain(n, DIMPCT_MIN, DIMPCT_MAX); Display::setDimLevel(c.dimPct); }
    else if (k == "vol")     { c.volume = (uint8_t)constrain(n, VOL_MIN, VOL_MAX); Out::setVolume(c.volume); }
    else if (k == "chime")   c.chime = n != 0;
    else if (k == "kiosk")   c.kiosk = n != 0;
    else if (k == "touchOff") { c.touchOff = n != 0; Display::setTouchEnabled(!c.touchOff); }
    else if (k == "wxOn")  { Weather::Cfg w = Weather::cfg(); w.on = n != 0; Weather::setCfg(w); }
    else if (k == "wxLat") { Weather::Cfg w = Weather::cfg(); w.lat = v.toFloat(); Weather::setCfg(w); }
    else if (k == "wxLon") { Weather::Cfg w = Weather::cfg(); w.lon = v.toFloat(); Weather::setCfg(w); }
    else if (k == "led")     { c.statusLed = n != 0; Out::setLedEnabled(c.statusLed); }
    else if (k == "audio")   c.audioApi = n != 0;
    else if (k.startsWith("L") && k.length() <= 3) {
      const int idx = k.substring(1).toInt();
      if (idx >= 0 && idx < c.floors) { Store::setLabel((uint8_t)idx, v.c_str()); rediscover = true; }
    }
    else if (k == "pin") {
      // Refuse 1234 again: a code everybody knows is no lock
      if (n >= 0 && n <= 9999 && v.length() == 4 && n != 1234) c.pin = (uint16_t)n;
    }
    else if (k == "mqon")    Mqtt::setEnabled(n != 0);
    else if (k == "mqhost")  Mqtt::setServer(v.c_str(), web.hasArg("mqport") ? (uint16_t)web.arg("mqport").toInt() : Mqtt::port());
    else if (k == "mquser")  Mqtt::setAuth(v.c_str(), web.hasArg("mqpass") && web.arg("mqpass").length() ? web.arg("mqpass").c_str() : nullptr);
    else if (k == "ssid")    { copyUtf8(s_newSsid, sizeof(s_newSsid), v);
                               copyUtf8(s_newPass, sizeof(s_newPass), web.arg("pass"));
                               s_netAction = 1; }
  }
  Store::save();
  Store::logAdd(EV_CONFIG, 0, 0, SRC_WEB);
  if (rediscover) Mqtt::rediscover();
  String o; o.reserve(900);
  cfgJson(o);
  web.send(200, "application/json", o);
}

// ───── Protocol map ─────
static void hMapGet() {
  if (!needAuth()) return;
  const LiftMap& m = Store::map();
  String o = "{\"preset\":"; o += m.preset;
  o += ",\"proto\":"; o += m.proto;
  o += ",\"coLift\":"; o += m.coLift;
  o += ",\"coNode\":"; o += m.coNode;
  o += ",\"coBase\":"; o += m.coBase;
  o += ",\"coCallId\":\""; o += String(Lift::coCallId(), HEX); o += '\"';
  o += ",\"sig\":[";
  char b[96];
  for (uint8_t i = 0; i < SIG_COUNT; i++) {
    const SigMap& g = m.sig[i];
    snprintf(b, sizeof(b), "%s[%d,\"%lX\",%d,%u,\"%02X\",%u,%d,%d]", i ? "," : "",
             (g.flags & MAP_USED) ? 1 : 0, (unsigned long)g.id, (g.flags & MAP_EXT) ? 1 : 0,
             g.byte, g.mask, g.enc, g.offset, (g.flags & MAP_INV) ? 1 : 0);
    o += b;
  }
  o += "],\"cmd\":[";
  for (uint8_t i = 0; i < CMD_COUNT; i++) {
    const CmdMap& k = m.cmd[i];
    char hx[17];
    for (uint8_t j = 0; j < 8; j++) snprintf(hx + j * 2, 3, "%02X", k.data[j]);
    snprintf(b, sizeof(b), "%s[%d,\"%lX\",%d,%u,\"%s\",%u,%u,%d,%u,%d,%d,%d]", i ? "," : "",
             (k.flags & MAP_USED) ? 1 : 0, (unsigned long)k.id, (k.flags & MAP_EXT) ? 1 : 0,
             k.dlc, hx, k.fbyte, k.fenc, k.offset, k.repeat,
             (k.flags & MAP_IDFLR) ? 1 : 0, (k.flags & MAP_REL) ? 1 : 0, (k.flags & MAP_MIRROR) ? 1 : 0);
    o += b;
  }
  o += "]}";
  web.send(200, "application/json", o);
}

// s<i> = "used,id,ext,byte,mask,enc,offset,invert"   c<i> = "used,id,ext,dlc,data hex,fbyte,fenc,offset,repeat"
static void hMapPost() {
  if (!needAuth()) return;
  LiftMap& m = Store::map();
  if (web.arg("preset") == "evl")    Lift::presetEvl(m);
  if (web.arg("preset") == "clear")  Lift::presetClear(m);
  if (web.arg("preset") == "cia417") Lift::presetCia417(m);
  if (web.arg("preset") == "fuji")   Lift::presetFuji(m);
  if (web.hasArg("proto"))  m.proto  = (uint8_t)constrain(web.arg("proto").toInt(), 0, PROTO_COUNT - 1);
  if (web.hasArg("coLift")) m.coLift = (uint8_t)constrain(web.arg("coLift").toInt(), 1, 8);
  if (web.hasArg("coNode")) m.coNode = (uint8_t)constrain(web.arg("coNode").toInt(), 1, 127);
  if (web.hasArg("coBase")) m.coBase = (int8_t)constrain(web.arg("coBase").toInt(), OFFSET_MIN, OFFSET_MAX);
  bool touched = false;
  for (uint8_t i = 0; i < SIG_COUNT; i++) {
    const String key = "s" + String(i);
    if (!web.hasArg(key)) continue;
    long f[8] = {0, 0, 0, 0, 0xFF, 0, 0, 0};
    const String v = web.arg(key);
    int p = 0;
    for (uint8_t j = 0; j < 8; j++) {
      const int q = v.indexOf(',', p);
      const String part = v.substring(p, q < 0 ? v.length() : q);
      f[j] = (j == 1 || j == 4) ? strtol(part.c_str(), nullptr, 16) : part.toInt();
      if (q < 0) break;
      p = q + 1;
    }
    SigMap& g = m.sig[i];
    g.flags  = (uint8_t)((f[0] ? MAP_USED : 0) | (f[2] ? MAP_EXT : 0) | (f[7] ? MAP_INV : 0));
    g.id     = (uint32_t)f[1] & (f[2] ? 0x1FFFFFFFUL : 0x7FFUL);
    g.byte   = (uint8_t)constrain(f[3], 0, 7);
    g.mask   = (uint8_t)f[4];
    g.enc    = (uint8_t)constrain(f[5], 0, FENC_COUNT - 1);
    g.offset = (int8_t)constrain(f[6], -64, 64);
    touched = true;
  }
  for (uint8_t i = 0; i < CMD_COUNT; i++) {
    const String key = "c" + String(i);
    if (!web.hasArg(key)) continue;
    String part[12];
    const String v = web.arg(key);
    int p = 0;
    for (uint8_t j = 0; j < 12; j++) {
      const int q = v.indexOf(',', p);
      part[j] = v.substring(p, q < 0 ? v.length() : q);
      if (q < 0) break;
      p = q + 1;
    }
    CmdMap& k = m.cmd[i];
    const bool ext = part[2].toInt() != 0;
    k.flags  = (uint8_t)((part[0].toInt() ? MAP_USED : 0) | (ext ? MAP_EXT : 0) |
                         (part[9].toInt() ? MAP_IDFLR : 0) | (part[10].toInt() ? MAP_REL : 0) |
                         (part[11].toInt() ? MAP_MIRROR : 0));
    k.id     = (uint32_t)strtoul(part[1].c_str(), nullptr, 16) & (ext ? 0x1FFFFFFFUL : 0x7FFUL);
    k.dlc    = (uint8_t)constrain(part[3].toInt(), 0, 8);
    memset(k.data, 0, 8);
    for (uint8_t j = 0; j < 8 && (int)(j * 2 + 1) < (int)part[4].length(); j++) {
      char hx[3] = {part[4][j * 2], part[4][j * 2 + 1], 0};
      k.data[j] = (uint8_t)strtoul(hx, nullptr, 16);
    }
    k.fbyte  = (uint8_t)constrain(part[5].toInt(), 0, 7);
    k.fenc   = (uint8_t)constrain(part[6].toInt(), 0, CENC_COUNT - 1);
    k.offset = (int8_t)constrain(part[7].toInt(), -64, 64);
    k.repeat = (uint8_t)constrain(part[8].toInt(), 1, (k.flags & MAP_MIRROR) ? 255 : 3);
    touched = true;
  }
  if (touched && !web.hasArg("preset")) m.preset = PRESET_CUSTOM;
  Store::saveMap();
  Store::logAdd(EV_CONFIG, 0, 0, SRC_WEB);
  Mqtt::rediscover();
  hMapGet();
}

// ═════════════════════════ The two HTML pages ═════════════════════════
// The status page `/` polls once a second so it must stay light; the settings
// page `/cfg` is heavier but loads once. Separate pages: the light one does not
// carry the heavy one every second.
static const char PAGE[] PROGMEM = R"HTML(<!doctype html><meta charset=utf-8>
<meta name=viewport content="width=device-width,initial-scale=1">
<title>Elevator</title>
<style>
:root{--bg:#0a0d12;--s:#141a23;--s2:#1c2531;--l:#293547;--t:#e9eef6;--d:#8695ab;--ok:#16a34a;--al:#e11d22;--fa:#f0a500;--iso:#3b82f6;--ar:#22d3ee}
body{margin:0;font:15px system-ui,sans-serif;background:var(--bg);color:var(--t)}
main{max-width:720px;margin:auto;padding:12px}
.top{display:flex;justify-content:space-between;color:var(--d);font-size:13px}
.ind{display:flex;align-items:center;gap:18px;background:#000;border:1px solid var(--l);border-radius:8px;padding:14px 20px;margin:10px 0;flex-wrap:wrap}
.arr{font-size:72px;color:var(--ar);width:70px;text-align:center}
.fl{font:800 120px/1 system-ui;letter-spacing:-4px;min-width:180px}
.side{flex:1;display:grid;gap:6px;min-width:180px}
.chip{background:var(--s);border:1px solid var(--l);border-radius:6px;padding:6px 10px}
.chip b{float:right}
.ban{padding:10px;border-radius:6px;font-weight:700;text-align:center;display:none}
.grid{display:grid;grid-template-columns:repeat(auto-fill,minmax(64px,1fr));gap:6px;margin:8px 0}
button{background:var(--s2);color:var(--t);border:1px solid var(--l);border-radius:6px;padding:10px;font:600 15px system-ui;cursor:pointer}
button.on{background:var(--ok);border-color:var(--ok)}
button:disabled{opacity:.35;cursor:not-allowed}
.dim{color:var(--d);font-size:13px}
table{width:100%;border-collapse:collapse;font-size:13px}td{border-bottom:1px solid var(--l);padding:4px}
a{color:var(--ar)}
</style>
<main>
<div class=top><span id=nm>…</span><span id=tm></span></div>
<div class=ind><div class=arr id=ar></div><div class=fl id=fl>--</div>
<div class=side><div class=chip>Door<b id=dr>—</b></div><div class=chip>Mode<b id=md>—</b></div>
<div class=chip>Load<b id=ld>—</b></div><div class=chip>CAN<b id=cn>—</b></div></div></div>
<div class=ban id=bn></div>
<h3>Car call <span class=dim id=why></span></h3>
<div class=dim>Web commands need a <a href=/cfg>login</a> and "Remote commands" ON (panel or settings page).</div>
<div class=grid id=gr></div>
<button id=bo onclick="cmd('open')">DOOR OPEN</button> <button id=bc onclick="cmd('close')">DOOR CLOSE</button> <button id=bh onclick="cmd('hold')">DOOR HOLD 15 s</button>
<p class=dim id=last></p>
<h3>Recent events</h3><table id=lg></table>
<p class=dim><a href=/debug>CAN debug</a> · <a href=/cfg>Settings</a> · <a href=/log.csv>Download log (CSV)</a></p>
</main>
<script>
const $=i=>document.getElementById(i);let S=null,nF=-1;
const T=()=>sessionStorage.t||'';
function bit(h,i){return (BigInt('0x'+h)>>BigInt(i))&1n}
async function cmd(c,f){if(!T()){location='/cfg';return}
 const r=await fetch('/api/cmd',{method:'POST',body:new URLSearchParams({t:T(),c,f:f??0})});
 if(r.status==401){location='/cfg';return}const j=await r.json();$('last').textContent=j.msg}
function grid(){$('gr').innerHTML=S.floors.map((l,i)=>`<button id=g${i} onclick="cmd('car',${i})">${l}</button>`).reverse().join('');nF=S.floors.length}
async function tick(){try{S=await(await fetch('/api/state')).json()}catch(e){return}
 $('nm').textContent=S.name+(S.sim?' · SIMULATION':'');$('tm').textContent=S.time;
 $('fl').textContent=S.floor;$('ar').textContent=S.dir=='up'?'▲':S.dir=='down'?'▼':'';
 $('ar').style.opacity=S.moving?1:.45;$('dr').textContent=S.door?'OPEN':'CLOSED';
 $('md').textContent=S.mode;$('ld').textContent=S.load<0?'—':S.load+' %';
 $('cn').textContent=(S.rate||S.kbps)+'k · '+S.fps+'/s';
 const C={fire:'--al',fault:'--fa',inspection:'--iso',out_of_service:'--d',overload:'--al',no_link:'--fa'};
 const b=$('bn');if(C[S.modeId]){b.style.display='block';b.style.background=`var(${C[S.modeId]})`;
  b.textContent=S.mode+(S.fault?' · code '+S.fault:'')}else b.style.display='none';
 if(nF!=S.floors.length)grid();
 S.floors.forEach((l,i)=>{const e=$('g'+i);e.disabled=!S.cmdOk;e.className=bit(S.car,i)?'on':''});
 $('bo').disabled=$('bc').disabled=!S.cmdOk;$('why').textContent=S.cmdOk?'':'— '+S.why;
 if(S.last)$('last').textContent='Last command: '+S.last}
async function logs(){const L=await(await fetch('/api/log')).json();
 $('lg').innerHTML=L.slice(0,12).map(r=>`<tr><td class=dim>${r[0]}<td>${r[1]}`).join('')}
tick();logs();setInterval(tick,1000);setInterval(logs,10000);
</script>)HTML";

static const char CFGPAGE[] PROGMEM = R"HTML(<!doctype html><meta charset=utf-8>
<meta name=viewport content="width=device-width,initial-scale=1">
<title>Elevator settings</title>
<style>
:root{--bg:#0a0d12;--s:#141a23;--s2:#1c2531;--l:#293547;--t:#e9eef6;--d:#8695ab;--ok:#16a34a;--al:#e11d22;--fa:#f0a500}
body{margin:0;font:14px system-ui,sans-serif;background:var(--bg);color:var(--t)}
main{max-width:860px;margin:auto;padding:12px}
.sec{background:var(--s);border:1px solid var(--l);border-radius:8px;padding:10px 14px;margin:12px 0;overflow-x:auto}
h2{font-size:16px;margin:4px 0 8px}.n{color:var(--d);font-size:13px;margin:0 0 8px}
.r{display:flex;justify-content:space-between;align-items:center;gap:10px;padding:6px 0;border-bottom:1px solid var(--l);flex-wrap:wrap}
.r:last-child{border:0}
input,select{background:var(--s2);color:var(--t);border:1px solid var(--l);border-radius:4px;padding:5px;font:inherit}
input[type=number]{width:80px}input.h{width:70px;font-family:monospace}input.lb{width:46px;text-transform:uppercase}
button{background:var(--s2);color:var(--t);border:1px solid var(--l);border-radius:5px;padding:7px 12px;font:600 14px system-ui;cursor:pointer}
button.p{background:var(--ok);border-color:var(--ok)}button.x{background:#3a2226}
table{border-collapse:collapse;width:100%;font-size:13px}th{color:var(--d);text-align:left;font-weight:500}
td,th{padding:3px;border-bottom:1px solid var(--l)}
.warn{color:var(--fa)}.err{color:var(--al)}#msg{position:sticky;top:0;background:var(--ok);padding:6px 10px;border-radius:5px;display:none}
.lbs{display:flex;flex-wrap:wrap;gap:6px}.lbs label{display:flex;flex-direction:column;align-items:center;color:var(--d);font-size:11px}
</style>
<main>
<div id=msg></div>
<div id=login class=sec><h2>Log in</h2><p class=n>Use the panel's technician code. 5 wrong attempts lock the page for 60 s.</p>
<input id=pin type=password inputmode=numeric maxlength=4> <button class=p onclick=login()>ENTER</button> <span id=le class=err></span></div>
<div id=app style=display:none></div>
<p class=n><a href=/ style=color:#22d3ee>← Status</a></p>
</main>
<script>
const $=i=>document.getElementById(i);let C=null,M=null;
const T=()=>sessionStorage.t||'';
const esc=s=>String(s).replace(/&/g,'&amp;').replace(/</g,'&lt;').replace(/"/g,'&quot;');
function flash(t,bad){const m=$('msg');m.textContent=t;m.style.background=bad?'var(--al)':'var(--ok)';m.style.display='block';setTimeout(()=>m.style.display='none',2500)}
async function api(u,body){const o=body?{method:'POST',body:new URLSearchParams({t:T(),...body})}:{};
 const r=await fetch(u+(body?'':'?t='+T()),o);if(r.status==401){sessionStorage.t='';show();throw 0}return r.json()}
async function login(){const r=await fetch('/api/login',{method:'POST',body:new URLSearchParams({pin:$('pin').value})});
 const j=await r.json();if(j.t){sessionStorage.t=j.t;show()}else $('le').textContent=j.err}
function setRemote(e){if(e.value=='1'&&!confirm('Turn remote commands ON? Anyone who has the technician code on this network — and Home Assistant — can then call the lift.')){e.value='0';return}set('remote',e.value)}
async function set(k,v){C=await api('/api/set',{[k]:v});flash('Saved');render()}
async function setMany(o){C=await api('/api/set',o);flash('Saved');render()}
const R=(n,h,c)=>`<div class=r><div>${n}${h?`<div class=n>${h}</div>`:''}</div><div>${c}</div></div>`;
const NUM=(k,v,mn,mx,st)=>`<input type=number value=${v} min=${mn} max=${mx} step=${st||1} onchange="set('${k}',this.value)">`;
const SEL=(k,v,o)=>`<select onchange="set('${k}',this.value)">${o.map((t,i)=>`<option value=${Array.isArray(t)?t[0]:i} ${(Array.isArray(t)?t[0]:i)==v?'selected':''}>${Array.isArray(t)?t[1]:t}</option>`).join('')}</select>`;
const SIG=['Floor','Up','Down','Moving','Door open','Fire service','Inspection','Overload','Out of service','Fault code','Load %','Car calls (bitmap)','Hall calls up (bitmap)','Hall calls down (bitmap)'];
const CMD=['Car call','Hall call up','Hall call down','Door open','Door close','Door hold'];
function mapHtml(){const s=M.sig.map((g,i)=>`<tr><td>${SIG[i]}<td><input type=checkbox id=su${i} ${g[0]?'checked':''}>
<td><input class=h id=si${i} value=${g[1]}><td><input type=checkbox id=sx${i} ${g[2]?'checked':''}>
<td><input type=number id=sb${i} value=${g[3]} min=0 max=7 style=width:44px><td><input class=h id=sm${i} value=${g[4]} style=width:40px>
<td>${i==0?`<select id=se${i}>${['binary','BCD','2 ASCII chars'].map((t,k)=>`<option value=${k} ${k==g[5]?'selected':''}>${t}`).join('')}</select>`:`<input type=hidden id=se${i} value=0>`}
<td>${i==0||i>=11?`<input type=number id=so${i} value=${g[6]} style=width:52px>`:`<input type=hidden id=so${i} value=0>`}
<td>${i>=1&&i<=8?`<input type=checkbox id=sv${i} ${g[7]?'checked':''}>`:`<input type=hidden id=sv${i}>`}`).join('');
const c=M.cmd.map((k,i)=>`<tr><td>${CMD[i]}<td><input type=checkbox id=cu${i} ${k[0]?'checked':''}>
<td><input class=h id=ci${i} value=${k[1]}><td><input type=checkbox id=cx${i} ${k[2]?'checked':''}>
<td><input type=number id=cd${i} value=${k[3]} min=0 max=8 style=width:44px><td><input id=ch${i} value=${k[4]} style="width:150px;font-family:monospace">
<td><input type=number id=cb${i} value=${k[5]} min=0 max=7 style=width:44px>
<td><select id=ce${i}>${['none','1 byte','bitmap'].map((t,j)=>`<option value=${j} ${j==k[6]?'selected':''}>${t}`).join('')}</select>
<td><input type=number id=co${i} value=${k[7]} style=width:52px><td><input type=number id=cr${i} value=${k[8]} min=1 max=3 style=width:40px>
<td><input type=checkbox id=cf${i} ${k[9]?'checked':''}><td><input type=checkbox id=cl${i} ${k[10]?'checked':''}><td><input type=checkbox id=cm${i} ${k[11]?'checked':''}>`).join('');
const co=M.proto==1;
const cob=`<div class=r><div>Protocol<div class=n>CiA 417 = built-in CANopen-Lift decoder; the signal map below still applies on top of it</div></div><div><select onchange="coSet({proto:this.value})"><option value=1 ${co?'selected':''}>CiA 417 (CANopen-Lift)<option value=0 ${co?'':'selected'}>signal map only</select></div></div>
${co?`<div class=r><div>Lift number<div class=n>which call controller: records on ID ${(0x3FF+M.coLift).toString(16).toUpperCase()}</div></div><div><input type=number min=1 max=8 value=${M.coLift} onchange="coSet({coLift:this.value})"></div></div>
<div class=r><div>Our node-ID<div class=n>calls are sent as a panel on ID ${M.coCallId.toUpperCase()} — must not be used by another device</div></div><div><input type=number min=1 max=127 value=${M.coNode} onchange="coSet({coNode:this.value})"></div></div>
<div class=r><div>Bus floor number of the lowest floor<div class=n>CiA 417 numbering in the records (usually 1)</div></div><div><input type=number min=-20 max=20 value=${M.coBase} onchange="coSet({coBase:this.value})"></div></div>`:''}`;
return cob+`<table><tr><th>Signal<th>Used<th>ID (hex)<th>29-bit<th>Byte<th>Mask<th>Encoding<th>Offset<th>Invert${s}</table>
<p class=n>FLOOR offset = the bus value of the lowest floor. Bitmap offset = the bit number of the lowest floor.</p>
<table><tr><th>Command<th>Used<th>ID (hex)<th>29-bit<th>DLC<th>Template data (hex)<th>Floor byte<th>Type<th>Offset<th>Repeat<th>ID + floor<th>Release<th>Mirror${c}</table>
<p class=n>ID + floor: one CAN-ID per floor (hall button boards). Release: 250 ms later the template is sent again without the floor bit, like a button let go. Mirror: the last frame received with this ID is re-sent with the template bits set, right after each of its next <i>Repeat</i> frames (up to 255) — for buttons a board reports in its own status frame.</p>
<p><button class=p onclick=saveMap()>SAVE MAP</button> <button onclick="presetMap('fuji')">LOAD FUJI</button> <button onclick="presetMap('cia417')">USE CiA 417</button> <button onclick="presetMap('evl')">LOAD EVL-CAN TEMPLATE</button> <button class=x onclick="presetMap('clear')">CLEAR ALL</button></p>`}
async function saveMap(){const o={};
 M.sig.forEach((g,i)=>o['s'+i]=[$('su'+i).checked?1:0,$('si'+i).value,$('sx'+i).checked?1:0,$('sb'+i).value,$('sm'+i).value,$('se'+i).value,$('so'+i).value,$('sv'+i).checked?1:0].join(','));
 M.cmd.forEach((k,i)=>o['c'+i]=[$('cu'+i).checked?1:0,$('ci'+i).value,$('cx'+i).checked?1:0,$('cd'+i).value,$('ch'+i).value.replace(/[^0-9a-f]/gi,'').padEnd(16,'0'),$('cb'+i).value,$('ce'+i).value,$('co'+i).value,$('cr'+i).value,$('cf'+i).checked?1:0,$('cl'+i).checked?1:0,$('cm'+i).checked?1:0].join(','));
 M=await api('/api/map',o);flash('Protocol map saved');render()}
async function coSet(o){M=await api('/api/map',o);flash('Saved');$('map').innerHTML=mapHtml()}
async function presetMap(p){if(p=='clear'&&!confirm('Clear the whole protocol map?'))return;M=await api('/api/map',{preset:p});flash('Loaded');render()}
function render(){const c=C;$('app').innerHTML=
`<div class=sec><h2>Lift</h2>
${R('Lift name','status bar, and the device name in Home Assistant',`<input value="${esc(c.name)}" maxlength=23 onchange="set('name',this.value)">`)}
${R('Floors','',NUM('floors',c.floors,2,64))}
${R('Main floor','fire recall floor',SEL('main',c.main,c.labels))}
${R('Signal timeout','no status frame for this long → NO SIGNAL',NUM('timeout',c.timeout,1,30)+' s')}
${R('Auto naming','',`basements ${NUM('base',c.base,0,9)} ground floor called ${SEL('ground',c.ground,['1','G','L','0'])} <button onclick="set('auto',1)">APPLY</button>`)}
<div class=r><div>Floor labels<div class=n>up to 3 characters: A–Z, 0–9, dash</div></div></div>
<div class=lbs>${c.labels.map((l,i)=>`<label><input class=lb maxlength=3 value="${esc(l)}" onchange="set('L${i}',this.value)">${i}</label>`).join('')}</div></div>
<div class=sec><h2>CAN bus</h2>
${R('Bit rate','changing it goes back to listening before ACK is enabled again',SEL('kbps',c.kbps,[[12,'12.5'],[16,'16'],[20,'20'],[25,'25'],[2778,'27.78'],[33,'33.3'],[40,'40'],[47,'47.6'],[50,'50'],[62,'62.5'],[66,'66.7'],[80,'80'],[83,'83.3'],[100,'100'],[125,'125'],[200,'200'],[250,'250'],[400,'400'],[500,'500'],[666,'666.7'],[800,'800'],[1000,'1000']].concat(c.kbps>1000&&c.kbps!=2778?[[c.kbps,(c.kbps/100).toFixed(2)+' (measured)']]:[]).map(([v,l])=>[v,l+' kbit/s'])))}
${R('Mode','can only be changed at the panel',c.mode?'<b class=warn>ACK</b>':'<b>LISTEN-ONLY</b>')}
${R('Send commands to the lift','can only be changed at the panel',c.tx?'<b class=warn>ON</b>':'<b>off</b>')}
${R('Remote commands (web + HA)','calls and door commands from this web site and Home Assistant — they still need SEND COMMANDS on at the panel',`<select onchange="setRemote(this)"><option value=0 ${c.remote?'':'selected'}>Off<option value=1 ${c.remote?'selected':''}>On</select>`)}</div>
<div class=sec><h2>Protocol</h2><p class=n>CiA 417 is the default. For any other lift, find IDs and bytes with the CAN debugger (<a href=/debug>/debug</a>) or the panel's RAW FRAMES screen: run the lift and watch which byte follows the floor.</p><div id=map>${M?mapHtml():'…'}</div></div>
<div class=sec><h2>MQTT · Home Assistant</h2>
${R('MQTT','',SEL('mqon',c.mqon,['Off','On']))}
${R('Broker','',`<input id=mh value="${esc(c.mqhost)}" placeholder=192.168.1.10> : <input id=mp type=number value=${c.mqport} style=width:70px>`)}
${R('User','',`<input id=mu value="${esc(c.mquser)}"> <input id=mw type=password placeholder="${c.mqpass?'(unchanged)':'password'}">`)}
${R('Base topic',c.mqerr?'<span class=err>'+esc(c.mqerr)+'</span>':'',`<code>${esc(c.mqbase)}</code>`)}
<p><button class=p onclick="setMany({mqhost:$('mh').value,mqport:$('mp').value,mquser:$('mu').value,mqpass:$('mw').value})">SAVE MQTT</button></p></div>
<div class=sec><h2>WiFi</h2>
${R('Network',`current: ${esc(c.ssid)||'(not set)'}`,`<input id=ws placeholder="network name"> <input id=wp type=password placeholder="password">`)}
<p><button onclick="if(confirm('The display will leave the current network to join the new one. Continue?'))setMany({ssid:$('ws').value,pass:$('wp').value})">JOIN THIS NETWORK</button></p></div>
<div class=sec><h2>Display & sound</h2>
${R('Brightness','',NUM('bright',c.bright,10,100,10)+' %')}
${R('Sleep after','0 = never',NUM('dimSec',c.dimSec,0,600,30)+' s')}
${R('Sleep brightness','0 = backlight off',NUM('dimPct',c.dimPct,0,60,10)+' %')}
${R('Display-only mode','hides the buttons; hold 2.5 s on the indicator to reach the menu',SEL('kiosk',c.kiosk,['Off','On']))}
${R('Touch screen','Off = the panel ignores every touch (the display keeps working). Turn it back on here or over USB: touch 1',SEL('touchOff',c.touchOff,['On','Off']))}
${R('Status LED','the RGB LED on the board',SEL('led',c.led,['Off','On']))}
${R('Weather','temperature + icon after the clock (Open-Meteo, every 15 min) · now: '+esc(c.wxNow),SEL('wxOn',c.wxOn,['Off','On']))}
${R('Weather location','0 / 0 = automatic from the internet address'+(c.wxCity?' (found: '+esc(c.wxCity)+')':''),`<input type=number step=0.0001 value=${c.wxLat} style=width:110px onchange="set('wxLat',this.value)"> <input type=number step=0.0001 value=${c.wxLon} style=width:110px onchange="set('wxLon',this.value)">`)}
${R('Volume','',NUM('vol',c.vol,1,10))}
${R('Audio announcements','HTTP port 81 /play and MQTT …/announce',SEL('audio',c.audio,['Off','On']))}
${R('Arrival chime','1 stroke = going up · 2 strokes = going down',SEL('chime',c.chime,['Off','On']))}</div>
<div class=sec><h2>System</h2>
${R('Change technician code',c.pinDefault?'<span class=warn>still the default 1234 — change it</span>':'',`<input id=p1 type=password maxlength=4 style=width:60px> <input id=p2 type=password maxlength=4 style=width:60px placeholder="again"> <button onclick="if($('p1').value.length==4&&$('p1').value==$('p2').value)set('pin',$('p1').value);else flash('The two entries differ',1)">CHANGE</button>`)}
${R('Event log','',`<a href=/log.csv style=color:#22d3ee>download CSV</a>`)}</div>
<div class=sec><h2>Firmware update</h2><p class=n>Upload <code>firmware.bin</code> (PlatformIO: <code>firmware/.pio/build/lift/firmware.bin</code>). It is written to the spare slot and only used if it arrives complete; the display then restarts. Settings are kept.</p>
<input type=file id=fw accept=.bin> <button class=p onclick=ota()>UPLOAD</button> <span id=otap class=n></span></div>
<div class=sec><h2>Deliberately NOT on the web</h2><table>
<tr><td>Enable ACK mode<td>a misconfigured acknowledging node is enough to make the controller report a communication fault and stop the lift
<tr><td>Enable sending commands<td>someone must be at the lift to see what it does
<tr><td>Simulation<td>detaches the display from the real bus
<tr><td>Touch calibration<td>needs a finger on the four corners
<tr><td>Factory reset<td>one wrong click over the network wipes the protocol map</table></div>`}
function ota(){const f=$('fw').files[0];if(!f){flash('Choose firmware.bin first',1);return}
 if(!confirm('Update the firmware? The display restarts when it is done.'))return;
 const x=new XMLHttpRequest();x.open('POST','/api/update?t='+T());
 x.upload.onprogress=e=>{if(e.lengthComputable)$('otap').textContent=Math.round(e.loaded*100/e.total)+' %'};
 x.onload=()=>{let j={};try{j=JSON.parse(x.responseText)}catch{}$('otap').textContent=j.msg||('HTTP '+x.status);
  if(j.ok)setTimeout(()=>location.reload(),12000)};
 x.onerror=()=>$('otap').textContent='upload failed';
 const fd=new FormData();fd.append('firmware',f,f.name);x.send(fd)}
async function show(){if(!T()){$('login').style.display='';$('app').style.display='none';return}
 $('login').style.display='none';$('app').style.display='';
 try{C=await api('/api/cfg');render();M=await api('/api/map');$('map').innerHTML=mapHtml()}catch(e){}}
show();
</script>)HTML";

static bool s_routes = false;
static void webBegin() {
  if (s_webUp) return;
  if (s_routes) { web.begin(); s_webUp = true; return; }   // after stop(): routes are still there
  s_routes = true;
  web.on("/", []() { web.send_P(200, "text/html; charset=utf-8", PAGE); });
  web.on("/cfg", []() { web.send_P(200, "text/html; charset=utf-8", CFGPAGE); });
  web.on("/api/state", hState);
  web.on("/api/log", hLog);
  web.on("/log.csv", hCsv);
  web.on("/api/login", HTTP_POST, hLogin);
  web.on("/api/cmd", HTTP_POST, hCmd);
  web.on("/debug", []() { web.send_P(200, "text/html; charset=utf-8", DEBUGPAGE); });
  web.on("/api/can/stream", HTTP_GET, []() {
    WiFiClient c = web.client();
    if (!CanDebug::adopt(c)) web.send(503, "text/plain", "two debug viewers already open");
  });
  web.on("/api/tx", HTTP_POST, hTx);
  web.on("/api/can/autobaud", HTTP_POST, hAutobaud);
  web.on("/api/can/probe", HTTP_GET, hProbe);
  web.on("/api/can/raw", HTTP_GET, hRaw);
  web.on("/api/screen", HTTP_GET, hScreen);
  web.on("/api/can/errors", HTTP_GET, hErrors);
  web.on("/api/update", HTTP_POST, hUpdateDone, hUpdateUpload);
  web.on("/api/cfg", HTTP_GET, hCfgGet);
  web.on("/api/set", HTTP_POST, hSet);
  web.on("/api/map", HTTP_GET, hMapGet);
  web.on("/api/map", HTTP_POST, hMapPost);
  // Announcements: the audio itself goes to port 81 (own task); these are the controls
  web.on("/api/audio", HTTP_GET, []() {
    String o = "{\"busy\":"; o += Audio::busy() ? "true" : "false";
    o += ",\"enabled\":"; o += Store::cfg().audioApi ? "true" : "false";
    o += ",\"played\":"; o += Audio::count();
    o += ",\"port\":"; o += AUDIO_PORT;
    o += ",\"last\":"; jsonEsc(o, Audio::lastText()); o += '}';
    web.send(200, "application/json", o);
  });
  web.on("/api/audio/stop", HTTP_POST, []() { Audio::stop(); web.send(200, "application/json", "{\"ok\":true}"); });
  web.on("/api/audio/play", HTTP_POST, []() {
    if (!Store::cfg().audioApi) { web.send(403, "application/json", "{\"error\":\"announcements are off\"}"); return; }
    const bool ok = Audio::playUrl(web.arg("url").c_str(), web.arg("chime") == "1" ? "chime=1" : "");
    web.send(ok ? 200 : 400, "application/json", ok ? "{\"ok\":true}" : "{\"error\":\"need url=http://...\"}");
  });
  web.onNotFound([]() { web.send(404, "text/plain", "not found"); });
  web.begin();
  s_webUp = true;
  Serial.println("Web  : port 80 open");
}

// ═════════════════════════ NTP time ═════════════════════════
// The callback runs in the lwIP task — it only raises a flag; the main loop logs.
static volatile bool s_ntpEvent = false;
static bool s_ntpOk = false, s_ntpAsked = false;
static void ntpCb(struct timeval*) { s_ntpEvent = true; }

static void ntpTick() {
  if (!s_ntpAsked && WiFi.status() == WL_CONNECTED && !s_apMode) {
    sntp_set_time_sync_notification_cb(ntpCb);
    configTime(7 * 3600, 0, "pool.ntp.org", "time.google.com", "time.cloudflare.com");
    s_ntpAsked = true;
  }
  if (s_ntpEvent) {
    s_ntpEvent = false;
    const bool first = !s_ntpOk;
    s_ntpOk = true;
    // Log only the FIRST sync: periodic re-syncs are drift correction, not events
    if (first) {
      Store::logAdd(EV_TIMESET, 0, -1);
      Serial.printf("Time : from network -> %s\n", Clock::stamp());
    }
  }
}

bool Net::timeSynced() { return s_ntpOk; }
const char* Net::timeSource() {
  if (s_ntpOk)        return "network";
  if (Clock::valid()) return "set by hand";
  return "NOT SET";
}

// ═════════════════════════ Lifecycle ═════════════════════════
// keepSta: the network is set but cannot be joined — run the AP AND keep the
// station trying (AP+STA). Before: after 4 failed tries the display stayed in AP
// mode for good, so a router that rebooted at night left it off the network
// until someone pulled the power.
static void startAp(bool keepSta) {
  WiFi.mode(keepSta ? WIFI_AP_STA : WIFI_AP);
  WiFi.softAP(AP_SSID, AP_PASS);
  s_apMode = true;
  webBegin();
}

static void stopAp() {
  WiFi.softAPdisconnect(true);
  WiFi.mode(WIFI_STA);
  s_apMode = false;
}

void Net::begin() {
  netLoad();
  if (!s_on) { WiFi.mode(WIFI_OFF); Serial.println("WiFi : off"); return; }
  // WiFi on but no network set yet → start the access point at once: the
  // installer needs a way in.
  if (!s_ssid[0]) {
    startAp(false);
    Serial.println("WiFi : no network set -> own AP " AP_SSID " / " AP_PASS);
    return;
  }
  WiFi.mode(WIFI_STA);
  WiFi.setSleep(false);
  WiFi.setHostname("lift-display");
  WiFi.begin(s_ssid, s_pass);
  Serial.printf("WiFi : joining \"%s\"...\n", s_ssid);
  s_lastTry = millis();
}

void Net::loop() {
  if (s_netAction) {
    s_netAction = 0;
    Net::setCreds(s_newSsid, s_newPass);
  }
  if (!s_on) return;

  static uint32_t lastCheck = 0;
  const uint32_t now = millis();
  if (now - lastCheck >= 1000) {
    lastCheck = now;
    const bool joined = WiFi.status() == WL_CONNECTED;
    if (s_ssid[0] && !joined) {
      // Retry every 8 s; after 4 misses also open our own AP (a mistyped password
      // must not lock the installer out) — and keep retrying every 30 s. While
      // someone is on the AP (configuring) the station is left alone: a retry
      // scans channels and would drop their connection.
      const uint32_t every = s_apMode ? 30000UL : 8000UL;
      if (now - s_lastTry >= every && !(s_apMode && WiFi.softAPgetStationNum() > 0)) {
        s_lastTry = now;
        if (!s_apMode && ++s_tries >= 4) {
          Serial.println("WiFi : cannot join -> own AP " AP_SSID " (still retrying the network)");
          startAp(true);
          Store::logAdd(EV_WIFI_AP, 1);
        } else {
          WiFi.disconnect(false);
          WiFi.begin(s_ssid, s_pass);
        }
      }
    } else if (s_apMode && s_ssid[0] && joined) {
      // The network is back: the AP goes off — it must only exist while the
      // display cannot reach the network.
      stopAp();
      s_tries = 0;
      Serial.printf("WiFi : joined, address %s - own AP off\n", WiFi.localIP().toString().c_str());
      Store::logAdd(EV_WIFI_AP, 0);
    } else if (!s_webUp) {
      Serial.printf("WiFi : joined, address %s\n", WiFi.localIP().toString().c_str());
      s_tries = 0;
      webBegin();
    }
  }
  if (s_webUp) web.handleClient();
  if (s_rebootAt && timeReached(millis(), s_rebootAt)) {
    Store::flush();
    delay(100);
    ESP.restart();
  }
  if (!s_espota && staConnected()) espotaBegin();
  // espota: polled 10 times a second (each poll allocates a packet buffer)
  static uint32_t otaAt = 0;
  if (s_espota && millis() - otaAt >= 100) { otaAt = millis(); ArduinoOTA.handle(); }
  ntpTick();
}

bool Net::on()           { return s_on; }
bool Net::connected()    { return s_on && (s_apMode || WiFi.status() == WL_CONNECTED); }
bool Net::staConnected() { return s_on && !s_apMode && WiFi.status() == WL_CONNECTED; }
bool Net::apMode()       { return s_apMode; }
const char* Net::ssid()  { return s_apMode ? AP_SSID : s_ssid; }
int  Net::rssi()         { return staConnected() ? WiFi.RSSI() : 0; }

String Net::ip() {
  if (!connected()) return String("-");
  return (s_apMode ? WiFi.softAPIP() : WiFi.localIP()).toString();
}

void Net::setEnabled(bool on) {
  if (s_on == on) return;
  s_on = on;
  netSave();
  if (!on) {
    if (s_webUp) { web.stop(); s_webUp = false; }
    WiFi.disconnect(true);
    WiFi.mode(WIFI_OFF);
    s_apMode = false; s_tries = 0;
    Serial.println("WiFi : off");
  } else {
    Net::begin();
  }
}

void Net::setCreds(const char* ssid, const char* pass) {
  strncpy(s_ssid, ssid, sizeof(s_ssid) - 1); s_ssid[sizeof(s_ssid) - 1] = 0;
  strncpy(s_pass, pass, sizeof(s_pass) - 1); s_pass[sizeof(s_pass) - 1] = 0;
  s_on = true;
  netSave();
  s_tries = 0; s_apMode = false;
  if (s_webUp) { web.stop(); s_webUp = false; }
  WiFi.disconnect(true);
  delay(100);
  Net::begin();
}

// ═════════════════════════ Network scan — ASYNCHRONOUS ═════════════════════════
// A blocking scan takes 2–4 s; meanwhile the main loop stands still and the
// CAN queue fills. Not a trade worth making.
void Net::scanStart() {
  if (WiFi.scanComplete() == WIFI_SCAN_RUNNING) return;
  if (!s_on) { s_on = true; netSave(); }
  WiFi.mode(s_apMode ? WIFI_AP_STA : WIFI_STA);
  WiFi.scanDelete();
  WiFi.scanNetworks(true, false);
}
int Net::scanState() { return WiFi.scanComplete(); }
void Net::scanClear() {
  if (WiFi.scanComplete() >= 0) WiFi.scanDelete();
  if (s_apMode) WiFi.mode(WIFI_AP);
}
bool Net::scanInfo(int i, char* ssid, size_t cap, int& rssi, bool& lock) {
  const int n = WiFi.scanComplete();
  if (n <= 0 || i < 0 || i >= n) return false;
  strncpy(ssid, WiFi.SSID(i).c_str(), cap - 1);
  ssid[cap - 1] = 0;
  rssi = WiFi.RSSI(i);
  lock = (WiFi.encryptionType(i) != WIFI_AUTH_OPEN);
  return true;
}
