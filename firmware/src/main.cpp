// Elevator display — ESP32-3248S035 (3.5" ST7796 480×320, resistive touch)
//   CAN:  built-in TWAI controller, transceiver on P3: TX = GPIO 22, RX = GPIO 35
//   Net:  WiFi + web config + MQTT / Home Assistant discovery — all optional
#include <Arduino.h>
#include <nvs.h>
#include <esp_task_wdt.h>
#include "drivers.h"
#include "store.h"
#include "lift.h"
#include "ui.h"

// Slowest loop pass in the last 10 s. The only honest answer to "does the
// network slow the CAN path down": the driver queue holds 256 frames, so as
// long as this stays well below ~250 ms at full bus load nothing is lost.
static uint32_t s_loopMaxUs = 0, s_loopMaxHold = 0, s_loopWinAt = 0;
uint32_t loopMaxUs() { return s_loopMaxHold; }

// Survives a software restart (not a power cut): set by the LVGL assert hook
static RTC_NOINIT_ATTR uint32_t s_lvAssert;
extern "C" void lvAssertRestart(void) {
  s_lvAssert = 0x5EE1A55Eu;
  esp_restart();
}

void setup() {
  // Hold CAN TX recessive before ANYTHING else. A floating TXD on an
  // SN65HVD230 can pull the whole lift bus dominant (see drv_can.cpp).
  pinMode(PIN_CAN_TX, OUTPUT);
  digitalWrite(PIN_CAN_TX, HIGH);

  Serial.begin(115200);
  delay(150);
  Serial.println("\n=== Elevator display · CAN bus · v" FW_VERSION " ===");

  Out::begin();
  Store::begin();                       // before the splash: it needs the LED setting
  Out::setLedEnabled(Store::cfg().statusLed);
  Display::begin();
  Display::setTouchEnabled(!Store::cfg().touchOff);
  UI::splash(1600);

  Lift::begin();
  const Settings& c = Store::cfg();
  Out::setVolume(c.volume);
  Display::setBrightness(c.brightness);
  Display::setDimLevel(c.dimPct);
  Display::setSleepTimeout((uint32_t)c.dimSec * 1000UL);

  // NO touch calibration in setup(): start-up must never depend on somebody
  // touching the screen (esp32Fire bug #4). Uncalibrated → defaults + a hint.
  Serial.printf("Touch: %s\n", Display::touchCalibrated() ? "calibrated" : "NOT calibrated, using defaults");

  Can::begin(c.canKbps, c.canMode);
  Net::begin();
  Mqtt::begin();
  Weather::begin();    // own task: Open-Meteo every 15 min once the network is up
  Audio::begin();      // speaker task; opens port 81 by itself once a network exists
  CanDebug::begin();   // live CAN stream for /debug; opens port 82 the same way
  // Reason of the last restart; an LVGL assert (out of memory) is marked in RTC
  // memory just before it restarts — reset reason 100 = "UI out of memory"
  uint8_t why = (uint8_t)esp_reset_reason();
  if (s_lvAssert == 0x5EE1A55Eu) why = 100;
  s_lvAssert = 0;
  Store::logAdd(EV_BOOT, why);
  UI::begin();
  Display::wake();      // wake(false): true would force 100 % over the user's level

  Serial.printf("Lift : \"%s\" · %u floors · CAN %u kbit/s · %s · commands %s\n",
                c.liftName, c.floors, c.canKbps, c.canMode == CAN_NORMAL ? "ACK wanted" : "listen-only",
                c.txEnable ? "ON" : "off");
  Serial.println("Type 'help' for console commands.");

  // Loop watchdog for unattended 24/7 running: if loop() stops coming round for
  // 30 s (a hang anywhere — SPI, WiFi, a deadlock) the chip restarts, and the
  // next boot logs "Power up · WATCHDOG". Long jobs (firmware upload, touch
  // calibration, screenshot) feed it themselves.
  esp_task_wdt_init(30, true);
  enableLoopWDT();
  Serial.println("WDT  : loop watchdog 30 s");
}

// ───────── Serial console (115200) ─────────
static int parseFloor(const char* s) {
  while (*s == ' ') s++;
  if (*s == '#') return atoi(s + 1);
  char up[LABEL_LEN];
  uint8_t n = 0;
  for (; *s && *s != ' ' && n < LABEL_LEN - 1; s++) up[n++] = (char)toupper(*s);
  up[n] = 0;
  return Store::findLabel(up);
}

