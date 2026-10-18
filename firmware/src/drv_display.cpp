// ST7796 display + XPT2046 touch + LVGL + idle dimming
// Carried over from esp32Fire — every lesson about calibration and the
// backlight is kept.
#include <TFT_eSPI.h>
#include <Preferences.h>
#include "drivers.h"

static TFT_eSPI tft;
static Preferences nvs;

// Draw buffer: 480 × 40 px × 2 bytes = 38.4 KB, allocated on the HEAP. As a
// static array it lands in .bss, the dram0_0_seg segment (~160 KB for static
// data) runs out, and LVGL's memory can no longer grow. The heap has ~200 KB.
static lv_disp_draw_buf_t s_drawBuf;
static lv_color_t*        s_buf = nullptr;
#define DRAW_BUF_PX (LCD_W * 40)
static lv_disp_drv_t      s_dispDrv;
static lv_indev_drv_t     s_indevDrv;

static uint8_t  s_brightness   = 80;      // % set by the user
static uint8_t  s_dimPct       = 30;      // % when idle, 0 = off
static uint8_t  s_curDuty      = 0;       // duty being output, for smooth fades
static uint8_t  s_tgtDuty      = 0;
static bool     s_asleep       = false;
static bool     s_sleepLocked  = false;
static uint32_t s_sleepTimeout = 0;
static uint16_t s_cal[5]       = {0, 0, 0, 0, 0};
static bool     s_calOk        = false;
static uint32_t s_nPress       = 0;
static bool     s_swallowUntilRelease = false;  // swallow the whole wake-up touch

static inline uint8_t pctToDuty(uint8_t pct) {
  if (pct > 100) pct = 100;
  // The eye is non-linear: squaring makes the low end smoother
  uint32_t d = (uint32_t)pct * pct * 255 / 10000;
  if (pct > 0 && d < 6) d = 6;          // never fully dark when the user asked for > 0
  return (uint8_t)d;
}

// ───────────────────────── LVGL glue ─────────────────────────
static void flushCb(lv_disp_drv_t* drv, const lv_area_t* area, lv_color_t* px) {
  const uint32_t w = area->x2 - area->x1 + 1;
  const uint32_t h = area->y2 - area->y1 + 1;
  tft.startWrite();
  tft.setAddrWindow(area->x1, area->y1, w, h);
  // LV_COLOR_16_SWAP=1, LVGL already swapped the bytes → do NOT swap again here.
  tft.pushColors((uint16_t*)&px->full, w * h, false);
  tft.endWrite();
  lv_disp_flush_ready(drv);
}

static bool s_touchOn = true;
void Display::setTouchEnabled(bool on) { s_touchOn = on; }
bool Display::touchEnabled() { return s_touchOn; }

static void touchCb(lv_indev_drv_t*, lv_indev_data_t* data) {
  if (!s_touchOn) { data->state = LV_INDEV_STATE_RELEASED; return; }   // disabled in DISPLAY / web
  uint16_t x = 0, y = 0;
  const bool pressed = tft.getTouch(&x, &y, 300);

  if (pressed && s_asleep) {
    // The wake-up touch is SWALLOWED: it must not press whatever is under the finger.
    Display::wake();
    s_swallowUntilRelease = true;
    data->state = LV_INDEV_STATE_RELEASED;
    return;
  }
  // ...and swallowed UNTIL THE FINGER LIFTS, not just one sample. Dropping only
  // the first sample (esp32Fire's old bug) sends the next one, 30 ms later with
  // the finger still down, straight to LVGL as a real press.
  if (s_swallowUntilRelease) {
    if (!pressed) s_swallowUntilRelease = false;
    else          lv_disp_trig_activity(NULL);   // still counts as activity
    data->state = LV_INDEV_STATE_RELEASED;
    return;
  }
  if (pressed) {
    s_nPress++;
    data->point.x = (lv_coord_t)x;
    data->point.y = (lv_coord_t)y;
    data->state   = LV_INDEV_STATE_PRESSED;
  } else {
    data->state = LV_INDEV_STATE_RELEASED;
  }
}

