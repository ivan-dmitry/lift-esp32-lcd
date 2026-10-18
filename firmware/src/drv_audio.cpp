// Audio announcements over HTTP, through the on-board speaker
//
// GPIO 26 is the ESP32's DAC2 and feeds the FM8002A amplifier, so the board can
// play real audio, not only the PWM beeps of drv_io.cpp. The I2S peripheral in
// "built-in DAC" mode clocks samples out by DMA; the DAC takes the top 8 bits.
//
// Playback runs in ITS OWN TASK with ITS OWN SERVER on port 81. The main web
// server (port 80) runs inside the main loop and buffers request bodies; a
// 10-second announcement would freeze the CAN path and the screen for 10 s.
// Here the task reads the body from the socket as it arrives and writes it
// straight to I2S — the DMA buffers pace the transfer, nothing is held in RAM,
// and an announcement of any length streams fine.
//
//   POST http://<ip>:81/play            body = WAV (PCM 8/16-bit, mono/stereo, 8–48 kHz)
//   POST http://<ip>:81/play?rate=16000&bits=16&ch=1    body = raw PCM (little endian)
//   GET  http://<ip>:81/play?url=http://host/file.wav   the display fetches and plays it
//   options: vol=1..10 (default: the SOUND volume) · chime=1 (ding-dong first)
#include <WiFi.h>
#include <HTTPClient.h>
#include "driver/i2s.h"
#include "driver/dac_common.h"
#include "drivers.h"
#include "store.h"

static WiFiServer   s_srv(AUDIO_PORT);
static TaskHandle_t s_task = nullptr;
static volatile bool s_busy = false;
static volatile bool s_stop = false;
static volatile uint32_t s_count = 0;
static char s_last[96] = "nothing played yet";
static char s_url[256];
static volatile bool s_hasUrl = false;
static portMUX_TYPE s_mux = portMUX_INITIALIZER_UNLOCKED;
static char s_urlOpts[64];             // query options that came with a queued URL
static volatile bool s_testReq = false;

bool Audio::busy() { return s_busy; }
void Audio::test() { s_testReq = true; }
void Audio::stop() { s_stop = true; }
const char* Audio::lastText() { return s_last; }
uint32_t Audio::count() { return s_count; }

bool Audio::playUrl(const char* url, const char* opts) {
  if (strncmp(url, "http://", 7)) return false;            // no TLS in this build
  portENTER_CRITICAL(&s_mux);
  strncpy(s_url, url, sizeof(s_url) - 1); s_url[sizeof(s_url) - 1] = 0;
  strncpy(s_urlOpts, opts ? opts : "", sizeof(s_urlOpts) - 1); s_urlOpts[sizeof(s_urlOpts) - 1] = 0;
  s_hasUrl = true;
  portEXIT_CRITICAL(&s_mux);
  if (s_busy) s_stop = true;          // a new announcement replaces the one playing
  return true;
}

// ───────────────────────── Body source: Content-Length, chunked, or until close ─────────────────────────
struct Src {
  WiFiClient* c;
  int32_t  left;        // bytes left (Content-Length), -1 = unknown
  bool     chunked;
  uint32_t chunkLeft;
  bool     done;
  const uint8_t* pre; size_t preN;     // bytes already read with the headers

  int rawRead(uint8_t* b, size_t n) {
    if (preN) { const size_t k = n < preN ? n : preN; memcpy(b, pre, k); pre += k; preN -= k; return (int)k; }
    const uint32_t t0 = millis();
    while (!c->available()) {
      if (!c->connected() || millis() - t0 > 3000 || s_stop) return 0;   // 3 s silence = end
      vTaskDelay(1);
    }
    return c->read(b, n);
  }
  bool rawLine(char* line, size_t cap) {
    size_t k = 0;
    uint8_t ch;
    while (rawRead(&ch, 1) == 1) {
      if (ch == '\n') { line[k] = 0; if (k && line[k - 1] == '\r') line[k - 1] = 0; return true; }
      if (k + 1 < cap) line[k++] = (char)ch;
    }
    return false;
  }
  int read(uint8_t* b, size_t n) {
    if (done) return 0;
    if (chunked) {
      if (!chunkLeft) {
        char line[24];
        if (!rawLine(line, sizeof(line))) { done = true; return 0; }
        if (!line[0] && !rawLine(line, sizeof(line))) { done = true; return 0; }   // CRLF after a chunk
        chunkLeft = (uint32_t)strtoul(line, nullptr, 16);
        if (!chunkLeft) { done = true; return 0; }
      }
      if (n > chunkLeft) n = chunkLeft;
      const int r = rawRead(b, n);
      if (r <= 0) { done = true; return 0; }
      chunkLeft -= (uint32_t)r;
      return r;
    }
    if (left == 0) { done = true; return 0; }
    if (left > 0 && (int32_t)n > left) n = (size_t)left;
    const int r = rawRead(b, n);
    if (r <= 0) { done = true; return 0; }
    if (left > 0) left -= r;
    return r;
  }
  bool exact(uint8_t* b, size_t n) {
    size_t g = 0;
    while (g < n) { const int r = read(b + g, n - g); if (r <= 0) return false; g += (size_t)r; }
    return true;
  }
  bool skip(uint32_t n) {
    uint8_t t[64];
    while (n) { const size_t k = n < sizeof(t) ? n : sizeof(t); if (!exact(t, k)) return false; n -= k; }
    return true;
  }
};

