// On-board RGB LED, speaker through the FM8002A, system clock
#include <sys/time.h>
#include "drivers.h"

// ═════════════════════ Speaker ═════════════════════
// Two lessons carried over from esp32Fire:
//  · Do NOT use ledcWriteTone(): it silently switches the duty resolution to
//    10 bits and writes 50 % on its own scale — compute duty on another scale
//    and the speaker is 8 dB quieter with nobody knowing why.
//  · The fundamental of a square wave scales with sin(π·D), not with D. Ten
//    equal duty steps make step 8 only 0.4 dB below step 10. To make each step
//    audible the sine has to be inverted.
static uint8_t  s_vol = 6;
static uint32_t s_toneHz = 0;

static uint32_t dutyFor(float amp) {          // amplitude 0..1 → 10-bit duty
  if (amp <= 0.0f) return 0;
  if (amp > 1.0f) amp = 1.0f;
  return (uint32_t)(asinf(amp) / (float)M_PI * (float)((1 << LEDC_SND_BITS) - 1));
}
static float volAmp() {                        // 2 dB per volume step
  return powf(10.0f, ((float)s_vol - 10.0f) / 10.0f);
}

static void toneSet(uint32_t hz, uint32_t duty) {
  if (!hz || !duty) { ledcWrite(LEDC_CH_SND, 0); return; }
  // Calling ledcSetup() over and over at the same frequency crackles — only on change
  if (hz != s_toneHz) { ledcSetup(LEDC_CH_SND, hz, LEDC_SND_BITS); s_toneHz = hz; }
  ledcWrite(LEDC_CH_SND, duty);
}

// Non-blocking note sequencer: Out::loop() walks through it.
struct Note { uint16_t hz; uint16_t ms; bool decay; };
static Note     s_seq[6];
static uint8_t  s_seqN = 0, s_seqI = 0;
static uint32_t s_noteAt = 0;
static bool     s_isChime = false;

static void seqStart(const Note* n, uint8_t cnt, bool chime) {
  if (cnt > 6) cnt = 6;
  memcpy(s_seq, n, cnt * sizeof(Note));
  s_seqN = cnt; s_seqI = 0;
  s_noteAt = millis();
  s_isChime = chime;
  toneSet(s_seq[0].hz, s_seq[0].hz ? dutyFor(volAmp()) : 0);
}

void Out::setVolume(uint8_t v) { s_vol = (uint8_t)constrain(v, VOL_MIN, VOL_MAX); }

void Out::beep(uint16_t hz, uint16_t ms) {
  if (Audio::busy()) return;                 // the speaker belongs to the announcement
  if (s_seqN && s_isChime) return;           // a key beep never cuts the arrival chime
  const Note n[1] = {{hz, ms, false}};
  seqStart(n, 1, false);
}

void Out::chime(uint8_t strokes) {
  if (Audio::busy()) return;
  // One high stroke = going up. Two falling strokes = going down.
  if (strokes >= 2) {
    const Note n[3] = {{1175, 650, true}, {0, 60, false}, {988, 950, true}};
    seqStart(n, 3, true);
  } else {
    const Note n[1] = {{1319, 1100, true}};
    seqStart(n, 1, true);
  }
}

bool Out::busy() { return s_seqN != 0; }

static void soundLoop(uint32_t now) {
  if (!s_seqN) return;
  if (Audio::busy()) { s_seqN = 0; return; }   // GPIO 26 is on the I2S DAC right now
  const Note& n = s_seq[s_seqI];
  const uint32_t t = now - s_noteAt;
  if (t >= n.ms) {
    if (++s_seqI >= s_seqN) { s_seqN = 0; toneSet(0, 0); return; }
    s_noteAt = now;
    const Note& m = s_seq[s_seqI];
    toneSet(m.hz, m.hz ? dutyFor(volAmp()) : 0);
    return;
  }
  if (n.decay && n.hz) {
    // A real chime decays exponentially; a flat square wave from start to end
    // sounds like a scooter horn. 50 updates per second is smooth enough.
    static uint32_t last = 0;
    if (now - last >= 20) {
      last = now;
      const float env = expf(-3.2f * (float)t / (float)n.ms);
      toneSet(n.hz, dutyFor(volAmp() * env));
    }
  }
}