// ───────────────────────── Touch calibration ─────────────────────────
// "Data present" is not enough: boards arrive with junk in NVS, and taking it
// as a calibration maps every touch off-screen.
// The REAL meaning of the 5 numbers TFT_eSPI::setTouch() takes (from
// Touch.cpp, calibrateTouch(), the "pre calculate" step before export):
//     c[0] = x0   — smallest raw X
//     c[1] = X SPAN, i.e. (x1 − x0). NOT x1!
//     c[2] = y0
//     c[3] = Y SPAN
//     c[4] = flags: bit0 = swap axes, bit1 = invert X, bit2 = invert Y
static bool calSane(const uint16_t* c) {
  if (c[0] > 4095 || c[2] > 4095) return false;
  if (c[1] < 1000 || c[3] < 1000) return false;       // a span that narrow is nonsense
  if ((uint32_t)c[0] + c[1] > 4300) return false;
  if ((uint32_t)c[2] + c[3] > 4300) return false;
  if (c[4] > 7) return false;
  return true;
}

static void loadCal() {
  // Same NVS namespace as esp32Fire: a board that ran the fire panel keeps its calibration.
  // Opened read-write so a missing namespace is created instead of logging an error
  nvs.begin("touch", false);
  const size_t n = nvs.isKey("cal") ? nvs.getBytesLength("cal") : 0;
  if (n == sizeof(s_cal)) {
    nvs.getBytes("cal", s_cal, sizeof(s_cal));
    s_calOk = calSane(s_cal);
    Serial.printf("Touch: stored cal %u %u %u %u %u -> %s\n",
                  s_cal[0], s_cal[1], s_cal[2], s_cal[3], s_cal[4],
                  s_calOk ? "valid" : "JUNK, ignored");
  } else {
    Serial.printf("Touch: no calibration (%u bytes)\n", (unsigned)n);
  }
  nvs.end();
}

bool Display::touchCalibrated() { return s_calOk; }

// Approximate factors for the XPT2046 on the 3.5" CYD. Not as good as a real
// calibration, but buttons are hittable — and the display NEVER has to stop
// and wait for someone to touch the screen before it can run.
static const uint16_t CAL_FALLBACK[5] = {268, 3576, 383, 3376, 3};

// Corner touch threshold. The library uses Z_THRESHOLD = 350 normally but
// halves it while calibrating because corners are less sensitive.
#define CAL_Z_MIN   175
#define CAL_RAWERR   20      // allowed difference between two consecutive samples

// One TRUSTWORTHY reading, done the way the library's validTouch() does it:
// wait for the pressure to stop rising (debounce), then require two samples a
// few ms apart to agree within CAL_RAWERR. Every loop here is bounded.
static bool readStable(uint16_t& x, uint16_t& y) {
  uint16_t z1 = 1, z2 = 0;
  for (uint8_t i = 0; i < 30 && z1 > z2; i++) {
    z2 = z1;
    z1 = tft.getTouchRawZ();
    delay(1);
  }
  if (z1 <= CAL_Z_MIN) return false;

  uint16_t x1, y1, x2, y2;
  tft.getTouchRaw(&x1, &y1);
  delay(1);
  if (tft.getTouchRawZ() <= CAL_Z_MIN) return false;
  delay(2);
  tft.getTouchRaw(&x2, &y2);
  if (abs((int)x1 - (int)x2) > CAL_RAWERR) return false;
  if (abs((int)y1 - (int)y2) > CAL_RAWERR) return false;
  x = x1; y = y1;
  return true;
}

// One corner: average of 8 trustworthy readings, with a deadline.
static bool waitPoint(uint16_t& x, uint16_t& y, uint32_t limitMs) {
  const uint32_t t0 = millis();
  uint32_t sx = 0, sy = 0;
  uint8_t  n = 0;
  while (millis() - t0 < limitMs) {
    feedLoopWDT();                              // calibration may wait up to 60 s
    uint16_t rx, ry;
    if (readStable(rx, ry)) {
      sx += rx; sy += ry;
      if (++n >= 8) { x = (uint16_t)(sx / n); y = (uint16_t)(sy / n); return true; }
    }
    delay(4);
  }
  return false;
}