// ───────────────────────── I2S built-in DAC ─────────────────────────
static int16_t s_gainQ = 0;             // Q12 gain from the volume

static void dacStart(uint32_t rate) {
  // Take GPIO 26 away from the LEDC tone generator for the duration
  ledcWrite(LEDC_CH_SND, 0);
  ledcDetachPin(PIN_SPEAKER);
  i2s_config_t c;
  memset(&c, 0, sizeof(c));
  c.mode = (i2s_mode_t)(I2S_MODE_MASTER | I2S_MODE_TX | I2S_MODE_DAC_BUILT_IN);
  c.sample_rate = rate;
  c.bits_per_sample = I2S_BITS_PER_SAMPLE_16BIT;
  c.channel_format = I2S_CHANNEL_FMT_RIGHT_LEFT;       // both slots written with the same sample
  c.communication_format = I2S_COMM_FORMAT_STAND_MSB;
  c.dma_buf_count = 8;
  c.dma_buf_len = 256;                                  // 8 × 256 frames ≈ 128 ms at 16 kHz
  c.tx_desc_auto_clear = true;                          // an underrun plays silence, not a buzz
  i2s_driver_install(I2S_NUM_0, &c, 0, nullptr);
  i2s_set_pin(I2S_NUM_0, nullptr);
  i2s_set_dac_mode(I2S_DAC_CHANNEL_LEFT_EN);            // left slot → DAC2 → GPIO 26
}

static void writeFrames(const uint16_t* f, size_t frames) {
  size_t w;
  i2s_write(I2S_NUM_0, f, frames * 4, &w, portMAX_DELAY);
}

// The pin sits at 0 V under LEDC and at mid-scale (1.65 V) while the DAC plays
// silence. Jumping between the two is a loud click — ramp over 20 ms instead.
static void ramp(uint32_t rate, bool up) {
  static uint16_t f[2 * 64];
  const uint32_t n = rate / 50;
  for (uint32_t i = 0; i < n; i += 64) {
    for (uint32_t k = 0; k < 64; k++) {
      const uint32_t p = i + k;
      const uint16_t v = (uint16_t)(0x8000UL * (up ? p : (n > p ? n - p : 0)) / n);
      f[2 * k] = f[2 * k + 1] = v;
    }
    writeFrames(f, 64);
  }
}

static void dacStop(uint32_t rate) {
  ramp(rate, false);
  vTaskDelay(pdMS_TO_TICKS(150));                       // let the DMA drain
  i2s_driver_uninstall(I2S_NUM_0);
  dac_output_disable(DAC_CHANNEL_2);
  ledcAttachPin(PIN_SPEAKER, LEDC_CH_SND);              // beeps and the arrival chime again
  ledcWrite(LEDC_CH_SND, 0);
}

static inline uint16_t toDac(int32_t s) {
  s = (s * s_gainQ) >> 12;
  if (s > 32767) s = 32767;
  if (s < -32768) s = -32768;
  return (uint16_t)(s + 32768);
}

