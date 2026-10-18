// Persistent storage: settings, protocol map, floor labels, event log
#pragma once
#include <Arduino.h>
#include "config.h"

// Bump this EVERY time struct Settings changes. Comparing sizeof is not enough:
// two new uint8_t fields can fit into old padding, sizeof stays the same, and
// old data is silently loaded into the new fields (bitten by this in esp32Fire).
#define CFG_MAGIC    0xE1A5
#define CFG_MAGIC_V1 0xE1A4     // the layout before touchOff — carried over, not reset (store.cpp)

struct Settings {
  uint16_t magic;
  uint8_t  brightness;        // %
  uint16_t dimSec;            // sleep after this many idle seconds, 0 = never
  uint8_t  dimPct;            // brightness while asleep, 0 = backlight off
  bool     kiosk;             // DISPLAY ONLY: no footer buttons, hold 2.5 s to reach the menu
  uint8_t  volume;            // 1–10
  bool     chime;             // arrival chime
  bool     keyBeep;           // beep on touch
  uint16_t pin;               // technician code (4 digits)
  uint16_t canKbps;
  uint8_t  canMode;           // CanMode the user asked for
  bool     txEnable;          // allow sending commands to the lift
  bool     allowRemote;       // allow commands from the web page and Home Assistant
  uint8_t  timeoutSec;        // no status frame for this long → NO SIGNAL
  uint8_t  floors;            // floors served
  uint8_t  mainFloor;         // index of the main (fire recall) floor
  uint8_t  basements;         // number of basement levels — only used for auto naming
  uint8_t  groundStyle;       // ground floor is called: 0 "1" · 1 "G" · 2 "L" · 3 "0"
  char     liftName[NAME_LEN];// "LIFT A" — status bar and device name in Home Assistant
  bool     statusLed;         // on-board RGB status LED (default off: it glows beside the screen)
  bool     audioApi;          // accept HTTP / MQTT audio announcements
  bool     touchOff;          // touch screen disabled: the panel ignores every touch
                              // (turned back on from the web page /cfg or the USB console)
};

// ───── Protocol map: which signal lives at which ID and byte ─────
#define MAP_USED 0x01
#define MAP_EXT  0x02     // 29-bit ID
#define MAP_INV  0x04     // inverted (bit = 0 means ACTIVE)
#define MAP_IDFLR 0x08    // command: CAN-ID + floor index (one ID per hall button board)
#define MAP_REL  0x10     // command: 250 ms later send the template again without the floor (button released)
#define MAP_MIRROR 0x20   // command: take the LAST RECEIVED frame of this ID, OR the template bits in, and
                          // send it right after each of the next <repeat> frames of that ID (a button held
                          // on a board that reports it in its own status frame — Fuji 07C door buttons)

struct SigMap {
  uint32_t id;
  uint8_t  flags;
  uint8_t  byte;          // 0–7
  uint8_t  mask;          // bit signals: which bit; byte signals: normally 0xFF
  uint8_t  enc;           // FloorEnc — SIG_FLOOR only
  int8_t   offset;        // FLOOR: bus value of the lowest floor · bitmaps: bit number of the lowest floor
};

struct CmdMap {
  uint32_t id;
  uint8_t  flags;
  uint8_t  dlc;
  uint8_t  data[8];       // frame template
  uint8_t  fbyte;         // byte that carries the floor
  uint8_t  fenc;          // CmdEnc
  int8_t   offset;        // value (or bit number) of the lowest floor
  uint8_t  repeat;        // send this many times back to back (1–3)
};

enum Preset : uint8_t { PRESET_NONE = 0, PRESET_EVL, PRESET_CUSTOM, PRESET_CIA417, PRESET_FUJI };

// Built-in decoders. PROTO_MAP: only the signal map below. PROTO_CIA417: the
// CANopen-Lift decoder in lift.cpp runs first, and mapped signals still apply on
// top of it (for a manufacturer's extra frames found with the debugger).
enum Proto : uint8_t { PROTO_MAP = 0, PROTO_CIA417, PROTO_COUNT };

struct LiftMap {
  SigMap  sig[SIG_COUNT];
  CmdMap  cmd[CMD_COUNT];
  uint8_t preset;         // display only: which template this map started from
  uint8_t proto;          // Proto
  uint8_t coLift;         // CiA 417: lift number 1–8 (which call controller / PDO block)
  uint8_t coNode;         // CiA 417: our node-ID when sending call requests (1–127)
  int8_t  coBase;         // CiA 417: bus floor number of the lowest floor
};
#define MAP_VER 3         // 3: door hold command + MAP_MIRROR (an older map is replaced by the default map)

// ───── Event log ─────
// Add new kinds at the END. The numbers are already stored in log entries in
// flash — inserting in the middle changes the meaning of every old entry.
enum EvKind : uint8_t {
  EV_BOOT = 0,
  EV_FAULT, EV_FAULT_CLR,
  EV_FIRE, EV_FIRE_CLR,
  EV_INSPECT, EV_INSPECT_CLR,
  EV_OOS, EV_OOS_CLR,
  EV_OVERLOAD, EV_OVERLOAD_CLR,
  EV_LINK_LOST, EV_LINK_OK,
  EV_BUSOFF,
  EV_FALLBACK,        // dropped back to listen-only because of bus errors
  EV_CMD,             // a command was sent to the lift
  EV_CMD_DENY,        // a command was refused
  EV_CONFIG,
  EV_LOGIN,
  EV_TIMESET,
  EV_RAW_TX,          // a raw frame was sent from the web debug page
  EV_OTA,             // firmware updated over the network
  EV_REARM,           // ACK mode armed again by itself after a fallback (val = minutes waited)
  EV_TXFAIL,          // repeated bus-off while sending → listen-only (transmit path broken?)
  EV_LOWMEM,          // free memory critically low → planned restart (val = KB free)
  EV_WIFI_AP,         // a = 1: network lost, own AP on (still retrying) · a = 0: network back, AP off
  EV_KIND_COUNT
};

struct Event {
  uint32_t at;        // epoch seconds (or millis/1000 while the time is unknown)
  uint8_t  kind;
  uint8_t  a;         // which command / refusal reason
  int16_t  val;       // fault code, floor index…
  uint8_t  src;       // CmdSrc
  bool     timeOk;    // false → timestamp not trustworthy, shown with '?'
};

#define LOG_CAP 200

namespace Store {
  void begin();
  void save();                          // schedule an NVS write — several changes, one write
  void saveMap();                       // protocol map changed
  void saveFloors();                    // floor labels changed
  void factoryReset();
  void flush();                         // write anything pending NOW (before a restart)

  Settings& cfg();
  LiftMap&  map();

  // Floor labels
  const char* label(uint8_t i);         // "?" if out of range
  void        setLabel(uint8_t i, const char* s);   // filters: only A–Z, 0–9, '-'
  int         findLabel(const char* s); // -1 if not found
  void        autoLabels();             // name floors from basement count + ground style

  // Event log
  void     logAdd(EvKind k, uint8_t a = 0, int16_t val = 0, uint8_t src = 0);
  void     logClear();
  uint16_t logCount();
  const Event& logAt(uint16_t i);       // 0 = newest
  const char*  logText(const Event& e, char* buf, size_t n);
  const char*  logStamp(const Event& e, char* buf, size_t n);
  void         logStampShort(const Event& e, char* day, size_t dn, char* hm, size_t hn);
  void exportCsv(Print& out);           // UTF-8 with BOM so Excel opens it cleanly
}

void storeTick();     // call from loop(): actually writes to flash when due