static void calTarget(int16_t x, int16_t y) {
  tft.fillScreen(TFT_BLACK);
  tft.drawFastHLine(x - 14, y, 29, TFT_RED);
  tft.drawFastVLine(x, y - 14, 29, TFT_RED);
  tft.drawCircle(x, y, 8, TFT_RED);
}

// tft.calibrateTouch() from the library loops `while(!validTouch(...));` with
// NO way out: a dead touch film or a loose FPC cable and the main loop stops
// for good. This version has a deadline for all four points together.
bool Display::runTouchCalibration(uint32_t limitMs) {
  Display::wake(true);
  tft.fillScreen(TFT_BLACK);
  tft.setTextColor(TFT_WHITE, TFT_BLACK);
  tft.setTextDatum(MC_DATUM);
  tft.drawString("TOUCH THE CROSS", LCD_W / 2, LCD_H / 2 - 16, 4);
  tft.drawString("use a stylus or a fingernail", LCD_W / 2, LCD_H / 2 + 12, 2);
  delay(700);

  uint16_t saved[5];
  memcpy(saved, s_cal, sizeof(saved));
  const bool hadCal = s_calOk;

  // Corner ORDER: top-left, bottom-left, top-right, bottom-right — the order
  // calibrateTouch() uses, because the math below is its math.
  const int16_t px[4] = {20, 20, (int16_t)(LCD_W - 20), (int16_t)(LCD_W - 20)};
  const int16_t py[4] = {20, (int16_t)(LCD_H - 20), 20, (int16_t)(LCD_H - 20)};
  uint16_t rx[4], ry[4];
  bool ok = true;
  const uint32_t t0 = millis();
  for (uint8_t i = 0; i < 4 && ok; i++) {
    const uint32_t used = millis() - t0;
    if (used >= limitMs) { ok = false; break; }
    calTarget(px[i], py[i]);
    ok = waitPoint(rx[i], ry[i], limitMs - used);
    if (ok) { tft.fillCircle(px[i], py[i], 8, TFT_GREEN); delay(250); }
  }

  if (ok) {
    // ── Factors: EXACTLY the formula of TFT_eSPI::calibrateTouch() ──
    // Lesson from esp32Fire: do not rewrite the library's math just to add a
    // deadline. Take the deadline, keep the math.
    uint16_t v[8];
    for (uint8_t i = 0; i < 4; i++) { v[i * 2] = rx[i]; v[i * 2 + 1] = ry[i]; }
    uint16_t x0, x1, y0, y1;
    bool rot = false, invX = false, invY = false;
    // Corners 0 and 1 differ only vertically on screen. If raw X changes more
    // than raw Y between them, the axes are swapped.
    if (abs((int)v[0] - (int)v[2]) > abs((int)v[1] - (int)v[3])) {
      rot = true;
      x0 = (uint16_t)((v[1] + v[3]) / 2);
      x1 = (uint16_t)((v[5] + v[7]) / 2);
      y0 = (uint16_t)((v[0] + v[4]) / 2);
      y1 = (uint16_t)((v[2] + v[6]) / 2);
    } else {
      x0 = (uint16_t)((v[0] + v[2]) / 2);
      x1 = (uint16_t)((v[4] + v[6]) / 2);
      y0 = (uint16_t)((v[1] + v[5]) / 2);
      y1 = (uint16_t)((v[3] + v[7]) / 2);
    }
    if (x0 > x1) { const uint16_t t = x0; x0 = x1; x1 = t; invX = true; }
    if (y0 > y1) { const uint16_t t = y0; y0 = y1; y1 = t; invY = true; }

    uint16_t c[5];
    c[0] = x0;
    c[1] = (uint16_t)(x1 - x0);        // SPAN, not the maximum
    c[2] = y0;
    c[3] = (uint16_t)(y1 - y0);
    c[4] = (uint16_t)((rot ? 1 : 0) | (invX ? 2 : 0) | (invY ? 4 : 0));
    Serial.printf("Touch: measured %u %u %u %u %u\n", c[0], c[1], c[2], c[3], c[4]);
    ok = calSane(c);
    if (ok) memcpy(s_cal, c, sizeof(s_cal));
  }

  if (!ok) {
    // A failed calibration must not damage the one that exists.
    memcpy(s_cal, hadCal ? saved : CAL_FALLBACK, sizeof(s_cal));
    tft.fillScreen(TFT_BLACK);
    tft.setTextDatum(MC_DATUM);
    tft.setTextColor(TFT_YELLOW, TFT_BLACK);
    tft.drawString("CALIBRATION SKIPPED", LCD_W / 2, LCD_H / 2 - 12, 4);
    tft.drawString(hadCal ? "previous calibration kept" : "using defaults - buttons may be off",
                   LCD_W / 2, LCD_H / 2 + 14, 2);
    delay(1200);
  } else {
    tft.fillScreen(TFT_BLACK);
    tft.setTextDatum(MC_DATUM);
    tft.setTextColor(TFT_WHITE, TFT_BLACK);
    tft.drawString("CALIBRATED", LCD_W / 2, LCD_H / 2, 4);
    delay(500);
  }
  tft.setTouch(s_cal);
  // Only store a REAL measurement. Storing the defaults would make the next
  // boot believe it is calibrated and stop reminding the installer.
  if (ok) {
    nvs.begin("touch", false);
    nvs.putBytes("cal", s_cal, sizeof(s_cal));
    nvs.end();
  }
  s_calOk = ok || hadCal;
  return ok;
}