// Announcement "ding-dong": two decaying sine tones, generated at 16 kHz
static void dingDong(uint32_t rate) {
  static uint16_t f[2 * 128];
  const float tones[2] = {880.0f, 698.5f};
  for (uint8_t t = 0; t < 2 && !s_stop; t++) {
    const uint32_t n = rate * 7 / 10;
    float ph = 0;
    const float dp = 2.0f * (float)M_PI * tones[t] / (float)rate;
    for (uint32_t i = 0; i < n && !s_stop; i += 128) {
      for (uint32_t k = 0; k < 128; k++) {
        const float env = expf(-4.0f * (float)(i + k) / (float)n);
        f[2 * k] = f[2 * k + 1] = toDac((int32_t)(sinf(ph) * env * 20000.0f));
        ph += dp;
        if (ph > 2.0f * (float)M_PI) ph -= 2.0f * (float)M_PI;
      }
      writeFrames(f, 128);
    }
  }
}

static float volAmp(int v) { return powf(10.0f, ((float)constrain(v, VOL_MIN, VOL_MAX) - 10.0f) / 10.0f); }

static int qInt(const char* q, const char* key, int def) {
  if (!q) return def;
  const size_t kl = strlen(key);
  for (const char* p = q; (p = strstr(p, key)); p += kl)
    if ((p == q || p[-1] == '&' || p[-1] == '?') && p[kl] == '=') return atoi(p + kl + 1);
  return def;
}

// Play one stream. Returns milliseconds played, or -1 with an error in `err`.
static int32_t play(Src& s, const char* query, char* err, size_t errN) {
  uint32_t rate = (uint32_t)qInt(query, "rate", 0);
  int bits = qInt(query, "bits", 16), ch = qInt(query, "ch", 1);
  uint32_t dataLeft = 0xFFFFFFFF;
  uint8_t hdr[12];

  if (!rate) {
    // ── WAV: RIFF header, then chunks until "data" ──
    if (!s.exact(hdr, 12) || memcmp(hdr, "RIFF", 4) || memcmp(hdr + 8, "WAVE", 4)) {
      snprintf(err, errN, "not a WAV file (send PCM with ?rate=&bits=&ch=)"); return -1;
    }
    bool fmt = false;
    for (;;) {
      uint8_t c[8];
      if (!s.exact(c, 8)) { snprintf(err, errN, "WAV has no data chunk"); return -1; }
      const uint32_t len = c[4] | c[5] << 8 | c[6] << 16 | (uint32_t)c[7] << 24;
      if (!memcmp(c, "fmt ", 4)) {
        uint8_t f[16];
        if (len < 16 || !s.exact(f, 16)) { snprintf(err, errN, "bad fmt chunk"); return -1; }
        const uint16_t format = f[0] | f[1] << 8;
        ch = f[2] | f[3] << 8;
        rate = f[4] | f[5] << 8 | f[6] << 16 | (uint32_t)f[7] << 24;
        bits = f[14] | f[15] << 8;
        if (format != 1 && format != 0xFFFE) { snprintf(err, errN, "WAV is compressed (format %u) - PCM only", format); return -1; }
        if (!s.skip(len - 16 + (len & 1))) { snprintf(err, errN, "truncated"); return -1; }
        fmt = true;
      } else if (!memcmp(c, "data", 4)) {
        if (!fmt) { snprintf(err, errN, "data before fmt"); return -1; }
        if (len && len != 0xFFFFFFFF) dataLeft = len;       // 0 / FFFFFFFF = streaming, play until end
        break;
      } else if (!s.skip(len + (len & 1))) { snprintf(err, errN, "truncated"); return -1; }
    }
  }
  if (rate < 8000 || rate > 48000 || (bits != 8 && bits != 16) || ch < 1 || ch > 2) {
    snprintf(err, errN, "unsupported: %lu Hz, %d-bit, %d ch", (unsigned long)rate, bits, ch); return -1;
  }

  s_gainQ = (int16_t)(volAmp(qInt(query, "vol", Store::cfg().volume)) * 4096.0f);
  dacStart(rate);
  ramp(rate, true);
  if (qInt(query, "chime", 0)) dingDong(rate);

  const size_t fb = (size_t)ch * (size_t)(bits / 8);
  static uint8_t in[1024];
  static uint16_t out[2 * 512];
  size_t have = 0;
  uint32_t frames = 0;
  while (!s_stop && dataLeft) {
    size_t want = sizeof(in) - have;
    if (dataLeft != 0xFFFFFFFF && want > dataLeft) want = dataLeft;
    const int r = s.read(in + have, want);
    if (r <= 0) break;
    if (dataLeft != 0xFFFFFFFF) dataLeft -= (uint32_t)r;
    have += (size_t)r;
    const size_t nf = have / fb;
    size_t o = 0;
    for (size_t i = 0; i < nf; i++) {
      const uint8_t* p = in + i * fb;
      int32_t v;
      if (bits == 16) {
        v = (int16_t)(p[0] | p[1] << 8);
        if (ch == 2) v = (v + (int16_t)(p[2] | p[3] << 8)) / 2;
      } else {
        v = ((int32_t)p[0] - 128) << 8;
        if (ch == 2) v = (v + (((int32_t)p[1] - 128) << 8)) / 2;
      }
      out[2 * o] = out[2 * o + 1] = toDac(v);
      if (++o == 512) { writeFrames(out, o); o = 0; }
    }
    if (o) writeFrames(out, o);
    frames += nf;
    const size_t used = nf * fb;
    memmove(in, in + used, have - used);
    have -= used;
  }
  dacStop(rate);
  return (int32_t)((uint64_t)frames * 1000 / rate);
}

