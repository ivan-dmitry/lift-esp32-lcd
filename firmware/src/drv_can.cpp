// CAN bus through the ESP32's built-in TWAI controller
//
// ───────── Rule number one: NEVER DISTURB THE LIFT'S BUS ─────────
//
// The CAN bus is how the lift controller talks to the car-top board, the call
// panels and the door operator. A CAN node configured wrongly — wrong bit rate,
// say — and running in normal mode sends an ERROR FRAME into the middle of
// every frame it cannot decode. The controller sees communication errors and
// stops the lift (most brands have a dedicated "CAN communication lost" fault).
// A display plugged in wrong can trap people in the car.
//
// So:
//   1. Default is LISTEN-ONLY (TWAI_MODE_LISTEN_ONLY): no ACK, no error frames,
//      no transmission at all — completely invisible on the bus.
//   2. ACK mode (required for sending commands) still STARTS IN LISTEN-ONLY,
//      and only switches to normal mode after hearing NORMAL_ARM_FRAMES valid
//      frames with almost no bus errors. At a wrong bit rate it never hears a
//      valid frame → it never starts acknowledging.
//   3. In ACK mode, bus errors above FALLBACK_ERR_PER_S for FALLBACK_SECONDS
//      seconds in a row → drop back to LISTEN-ONLY and stay there until the
//      user re-enables it. No automatic retry: flapping on and off is worse
//      than staying off.
//   4. Bit-rate detection only ever runs in LISTEN-ONLY.
//
// TX pin: between ESP32 reset and the moment the driver takes the pin, GPIO 21
// floats. The SN65HVD230 has NO pull-up on D and NO dominant time-out — D
// drifting low holds the whole bus dominant and every other node goes silent.
// Fit a 10 kΩ pull-up from GPIO 21 to 3V3 (README, wiring). The code also
// drives the pin high at the very start of setup() and every time the driver
// is removed.
#include "driver/twai.h"
#include "soc/twai_struct.h"
#include "soc/gpio_struct.h"
#include <xtensa/hal.h>
#include <algorithm>
#include "drivers.h"
#include "lift.h"
#include "store.h"

static uint16_t s_kbps        = 125;
static uint8_t  s_modeWanted  = CAN_LISTEN;
static uint8_t  s_modeActive  = CAN_LISTEN;
static bool     s_installed   = false;
static bool     s_arming      = false;
static bool     s_fellBack    = false;
static uint32_t s_armFrames   = 0;
static uint32_t s_armErr0     = 0;
static uint8_t  s_badSecs     = 0;
// 24/7: after a fallback, ACK is armed again by itself after a quiet wait —
// 10 min, doubled after each fallback that follows a re-arm within the hour
// (max 8 h), back to 10 min after a clean day. A broken transmitter (bus-off
// three times in a minute) is handled the same way.
static uint32_t s_rearmAt     = 0;      // millis() deadline, 0 = none
static uint32_t s_rearmMin    = 10;     // current wait in minutes
static uint32_t s_rearmedAt   = 0;      // millis() of the last automatic re-arm
static uint32_t s_busOffAt[3] = {0, 0, 0};
static bool     s_txFail      = false;  // the fallback was caused by bus-off, not bus errors
static void   (*s_simSink)(const CanFrame&) = nullptr;

static Can::Stats s_st;
static uint32_t s_errLast = 0;           // bus_error_count at the previous poll
static uint32_t s_ecc[256];              // error code captures seen, by ECC register value
static uint32_t s_eccN = 0;
static bool customTiming(uint32_t bps, twai_timing_config_t& t, bool& div);
static uint32_t s_missLast = 0, s_arbLast = 0, s_txfLast = 0;
static uint32_t s_fpsWin = 0, s_secAt = 0, s_errSec0 = 0;

// ───── table of frames heard ─────
static Can::Seen s_seen[SEEN_MAX];
static uint8_t   s_seenN   = 0;
static uint16_t  s_seenGen = 0;
static bool      s_paused  = false;

// ───── bit-rate detection ─────
static bool     s_ab = false;
static uint8_t  s_abStep = 0;
static uint32_t s_abAt = 0, s_abRx0 = 0;
static uint32_t s_abFrames[CAN_KBPS_N];
static uint16_t s_abResult = 0;
static uint16_t s_abKbps0 = 125;
#define AB_DWELL_MS 900                 // slow hall-call buses send less often

