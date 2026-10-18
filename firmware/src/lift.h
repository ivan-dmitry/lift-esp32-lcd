// Lift core: decode CAN frames through the protocol map → lift state, build
// command frames, event logging, arrival chime, simulator.
#pragma once
#include <Arduino.h>
#include "config.h"
#include "drivers.h"

struct LiftMap;

struct LiftState {
  bool     seen;            // at least one frame of a mapped signal has arrived
  bool     linkOk;          // still hearing frames within timeoutSec
  int16_t  floorIdx;        // floor index 0..floors-1, -1 = unknown / out of range
  char     label[LABEL_LEN];// what to show: "B1", "12", "?"
  int32_t  rawFloor;        // raw bus value (INT32_MIN = none yet) — for finding the offset
  uint8_t  dir;             // 0 stopped · 1 up · 2 down
  bool     moving, door, fire, inspect, overload, oos;
  uint8_t  fault;           // 0 = no fault
  int16_t  load;            // %, -1 = no load signal
  uint64_t callCar, callUp, callDn;   // bit i = a call is registered at floor i
  uint32_t lastAt;          // millis() of the last frame matching a mapped signal
  uint32_t gen;             // bumps whenever anything changes — UI and MQTT watch it
};

enum CmdResult : uint8_t {
  RES_OK = 0,
  RES_NOT_MAPPED,     // no frame defined for this command
  RES_TX_OFF,         // "send commands" switch is off
  RES_LISTEN,         // CAN is listen-only (or still listening first, or fell back)
  RES_REMOTE_OFF,     // web/HA command while the remote-commands switch is off
  RES_MODE,           // lift in fire service / inspection / fault / no signal
  RES_BAD_FLOOR,
  RES_RATE,           // two commands too close together
  RES_BUS,            // driver would not take the frame (TX queue full, bus-off…)
  RES_MOVING,         // door command while the car is moving
  RES_COUNT
};

namespace Lift {
  void begin();
  void loop();
  void onFrame(const CanFrame& f);     // Can calls this for EVERY received frame

  const LiftState& st();
  LiftMode    mode();
  const char* modeText(uint8_t m);     // "NORMAL"…
  const char* modeId(uint8_t m);       // "normal"… — machine value for MQTT

  // Last raw value of a signal (for the PROTOCOL screen)
  bool sigValue(uint8_t s, int32_t& v, uint32_t& ageMs);

  // Commands to the lift. floor = floor index (0 = lowest), ignored for door commands.
  CmdResult command(uint8_t cmd, int floor, uint8_t src);
  bool        cmdAllowed(uint8_t src, CmdResult* why = nullptr);   // UI greys out buttons
  const char* resultText(uint8_t r);
  const char* lastCmdText();           // "Car call 5 · panel · sent"
  uint32_t    lastCmdAt();

  // Templates
  void presetCia417(LiftMap& m);       // CiA 417 CANopen-Lift (the default) — built-in decoder, empty signal map
  void presetEvl(LiftMap& m);          // the project's own EVL-CAN protocol (README §EVL-CAN)
  void presetFuji(LiftMap& m);         // Fuji hall-call bus, 27.78 kbit/s — decoded on site (README §Fuji)
  void presetClear(LiftMap& m);
  const char* protoText();             // "CiA 417 · lift 1", "EVL-CAN", "custom map"…

  // CiA 417 / CANopen: what the decoder has heard (PROTOCOL screen, web, console)
  struct CoInfo {
    uint32_t hb, rec, req, mpdo, drive, emcy;   // frames counted per kind
    uint8_t  nodes;                             // nodes with a heartbeat in the last 5 s
    char     nodeList[48];                      // "1 op · 2 op · 10 pre"
    uint8_t  lastRec[6];                        // last call-controller record of our lift
    uint32_t lastRecAge;                        // ms, UINT32_MAX = none yet
    int32_t  velocity;                          // drive: velocity actual value (INT32_MIN = none)
    uint32_t position;                          // car position unit: position value
    bool     hasPos;
    uint16_t emcyCode; uint8_t emcyNode;        // last emergency message
  };
  void coInfo(CoInfo& o);
  uint16_t coCallId();                         // CAN-ID our call requests go out on (0x480 + node)

  // Simulator: a virtual lift that emits proper EVL-CAN frames into the decoder
  // and answers the panel's own calls. For trying the UI, MQTT and Home
  // Assistant before a real lift is available. While simulating the CAN driver
  // is DETACHED FROM THE BUS.
  void simStart();
  void simStop();
  bool simOn();
  void simSet(char what, int v);       // 'f' fire · 'i' inspection · 'o' overload · 's' out of service · 'e' fault code
}
