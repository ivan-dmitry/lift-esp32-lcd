// Live CAN stream for the web debug page (http://<ip>/debug)
//
// Every frame the driver receives — and every frame this display sends — is
// copied into a ring buffer while at least one browser is watching, and a task
// on port 82 streams it as Server-Sent Events. Same design as the audio
// server: its own task, its own socket, so a slow browser or a busy bus can
// never stall the main loop (CAN decoding, LVGL).
//
//   GET http://<ip>/api/can/stream   same origin as the /debug page (handed over by the
//                                    port-80 server, see adopt())
//   GET http://<ip>:82/stream        the same stream on its own port, for curl / tools
//     data lines:  "<µs since boot> <id hex> <dlc> <data hex> <r|t>"   (8-digit id = 29-bit)
//     event: stat  {"fps":…,"rx":…,"err":…,"dropped":…,"kbps":…,"state":"…"}
//
// The browser does the recording, the per-ID table and the file download; the
// device only forwards frames. At 1000 frames/s a line is ~35 bytes → 35 KB/s,
// well within WiFi.
#include <WiFi.h>
#include "drivers.h"

#define RING 512                       // ~0.5 s of a fully loaded 125 kbit/s bus
struct Ent { int64_t t; CanFrame f; uint8_t tx; };
static Ent s_ring[RING];
static volatile uint16_t s_head = 0, s_tail = 0;
static volatile uint32_t s_dropped = 0;
static volatile uint8_t  s_clients = 0;
static portMUX_TYPE s_mux = portMUX_INITIALIZER_UNLOCKED;

static WiFiServer   s_srv(CANDBG_PORT);
static WiFiClient   s_cl[2];
static TaskHandle_t s_task = nullptr;

// A connection handed over by the port-80 web server, waiting for the task
static SemaphoreHandle_t s_inLock = nullptr;
static WiFiClient   s_incoming;
static volatile bool s_hasIncoming = false;

static const char SSE_HEAD[] = "HTTP/1.1 200 OK\r\nContent-Type: text/event-stream\r\nCache-Control: no-cache\r\n"
                               "Access-Control-Allow-Origin: *\r\nConnection: keep-alive\r\n\r\n: connected\n\n";

// Browsers (and this desktop app's own browser) may refuse a page on port 80
// talking to port 82 of the same device. So the /debug page asks port 80 for
// the stream, and the web server hands the socket to this task: WebServer only
// drops ITS reference after the handler returns — WiFiClient is reference
// counted, so our copy keeps the connection open and the main loop is free.
bool CanDebug::adopt(WiFiClient& c) {
  if (!s_inLock || s_clients >= 2 || s_hasIncoming) return false;
  c.print(SSE_HEAD);
  xSemaphoreTake(s_inLock, portMAX_DELAY);
  s_incoming = c;
  s_hasIncoming = true;
  xSemaphoreGive(s_inLock);
  return true;
}

uint8_t CanDebug::clients() { return s_clients; }
uint32_t CanDebug::dropped() { return s_dropped; }

// Called from the main loop (driver receive path and Can::send). Costs nothing
// while nobody watches.
void CanDebug::onFrame(const CanFrame& f, bool tx) {
  if (!s_clients) return;
  const int64_t t = esp_timer_get_time();
  portENTER_CRITICAL(&s_mux);
  const uint16_t n = (uint16_t)((s_head + 1) % RING);
  if (n == s_tail) s_dropped++;            // browser too slow: count it, never block the bus side
  else { s_ring[s_head].t = t; s_ring[s_head].f = f; s_ring[s_head].tx = tx; s_head = n; }
  portEXIT_CRITICAL(&s_mux);
}

static void accept(WiFiClient& c) {
  c.setTimeout(1);
  char line[160];
  size_t k = 0;
  const uint32_t t0 = millis();
  // request line only — the headers are drained and ignored
  while (millis() - t0 < 1000 && k + 1 < sizeof(line)) {
    if (!c.available()) { vTaskDelay(1); continue; }
    const char ch = (char)c.read();
    if (ch == '\n') break;
    line[k++] = ch;
  }
  line[k] = 0;
  vTaskDelay(pdMS_TO_TICKS(20));
  while (c.available()) c.read();
  if (strncmp(line, "GET /stream", 11)) {
    c.print("HTTP/1.1 404 Not Found\r\nAccess-Control-Allow-Origin: *\r\nContent-Length: 13\r\n\r\nGET /stream\r\n");
    c.stop();
    return;
  }
  WiFiClient* slot = nullptr;
  for (auto& s : s_cl) if (!s || !s.connected()) { slot = &s; break; }
  if (!slot) {
    c.print("HTTP/1.1 503 Busy\r\nAccess-Control-Allow-Origin: *\r\nContent-Length: 31\r\n\r\ntwo debug viewers already open\n");
    c.stop();
    return;
  }
  c.print(SSE_HEAD);
  *slot = c;
}