// ───────────────────────── Lifecycle ─────────────────────────
void Display::begin() {
  pinMode(PIN_TOUCH_IRQ, INPUT);

  tft.init();
  tft.setRotation(LCD_ROTATION);
  tft.fillScreen(TFT_BLACK);

  // PWM MUST be attached AFTER tft.init(). TFT_eSPI's init runs
  //     pinMode(TFT_BL, OUTPUT); digitalWrite(TFT_BL, TFT_BACKLIGHT_ON);
  // which takes pin 27 BACK from the LEDC peripheral. Attach before, and the
  // backlight is stuck at 100 % forever while every ledcWrite still "succeeds".
  ledcSetup(LEDC_CH_BL, 5000, 8);
  ledcAttachPin(PIN_BACKLIGHT, LEDC_CH_BL);
  ledcWrite(LEDC_CH_BL, 0);             // dark until the first frame is drawn

  loadCal();
  // No valid calibration → defaults: buttons may be a few dozen pixels off but
  // usable, and the display NEVER waits for anyone.
  if (!s_calOk) memcpy(s_cal, CAL_FALLBACK, sizeof(s_cal));
  tft.setTouch(s_cal);

  lv_init();
  s_buf = (lv_color_t*)malloc(DRAW_BUF_PX * sizeof(lv_color_t));
  if (!s_buf) {
    Serial.println("COULD NOT ALLOCATE THE DRAW BUFFER - restarting");
    delay(200);
    ESP.restart();
  }
  lv_disp_draw_buf_init(&s_drawBuf, s_buf, nullptr, DRAW_BUF_PX);

  lv_disp_drv_init(&s_dispDrv);
  s_dispDrv.hor_res  = LCD_W;
  s_dispDrv.ver_res  = LCD_H;
  s_dispDrv.flush_cb = flushCb;
  s_dispDrv.draw_buf = &s_drawBuf;
  lv_disp_drv_register(&s_dispDrv);

  lv_indev_drv_init(&s_indevDrv);
  s_indevDrv.type    = LV_INDEV_TYPE_POINTER;
  s_indevDrv.read_cb = touchCb;
  // A long press needs 2.5 s: in display-only mode it is the only way into the
  // menu, and a passer-by brushing the screen must not get in.
  s_indevDrv.long_press_time = 2500;
  lv_indev_drv_register(&s_indevDrv);

  s_tgtDuty = pctToDuty(s_brightness);
}

