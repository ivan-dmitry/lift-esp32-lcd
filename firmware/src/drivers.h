// Shared declarations for the hardware layer
#pragma once
#include <Arduino.h>
#include <lvgl.h>
#include "config.h"

// ═══════════ Display + touch (carried over from esp32Fire) ═══════════
namespace Display {
  void begin();                       // TFT + LVGL + touch + backlight
  void loop();                        // lv_timer_handler + idle dimming
  void setBrightness(uint8_t pct);
  uint8_t brightness();
  void setDimLevel(uint8_t pct);      // brightness when idle, 0 = backlight off
  void wake(bool force = false);      // full brightness again, restart the idle timer
  void sleepNow();
  bool isAsleep();
  uint8_t duty();
  uint32_t idleMs();
  uint32_t nPress();
  void setSleepTimeout(uint32_t ms);  // 0 = never dim
  void lockSleep(bool locked);
  bool touchCalibrated();
  void setTouchEnabled(bool on);      // off = every touch is ignored (no press, no wake-up)
  bool touchEnabled();
  bool touchRaw(uint16_t& x, uint16_t& y);
  bool touchPoint(uint16_t& x, uint16_t& y);
  // 4 points, stored in NVS. The time budget covers ALL FOUR points, not each.
  bool runTouchCalibration(uint32_t limitMs = 60000);
  // Read the panel back over SPI and stream it RLE-compressed (serial "shot").
  // Format: "SHOT w h\n", then per row runs of [count 1..255][RGB565 hi][lo], then "END\n".
  void screenshot(Print& out);
  void readRow(int y, uint16_t* row);  // one panel row read back (480 px, panel byte order)
}

// ═══════════ On-board RGB LED + speaker ═══════════
namespace Out {
  void begin();
  void loop();
  enum LedMode : uint8_t { LED_OFF, LED_OK, LED_NOLINK, LED_FAULT, LED_FIRE, LED_SIM };
  void ledMode(LedMode m);
  void setLedEnabled(bool on);       // false = the RGB LED stays dark whatever the mode
  bool ledEnabled();
  void ledSolid(bool r, bool g, bool b);
  void setVolume(uint8_t v1to10);
  void beep(uint16_t hz, uint16_t ms);
  // Arrival chime, following the hall-lantern convention: one stroke = the car
  // will go UP, two strokes = it will go DOWN. Waiting passengers know without
  // looking.
  void chime(uint8_t strokes);
  bool busy();
}

// ═══════════ Audio announcements: WAV/PCM over HTTP → I2S built-in DAC on GPIO 26 ═══════════
namespace Audio {
  void begin();                        // starts the port-81 server task (needs the network up)
  bool busy();
  void test();                         // speaker check: chime + arpeggio, no network needed
  void stop();                         // stop what is playing (any task)
  bool playUrl(const char* url, const char* opts = nullptr);   // queue "fetch and play"; replaces the current one
  const char* lastText();
  uint32_t count();
}

// ═══════════ Clock: system time, set by NTP or by hand ═══════════
namespace Clock {
  bool valid();                        // false = time unknown, log entries get a '?'
  void now(struct tm& t);
  void set(const struct tm& t);
  uint32_t epoch();                    // 0 if the time is unknown
  const char* stamp();                 // "Sat 12/09/2026 09:41"
  const char* timeOnly();              // "09:41"
}

// ═══════════ CAN bus (TWAI) ═══════════
// The whole driver runs on the main loop, NO separate task: the TWAI driver
// receives frames in its ISR and queues them (256 slots); the loop just drains
// the queue. At 125 kbit/s a 100 % loaded bus is only ~960 frames/s, so 256
// slots are ~260 ms of buffer — more than the slowest screen rebuild. One
// thread means no locks.
struct CanFrame {
  uint32_t id;
  bool     ext;          // 29-bit
  uint8_t  dlc;
  uint8_t  d[8];
};

namespace Can {
  void begin(uint16_t kbps, uint8_t mode);
  void loop();
  void setBitrate(uint16_t kbps);
  void setMode(uint8_t mode);          // CAN_LISTEN / CAN_NORMAL
  uint16_t bitrate();
  uint8_t  modeWanted();               // what the user asked for
  uint8_t  modeActive();               // what is really running (may still be listening first)
  bool     arming();                   // ACK requested, still listening for clean frames
  bool     fellBack();                 // dropped back to listen-only because of bus errors
  void     clearFallback();
  const char* stateText();

  struct Stats {
    uint32_t rx, tx, txFail, busErr, rxMissed, arbLost;
    uint16_t fps;                      // frames received per second
    uint8_t  tec, rec;                 // transmit / receive error counters
    uint8_t  state;                    // twai_state_t
  };
  void stats(Stats& s);

  // Transmit one frame. Only while really running CAN_NORMAL; in simulation the
  // frame goes to the simulator instead of the bus.
  bool send(const CanFrame& f);

  // Table of frames heard — source of the RAW FRAMES screen. Sorted by ID so
  // every row stays in place: a table that jumps around makes it impossible to
  // see which byte is changing.
  struct Seen {
    CanFrame f;
    uint32_t chgAt[8];                 // millis() when that byte last changed
    uint32_t count;
    uint32_t lastAt;
    uint16_t rate;                     // frames / second
    uint16_t win;                      // count in the current second
  };
  #define SEEN_MAX 48
  uint8_t      seenCount();
  const Seen*  seenAt(uint8_t i);
  int          seenFind(uint32_t id, bool ext);
  void         seenClear();
  uint16_t     seenGen();              // bumps when rows are added/removed — UI rebuilds then
  void         setPaused(bool p);
  bool         paused();