static void sendAll(const char* b, size_t n) {
  for (auto& c : s_cl) {
    if (!c || !c.connected()) continue;
    if (c.write((const uint8_t*)b, n) != n) c.stop();   // write failed or timed out: drop that viewer
  }
}

static void task(void*) {
  static char buf[6144];
  bool listening = false;
  uint32_t lastStat = 0;
  for (;;) {
    if (!listening) {
      if (Net::connected()) { s_srv.begin(); listening = (bool)s_srv; if (!listening) { vTaskDelay(pdMS_TO_TICKS(2000)); continue; } }
      else { vTaskDelay(pdMS_TO_TICKS(500)); continue; }
    }
    WiFiClient nc = s_srv.available();
    if (nc) accept(nc);
    if (s_hasIncoming) {
      xSemaphoreTake(s_inLock, portMAX_DELAY);
      WiFiClient* slot = nullptr;
      for (auto& s : s_cl) if (!s || !s.connected()) { slot = &s; break; }
      if (slot) *slot = s_incoming; else s_incoming.stop();
      s_incoming = WiFiClient();
      s_hasIncoming = false;
      xSemaphoreGive(s_inLock);
    }

    uint8_t n = 0;
    for (auto& c : s_cl) { if (c && c.connected()) n++; else if (c) c.stop(); }
    if (!n && s_clients) { portENTER_CRITICAL(&s_mux); s_tail = s_head; portEXIT_CRITICAL(&s_mux); }
    s_clients = n;
    if (!n) { vTaskDelay(pdMS_TO_TICKS(50)); continue; }

    // ── one SSE event per pass, many frames per event ──
    size_t k = 0;
    while (k + 64 < sizeof(buf)) {
      Ent e;
      portENTER_CRITICAL(&s_mux);
      const bool have = s_tail != s_head;
      if (have) { e = s_ring[s_tail]; s_tail = (uint16_t)((s_tail + 1) % RING); }
      portEXIT_CRITICAL(&s_mux);
      if (!have) break;
      k += snprintf(buf + k, sizeof(buf) - k, e.f.ext ? "data: %lld %08lX %u " : "data: %lld %03lX %u ",
                    (long long)e.t, (unsigned long)e.f.id, e.f.dlc);
      for (uint8_t i = 0; i < e.f.dlc && i < 8; i++) k += snprintf(buf + k, sizeof(buf) - k, "%02X", e.f.d[i]);
      k += snprintf(buf + k, sizeof(buf) - k, " %c\n", e.tx ? 't' : 'r');
    }
    if (k) { buf[k++] = '\n'; sendAll(buf, k); }

    const uint32_t now = millis();
    if (now - lastStat >= 1000) {
      lastStat = now;
      Can::Stats s; Can::stats(s);
      const int m = snprintf(buf, sizeof(buf),
        "event: stat\ndata: {\"fps\":%u,\"rx\":%lu,\"tx\":%lu,\"err\":%lu,\"miss\":%lu,\"dropped\":%lu,"
        "\"kbps\":%u,\"ack\":%s,\"state\":\"%s\"}\n\n",
        s.fps, (unsigned long)s.rx, (unsigned long)s.tx, (unsigned long)s.busErr, (unsigned long)s.rxMissed,
        (unsigned long)s_dropped, Can::bitrate(), Can::modeActive() == CAN_NORMAL ? "true" : "false",
        Can::stateText());
      sendAll(buf, (size_t)m);
    }
    vTaskDelay(pdMS_TO_TICKS(k ? 10 : 25));
  }
}

void CanDebug::begin() {
  if (s_task) return;
  s_inLock = xSemaphoreCreateMutex();
  xTaskCreatePinnedToCore(task, "candbg", 5120, nullptr, 2, &s_task, 0);
}