// Exact timings from the 80 MHz APB clock. 20 time quanta per bit, sample
// point 80 % (sync 1 + tseg1 15 of 20) — the same shape as Espressif's macros —
// except 25 k and 800 k, which need 25 quanta to keep BRP even and ≤ 128.
static bool s_needDiv = false;          // the timing just returned needs BRP_DIV (×2)
static twai_timing_config_t timingFor(uint16_t kbps) {
  struct R { uint16_t k; uint8_t brp, t1, t2; bool div; };
  static const R T[] = {
    {12, 128, 16, 8, true}, {16, 100, 16, 8, true}, {20, 100, 15, 4, true},
    {25, 128, 16, 8}, {33, 120, 15, 4}, {40, 100, 15, 4}, {47, 84, 15, 4}, {50, 80, 15, 4},
    {62, 64, 15, 4},  {66, 60, 15, 4},  {80, 50, 15, 4},  {83, 48, 15, 4}, {100, 40, 15, 4},
    {125, 32, 15, 4}, {200, 20, 15, 4}, {250, 16, 15, 4}, {400, 10, 15, 4}, {500, 8, 15, 4},
    {666, 6, 15, 4},  {800, 4, 16, 8},  {1000, 4, 15, 4},
  };
  twai_timing_config_t t = TWAI_TIMING_CONFIG_125KBITS();
  s_needDiv = false;
  for (const R& r : T)
    if (r.k == kbps) { t.brp = r.brp; t.tseg_1 = r.t1; t.tseg_2 = r.t2; t.sjw = 3; t.triple_sampling = false; s_needDiv = r.div; }
  if (kbps >= CAN_CUSTOM_MIN) customTiming(canRateBps(kbps), t, s_needDiv);
  return t;
}

// Timing for a measured, non-standard rate: the prescaler (even, 2–128, doubled
// by BRP_DIV above that) and 8–25 time quanta that hit the rate best; sample
// point near 75 %. false = no setting within 0.5 %.
static bool customTiming(uint32_t bps, twai_timing_config_t& t, bool& div) {
  if (bps < 10000 || bps > 1000000) return false;
  float best = 1.0f;
  int bReg = 0, bTq = 0; bool bDiv = false;
  for (int tq = 25; tq >= 8; tq--) {
    for (int d = 0; d < 2; d++) {
      const float eff = 80e6f / ((float)bps * tq) / (d ? 2 : 1);
      const int reg = 2 * (int)lroundf(eff / 2);
      if (reg < 2 || reg > 128) continue;
      const float real = 80e6f / ((d ? 2 : 1) * (float)reg * tq);
      const float err = fabsf(real - bps) / bps;
      if (err < best - 1e-5f) { best = err; bReg = reg; bTq = tq; bDiv = d; }
    }
  }
  if (!bReg || best > 0.005f) return false;
  int t1 = (int)lroundf(0.75f * bTq) - 1, t2 = bTq - 1 - t1;
  if (t2 > 8)  { t2 = 8;  t1 = bTq - 9; }
  if (t1 > 16) { t1 = 16; t2 = bTq - 17; }
  if (t2 < 1 || t2 > 8) return false;
  t.brp = (uint32_t)bReg; t.tseg_1 = (uint8_t)t1; t.tseg_2 = (uint8_t)t2;
  t.sjw = (uint8_t)(t2 < 3 ? t2 : 3); t.triple_sampling = false;
  div = bDiv;
  return true;
}

bool Can::rateOk(uint16_t k) {
  if (k >= CAN_CUSTOM_MIN) { twai_timing_config_t t; bool d; return customTiming(canRateBps(k), t, d); }
  for (uint8_t i = 0; i < CAN_KBPS_N; i++) if (CAN_KBPS[i] == k) return true;
  return false;
}