  // Bit-timing probe: time the pulses on the RX pin with the RMT peripheral
  // (0.1 µs) for `ms`, and work out the bit time — standard rate or not. Too few
  // edges → a second, slow pass (1 µs, 30 ms gaps, 1 s) for slow serial lines.
  struct Probe {
    uint16_t pulses;                   // pulses captured (0 = no edges at all)
    uint8_t  highPct;                  // share of the captured time RX was high (recessive)
    uint32_t minTicks;                 // shortest pulse, 0.1 µs
    float    bitUs;                    // estimated bit time
    uint32_t rateBps;                  // estimated bit rate
    float    fit;                      // 0 = pulses are clean multiples of a bit; > 0.15 = not CAN-like
    uint16_t nearestKbps;              // closest supported rate code
    bool     nearestOk;                // within 3 %
    bool     slow;                     // result from the slow pass (1 µs ticks)
    uint16_t gapUs;                    // quiet gap that closed a capture in the pass used
    uint16_t edgesPerSec;
    uint8_t  maxRun;                   // longest level in bits (CAN: ≤ 6 inside frames, UART: up to 10)
    uint32_t uartBaud;                 // standard UART rate within 3 % of the bit rate, 0 = none
    uint8_t  rawN;                     // first pulses, µs, with their level (for a human look)
    uint32_t rawUs[32];
    uint32_t rawHi;                    // bit i = pulse i was high
  };
  bool probe(Probe& r, uint32_t ms = 300);

  // Logic analyzer on the RX pin: edge times in CPU cycles from the start, for
  // `ms` (≤ 100). level0 = pin level at the start (1 = recessive).
  uint16_t rawCapture(uint32_t* edges, uint16_t maxEdges, uint32_t ms, uint8_t& level0, uint32_t& cyclesPerUs);
  // Error code captures since the last rate change, by ECC register value
  // (bits 7-6 type: bit/form/stuff/other · bit 5: 1 = while receiving · 4-0: frame segment)
  uint32_t eccCounts(uint32_t* out256);
  bool     rateOk(uint16_t code);      // a table rate, or a measured one (code > 1000) with a timing

  // Bit-rate detection: step through every rate in LISTEN-ONLY mode (no ACK, no
  // error frames — harmless even at the wrong rate) and count VALID frames.
  void     autoBaudStart();
  bool     autoBauding();
  uint8_t  autoBaudStep();             // 0..CAN_KBPS_N
  uint16_t autoBaudResult();           // 0 = nothing heard
  uint32_t autoBaudFrames(uint8_t i);

  // Feed a frame as if it had just been received — used by the simulator and the serial console
  void inject(const CanFrame& f);
  void setSimSink(void (*sink)(const CanFrame&));   // where transmitted frames go while simulating
}

// ═══════════ Live CAN stream for the web debug page ═══════════
class WiFiClient;
namespace CanDebug {
  void begin();                        // port-82 SSE server task (opens once a network exists)
  void onFrame(const CanFrame& f, bool tx);   // driver hook; free while nobody watches
  bool adopt(WiFiClient& c);           // take over a port-80 request as an SSE stream
  uint8_t  clients();
  uint32_t dropped();
}

// ═══════════ Network: WiFi + web config ═══════════
namespace Net {
  void begin();
  void loop();
  bool on();
  bool connected();                    // has a network (joined as STA, or running its own AP)
  bool staConnected();                 // joined the site WiFi — MQTT only runs then
  bool apMode();
  const char* ssid();
  int  rssi();
  String ip();
  void setEnabled(bool on);
  void setCreds(const char* ssid, const char* pass);
  void scanStart();
  int  scanState();                    // −1 scanning · −2 not scanned · ≥0 networks found
  bool scanInfo(int i, char* ssid, size_t cap, int& rssi, bool& lock);
  void scanClear();
  bool timeSynced();
  const char* timeSource();
}

// ═══════════ MQTT + Home Assistant ═══════════
namespace Mqtt {
  void begin();
  void loop();
  bool on();
  bool connected();
  void setEnabled(bool on);
  void setServer(const char* host, uint16_t port);
  void setAuth(const char* user, const char* pass);
  const char* host();
  uint16_t    port();
  const char* user();
  bool        hasPass();
  const char* baseTopic();             // "elevator/a1b2c3"
  const char* lastError();
  uint32_t    sentCount();
  void        rediscover();            // resend discovery (floor names changed, remote commands toggled)
}

// ───── Weather (drv_weather.cpp): Open-Meteo, in its own task ─────
namespace Weather {
  struct Now {
    bool     valid;                    // false: none yet, older than an hour, or switched off
    float    tempC;
    int16_t  code;                     // WMO weather code (0 clear … 95 thunderstorm)
    bool     isDay;
    uint32_t at;                       // millis() of the reading
    uint32_t gen;                      // bumps on every change
  };
  struct Cfg { bool on; float lat, lon; };   // lat = lon = 0: locate from the internet address
  void begin();
  Now  now();
  Cfg  cfg();
  void setCfg(const Cfg& c);
  void city(char* out, size_t n);      // city found by the automatic location ("" if none)
}
