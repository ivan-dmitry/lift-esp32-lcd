#include <Preferences.h>
#include "store.h"
#include "drivers.h"
#include "lift.h"

static Preferences nvs;
static Settings  s_cfg;
static LiftMap   s_map;
static char      s_label[MAX_FLOORS][LABEL_LEN];

static Event     s_log[LOG_CAP];
static uint16_t  s_logHead = 0;      // next write position
static uint16_t  s_logN    = 0;

// Each block is written separately and only when dirty. esp32Fire rewrote ALL
// settings + device table + log for every new log line; here a Home Assistant
// automation may send dozens of calls a day, and rewriting a 5 KB block for
// each one wears the flash for nothing.
static uint32_t  s_saveAt  = 0;
static bool      s_dCfg = false, s_dMap = false, s_dFlr = false, s_dLog = false;

static void defaults() {
  memset(&s_cfg, 0, sizeof(s_cfg));
  s_cfg.magic       = CFG_MAGIC;
  s_cfg.brightness  = 80;
  s_cfg.dimSec      = 30;      // backlight OFF after 30 s untouched; any lift activity
  s_cfg.dimPct      = 0;       // (floor change, arrival, mode change, announcement) wakes it
  s_cfg.kiosk       = false;
  s_cfg.volume      = 6;
  s_cfg.chime       = true;
  s_cfg.keyBeep     = true;
  s_cfg.statusLed   = false;
  s_cfg.audioApi    = true;
  s_cfg.pin         = 1234;
  s_cfg.canKbps     = CAN_DEFAULT_KBPS;   // 27.78k, the Fuji hall bus (CiA 417 itself uses 250k)
  // ACK, sending and remote commands ON by default (asked for on the Fuji lift).
  // ACK still arms only after 20 clean frames and falls back to listen-only on
  // bus errors; calls are still refused in fire service, inspection, faults.
  s_cfg.canMode     = CAN_NORMAL;
  s_cfg.txEnable    = true;
  s_cfg.allowRemote = true;
  s_cfg.timeoutSec  = 3;
  s_cfg.floors      = 10;
  s_cfg.mainFloor   = 0;
  s_cfg.basements   = 0;
  s_cfg.groundStyle = 0;
  strncpy(s_cfg.liftName, "LIFT A", NAME_LEN - 1);

  Lift::presetFuji(s_map);            // the lift this display was built for (CiA 417 is one tap away)
  memset(s_label, 0, sizeof(s_label));
  Store::autoLabels();
}