// ───────────────────────── Fetch-and-play ─────────────────────────
static void playFromUrl(const char* url, const char* opts) {
  HTTPClient http;
  const char* hk[] = {"Transfer-Encoding"};
  http.setTimeout(5000);
  if (!http.begin(url)) { snprintf(s_last, sizeof(s_last), "bad URL"); return; }
  http.collectHeaders(hk, 1);
  const int code = http.GET();
  if (code != 200) {
    snprintf(s_last, sizeof(s_last), "URL answered %d", code);
    http.end();
    return;
  }
  WiFiClient* c = http.getStreamPtr();
  Src s = {c, http.getSize(), http.header("Transfer-Encoding").equalsIgnoreCase("chunked"), 0, false, nullptr, 0};
  char err[64] = "";
  s_busy = true;
  const int32_t ms = play(s, opts, err, sizeof(err));
  s_busy = false;
  if (ms >= 0) { s_count++; snprintf(s_last, sizeof(s_last), "played %ld ms from URL", (long)ms); }
  else snprintf(s_last, sizeof(s_last), "%s", err);
  http.end();
}

// ───────────────────────── The port-81 server ─────────────────────────
static void reply(WiFiClient& c, int code, const char* body) {
  c.printf("HTTP/1.1 %d %s\r\nContent-Type: application/json\r\nContent-Length: %u\r\n"
           "Access-Control-Allow-Origin: *\r\nConnection: close\r\n\r\n%s",
           code, code == 200 ? "OK" : code == 400 ? "Bad Request" : code == 403 ? "Forbidden" : "Not Found",
           (unsigned)strlen(body), body);
}