// Raw capture — a logic analyzer on the RX pin. Polls the pin as fast as the CPU
// goes and stamps every edge with the cycle counter: no gap rules, so a busy or
// odd line is seen whole. Interrupts are off on this core for the window (≤ 100 ms;
// WiFi runs on the other core).
uint16_t Can::rawCapture(uint32_t* edges, uint16_t maxEdges, uint32_t ms, uint8_t& level0, uint32_t& cyclesPerUs) {
  const uint32_t mask = 1UL << PIN_CAN_RX;
  cyclesPerUs = ESP.getCpuFreqMHz();
  const uint32_t dur = ms * 1000UL * cyclesPerUs;
  uint16_t n = 0;
  portDISABLE_INTERRUPTS();
  uint32_t last = GPIO.in & mask;
  level0 = last ? 1 : 0;
  const uint32_t c0 = xthal_get_ccount();
  uint32_t now;
  do {
    const uint32_t v = GPIO.in & mask;
    now = xthal_get_ccount() - c0;
    if (v != last) { last = v; edges[n++] = now; }
  } while (now < dur && n < maxEdges);
  portENABLE_INTERRUPTS();
  return n;
}

uint32_t Can::eccCounts(uint32_t* out256) { memcpy(out256, s_ecc, sizeof(s_ecc)); return s_eccN; }

static void holdTxRecessive() {
  pinMode(PIN_CAN_TX, OUTPUT);
  digitalWrite(PIN_CAN_TX, HIGH);       // high = recessive
}

static void uninstall() {
  if (!s_installed) return;
  twai_stop();
  twai_driver_uninstall();
  s_installed = false;
  holdTxRecessive();
}

static bool install(uint16_t kbps, uint8_t mode) {
  uninstall();
  // A driver left installed by a failed uninstall (it refuses while recovering
  // from bus-off) would make every install below fail for good: clear it.
  twai_stop();
  twai_driver_uninstall();
  twai_general_config_t g = TWAI_GENERAL_CONFIG_DEFAULT(
      (gpio_num_t)PIN_CAN_TX, (gpio_num_t)PIN_CAN_RX,
      mode == CAN_NORMAL ? TWAI_MODE_NORMAL : TWAI_MODE_LISTEN_ONLY);
  g.rx_queue_len   = CAN_RXQ_LEN;
  g.tx_queue_len   = CAN_TXQ_LEN;
  g.alerts_enabled = TWAI_ALERT_BUS_OFF | TWAI_ALERT_BUS_RECOVERED | TWAI_ALERT_ERR_PASS |
                     TWAI_ALERT_RX_QUEUE_FULL;
  const twai_timing_config_t t = timingFor(kbps);
  const twai_filter_config_t f = TWAI_FILTER_CONFIG_ACCEPT_ALL();
  if (twai_driver_install(&g, &t, &f) != ESP_OK) {
    Serial.printf("CAN  : could NOT install the driver at %u kbit/s\n", kbps);
    holdTxRecessive();
    return false;
  }
  if (s_needDiv) {
    // 12.5 / 16 / 20 kbit/s: the driver programmed half the rate's prescaler;
    // BRP_DIV doubles it. Only exists from chip revision 2 on — older chips
    // would silently run at twice the rate, so refuse instead.
    if (ESP.getChipRevision() < 2) {
      twai_driver_uninstall();
      holdTxRecessive();
      Serial.printf("CAN  : %s kbit/s needs ESP32 rev 2+ (this chip is rev %d)\n", canRateLabel(kbps), ESP.getChipRevision());
      return false;
    }
    TWAI.interrupt_enable_reg.brp_div = 1;      // still in reset mode: timing registers are writable
  }
  if (twai_start() != ESP_OK) {
    twai_driver_uninstall();
    holdTxRecessive();
    Serial.println("CAN  : could NOT start the driver");
    return false;
  }
  if (mode == CAN_LISTEN) {
    // ESP32 errata: in listen-only mode the controller still sends DOMINANT
    // active error flags when it detects an error — at a wrong bit rate, during
    // detection, after a fallback. The prebuilt core lacks IDF's
    // TWAI_ERRATA_FIX_LISTEN_ONLY_DOM, so do what that fix does: make the
    // controller error-passive (REC = 128). An error-passive node only sends
    // recessive (passive) error flags, and in listen-only mode the counters do
    // not move, so it stays that way. Truly invisible on the bus.
    TWAI.mode_reg.rm = 1;
    TWAI.rx_error_counter_reg.rxerr = 128;
    TWAI.mode_reg.rm = 0;
  }
  s_installed  = true;
  s_modeActive = mode;
  s_kbps       = kbps;
  memset(s_ecc, 0, sizeof(s_ecc));              // error codes only mean something at one rate
  s_eccN = 0;
  s_errLast = s_missLast = s_arbLast = s_txfLast = 0;   // the driver's counters just restarted at 0
  s_errSec0 = s_st.busErr;
  s_badSecs = 0;
  Serial.printf("CAN  : %s kbit/s · %s\n", canRateLabel(kbps), mode == CAN_NORMAL ? "ACK" : "LISTEN-ONLY");
  return true;
}

