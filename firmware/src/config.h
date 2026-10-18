// Pin map and system-wide constants — CAN bus elevator display
#pragma once
#include <Arduino.h>

#define FW_VERSION      "1.0.0"

// ───────── ST7796 display (TFT_eSPI takes its pins from platformio.ini) ─────────
#define PIN_BACKLIGHT   27          // backlight PWM
#define LCD_W           480
#define LCD_H           320
#define LCD_ROTATION    1           // landscape, ribbon cable on the left

// ───────── XPT2046 resistive touch ─────────
#define PIN_TOUCH_IRQ   36          // input only

// ───────── CAN bus — the ESP32's built-in TWAI controller ─────────
// The ESP32 has a CAN 2.0B controller on chip (Espressif calls it TWAI); only
// the physical transceiver is missing. Wired on connector CN1 (3V3 · GND · 21 · 22),
// nothing soldered onto the board:
//   GPIO 21 → transceiver CTX / D  (driver input)
//   GPIO 22 ← transceiver CRX / R  (receiver output, push-pull)
// The transceiver MUST be a 3.3 V part (SN65HVD230 "VP230", or an isolated one
// such as TD321DCANH / ADM3053). A TJA1050 runs at 5 V and drives 5 V into the
// ESP32's RX pin.
#define PIN_CAN_TX      21
#define PIN_CAN_RX      22
#define CAN_RXQ_LEN    256          // driver receive queue — see drv_can.cpp
#define CAN_TXQ_LEN      8

// ───────── Audio announcements (drv_audio.cpp) ─────────
#define AUDIO_PORT      81          // own server + own task: streaming never blocks the main loop
#define CANDBG_PORT     82          // live CAN stream for the /debug page (drv_candebug.cpp)

// ───────── The last free pin on P3 ─────────
#define PIN_SPARE       35          // unused, input only — reserved for a dry-contact input (e.g. fire signal)

// ───────── On-board LED / speaker / light sensor ─────────
#define PIN_LED_R        4          // RGB LED: ACTIVE LOW
#define PIN_LED_G       16
#define PIN_LED_B       17
#define PIN_SPEAKER     26          // DAC2 → FM8002A amplifier
#define PIN_LDR         34          // light sensor, input only

#define LEDC_CH_BL       0          // backlight PWM channel
#define LEDC_CH_SND      2          // speaker PWM channel
#define LEDC_SND_BITS   10          // speaker duty resolution

// ───────── Scale ─────────
#define MAX_FLOORS      64          // fits one 64-bit call bitmap
#define LABEL_LEN        4          // floor label: up to 3 characters + NUL ("B2", "12A")
#define NAME_LEN        24

// ───────── Behaviour ─────────
#define IDLE_HOME_MS        60000UL       // away from the main screen 60 s untouched → back home
#define CMD_MIN_GAP_MS        300UL       // minimum gap between two commands sent to the lift
#define STABLE_LOG_MS        2000UL       // a state must hold this long before it is logged
#define NORMAL_ARM_FRAMES       20        // clean frames that must be heard before enabling ACK
#define FALLBACK_ERR_PER_S      20        // bus errors/s in ACK mode → fall back to listen-only
#define FALLBACK_SECONDS         3

// ───────── Limits of every setting ─────────
// Two places can change settings — the panel (ui.cpp) and the web page
// (drv_net.cpp) — so every bound lives HERE, once. Two hand-copied sets of
// limits drift apart sooner or later.
#define VOL_MIN            1
#define VOL_MAX           10
#define BRIGHT_MIN        10        // %, never 0: a black screen looks broken
#define BRIGHT_MAX       100
#define BRIGHT_STEP       10
#define DIMPCT_MIN         0        // % when idle, 0 = backlight off
#define DIMPCT_MAX        60
#define DIMPCT_STEP       10
#define DIMSEC_MAX       600        // seconds, 0 = never dim
#define DIMSEC_STEP       30
#define FLOORS_MIN         2
#define TIMEOUT_MIN        1        // seconds without a status frame → NO SIGNAL
#define TIMEOUT_MAX       30
#define OFFSET_MIN       -20        // bus value of the lowest floor
#define OFFSET_MAX        40