static uint8_t parseHex(const char* s, uint8_t* out, uint8_t max) {
  uint8_t n = 0;
  while (*s && n < max) {
    while (*s == ' ') s++;
    if (!isxdigit((unsigned char)s[0]) || !isxdigit((unsigned char)s[1])) break;
    char h[3] = {s[0], s[1], 0};
    out[n++] = (uint8_t)strtoul(h, nullptr, 16);
    s += 2;
  }
  return n;
}

static void printMap() {
  const LiftMap& m = Store::map();
  Serial.printf("preset=%u proto=%s lift=%u node=%u base=%d\n", m.preset,
                m.proto == PROTO_CIA417 ? "cia417" : "map", m.coLift, m.coNode, m.coBase);
  if (m.proto == PROTO_CIA417) {
    Lift::CoInfo o;
    Lift::coInfo(o);
    Serial.printf("  CiA 417 heard: hb=%lu rec=%lu req=%lu mpdo=%lu drive=%lu emcy=%lu · nodes: %s\n",
                  (unsigned long)o.hb, (unsigned long)o.rec, (unsigned long)o.req, (unsigned long)o.mpdo,
                  (unsigned long)o.drive, (unsigned long)o.emcy, o.nodes ? o.nodeList : "none");
    if (o.lastRecAge != UINT32_MAX)
      Serial.printf("  last record %02X %02X %02X %02X %02X %02X (%lu ms ago)\n", o.lastRec[0], o.lastRec[1],
                    o.lastRec[2], o.lastRec[3], o.lastRec[4], o.lastRec[5], (unsigned long)o.lastRecAge);
    if (o.velocity != INT32_MIN) Serial.printf("  drive velocity %ld\n", (long)o.velocity);
    if (o.hasPos) Serial.printf("  car position %lu\n", (unsigned long)o.position);
    if (o.emcy) Serial.printf("  last EMCY %04X from node %u\n", o.emcyCode, o.emcyNode);
  }
  for (uint8_t i = 0; i < SIG_COUNT; i++) {
    const SigMap& g = m.sig[i];
    Serial.printf("  %2u %-16s %s id=%lX%s byte=%u mask=%02X enc=%u off=%d%s\n", i, SIG_NAME[i],
                  (g.flags & MAP_USED) ? "USED" : "  - ", (unsigned long)g.id,
                  (g.flags & MAP_EXT) ? "(x)" : "", g.byte, g.mask, g.enc, g.offset,
                  (g.flags & MAP_INV) ? " inv" : "");
  }
  for (uint8_t i = 0; i < CMD_COUNT; i++) {
    const CmdMap& k = m.cmd[i];
    Serial.printf("  C%u %-16s %s id=%lX dlc=%u data=", i, CMD_NAME[i],
                  (k.flags & MAP_USED) ? "USED" : "  - ", (unsigned long)k.id, k.dlc);
    for (uint8_t j = 0; j < 8; j++) Serial.printf("%02X", k.data[j]);
    Serial.printf(" fbyte=%u fenc=%u off=%d rep=%u\n", k.fbyte, k.fenc, k.offset, k.repeat);
  }
}

static void dumpTree(lv_obj_t* o, int depth) {
  for (uint32_t i = 0; i < lv_obj_get_child_cnt(o); i++) {
    lv_obj_t* c = lv_obj_get_child(o, i);
    lv_area_t a;
    lv_obj_get_coords(c, &a);
    const bool isLbl = lv_obj_check_type(c, &lv_label_class);
    Serial.printf("%*s%s x=%d..%d y=%d..%d%s %s\n", depth * 2, "", isLbl ? "T" : "#",
                  a.x1, a.x2, a.y1, a.y2, (a.y2 > 319 || a.x2 > 479) ? "  <<< OFF SCREEN" : "",
                  isLbl ? lv_label_get_text(c) : "");
    dumpTree(c, depth + 1);
  }
}