static void fallBack(bool txFail) {
  const uint32_t now = millis();
  // Fell back again soon after an automatic re-arm → wait twice as long next time
  if (s_rearmedAt && now - s_rearmedAt < 3600000UL) s_rearmMin = (s_rearmMin * 2 > 480) ? 480 : s_rearmMin * 2;
  s_fellBack = true;
  s_txFail   = txFail;
  s_rearmAt  = now + s_rearmMin * 60000UL;
  if (!s_rearmAt) s_rearmAt = 1;
  install(s_kbps, CAN_LISTEN);
}

// One single path every time the bit rate or the mode changes: always go to
// LISTEN-ONLY first, and let the arming step decide whether ACK comes on.
static void restart() {
  if (s_simSink) { uninstall(); return; }       // simulating: detached from the bus
  install(s_kbps, CAN_LISTEN);
  s_arming = (s_modeWanted == CAN_NORMAL) && !s_fellBack;
  s_armFrames = 0;
  s_armErr0 = s_st.busErr;
}

void Can::begin(uint16_t kbps, uint8_t mode) {
  holdTxRecessive();
  s_kbps = kbps;
  s_modeWanted = mode;
  restart();
}

void Can::setBitrate(uint16_t kbps) {
  if (kbps == s_kbps && s_installed) return;
  s_kbps = kbps;
  restart();
}

void Can::setMode(uint8_t mode) {
  s_modeWanted = mode;
  s_fellBack = false;
  restart();
}

void Can::clearFallback() {
  if (!s_fellBack) return;
  s_fellBack = false;
  s_txFail   = false;
  s_rearmAt  = 0;
  restart();
}

uint16_t Can::bitrate()    { return s_kbps; }
uint8_t  Can::modeWanted() { return s_modeWanted; }
uint8_t  Can::modeActive() { return s_installed ? s_modeActive : CAN_LISTEN; }
bool     Can::arming()     { return s_arming; }
bool     Can::fellBack()   { return s_fellBack; }

const char* Can::stateText() {
  if (s_simSink)    return "SIMULATION · off the bus";
  if (s_ab)         return "DETECTING BIT RATE";
  if (!s_installed) return "DRIVER NOT RUNNING";
  if (s_st.state == TWAI_STATE_BUS_OFF)    return "BUS-OFF";
  if (s_st.state == TWAI_STATE_RECOVERING) return "RECOVERING";
  if (s_fellBack)   return s_txFail ? "LISTEN-ONLY · SENDING FAILS (CTX wire?)" : "FELL BACK TO LISTEN · bus errors";
  if (s_arming)     return "LISTENING before ACK";
  // In listen-only the controller is error-passive on purpose (see install())
  if (s_modeActive == CAN_NORMAL && (s_st.tec >= 128 || s_st.rec >= 128)) return "ERROR PASSIVE";
  return s_modeActive == CAN_NORMAL ? "RUNNING · ACK" : "RUNNING · LISTEN-ONLY";
}

void Can::stats(Stats& s) { s = s_st; }

// ═════════════════════════ Table of frames heard ═════════════════════════
static inline uint32_t keyOf(uint32_t id, bool ext) { return (ext ? 0x80000000UL : 0) | id; }