void Display::loop() {
  lv_timer_handler();

  // Panel keep-alive: SLPOUT, DISPON, pixel format and orientation again, then a
  // full redraw. All idempotent on a healthy panel (nothing visible changes); a
  // panel knocked into sleep or display-off by ESD comes back within 2 minutes.
  static uint32_t lastKeep = 0;
  if (millis() - lastKeep >= 120000UL) {
    lastKeep = millis();
    tft.writecommand(0x11);                 // sleep out (ignored when awake)
    tft.writecommand(0x3A); tft.writedata(0x55);   // 16-bit pixels
    tft.setRotation(LCD_ROTATION);          // MADCTL
    tft.writecommand(0x29);                 // display on
    lv_obj_invalidate(lv_scr_act());
  }

  // Smooth brightness changes without blocking
  static uint32_t lastFade = 0;
  const uint32_t now = millis();
  if (s_curDuty != s_tgtDuty && now - lastFade >= 8) {
    lastFade = now;
    const int step = (s_tgtDuty > s_curDuty) ? 12 : -12;
    int v = (int)s_curDuty + step;
    if ((step > 0 && v > s_tgtDuty) || (step < 0 && v < s_tgtDuty)) v = s_tgtDuty;
    s_curDuty = (uint8_t)v;
    ledcWrite(LEDC_CH_BL, s_curDuty);
  }

  if (!s_asleep && !s_sleepLocked && s_sleepTimeout > 0 &&
      lv_disp_get_inactive_time(NULL) > s_sleepTimeout) {
    sleepNow();
  }
}

void Display::setBrightness(uint8_t pct) {
  s_brightness = pct > 100 ? 100 : pct;
  if (!s_asleep) s_tgtDuty = pctToDuty(s_brightness);
}
uint8_t Display::brightness() { return s_brightness; }

void Display::setDimLevel(uint8_t pct) {
  s_dimPct = pct > 100 ? 100 : pct;
  if (s_asleep) s_tgtDuty = s_dimPct ? pctToDuty(s_dimPct) : 0;
}

void Display::wake(bool force) {
  s_asleep  = false;
  s_tgtDuty = pctToDuty(force ? 100 : s_brightness);
  lv_disp_trig_activity(NULL);
}

void Display::sleepNow() {
  if (s_sleepLocked) return;
  s_asleep  = true;
  // A lift indicator only DIMS by default instead of going dark — people
  // waiting in the lobby must still see where the car is. The CPU keeps
  // listening to CAN as usual.
  s_tgtDuty = s_dimPct ? pctToDuty(s_dimPct) : 0;
}

bool Display::isAsleep() { return s_asleep; }
void Display::setSleepTimeout(uint32_t ms) { s_sleepTimeout = ms; }

void Display::lockSleep(bool locked) {
  s_sleepLocked = locked;
  if (locked && s_asleep) wake(true);
}

uint8_t  Display::duty()   { return s_curDuty; }
uint32_t Display::idleMs() { return lv_disp_get_inactive_time(NULL); }
uint32_t Display::nPress() { return s_nPress; }

// Touch readings for diagnostics, through TFT_eSPI's OWN bus. Never bit-bang
// pins 14/12/13: they belong to the HSPI peripheral the display uses, and
// grabbing them freezes the screen until power-off.
bool Display::touchRaw(uint16_t& x, uint16_t& y) {
  return tft.getTouchRaw(&x, &y) && tft.getTouchRawZ() > 300;
}

bool Display::touchPoint(uint16_t& x, uint16_t& y) {
  if (!s_calOk) return false;
  return tft.getTouch(&x, &y, 300);
}

// The only way to SEE this UI without standing in front of it: read the frame
// back from the ST7796 (MISO is wired, TFT_eSPI can read) and send it as
// RLE runs — flat UI areas compress ~20×, a full screen is a few KB.
void Display::readRow(int y, uint16_t* row) { tft.readRect(0, y, LCD_W, 1, row); }

void Display::screenshot(Print& out) {
  static uint16_t row[LCD_W];
  out.printf("SHOT %d %d\n", LCD_W, LCD_H);
  for (int y = 0; y < LCD_H; y++) {
    if ((y & 31) == 0) feedLoopWDT();           // a screenshot over 115200 baud takes many seconds
    tft.readRect(0, y, LCD_W, 1, row);
    int x = 0;
    while (x < LCD_W) {
      const uint16_t c = row[x];
      int n = 1;
      while (x + n < LCD_W && row[x + n] == c && n < 255) n++;
      out.write((uint8_t)n);
      out.write((uint8_t)(c >> 8));
      out.write((uint8_t)(c & 0xFF));
      x += n;
    }
  }
  out.print("END\n");
}