void Store::begin() {
  defaults();
  // Read-write open + isKey(): on a fresh board the namespace and keys do not
  // exist yet, and the read-only path logs an [E] line for every one of them.
  nvs.begin("lift", false);
  auto len = [](const char* k) -> size_t { return nvs.isKey(k) ? nvs.getBytesLength(k) : 0; };
  // Settings of the previous layout (CFG_MAGIC_V1) are carried over: the new
  // field sits at the end, so the old bytes map 1:1 onto the start of the struct.
  // Depending on padding the old blob is shorter or the same size.
  const size_t oldSize = (offsetof(Settings, touchOff) + 1) & ~(size_t)1;
  const size_t n = len("cfg");
  if (n == sizeof(s_cfg) || n == oldSize) {
    nvs.getBytes("cfg", &s_cfg, n);
    if (s_cfg.magic == CFG_MAGIC_V1) {
      s_cfg.magic    = CFG_MAGIC;
      s_cfg.touchOff = false;
      s_dCfg = true;
      s_saveAt = millis() + 1200;
      Serial.println("Settings carried over from the previous layout");
    } else if (s_cfg.magic != CFG_MAGIC) {
      defaults();
      Serial.println("Stored settings do not match -> using defaults");
    }
  }
  const uint16_t mver = nvs.getUShort("mver", 0);
  if (mver == MAP_VER && len("map") == sizeof(s_map))
    nvs.getBytes("map", &s_map, sizeof(s_map));
  else if (mver != 0) {
    // A map from an older firmware: CiA 417 is the default now. The bit rate is
    // kept — it may have been found on site.
    s_dMap = true;
    s_saveAt = millis() + 1200;
    Serial.println("Protocol map from older firmware -> default map");
  }
  // Defaults version: a changed default that must reach boards already in use,
  // once — after that the technician's own choice sticks.
  //   1: bit rate 27.78 kbit/s (measured at the Fuji lift)
  if (nvs.getUChar("dver", 0) < 1) {
    s_cfg.canKbps = CAN_DEFAULT_KBPS;
    nvs.putUChar("dver", 1);
    s_dCfg = true;
    s_saveAt = millis() + 1200;
    Serial.println("Defaults v1: CAN 27.78 kbit/s");
  }
  //   2: the Fuji protocol map (only when no signal is mapped) and the lift's 6 floors
  if (nvs.getUChar("dver", 0) < 2) {
    bool any = false;
    for (uint8_t i = 0; i < SIG_COUNT; i++) if (s_map.sig[i].flags & MAP_USED) any = true;
    if (!any) { Lift::presetFuji(s_map); s_dMap = true; }
    s_cfg.floors = 6;
    if (s_cfg.mainFloor >= 6) s_cfg.mainFloor = 0;
    nvs.putUChar("dver", 2);
    s_dCfg = true;
    s_saveAt = millis() + 1200;
    Serial.println("Defaults v2: Fuji map, 6 floors");
  }
  //   3: ACK mode, sending commands and remote commands on
  if (nvs.getUChar("dver", 0) < 3) {
    s_cfg.canMode = CAN_NORMAL;
    s_cfg.txEnable = true;
    s_cfg.allowRemote = true;
    nvs.putUChar("dver", 3);
    s_dCfg = true;
    s_saveAt = millis() + 1200;
    Serial.println("Defaults v3: ACK, commands and remote commands on");
  }
  if (len("flr") == sizeof(s_label))
    nvs.getBytes("flr", s_label, sizeof(s_label));
  if (len("log") == sizeof(s_log)) {
    nvs.getBytes("log", s_log, sizeof(s_log));
    s_logHead = nvs.getUShort("logh", 0);
    s_logN    = nvs.getUShort("logn", 0);
    if (s_logHead >= LOG_CAP) s_logHead = 0;
    if (s_logN > LOG_CAP)     s_logN = LOG_CAP;
  }
  nvs.end();

  // Clamp everything just loaded. Flash contents may come from another firmware
  // build, or from this build's web page with odd values.
  if (s_cfg.floors < FLOORS_MIN || s_cfg.floors > MAX_FLOORS) s_cfg.floors = 10;
  if (s_cfg.mainFloor >= s_cfg.floors) s_cfg.mainFloor = 0;
  s_cfg.volume = (uint8_t)constrain(s_cfg.volume, VOL_MIN, VOL_MAX);
  s_cfg.timeoutSec = (uint8_t)constrain(s_cfg.timeoutSec, TIMEOUT_MIN, TIMEOUT_MAX);
  s_cfg.liftName[NAME_LEN - 1] = 0;
  if (!Can::rateOk(s_cfg.canKbps)) s_cfg.canKbps = CAN_DEFAULT_KBPS;
  if (s_map.proto >= PROTO_COUNT) s_map.proto = PROTO_MAP;
  s_map.coLift = (uint8_t)constrain(s_map.coLift, 1, 8);
  s_map.coNode = (uint8_t)constrain(s_map.coNode, 1, 127);
  for (uint8_t i = 0; i < MAX_FLOORS; i++) s_label[i][LABEL_LEN - 1] = 0;
}

// The deadline is set by the FIRST change and never pushed back. Pushing it on
// every change means a steady stream of events (a flapping signal, a busy
// automation) postpones the write forever — and a power cut then loses all of it.
static void schedule() { if (!s_saveAt) s_saveAt = millis() + 1200; }
void Store::save()       { s_dCfg = true; schedule(); }
void Store::saveMap()    { s_dMap = true; schedule(); }
void Store::saveFloors() { s_dFlr = true; schedule(); }

// The log blob is 2.4 KB — about 60 % of an NVS page per write. Written for
// every event it would wear the flash out in months under a chatty lift, so it
// goes at most once a minute; a power cut loses at most that minute.
static uint32_t s_logWroteAt = 0;
static void flushNow(bool force = false) {
  const uint32_t now = millis();
  const bool logDue = s_dLog && (force || !s_logWroteAt || now - s_logWroteAt >= 60000UL);
  if (!s_dCfg && !s_dMap && !s_dFlr && !logDue) {
    if (s_dLog) {                                        // only the log waits: come back then
      s_saveAt = s_logWroteAt + 60000UL;
      if (!s_saveAt) s_saveAt = 1;
    }
    return;
  }
  nvs.begin("lift", false);
  if (s_dCfg) nvs.putBytes("cfg", &s_cfg, sizeof(s_cfg));
  if (s_dMap) { nvs.putBytes("map", &s_map, sizeof(s_map)); nvs.putUShort("mver", MAP_VER); }
  if (s_dFlr) nvs.putBytes("flr", s_label, sizeof(s_label));
  if (logDue) {
    s_logWroteAt = now ? now : 1;
    nvs.putBytes("log", s_log, sizeof(s_log));
    nvs.putUShort("logh", s_logHead);
    nvs.putUShort("logn", s_logN);
  }
  nvs.end();
  s_dCfg = s_dMap = s_dFlr = false;
  if (logDue) s_dLog = false;
  else if (s_dLog) s_saveAt = s_logWroteAt + 60000UL;   // log still pending
}