static void help() {
  Serial.println(
    "status   ?                         · mem · nvs · net · log · csv\n"
    "screens  s <n> · dump · shot · click <n> · deep <n> · cal\n"
    "sim      sim (toggle) · sim fire|insp|over|oos 0/1 · sim fault <code>\n"
    "frames   inj <id> <hex bytes>      e.g. inj 100 0501000000\n"
    "         tx <id> <hex bytes>       raw transmit (needs ACK mode + commands ON)\n"
    "can      kbps <n> · mode listen|ack · txen 0|1 · remote 0|1 · autobaud\n"
    "calls    call <label|#idx> · up <..> · dn <..> · open · close · hold\n"
    "map      map · preset fuji|cia417|evl|clear · sig <n> <id> <byte> <mask> [enc] [offset] [inv]\n"
    "cia417   co lift <1-8> · co node <1-127> · co base <bus floor no. of the lowest floor>\n"
    "floors   floors <n> · label <idx> <text> · offset <n> · autolabel\n"
    "wifi     wifi <ssid> <pass> · wifi off · wifi clear\n"
    "mqtt     mqtt <host> [port] · mqtt user <u> <p> · mqtt on · mqtt off\n"
    "audio    audiotest · play <http://…/file.wav>\n"
    "other    led 0|1 · touch 0|1 · stress <n> · factory");
}