static void handle(WiFiClient& c) {
  c.setTimeout(3);
  char line[320];
  Src s = {&c, -1, false, 0, false, nullptr, 0};
  if (!s.rawLine(line, sizeof(line))) return;
  char method[8] = "", target[300] = "";
  sscanf(line, "%7s %299s", method, target);
  bool expect = false;
  for (;;) {
    char h[200];
    if (!s.rawLine(h, sizeof(h)) || !h[0]) break;
    if (!strncasecmp(h, "Content-Length:", 15)) s.left = atol(h + 15);
    else if (!strncasecmp(h, "Transfer-Encoding:", 18) && strcasestr(h, "chunked")) s.chunked = true;
    else if (!strncasecmp(h, "Expect:", 7) && strcasestr(h, "100-continue")) expect = true;
  }
  char* q = strchr(target, '?');
  if (q) *q++ = 0;
  char body[200];

  if (!strcmp(target, "/") || !strcmp(target, "/status")) {
    snprintf(body, sizeof(body), "{\"busy\":%s,\"played\":%lu,\"last\":\"%s\",\"enabled\":%s}",
             s_busy ? "true" : "false", (unsigned long)s_count, s_last, Store::cfg().audioApi ? "true" : "false");
    reply(c, 200, body);
    return;
  }
  if (strcmp(target, "/play")) { reply(c, 404, "{\"error\":\"POST /play\"}"); return; }
  if (!Store::cfg().audioApi) { reply(c, 403, "{\"error\":\"announcements are turned off (SOUND menu)\"}"); return; }

  // ?url=…: fetch the file ourselves (handy from Home Assistant's rest_command)
  if (q && strstr(q, "url=")) {
    const char* u = strstr(q, "url=") + 4;
    char url[256];
    size_t k = 0;
    for (; *u && *u != '&' && k + 1 < sizeof(url); u++) {          // URL-decode %xx
      if (*u == '%' && isxdigit((unsigned char)u[1]) && isxdigit((unsigned char)u[2])) {
        char h[3] = {u[1], u[2], 0}; url[k++] = (char)strtol(h, nullptr, 16); u += 2;
      } else url[k++] = (*u == '+') ? ' ' : *u;
    }
    url[k] = 0;
    s_stop = false;
    playFromUrl(url, q);
    snprintf(body, sizeof(body), "{\"result\":\"%s\"}", s_last);
    reply(c, 200, body);
    return;
  }
  if (!s.chunked && s.left <= 0) { reply(c, 400, "{\"error\":\"send the audio as the request body, or ?url=\"}"); return; }
  if (expect) c.print("HTTP/1.1 100 Continue\r\n\r\n");     // curl waits for this before sending a body

  char err[64] = "";
  s_stop = false;
  s_busy = true;
  const int32_t ms = play(s, q, err, sizeof(err));
  s_busy = false;
  if (ms >= 0) {
    s_count++;
    snprintf(s_last, sizeof(s_last), "played %ld ms", (long)ms);
    snprintf(body, sizeof(body), "{\"ok\":true,\"ms\":%ld}", (long)ms);
    reply(c, 200, body);
  } else {
    snprintf(s_last, sizeof(s_last), "%s", err);
    snprintf(body, sizeof(body), "{\"ok\":false,\"error\":\"%s\"}", err);
    reply(c, 400, body);
  }
}

// Speaker check without any network: ding-dong + a rising arpeggio through the
// same I2S → DAC path the announcements use.
static void selfTest() {
  const uint32_t rate = 16000;
  s_gainQ = (int16_t)(volAmp(Store::cfg().volume) * 4096.0f);
  s_busy = true;
  dacStart(rate);
  ramp(rate, true);
  dingDong(rate);
  static uint16_t f[2 * 128];
  const float notes[4] = {523.3f, 659.3f, 784.0f, 1046.5f};
  for (uint8_t t = 0; t < 4 && !s_stop; t++) {
    float ph = 0;
    const float dp = 2.0f * (float)M_PI * notes[t] / (float)rate;
    for (uint32_t i = 0; i < rate / 4; i += 128) {
      for (uint32_t k = 0; k < 128; k++) {
        f[2 * k] = f[2 * k + 1] = toDac((int32_t)(sinf(ph) * 16000.0f));
        ph += dp;
        if (ph > 2.0f * (float)M_PI) ph -= 2.0f * (float)M_PI;
      }
      writeFrames(f, 128);
    }
  }
  dacStop(rate);
  s_busy = false;
  s_count++;
  snprintf(s_last, sizeof(s_last), "speaker test played");
}

static void task(void*) {
  bool listening = false;
  for (;;) {
    // The socket can only listen once the network stack is up (WiFi joined, or own AP)
    if (!listening && Net::connected()) {
      s_srv.begin();
      listening = (bool)s_srv;                  // socket/bind can fail: try again later
      if (!listening) vTaskDelay(pdMS_TO_TICKS(2000));
    }
    if (s_testReq) { s_testReq = false; s_stop = false; selfTest(); }
    if (s_hasUrl) {
      char url[256], opts[64];
      portENTER_CRITICAL(&s_mux);
      memcpy(url, s_url, sizeof(url));
      memcpy(opts, s_urlOpts, sizeof(opts));
      s_hasUrl = false;
      portEXIT_CRITICAL(&s_mux);
      s_stop = false;
      playFromUrl(url, opts);
    }
    WiFiClient c;
    if (listening) c = s_srv.available();
    if (c) { handle(c); c.stop(); }
    else vTaskDelay(pdMS_TO_TICKS(20));
  }
}

// Started at boot; the task opens port 81 by itself once a network exists
void Audio::begin() {
  if (s_task) return;
  // Core 0, beside WiFi: the main loop (CAN, LVGL) keeps core 1 to itself
  xTaskCreatePinnedToCore(task, "audio", 6144, nullptr, 3, &s_task, 0);
  Serial.printf("Audio: announcements on http://<ip>:%d/play\n", AUDIO_PORT);
}