// CAN bit rates. Hall-call (landing) buses run slow because of their long
// cables, so detection starts at the bottom. 12.5, 16 and 20 kbit/s need a
// prescaler above 128: ESP32 rev 2+ has a hardware divide-by-2 bit (BRP_DIV)
// that the prebuilt Arduino core never sets, so drv_can.cpp sets it itself.
// 10 kbit/s would need 320 and stays out of reach.
//
// Values are kbit/s CODES: 33, 47, 62, 66, 83 and 666 stand for 33.33, 47.62,
// 62.5, 66.67, 83.33 and 666.7 kbit/s — rates lift controllers use that are not
// whole kbit/s. Every entry has an exact timing in drv_can.cpp (BRP even, ≤ 128).
// 2778 = 27.78 kbit/s (a measured-rate code, see below): the Fuji hall-call bus.
static const uint16_t CAN_KBPS[] = {12, 16, 20, 25, 2778, 33, 40, 47, 50, 62, 66, 80, 83, 100, 125, 200, 250, 400, 500, 666, 800, 1000};
#define CAN_KBPS_N (sizeof(CAN_KBPS) / sizeof(CAN_KBPS[0]))
// Codes ABOVE 1000 are a measured, non-standard rate: bit/s ÷ 10 (2778 = 27 780 bit/s).
// Some lift controllers run odd rates (one Fuji hall bus measured 27.8 kbit/s);
// their timing is computed in drv_can.cpp.
#define CAN_CUSTOM_MIN 1001
#define CAN_DEFAULT_KBPS 2778   // 27.78 kbit/s — measured on the Fuji lift's hall-call bus
static inline uint32_t canRateBps(uint16_t k) {
  if (k >= CAN_CUSTOM_MIN) return (uint32_t)k * 10;
  switch (k) {
    case 12:  return 12500;
    case 33:  return 33333;  case 47: return 47619; case 62: return 62500;
    case 66:  return 66667;  case 83: return 83333; case 666: return 666667;
    default:  return (uint32_t)k * 1000;
  }
}
// "62.5" — for screens and the web page
static inline const char* canRateLabel(uint16_t k) {
  static char b[12];
  const uint32_t r = canRateBps(k);
  if (k >= CAN_CUSTOM_MIN) snprintf(b, sizeof(b), "%lu.%02lu", (unsigned long)(r / 1000), (unsigned long)((r % 1000 + 5) / 10));
  else if (r % 1000 == 0) snprintf(b, sizeof(b), "%lu", (unsigned long)(r / 1000));
  else snprintf(b, sizeof(b), "%lu.%lu", (unsigned long)(r / 1000), (unsigned long)((r % 1000 + 50) / 100));
  return b;
}

enum CanMode : uint8_t {
  CAN_LISTEN = 0,   // listen-only: no ACK, no error frames, never transmits — invisible on the bus
  CAN_NORMAL = 1,   // acknowledges frames, may send commands
};

// ───────── Signals read from the bus ─────────
// Each signal is "which ID · which byte · which mask". This order is stored in
// NVS — add new signals at the END and bump MAP_VER.
enum SigId : uint8_t {
  SIG_FLOOR = 0,    // car position (byte)
  SIG_UP,           // up arrow (bit)
  SIG_DOWN,         // down arrow (bit)
  SIG_MOVING,       // running (bit)
  SIG_DOOR,         // door open (bit)
  SIG_FIRE,         // firefighter / fire recall mode (bit)
  SIG_INSPECT,      // inspection / maintenance (bit)
  SIG_OVERLOAD,     // overload (bit)
  SIG_OOS,          // out of service (bit)
  SIG_FAULT,        // fault code, 0 = no fault (byte)
  SIG_LOAD,         // load % (byte)
  SIG_CALL_CAR,     // car call bitmap (from the given byte to the end of the frame)
  SIG_CALL_UP,      // hall call up bitmap
  SIG_CALL_DN,      // hall call down bitmap
  SIG_COUNT
};
extern const char* const SIG_NAME[SIG_COUNT];
static inline bool sigIsBit(uint8_t s) { return s >= SIG_UP && s <= SIG_OOS; }
static inline bool sigIsBitmap(uint8_t s) { return s >= SIG_CALL_CAR && s <= SIG_CALL_DN; }

// How the floor value is encoded on the bus
enum FloorEnc : uint8_t {
  FENC_BIN = 0,     // binary: 0x0C = 12
  FENC_BCD,         // BCD: 0x12 = 12
  FENC_ASCII,       // two display characters: 'B','1' — many controllers send exactly what to show
  FENC_COUNT
};
extern const char* const FENC_NAME[FENC_COUNT];

// ───────── Commands sent to the lift ─────────
enum CmdId : uint8_t {
  CMD_CAR = 0,      // car call
  CMD_HALL_UP,      // hall call, up
  CMD_HALL_DN,      // hall call, down
  CMD_DOOR_OPEN,    // door open / hold
  CMD_DOOR_CLOSE,   // door close
  CMD_DOOR_HOLD,    // hold the door open (the open button held)
  CMD_COUNT
};
extern const char* const CMD_NAME[CMD_COUNT];

// How the floor number is placed into a command frame
enum CmdEnc : uint8_t {
  CENC_NONE = 0,    // no floor (door commands)
  CENC_VALUE,       // one byte = floor index + offset
  CENC_BITMAP,      // set bit (floor index + offset), counted from the given byte
  CENC_COUNT
};
extern const char* const CENC_NAME[CENC_COUNT];

// Who issued a command — logged, and remote sources need an extra switch
enum CmdSrc : uint8_t { SRC_PANEL = 0, SRC_WEB, SRC_MQTT, SRC_SERIAL, SRC_COUNT };
extern const char* const SRC_NAME[SRC_COUNT];

// ───────── Lift mode, highest priority first ─────────
enum LiftMode : uint8_t {
  LM_NOLINK = 0,    // not hearing the lift — every other value is stale
  LM_FIRE,          // fire service
  LM_FAULT,         // fault code present
  LM_INSPECT,       // inspection
  LM_OOS,           // out of service
  LM_OVERLOAD,      // overload
  LM_NORMAL,
  LM_COUNT
};

// ───────── Timestamp comparison that survives millis() wrap ─────────
// millis() wraps after 49.7 days. Subtract as unsigned, compare as signed:
// correct for any distance below 24 days.
static inline bool timeReached(uint32_t now, uint32_t deadline) {
  return (int32_t)(now - deadline) >= 0;
}
static inline uint32_t timeLeft(uint32_t now, uint32_t deadline) {
  return timeReached(now, deadline) ? 0u : (deadline - now);
}