static void seenUpdate(const CanFrame& f, uint32_t now) {
  if (s_paused) return;
  const uint32_t k = keyOf(f.id, f.ext);
  int lo = 0, hi = (int)s_seenN - 1;
  while (lo <= hi) {                         // binary search, table is sorted by ID
    const int mid = (lo + hi) / 2;
    const uint32_t km = keyOf(s_seen[mid].f.id, s_seen[mid].f.ext);
    if (km == k) {
      Can::Seen& e = s_seen[mid];
      for (uint8_t i = 0; i < 8; i++)
        if (i < f.dlc && (i >= e.f.dlc || e.f.d[i] != f.d[i])) e.chgAt[i] = now;
      e.f = f;
      e.count++;
      e.win++;
      e.lastAt = now;
      return;
    }
    if (km < k) lo = mid + 1; else hi = mid - 1;
  }
  if (s_seenN >= SEEN_MAX) return;           // table full: new IDs are dropped, the screen says so
  memmove(&s_seen[lo + 1], &s_seen[lo], (s_seenN - lo) * sizeof(Can::Seen));
  Can::Seen& e = s_seen[lo];
  memset(&e, 0, sizeof(e));
  e.f = f;
  e.count = 1; e.win = 1; e.lastAt = now;
  s_seenN++;
  s_seenGen++;
}

uint8_t Can::seenCount() { return s_seenN; }
const Can::Seen* Can::seenAt(uint8_t i) { return i < s_seenN ? &s_seen[i] : nullptr; }
int Can::seenFind(uint32_t id, bool ext) {
  for (uint8_t i = 0; i < s_seenN; i++)
    if (s_seen[i].f.id == id && s_seen[i].f.ext == ext) return i;
  return -1;
}
void Can::seenClear() { s_seenN = 0; s_seenGen++; }
uint16_t Can::seenGen() { return s_seenGen; }
void Can::setPaused(bool p) { s_paused = p; }
bool Can::paused() { return s_paused; }

// ═════════════════════════ Receive / transmit ═════════════════════════
static void handle(const CanFrame& f) {
  const uint32_t now = millis();
  s_st.rx++;
  s_fpsWin++;
  if (s_arming) s_armFrames++;
  seenUpdate(f, now);
  Lift::onFrame(f);
  CanDebug::onFrame(f, false);
}

void Can::inject(const CanFrame& f) { handle(f); }

void Can::setSimSink(void (*sink)(const CanFrame&)) {
  s_simSink = sink;
  restart();                                  // entering simulation removes the driver; leaving reinstalls it
}

bool Can::send(const CanFrame& f) {
  if (s_simSink) { s_simSink(f); s_st.tx++; CanDebug::onFrame(f, true); return true; }
  if (!s_installed || s_modeActive != CAN_NORMAL || s_arming) return false;
  twai_message_t m = {};
  m.identifier = f.id;
  m.extd = f.ext ? 1 : 0;
  m.data_length_code = f.dlc > 8 ? 8 : f.dlc;
  memcpy(m.data, f.d, m.data_length_code);
  // Wait 0 ms: a full TX queue means the bus is in trouble; waiting only stalls the main loop.
  if (twai_transmit(&m, 0) == ESP_OK) { s_st.tx++; CanDebug::onFrame(f, true); return true; }
  s_st.txFail++;
  return false;
}

// ═════════════════════════ Bit-timing probe ═════════════════════════
// When no rate gives a single valid frame, guessing more rates is blind. On CAN,
// every level lasts a whole number of bits (1–5 inside a frame, thanks to bit
// stuffing), so the pulse lengths reveal the bit time — at ANY rate, standard or
// not — and whether the signal looks like CAN at all. Purely listening.
// The pulses come from rawCapture() (the CPU polling the pin), not the RMT: the
// RMT closes a capture only on a quiet gap and holds 512 levels, so on a busy
// line it overflowed and saw almost nothing — the first probe at the lift did.
static uint32_t s_pdBuf[2][800];
static uint8_t  s_plBuf[2][800];
static uint32_t* s_pd = s_pdBuf[0];
static uint8_t*  s_pl = s_plBuf[0];