static void handleCmd(char* c) {
  while (*c == ' ') c++;
  if (!*c) return;
  Settings& cfg = Store::cfg();

  if (!strcmp(c, "help")) { help(); return; }
  if (c[0] == 's' && c[1] == ' ') {
    const int n = atoi(c + 2);
    if (n >= 0 && n < SCR_COUNT) { UI::show((Screen)n); Serial.printf("-> screen %d\n", n); }
    return;
  }
  if (!strncmp(c, "sim", 3)) {
    char* p = c + 3;
    while (*p == ' ') p++;
    if (!*p) { if (Lift::simOn()) Lift::simStop(); else Lift::simStart(); return; }
    if (!Lift::simOn()) { Serial.println("-> start the simulation first: sim"); return; }
    char* sp = strchr(p, ' ');
    const int v = sp ? atoi(sp + 1) : 1;
    if (!strncmp(p, "fire", 4))       Lift::simSet('f', v);
    else if (!strncmp(p, "insp", 4))  Lift::simSet('i', v);
    else if (!strncmp(p, "over", 4))  Lift::simSet('o', v);
    else if (!strncmp(p, "oos", 3))   Lift::simSet('s', v);
    else if (!strncmp(p, "fault", 5)) Lift::simSet('e', v);
    Serial.printf("-> sim %s = %d\n", p, v);
    return;
  }
  if (!strncmp(c, "inj ", 4) || !strncmp(c, "tx ", 3)) {
    const bool tx = c[0] == 't';
    char* p = nullptr;
    CanFrame f = {};
    f.id = (uint32_t)strtoul(c + (tx ? 3 : 4), &p, 16);
    f.ext = f.id > 0x7FF;
    f.dlc = p ? parseHex(p, f.d, 8) : 0;
    if (tx) {
      if (!cfg.txEnable) { Serial.println("-> commands are OFF (txen 1)"); return; }
      Serial.printf("-> tx %lX dlc %u: %s\n", (unsigned long)f.id, f.dlc, Can::send(f) ? "queued" : "REFUSED");
    } else {
      Can::inject(f);
      Serial.printf("-> injected %lX dlc %u\n", (unsigned long)f.id, f.dlc);
    }
    return;
  }
  if (!strncmp(c, "kbps ", 5)) {
    // "kbps 125" = a table rate · "kbps 27.78" = any measured rate in kbit/s
    const int k = strchr(c + 5, '.') ? (int)lroundf(atof(c + 5) * 100) : atoi(c + 5);
    if (k > 0 && k <= 65535 && Can::rateOk((uint16_t)k)) {
      cfg.canKbps = (uint16_t)k; Can::setBitrate(cfg.canKbps); Store::save();
      Serial.printf("-> %s kbit/s\n", canRateLabel(cfg.canKbps));
      return;
    }
    Serial.println("-> rate codes: 12 16 20 25 27.78 33 40 47 50 62 66 80 83 100 125 200 250 400 500 666 800 1000, or a measured rate like 27.78");
    return;
  }
  if (!strncmp(c, "mode ", 5)) {
    cfg.canMode = !strncmp(c + 5, "ack", 3) ? CAN_NORMAL : CAN_LISTEN;
    Can::setMode(cfg.canMode);
    Store::save();
    return;
  }
  if (!strncmp(c, "txen ", 5))   { cfg.txEnable = atoi(c + 5) != 0; Store::save(); Serial.printf("-> commands %s\n", cfg.txEnable ? "ON" : "off"); return; }
  if (!strncmp(c, "remote ", 7)) { cfg.allowRemote = atoi(c + 7) != 0; Store::save(); Mqtt::rediscover(); Serial.printf("-> remote %s\n", cfg.allowRemote ? "ON" : "off"); return; }
  if (!strncmp(c, "touch ", 6))  { cfg.touchOff = atoi(c + 6) == 0; Display::setTouchEnabled(!cfg.touchOff); Store::save(); Serial.printf("-> touch screen %s\n", cfg.touchOff ? "OFF" : "on"); return; }
  if (!strncmp(c, "led ", 4))    { cfg.statusLed = atoi(c + 4) != 0; Out::setLedEnabled(cfg.statusLed); Store::save(); Serial.printf("-> status LED %s\n", cfg.statusLed ? "on" : "off"); return; }
  if (!strcmp(c, "audiotest"))   { Audio::test(); Serial.println("-> speaker test"); return; }
  if (!strncmp(c, "play ", 5))   { Serial.println(Audio::playUrl(c + 5, "chime=1") ? "-> queued" : "-> need http://..."); return; }
  if (!strcmp(c, "probe")) {
    Can::Probe p;
    if (!Can::probe(p)) { Serial.println("-> probe failed to start"); return; }
    Serial.printf("-> %u pulses · RX high %u%% · shortest %.1f us · bit %.2f us = %lu bit/s · fit %.3f · nearest %s kbit/s%s\n",
                  p.pulses, p.highPct, p.minTicks / 10.0f, p.bitUs, (unsigned long)p.rateBps, p.fit,
                  canRateLabel(p.nearestKbps), p.nearestOk ? "" : " (NOT within 3 %)");
    Serial.printf("   %u edges/s · longest level %u bits%s · first pulses (us):",
                  p.edgesPerSec, p.maxRun, p.uartBaud ? " · looks like UART" : "");
    if (p.uartBaud) Serial.printf(" [%lu baud]", (unsigned long)p.uartBaud);
    for (uint8_t i = 0; i < p.rawN; i++) Serial.printf(" %c%lu", (p.rawHi >> i) & 1 ? 'H' : 'L', (unsigned long)p.rawUs[i]);
    Serial.println();
    return;
  }
  if (!strcmp(c, "autobaud"))    { Can::autoBaudStart(); return; }
  if (!strncmp(c, "call ", 5))   { Lift::command(CMD_CAR, parseFloor(c + 5), SRC_SERIAL); return; }
  if (!strncmp(c, "up ", 3))     { Lift::command(CMD_HALL_UP, parseFloor(c + 3), SRC_SERIAL); return; }
  if (!strncmp(c, "dn ", 3))     { Lift::command(CMD_HALL_DN, parseFloor(c + 3), SRC_SERIAL); return; }
  if (!strcmp(c, "open"))        { Lift::command(CMD_DOOR_OPEN, 0, SRC_SERIAL); return; }
  if (!strcmp(c, "close"))       { Lift::command(CMD_DOOR_CLOSE, 0, SRC_SERIAL); return; }
  if (!strcmp(c, "hold"))        { Lift::command(CMD_DOOR_HOLD, 0, SRC_SERIAL); return; }
  if (!strcmp(c, "map"))         { printMap(); return; }
  if (!strncmp(c, "preset ", 7)) {
    if (!strncmp(c + 7, "evl", 3))      Lift::presetEvl(Store::map());
    else if (!strncmp(c + 7, "fuji", 4)) Lift::presetFuji(Store::map());
    else if (!strncmp(c + 7, "cia", 3)) Lift::presetCia417(Store::map());
    else                                 Lift::presetClear(Store::map());
    Store::saveMap(); Mqtt::rediscover(); printMap();
    return;
  }
  if (!strncmp(c, "co ", 3)) {
    LiftMap& m = Store::map();
    const int v = atoi(c + 8);
    if (!strncmp(c + 3, "lift ", 5))      m.coLift = (uint8_t)constrain(v, 1, 8);
    else if (!strncmp(c + 3, "node ", 5)) m.coNode = (uint8_t)constrain(v, 1, 127);
    else if (!strncmp(c + 3, "base ", 5)) m.coBase = (int8_t)constrain(v, OFFSET_MIN, OFFSET_MAX);
    else { Serial.println("-> co lift <n> | co node <n> | co base <n>"); return; }
    Store::saveMap(); printMap();
    return;
  }
  if (!strncmp(c, "sig ", 4)) {
    // sig <n> <id hex> <byte> <mask hex> [enc] [offset] [inv]
    char* p = c + 4;
    const int n = (int)strtol(p, &p, 10);
    if (n < 0 || n >= SIG_COUNT) { Serial.println("-> signal 0..13 (see 'map')"); return; }
    SigMap& g = Store::map().sig[n];
    g.id = (uint32_t)strtoul(p, &p, 16);
    g.byte = (uint8_t)strtol(p, &p, 10);
    g.mask = (uint8_t)strtoul(p, &p, 16);
    g.enc = (uint8_t)strtol(p, &p, 10);
    g.offset = (int8_t)strtol(p, &p, 10);
    const bool inv = strtol(p, &p, 10) != 0;
    g.flags = (uint8_t)(MAP_USED | (g.id > 0x7FF ? MAP_EXT : 0) | (inv ? MAP_INV : 0));
    if (!g.mask) g.mask = 0xFF;
    Store::map().preset = PRESET_CUSTOM;
    Store::saveMap(); Mqtt::rediscover(); printMap();
    return;
  }
  if (!strncmp(c, "floors ", 7)) {
    cfg.floors = (uint8_t)constrain(atoi(c + 7), FLOORS_MIN, MAX_FLOORS);
    if (cfg.mainFloor >= cfg.floors) cfg.mainFloor = 0;
    Store::save(); Mqtt::rediscover();
    return;
  }
  if (!strncmp(c, "label ", 6)) {
    char* p = nullptr;
    const int i = (int)strtol(c + 6, &p, 10);
    while (p && *p == ' ') p++;
    if (i >= 0 && i < MAX_FLOORS && p) { Store::setLabel((uint8_t)i, p); Mqtt::rediscover(); }
    Serial.printf("-> floor %d = \"%s\"\n", i, Store::label((uint8_t)i));
    return;
  }
  if (!strncmp(c, "offset ", 7)) {
    Store::map().sig[SIG_FLOOR].offset = (int8_t)atoi(c + 7);
    Store::saveMap();
    return;
  }
  if (!strcmp(c, "autolabel")) { Store::autoLabels(); Mqtt::rediscover(); return; }
  if (!strncmp(c, "wifi ", 5)) {
    char* p = c + 5;
    while (*p == ' ') p++;
    if (!strncmp(p, "off", 3))   { Net::setEnabled(false); return; }
    if (!strncmp(p, "clear", 5)) { Net::setCreds("", ""); Serial.println("-> network cleared, own AP starts"); return; }
    char* sp = strchr(p, ' ');
    if (!sp) { Serial.println("-> usage: wifi <ssid> <password>"); return; }
    *sp = 0;
    Net::setCreds(p, sp + 1);
    // Never echo the password: serial logs get pasted into chats and tickets
    Serial.printf("-> network \"%s\" set, joining...\n", p);
    return;
  }
  if (!strncmp(c, "mqtt ", 5)) {
    char* p = c + 5;
    while (*p == ' ') p++;
    if (!strcmp(p, "on"))  { Mqtt::setEnabled(true); return; }
    if (!strcmp(p, "off")) { Mqtt::setEnabled(false); return; }
    if (!strncmp(p, "user ", 5)) {
      char* u = p + 5;
      char* sp = strchr(u, ' ');
      if (sp) { *sp = 0; Mqtt::setAuth(u, sp + 1); } else Mqtt::setAuth(u, "");
      Serial.println("-> MQTT user set");
      return;
    }
    char* sp = strchr(p, ' ');
    uint16_t port = 1883;
    if (sp) { *sp = 0; port = (uint16_t)atoi(sp + 1); }
    Mqtt::setServer(p, port);
    Mqtt::setEnabled(true);
    Serial.printf("-> MQTT broker %s:%u\n", p, port);
    return;
  }
  if (!strcmp(c, "net")) {
    Serial.printf("WiFi : %s · \"%s\" · %s · %d dBm\n", Net::on() ? "on" : "off", Net::ssid(),
                  Net::ip().c_str(), Net::rssi());
    Serial.printf("MQTT : %s · %s:%u · %s · sent %lu · %s\n", Mqtt::on() ? "on" : "off", Mqtt::host(),
                  Mqtt::port(), Mqtt::connected() ? "CONNECTED" : "not connected",
                  (unsigned long)Mqtt::sentCount(), Mqtt::lastError());
    Serial.printf("Topic: %s/state · remote commands %s\n", Mqtt::baseTopic(), cfg.allowRemote ? "ON" : "off");
    return;
  }
  if (!strcmp(c, "log")) {
    char ts[48], tx[80];
    for (uint16_t i = 0; i < Store::logCount() && i < 30; i++) {
      const Event& e = Store::logAt(i);
      Serial.printf("  %s  %s\n", Store::logStamp(e, ts, sizeof(ts)), Store::logText(e, tx, sizeof(tx)));
    }
    return;
  }
  if (!strcmp(c, "csv")) { Store::exportCsv(Serial); return; }
  if (!strcmp(c, "mem")) {
    Serial.printf("heap %lu free, lowest ever %lu, largest block %lu\n",
                  (unsigned long)ESP.getFreeHeap(), (unsigned long)ESP.getMinFreeHeap(),
                  (unsigned long)ESP.getMaxAllocHeap());
    return;
  }
  if (!strcmp(c, "nvs")) {
    nvs_stats_t st;
    if (nvs_get_stats(nullptr, &st) == ESP_OK)
      Serial.printf("NVS: %u / %u entries used, %u%% free\n", st.used_entries, st.total_entries,
                    st.total_entries ? 100u * st.free_entries / st.total_entries : 0u);
    return;
  }
  if (!strcmp(c, "dump")) {
    Serial.printf("--- screen %d ---\n", (int)UI::current());
    dumpTree(lv_scr_act(), 0);
    Serial.println("--- end ---");
    return;
  }
  if (!strncmp(c, "click ", 6) || !strncmp(c, "deep ", 5)) {
    // Fire LV_EVENT_CLICKED into the n-th clickable object — the SAME path as a
    // real touch, including running inside LVGL's event dispatch.
    const int want = atoi(strchr(c, ' ') + 1);
    int found = 0;
    lv_obj_t* hit = nullptr;
    struct Wk {
      static void walk(lv_obj_t* o, int& n, int want, lv_obj_t*& hit) {
        for (uint32_t i = 0; i < lv_obj_get_child_cnt(o) && !hit; i++) {
          lv_obj_t* ch = lv_obj_get_child(o, i);
          if (lv_obj_has_flag(ch, LV_OBJ_FLAG_CLICKABLE) && !lv_obj_check_type(ch, &lv_label_class)) {
            if (n == want) { hit = ch; return; }
            n++;
          }
          walk(ch, n, want, hit);
        }
      }
    };
    Wk::walk(lv_scr_act(), found, want, hit);
    if (!hit) { Serial.printf("-> screen %d has %d clickable objects\n", (int)UI::current(), found); return; }
    Serial.printf("-> click #%d on screen %d\n", want, (int)UI::current());
    lv_event_send(hit, LV_EVENT_CLICKED, nullptr);
    return;
  }
  if (!strcmp(c, "shot")) { Display::screenshot(Serial); return; }
  if (!strcmp(c, "cal")) { Display::runTouchCalibration(); UI::show(UI::current()); return; }
  if (!strncmp(c, "stress ", 7)) {
    // Build the heaviest screens over and over to look for heap fragmentation.
    // LVGL allocates with malloc; a display that runs for years must prove that
    // hundreds of rebuilds do not slowly eat the heap.
    const int n = atoi(c + 7);
    const uint32_t h0 = ESP.getFreeHeap();
    const Screen tour[6] = {SCR_SNIFF, SCR_MAP, SCR_LOG, SCR_CALL, SCR_FLOORLIST, SCR_MAIN};
    for (int i = 0; i < n; i++) {
      feedLoopWDT();
      for (uint8_t k = 0; k < 6; k++) {
        UI::show(tour[k]);
        UI::loop();
        Display::loop();
        Can::loop();
        Lift::loop();
      }
    }
    UI::show(SCR_MAIN); UI::loop();
    const uint32_t h1 = ESP.getFreeHeap();
    Serial.printf("-> %d x 6 screens: heap before %lu, after %lu, diff %ld bytes\n", n,
                  (unsigned long)h0, (unsigned long)h1, (long)h1 - (long)h0);
    return;
  }
  if (!strcmp(c, "factory")) {
    Store::factoryReset();
    Serial.println("-> settings erased, restarting");
    delay(200);
    ESP.restart();
  }
  if (c[0] == '?') {
    const LiftState& s = Lift::st();
    Can::Stats cs; Can::stats(cs);
    Serial.printf("lift : floor %s (#%d raw %ld) dir %u moving %d door %d mode %s fault %u load %d\n",
                  s.label, s.floorIdx, (long)(s.rawFloor == INT32_MIN ? -1 : s.rawFloor), s.dir,
                  s.moving, s.door, Lift::modeText(Lift::mode()), s.fault, s.load);
    Serial.printf("can  : %s · %s kbit/s · rx %lu (%u/s) · tx %lu fail %lu · err %lu · miss %lu · TEC %u REC %u\n",
                  Can::stateText(), canRateLabel(Can::bitrate()), (unsigned long)cs.rx, cs.fps, (unsigned long)cs.tx,
                  (unsigned long)cs.txFail, (unsigned long)cs.busErr, (unsigned long)cs.rxMissed,
                  cs.tec, cs.rec);
    Serial.printf("cmd  : commands %s · remote %s · %s\n", cfg.txEnable ? "ON" : "off",
                  cfg.allowRemote ? "ON" : "off", Lift::lastCmdText());
    Serial.printf("sys  : screen %d · heap %u KB · slowest loop %lu us · sim %d · backlight %u%s · idle %lus\n",
                  (int)UI::current(), (unsigned)(ESP.getFreeHeap() / 1024), (unsigned long)s_loopMaxHold,
                  Lift::simOn(), Display::duty(), Display::isAsleep() ? " (asleep)" : "",
                  (unsigned long)(Display::idleMs() / 1000));
    return;
  }
  Serial.println("unknown command — type 'help'");
}