// ═════════════════════ RGB LED (active low) ═════════════════════
static Out::LedMode s_led = Out::LED_OFF;

void Out::ledSolid(bool r, bool g, bool b) {
  digitalWrite(PIN_LED_R, r ? LOW : HIGH);
  digitalWrite(PIN_LED_G, g ? LOW : HIGH);
  digitalWrite(PIN_LED_B, b ? LOW : HIGH);
}

static bool s_ledOn = false;
void Out::ledMode(LedMode m) { s_led = m; }
void Out::setLedEnabled(bool on) { s_ledOn = on; if (!on) Out::ledSolid(false, false, false); }
bool Out::ledEnabled() { return s_ledOn; }

static void ledLoop(uint32_t now) {
  static uint32_t last = 0;
  if (now - last < 50) return;
  last = now;
  const bool slow = (now / 500) & 1, fast = (now / 250) & 1;
  if (!s_ledOn) return;                // disabled: leave it dark
  switch (s_led) {
    case Out::LED_OFF:    Out::ledSolid(false, false, false); break;
    case Out::LED_OK:     Out::ledSolid(false, true, false); break;
    case Out::LED_NOLINK: Out::ledSolid(false, false, slow); break;     // blue blinking
    case Out::LED_FAULT:  Out::ledSolid(true, true, false); break;      // amber
    case Out::LED_FIRE:   Out::ledSolid(fast, false, false); break;     // red, fast
    case Out::LED_SIM:    Out::ledSolid(false, slow, slow); break;      // cyan: simulating
  }
}

void Out::begin() {
  pinMode(PIN_LED_R, OUTPUT);
  pinMode(PIN_LED_G, OUTPUT);
  pinMode(PIN_LED_B, OUTPUT);
  ledSolid(false, false, false);
  ledcSetup(LEDC_CH_SND, 2000, LEDC_SND_BITS);
  ledcAttachPin(PIN_SPEAKER, LEDC_CH_SND);
  ledcWrite(LEDC_CH_SND, 0);
  s_toneHz = 2000;
  // Time zone set up front: setting the clock by hand before any network must
  // also be read as local time. UTC+7 (Indochina Time), no DST.
  setenv("TZ", "ICT-7", 1);
  tzset();
}

void Out::loop() {
  const uint32_t now = millis();
  soundLoop(now);
  ledLoop(now);
}

// ═════════════════════ Clock ═════════════════════
// No pins are left for a DS3231 (I2C 21/22 gave pin 22 to CAN), so the ESP32
// system clock is used directly: NTP sets it when there is a network, or it is
// set by hand on the SYSTEM screen. Power loss loses the time — log entries
// then carry a '?' instead of a made-up date.
static bool s_manual = false;
static char s_stampBuf[40];
static char s_timeBuf[8];

uint32_t Clock::epoch() {
  const time_t t = time(nullptr);
  return t > 1704067200 ? (uint32_t)t : 0;      // before 2024-01-01 = no time
}

bool Clock::valid() { return epoch() != 0 && (s_manual || Net::timeSynced()); }

void Clock::now(struct tm& t) {
  const time_t e = time(nullptr);
  localtime_r(&e, &t);
}

void Clock::set(const struct tm& t) {
  struct tm tmp = t;
  const time_t e = mktime(&tmp);
  struct timeval tv = {e, 0};
  settimeofday(&tv, nullptr);
  s_manual = true;
}

const char* Clock::stamp() {
  static const char* WD[7] = {"Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat"};
  if (!valid()) { snprintf(s_stampBuf, sizeof(s_stampBuf), "clock not set"); return s_stampBuf; }
  struct tm t;
  now(t);
  const uint8_t wd = (t.tm_wday >= 0 && t.tm_wday < 7) ? t.tm_wday : 0;
  snprintf(s_stampBuf, sizeof(s_stampBuf), "%s %04d-%02d-%02d %02d:%02d",
           WD[wd], t.tm_year + 1900, t.tm_mon + 1, t.tm_mday, t.tm_hour, t.tm_min);
  return s_stampBuf;
}

const char* Clock::timeOnly() {
  if (!valid()) return "--:--";
  struct tm t;
  now(t);
  snprintf(s_timeBuf, sizeof(s_timeBuf), "%02d:%02d", t.tm_hour, t.tm_min);
  return s_timeBuf;
}