bool Can::probe(Probe& r, uint32_t ms) {
  memset(&r, 0, sizeof(r));
  const uint16_t MAXE = 1200;
  uint32_t* e = (uint32_t*)malloc(MAXE * sizeof(uint32_t));
  if (!e) return false;
  // A few short windows (interrupts are off during each), until ~300 pulses
  uint16_t n = 0;
  uint64_t hi = 0, lo = 0;
  uint32_t win = 0;
  const int windows = (int)constrain(ms / 75, 1UL, 8UL);
  for (int w = 0; w < windows && n < 300; w++) {
    uint8_t lvl0 = 1;
    uint32_t cpu = 240;
    const uint16_t k = rawCapture(e, MAXE, 75, lvl0, cpu);
    win += 75;
    uint8_t lv = lvl0 ^ 1;                        // level after the first edge
    for (uint16_t i = 1; i < k && n < 790; i++) { // the part before the first edge is partial
      const uint32_t d = (e[i] - e[i - 1]) * 10 / cpu;   // 0.1 µs
      s_pd[n] = d; s_pl[n] = lv; n++;
      (lv ? hi : lo) += d;
      lv ^= 1;
    }
    if (w + 1 < windows) delay(2);                // let the other interrupts run
  }
  free(e);
  r.slow  = false;
  r.gapUs = 0;
  const uint32_t* d = s_pd;
  r.pulses = n;
  r.edgesPerSec = (uint16_t)min<uint32_t>(65535, n * 1000UL / win);
  r.highPct = (hi + lo) ? (uint8_t)(hi * 100 / (hi + lo)) : 0;
  r.rawN = (uint8_t)min<uint16_t>(n, 32);
  for (uint8_t i = 0; i < r.rawN; i++) {
    r.rawUs[i] = (d[i] + 5) / 10;
    if (s_pl[i]) r.rawHi |= 1UL << i;
  }
  if (n < 20) return true;                        // (almost) no edges: idle bus or dead line

  // Shortest common pulse ≈ one bit: take the 10th percentile, not the minimum,
  // so a stray glitch cannot pull the estimate down.
  uint32_t* s = s_pdBuf[1];                       // free again: scratch for sorting
  memcpy(s, d, n * sizeof(uint32_t));
  std::sort(s, s + n);
  r.minTicks = s[0];
  const float base = s[n / 10];
  // Refine: the bit time that makes the short pulses closest to whole multiples
  float bestT = base, bestErr = 1e9f;
  for (float T = base * 0.8f; T <= base * 1.25f; T += base * 0.0025f) {
    float err = 0; uint16_t used = 0;
    for (uint16_t i = 0; i < n; i++) {
      const float x = d[i] / T;
      if (x < 0.5f || x > 6.5f) continue;
      err += fabsf(x - roundf(x));
      used++;
    }
    if (used > n / 3 && err / used < bestErr) { bestErr = err / used; bestT = T; }
  }
  r.bitUs = bestT / 10.0f;
  r.rateBps = (uint32_t)(10000000.0f / bestT + 0.5f);
  r.fit = bestErr;                                // 0 = perfect multiples; > 0.15 = not CAN-like
  uint32_t bestD = 0xFFFFFFFF;
  for (uint8_t i = 0; i < CAN_KBPS_N; i++) {
    const uint32_t b = canRateBps(CAN_KBPS[i]);
    const uint32_t dd = b > r.rateBps ? b - r.rateBps : r.rateBps - b;
    if (dd < bestD) { bestD = dd; r.nearestKbps = CAN_KBPS[i]; }
  }
  r.nearestOk = bestD * 100 <= r.rateBps * 3;   // within 3 %
  for (uint16_t i = 0; i < n; i++) {
    const float x = d[i] / bestT;
    if (x <= 12.5f && roundf(x) > r.maxRun) r.maxRun = (uint8_t)roundf(x);
  }
  static const uint32_t UART[] = {300, 600, 1200, 2400, 4800, 9600, 14400, 19200, 28800,
                                  38400, 57600, 76800, 115200, 230400};
  for (uint32_t u : UART) {
    const uint32_t dd = u > r.rateBps ? u - r.rateBps : r.rateBps - u;
    if (dd * 100 <= u * 3) r.uartBaud = u;
  }
  return true;
}

// ═════════════════════════ Bit-rate detection ═════════════════════════
void Can::autoBaudStart() {
  if (s_simSink || s_ab) return;
  s_abKbps0 = s_kbps;
  s_ab = true;
  s_abStep = 0;
  s_abResult = 0;
  s_arming = false;
  memset(s_abFrames, 0, sizeof(s_abFrames));
  install(CAN_KBPS[0], CAN_LISTEN);
  s_abAt = millis();
  s_abRx0 = s_st.rx;
}
bool     Can::autoBauding()   { return s_ab; }
uint8_t  Can::autoBaudStep()  { return s_abStep; }
uint16_t Can::autoBaudResult(){ return s_abResult; }
uint32_t Can::autoBaudFrames(uint8_t i) { return i < CAN_KBPS_N ? s_abFrames[i] : 0; }