static void serialConsole() {
  static char line[128];
  static uint8_t n = 0;
  while (Serial.available()) {
    const char c = (char)Serial.read();
    if (c == '\n' || c == '\r') {
      line[n] = 0;
      if (n) handleCmd(line);
      n = 0;
    } else if (n < sizeof(line) - 1) {
      line[n++] = c;
    }
  }
}

// Memory guard: months of WiFi, MQTT and web clients can fragment the heap.
// Critically low for a whole minute → a planned restart, done while the car is
// not moving (the indicator then comes back within seconds) and logged.
static void memGuard() {
  static uint32_t at = 0, lowSince = 0;
  const uint32_t now = millis();
  if (now - at < 5000) return;
  at = now;
  const uint32_t freeB = ESP.getFreeHeap(), block = ESP.getMaxAllocHeap();
  if (freeB > 24000 && block > 10000) { lowSince = 0; return; }
  if (!lowSince) { lowSince = now ? now : 1; Serial.printf("MEM  : low - %u free, largest block %u\n", (unsigned)freeB, (unsigned)block); }
  if (now - lowSince >= 60000 && !(Lift::st().linkOk && Lift::st().moving)) {
    Store::logAdd(EV_LOWMEM, 0, (int16_t)(freeB / 1024));
    Store::flush();
    Serial.println("MEM  : restarting");
    delay(100);
    ESP.restart();
  }
}

void loop() {
  const uint32_t t0 = micros();
  serialConsole();
  Can::loop();         // drain the TWAI queue → decoder
  Lift::loop();        // timeouts, logging, chime, simulator
  Out::loop();         // sound sequencer, LED
  Net::loop();         // WiFi + one web request — losing the network changes nothing above
  Mqtt::loop();
  UI::loop();

  const uint32_t dt = micros() - t0;
  if (dt > s_loopMaxUs) s_loopMaxUs = dt;
  if (timeReached(millis(), s_loopWinAt)) {
    s_loopWinAt   = millis() + 10000;
    s_loopMaxHold = s_loopMaxUs;
    s_loopMaxUs   = 0;
  }
  Display::loop();     // lv_timer_handler + idle dimming
  storeTick();         // write NVS when due
  memGuard();
  delay(2);
}