void Store::flush() { if (s_saveAt || s_dLog) { s_saveAt = 0; flushNow(true); } }

void storeTick() {
  if (s_saveAt && timeReached(millis(), s_saveAt)) {
    s_saveAt = 0;
    flushNow();
  }
}

void Store::factoryReset() {
  nvs.begin("lift", false);
  nvs.clear();
  nvs.end();
  defaults();
  s_logHead = s_logN = 0;
}

Settings& Store::cfg() { return s_cfg; }
LiftMap&  Store::map() { return s_map; }

// ───────── Floor labels ─────────
const char* Store::label(uint8_t i) {
  if (i >= MAX_FLOORS || !s_label[i][0]) return "?";
  return s_label[i];
}

// The floor font only has digits, upper-case letters and '-' — anything else
// renders as an empty box at 140 px in the middle of the screen. Filter HERE,
// once, for the panel, the web page and the serial console alike.
void Store::setLabel(uint8_t i, const char* s) {
  if (i >= MAX_FLOORS) return;
  uint8_t n = 0;
  for (const char* p = s; *p && n < LABEL_LEN - 1; p++) {
    char c = *p;
    if (c >= 'a' && c <= 'z') c = (char)(c - 32);
    if ((c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '-') s_label[i][n++] = c;
  }
  s_label[i][n] = 0;
  if (!n) snprintf(s_label[i], LABEL_LEN, "%u", (unsigned)(i + 1));
  saveFloors();
}

int Store::findLabel(const char* s) {
  for (uint8_t i = 0; i < s_cfg.floors && i < MAX_FLOORS; i++)
    if (!strcmp(s_label[i], s)) return i;
  return -1;
}

void Store::autoLabels() {
  static const char* const GROUND[4] = {"1", "G", "L", "0"};
  const uint8_t nb = s_cfg.basements;
  for (uint8_t i = 0; i < MAX_FLOORS; i++) {
    if (i < nb)       snprintf(s_label[i], LABEL_LEN, "B%u", (unsigned)(nb - i));
    else if (i == nb) snprintf(s_label[i], LABEL_LEN, "%s", GROUND[s_cfg.groundStyle & 3]);
    else {
      // Ground called "1": the floor above is 2. Ground called G/L/0: above is 1.
      const unsigned n = (s_cfg.groundStyle == 0) ? (i - nb + 1) : (i - nb);
      snprintf(s_label[i], LABEL_LEN, "%u", n);
    }
  }
  saveFloors();
}

// ───────── Event log ─────────
void Store::logAdd(EvKind k, uint8_t a, int16_t val, uint8_t src) {
  Event& e = s_log[s_logHead];
  e.kind   = k;
  e.a      = a;
  e.val    = val;
  e.src    = src;
  e.timeOk = Clock::valid();
  e.at     = e.timeOk ? Clock::epoch() : millis() / 1000;
  s_logHead = (uint16_t)((s_logHead + 1) % LOG_CAP);
  if (s_logN < LOG_CAP) s_logN++;
  s_dLog = true;
  schedule();
}

// Clearing the log is itself logged — the new book starts with "someone
// cleared the old one".
void Store::logClear() {
  const uint16_t had = s_logN;
  s_logHead = 0;
  s_logN    = 0;
  memset(s_log, 0, sizeof(s_log));
  logAdd(EV_CONFIG, 1, (int16_t)had);     // a = 1: "log cleared"
}

uint16_t Store::logCount() { return s_logN; }

const Event& Store::logAt(uint16_t i) {
  if (i >= s_logN) i = 0;
  const uint16_t idx = (uint16_t)((s_logHead + LOG_CAP - 1 - i) % LOG_CAP);
  return s_log[idx];
}

const char* Store::logText(const Event& e, char* b, size_t n) {
  const char* fl = (e.val >= 0 && e.val < MAX_FLOORS) ? label((uint8_t)e.val) : "?";
  switch (e.kind) {
    case EV_BOOT: {
      // e.a = esp_reset_reason(): why the previous run ended — the first thing to
      // look at when a display that runs 24/7 restarted by itself
      static const char* const R[] = {"", "power on", "external reset", "restart", "CRASH",
                                       "WATCHDOG (interrupt)", "WATCHDOG (task)", "WATCHDOG",
                                       "deep sleep", "BROWNOUT (power dip)", "SDIO"};
      const char* r = e.a == 100 ? "UI OUT OF MEMORY" : e.a < sizeof(R) / sizeof(R[0]) ? R[e.a] : "";
      if (r[0]) snprintf(b, n, "Power up · %s", r); else snprintf(b, n, "Power up");
      break;
    }
    case EV_FAULT:        snprintf(b, n, "LIFT FAULT · code %d", e.val); break;
    case EV_FAULT_CLR:    snprintf(b, n, "Fault cleared (code %d)", e.val); break;
    case EV_FIRE:         snprintf(b, n, "FIRE SERVICE MODE"); break;
    case EV_FIRE_CLR:     snprintf(b, n, "Fire service ended"); break;
    case EV_INSPECT:      snprintf(b, n, "Inspection mode"); break;
    case EV_INSPECT_CLR:  snprintf(b, n, "Inspection ended"); break;
    case EV_OOS:          snprintf(b, n, "Out of service"); break;
    case EV_OOS_CLR:      snprintf(b, n, "Back in service"); break;
    case EV_OVERLOAD:     snprintf(b, n, "Overload"); break;
    case EV_OVERLOAD_CLR: snprintf(b, n, "Overload cleared"); break;
    case EV_LINK_LOST:    snprintf(b, n, "CAN SIGNAL LOST"); break;
    case EV_LINK_OK:      snprintf(b, n, "CAN signal back"); break;
    case EV_BUSOFF:       snprintf(b, n, "CAN bus-off — recovering"); break;
    case EV_FALLBACK:     snprintf(b, n, "Bus errors -> back to LISTEN-ONLY"); break;
    case EV_CMD:
      if (e.a <= CMD_HALL_DN) snprintf(b, n, "%s %s · %s", CMD_NAME[e.a % CMD_COUNT], fl,
                                       SRC_NAME[e.src % SRC_COUNT]);
      else                    snprintf(b, n, "%s · %s", CMD_NAME[e.a % CMD_COUNT],
                                       SRC_NAME[e.src % SRC_COUNT]);
      break;
    case EV_CMD_DENY:     snprintf(b, n, "Command refused · %s · %s", Lift::resultText(e.a),
                                   SRC_NAME[e.src % SRC_COUNT]); break;
    case EV_CONFIG:       snprintf(b, n, e.a == 1 ? "Log cleared (%d entries)" : "Settings changed", e.val); break;
    case EV_LOGIN:        snprintf(b, n, "Technician login · %s", SRC_NAME[e.src % SRC_COUNT]); break;
    case EV_RAW_TX:       snprintf(b, n, "Raw frame sent · ID %X · %s", (unsigned)(uint16_t)e.val, SRC_NAME[e.src % SRC_COUNT]); break;
    case EV_OTA:          snprintf(b, n, "Firmware updated over the network (%s)", e.a ? "espota" : "web"); break;
    case EV_REARM:        snprintf(b, n, "ACK armed again by itself (after %d min)", e.val); break;
    case EV_TXFAIL:       snprintf(b, n, "CAN SEND FAILS (bus-off x3) -> LISTEN-ONLY · check CTX wire"); break;
    case EV_LOWMEM:       snprintf(b, n, "Low memory (%d KB) -> planned restart", e.val); break;
    case EV_WIFI_AP:      snprintf(b, n, e.a ? "WiFi lost -> own AP on, still retrying" : "WiFi back -> own AP off"); break;
    case EV_TIMESET:      snprintf(b, n, e.val < 0 ? "Clock set (network time)" : "Clock set (by hand)"); break;
    default:              snprintf(b, n, "Event %u", e.kind); break;
  }
  return b;
}

const char* Store::logStamp(const Event& e, char* b, size_t n) {
  if (!e.timeOk) {
    snprintf(b, n, "no clock · +%lus after boot", (unsigned long)e.at);
    return b;
  }
  const time_t t = (time_t)e.at;
  struct tm tm;
  localtime_r(&t, &tm);
  snprintf(b, n, "%04d-%02d-%02d %02d:%02d:%02d", tm.tm_year + 1900, tm.tm_mon + 1,
           tm.tm_mday, tm.tm_hour, tm.tm_min, tm.tm_sec);
  return b;
}

void Store::logStampShort(const Event& e, char* day, size_t dn, char* hm, size_t hn) {
  if (!e.timeOk) {
    snprintf(day, dn, "?");
    snprintf(hm, hn, "+%lus", (unsigned long)e.at);
    return;
  }
  const time_t t = (time_t)e.at;
  struct tm tm;
  localtime_r(&t, &tm);
  snprintf(day, dn, "%02d/%02d", tm.tm_mday, tm.tm_mon + 1);
  snprintf(hm, hn, "%02d:%02d", tm.tm_hour, tm.tm_min);
}

void Store::exportCsv(Print& out) {
  out.write(0xEF); out.write(0xBB); out.write(0xBF);
  out.println("time,event");
  char ts[48], tx[80];
  for (uint16_t i = 0; i < s_logN; i++) {
    const Event& e = logAt((uint16_t)(s_logN - 1 - i));     // oldest first
    out.printf("%s,\"%s\"\r\n", logStamp(e, ts, sizeof(ts)), logText(e, tx, sizeof(tx)));
  }
  out.println("# end");
}