static void autoBaudTick(uint32_t now) {
  if (now - s_abAt < AB_DWELL_MS) return;
  s_abFrames[s_abStep] = s_st.rx - s_abRx0;
  Serial.printf("CAN  : %6s kbit/s -> %lu valid frames\n", canRateLabel(CAN_KBPS[s_abStep]),
                (unsigned long)s_abFrames[s_abStep]);
  s_abStep++;
  if (s_abStep < CAN_KBPS_N) {
    install(CAN_KBPS[s_abStep], CAN_LISTEN);
    s_abAt = millis();
    s_abRx0 = s_st.rx;
    return;
  }
  // Pick the rate with the MOST valid frames. At a wrong rate the controller
  // almost never assembles a frame with a correct CRC, so the gap is obvious.
  uint32_t best = 0;
  for (uint8_t i = 0; i < CAN_KBPS_N; i++)
    if (s_abFrames[i] > best) { best = s_abFrames[i]; s_abResult = CAN_KBPS[i]; }
  s_ab = false;
  if (s_abResult) {
    s_kbps = s_abResult;
    Store::cfg().canKbps = s_abResult;
    Store::save();
    Store::logAdd(EV_CONFIG);
    Serial.printf("CAN  : picked %s kbit/s\n", canRateLabel(s_abResult));
  } else {
    s_kbps = s_abKbps0;
    Serial.println("CAN  : no frames at any rate - check CANH/CANL wiring and transceiver power");
  }
  restart();
}

// ═════════════════════════ Loop ═════════════════════════
void Can::loop() {
  const uint32_t now = millis();
  // Not simulating, not detecting, yet no driver: an install failed (e.g. no
  // memory for the queue). Try again every 10 s instead of NO SIGNAL for ever.
  static uint32_t reinstAt = 0;
  if (!s_installed && !s_simSink && !s_ab && now - reinstAt >= 10000) {
    reinstAt = now;
    Serial.println("CAN  : driver not running -> installing again");
    restart();
  }
  if (!s_installed) {
    // Simulation (driver removed) still feeds frames through inject(): keep
    // the rates of the RAW FRAMES table and the frames/s counter alive.
    if (now - s_secAt >= 1000) {
      s_secAt = now;
      s_st.fps = (uint16_t)s_fpsWin;
      s_fpsWin = 0;
      for (uint8_t i = 0; i < s_seenN; i++) { s_seen[i].rate = s_seen[i].win; s_seen[i].win = 0; }
    }
    return;
  }

  // ── Drain the queue. At most 64 frames per pass so a frame storm cannot hold
  //    the loop hostage — the rest waits in the queue. ──
  twai_message_t m;
  for (uint8_t n = 0; n < 64 && twai_receive(&m, 0) == ESP_OK; n++) {
    if (m.rtr) continue;
    CanFrame f;
    f.id = m.identifier;
    f.ext = m.extd;
    f.dlc = m.data_length_code > 8 ? 8 : m.data_length_code;
    memcpy(f.d, m.data, 8);
    handle(f);
  }

  // ── Driver alerts ──
  uint32_t al = 0;
  if (twai_read_alerts(&al, 0) == ESP_OK && al) {
    if (al & TWAI_ALERT_BUS_OFF) {
      // Our own sends failed until bus-off. Drop what is queued (an old call must
      // not fire later). Three times within a minute = the transmitter does not
      // reach the bus (loose CTX wire, broken transceiver): stop trying, listen
      // only — instead of recovering and failing again forever.
      twai_clear_transmit_queue();
      s_busOffAt[0] = s_busOffAt[1]; s_busOffAt[1] = s_busOffAt[2]; s_busOffAt[2] = now ? now : 1;
      if (s_busOffAt[0] && now - s_busOffAt[0] < 60000UL) {
        Serial.println("CAN  : BUS-OFF x3 within a minute -> LISTEN-ONLY (sending fails)");
        memset(s_busOffAt, 0, sizeof(s_busOffAt));
        Store::logAdd(EV_TXFAIL);
        fallBack(true);
        return;
      }
      Store::logAdd(EV_BUSOFF);
      Serial.println("CAN  : BUS-OFF -> recovering");
      twai_initiate_recovery();
    }
    if (al & TWAI_ALERT_BUS_RECOVERED) {
      if (twai_start() == ESP_OK) Serial.println("CAN  : recovered");
      else { Serial.println("CAN  : could not restart after bus-off -> reinstalling"); restart(); }
    }
  }

  // ── Status, 4 times a second ──
  static uint32_t lastPoll = 0;
  if (now - lastPoll >= 250) {
    lastPoll = now;
    twai_status_info_t si;
    if (twai_get_status_info(&si) == ESP_OK) {
      s_st.state = (uint8_t)si.state;
      s_st.tec   = (uint8_t)(si.tx_error_counter > 255 ? 255 : si.tx_error_counter);
      s_st.rec   = (uint8_t)(si.rx_error_counter > 255 ? 255 : si.rx_error_counter);
      // A new bus error since the last poll: sample the error code capture
      // register — what kind of error, in which part of the frame. The driver's
      // ISR reads it on every error (re-arming it); the latest capture stays.
      if (si.bus_error_count != s_errLast) {
        s_ecc[TWAI.error_code_capture_reg.val & 0xFF]++;
        s_eccN++;
      }
      s_st.busErr   += si.bus_error_count  - s_errLast;  s_errLast  = si.bus_error_count;
      s_st.rxMissed += si.rx_missed_count  - s_missLast; s_missLast = si.rx_missed_count;
      s_st.arbLost  += si.arb_lost_count   - s_arbLast;  s_arbLast  = si.arb_lost_count;
      s_st.txFail   += si.tx_failed_count  - s_txfLast;  s_txfLast  = si.tx_failed_count;
      // Error passive with frames still waiting → nobody is acknowledging.
      // Flush the queue: an old car call must not fire when the bus comes back.
      if (si.tx_error_counter >= 128 && si.msgs_to_tx) twai_clear_transmit_queue();
    }
  }

  // ── Once a second: frames/s, arming, fallback ──
  if (now - s_secAt >= 1000) {
    s_secAt = now;
    s_st.fps = (uint16_t)s_fpsWin;
    s_fpsWin = 0;
    for (uint8_t i = 0; i < s_seenN; i++) { s_seen[i].rate = s_seen[i].win; s_seen[i].win = 0; }

    const uint32_t errSec = s_st.busErr - s_errSec0;
    s_errSec0 = s_st.busErr;

    if (s_arming && s_armFrames >= NORMAL_ARM_FRAMES) {
      if (s_st.busErr - s_armErr0 <= 2) {
        s_arming = false;
        install(s_kbps, CAN_NORMAL);
      } else {
        // Hearing frames but with errors mixed in: not clean enough for ACK. Start over.
        s_armFrames = 0;
        s_armErr0 = s_st.busErr;
      }
    }
    if (s_modeActive == CAN_NORMAL && !s_arming) {
      s_badSecs = (errSec > FALLBACK_ERR_PER_S) ? (uint8_t)(s_badSecs + 1) : 0;
      if (s_badSecs >= FALLBACK_SECONDS) {
        Serial.printf("CAN  : %lu bus errors/s -> FALLING BACK TO LISTEN-ONLY\n", (unsigned long)errSec);
        fallBack(false);
        Store::logAdd(EV_FALLBACK, 0, (int16_t)(errSec > 32767 ? 32767 : errSec));
      }
    }
  }

  // Automatic re-arm after a fallback (24/7: nobody may be there to press RETRY ACK).
  // Arming still starts listen-only and needs 20 clean frames, as always.
  if (s_fellBack && s_rearmAt && s_modeWanted == CAN_NORMAL && !s_simSink && !s_ab &&
      timeReached(now, s_rearmAt)) {
    Serial.printf("CAN  : re-arming ACK after %lu min\n", (unsigned long)s_rearmMin);
    Store::logAdd(EV_REARM, 0, (int16_t)s_rearmMin);
    s_fellBack = false;
    s_txFail   = false;
    s_rearmAt  = 0;
    s_rearmedAt = now ? now : 1;
    restart();
  }
  // A clean day after the last re-arm: back to the short wait
  if (s_rearmedAt && !s_fellBack && now - s_rearmedAt > 86400000UL) { s_rearmedAt = 0; s_rearmMin = 10; }

  if (s_ab) autoBaudTick(now);
}
