// Lift core — decoding, commands, logging, chime, simulator
#include "lift.h"
#include "store.h"

const char* const SIG_NAME[SIG_COUNT] = {
  "Floor", "Up", "Down", "Moving", "Door open", "Fire service", "Inspection",
  "Overload", "Out of service", "Fault code", "Load %", "Car calls", "Hall calls up",
  "Hall calls down",
};
const char* const FENC_NAME[FENC_COUNT] = {"binary", "BCD", "2 ASCII chars"};
const char* const CMD_NAME[CMD_COUNT]   = {"Car call", "Hall call up", "Hall call down",
                                           "Door open", "Door close", "Door hold"};
const char* const CENC_NAME[CENC_COUNT] = {"no floor", "one byte", "bitmap"};
const char* const SRC_NAME[SRC_COUNT]   = {"panel", "web", "Home Assistant", "serial"};

static LiftState s_st;
static int32_t   s_sigVal[SIG_COUNT];
static uint32_t  s_sigAt[SIG_COUNT];
static bool      s_sigHas[SIG_COUNT];

static uint32_t  s_lastCmdAt = 0;
static char      s_lastCmd[72] = "";
static uint32_t  s_denyLogAt = 0;

// ═════════════════════════ Decoding ═════════════════════════
static void setLabelFromIdx() {
  if (s_st.floorIdx >= 0) snprintf(s_st.label, LABEL_LEN, "%s", Store::label((uint8_t)s_st.floorIdx));
  else                    snprintf(s_st.label, LABEL_LEN, "?");
}

static uint64_t readBitmap(const CanFrame& f, uint8_t from) {
  uint64_t v = 0;
  for (uint8_t i = from; i < f.dlc && i < 8; i++) v |= (uint64_t)f.d[i] << (8 * (i - from));
  return v;
}

// ═════════════════════════ CiA 417 (CANopen-Lift) ═════════════════════════
// What is decoded, and where each piece comes from (the CiA 417-3 PDO tables are
// not public):
//  • Heartbeat 0x700+node, 1 byte NMT state — CiA 301; CiA 417-2 makes it
//    mandatory for every device (1 s). The call/drive/door controller is node 1.
//  • Call controller of lift L → all output panels: CAN-ID 0x400+L-1, carrying the
//    virtual output mapping (6011h). Input panels → call controllers: 0x480+node,
//    virtual input mapping (6010h). Both are a 6-byte record:
//        B0 function · B1 sub-function · B2 lift bit mask · B3 panel (= floor)
//        B4 door (high nibble source, low nibble destination) · B5 data (bit 0 = on)
//    Function codes, CAN-IDs and layout as in Xyntos' Apache-2.0 CANoopEn stack.
//  • MPDO 0x500+node, destination address mode: object writes to the panels —
//    62B0h text position indication (ASCII), 6218h position indication.
//  • Car drive PDOs of lift 1 — from the WEG ADL300 CiA 417 manual: 0x182
//    controlword · mode · target velocity, 0x183 statusword · mode display ·
//    velocity actual, 0x18C position value. Other lifts: assumed 16 IDs further
//    each (the PDO numbers step by 16 per lift) — NOT verified.
// NOT verified: which byte of a position record (function 40h) holds the floor.
// The panel byte is used, the sub-function when the panel byte is FFh ("all").
// NOT decoded (IDs unknown): door status 6301h, load 6482h, special indications.
static struct {
  uint32_t hbAt[128];       // millis() of the last heartbeat per node, 0 = never
  uint8_t  hbSt[128];
  uint32_t hb, rec, req, mpdo, drive, emcy;
  uint8_t  lastRec[6];
  uint32_t lastRecAt;
  bool     dirSeen;         // direction records seen → they drive the arrow, not the velocity
  int32_t  vel;  bool hasVel;
  uint32_t pos;  bool hasPos;
  uint16_t emcyCode; uint8_t emcyNode;
} C;

// A mirror command in progress (MAP_MIRROR): send the board's own frame with our
// bits set, right after each of its next <left> frames
static struct { uint32_t id; bool ext; uint8_t bits[8]; uint16_t left; uint32_t until; } s_mir = {};

static CanFrame s_coRel;            // the "button released" record of the last call request
static uint32_t s_coRelAt = 0;

static int coIdx(int busFloor) {
  const int i = busFloor - Store::map().coBase;
  return (i >= 0 && i < Store::cfg().floors) ? i : -1;
}
static void coSetFloor(int busFloor) {
  s_st.rawFloor = busFloor;
  s_st.floorIdx = (int16_t)coIdx(busFloor);
  setLabelFromIdx();
}
static void coBit(uint64_t& v, int busFloor, bool on) {
  const int i = coIdx(busFloor);
  if (i < 0) return;
  const uint64_t b = (uint64_t)1 << i;
  v = on ? (v | b) : (v & ~b);
}
static uint32_t le32(const uint8_t* p) {
  return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}

// true = the frame belongs to the lift's CANopen network (counts as hearing the lift)
static bool coFrame(const CanFrame& f, bool& up, bool& dn) {
  if (f.ext) return false;
  const LiftMap& m = Store::map();
  const uint8_t  L = m.coLift;
  const uint32_t id = f.id, now = millis();

  if (id >= 0x701 && id <= 0x77F && f.dlc == 1) {          // heartbeat / boot-up
    const uint8_t s = f.d[0] & 0x7F;
    if (s != 0x00 && s != 0x04 && s != 0x05 && s != 0x7F) return false;
    C.hbAt[id - 0x700] = now ? now : 1;
    C.hbSt[id - 0x700] = s;
    C.hb++;
    return true;
  }
  if (id >= 0x081 && id <= 0x0FF && f.dlc == 8) {          // emergency: noted, not "hearing the lift"
    C.emcy++;
    C.emcyCode = (uint16_t)(f.d[0] | f.d[1] << 8);
    C.emcyNode = (uint8_t)(id - 0x080);
    return false;
  }
  if (id == 0x400u + L - 1 && f.dlc >= 6) {                 // our call controller → panels
    const uint8_t fn = f.d[0], sub = f.d[1], lifts = f.d[2], pan = f.d[3];
    const bool on = f.d[5] & 0x01;
    if (!fn || (lifts && !(lifts & (1u << (L - 1))))) return true;
    C.rec++;
    memcpy(C.lastRec, f.d, 6);
    C.lastRecAt = now;
    switch (fn) {
      case 0x40: if (on) coSetFloor(pan != 0xFF ? pan : sub); break;   // position indicator
      case 0x42:                                                       // direction indication
        C.dirSeen = true;
        if (sub & 0x01) up = on;
        if (sub & 0x02) dn = on;
        break;
      case 0x02:                                                       // hall call acknowledged
        if (sub & 0x01) coBit(s_st.callUp, pan, on);
        if (sub & 0x02) coBit(s_st.callDn, pan, on);
        break;
      case 0x05: coBit(s_st.callCar, pan, on); break;                  // car call acknowledged
    }
    return true;
  }
  if (id >= 0x481 && id <= 0x4FF && f.dlc >= 6) { C.req++; return true; }   // a panel's call request
  if (id >= 0x501 && id <= 0x57F && f.dlc == 8) {          // MPDO
    C.mpdo++;
    if (f.d[0] & 0x80) {                                   // destination address mode = object write
      const int rel = (int)(f.d[1] | f.d[2] << 8) - 0x800 * (L - 1);
      if (rel == 0x62B0) {                                 // text position indication
        char t[5] = {0};
        uint8_t n = 0;
        for (uint8_t k = 4; k < 8 && n < 4; k++)
          if (f.d[k] > ' ' && f.d[k] < 0x7F) t[n++] = (char)f.d[k];
        if (n) {
          s_st.rawFloor = (int32_t)le32(f.d + 4);
          s_st.floorIdx = (int16_t)Store::findLabel(t);
          if (s_st.floorIdx >= 0) setLabelFromIdx();
          else snprintf(s_st.label, LABEL_LEN, "%s", t);
        }
      } else if (rel == 0x6218) {                          // position indication (floor number)
        coSetFloor(f.d[4]);
      }
    }
    return true;
  }
  const uint32_t drv = 0x180u + 0x10u * (L - 1);
  if (id == drv + 3 && f.dlc == 8) {                       // drive: statusword · mode · – · velocity
    C.drive++;
    C.vel = (int32_t)le32(f.d + 4);
    C.hasVel = true;
    s_st.moving = C.vel != 0;
    if (!C.dirSeen) { up = C.vel > 0; dn = C.vel < 0; }
    return true;
  }
  if (id == drv + 2 && f.dlc == 8) { C.drive++; return true; }        // controller → drive
  if (id == drv + 0xC && f.dlc >= 4) {                     // car position unit: position value
    C.drive++;
    C.pos = le32(f.d);
    C.hasPos = true;
    return true;
  }
  return false;
}

uint16_t Lift::coCallId() { return (uint16_t)(0x480 + Store::map().coNode); }

void Lift::coInfo(CoInfo& o) {
  memset(&o, 0, sizeof(o));
  o.hb = C.hb; o.rec = C.rec; o.req = C.req; o.mpdo = C.mpdo; o.drive = C.drive; o.emcy = C.emcy;
  const uint32_t now = millis();
  size_t k = 0;
  for (uint8_t n = 1; n < 128; n++) {
    if (!C.hbAt[n] || now - C.hbAt[n] > 5000) continue;
    o.nodes++;
    const uint8_t s = C.hbSt[n];
    const char* t = s == 0x05 ? "op" : s == 0x7F ? "pre" : s == 0x04 ? "stop" : "boot";
    if (k < sizeof(o.nodeList) - 12)
      k += snprintf(o.nodeList + k, sizeof(o.nodeList) - k, "%s%u %s", k ? " · " : "", n, t);
  }
  memcpy(o.lastRec, C.lastRec, 6);
  o.lastRecAge = C.lastRecAt ? now - C.lastRecAt : UINT32_MAX;
  o.velocity = C.hasVel ? C.vel : INT32_MIN;
  o.position = C.pos; o.hasPos = C.hasPos;
  o.emcyCode = C.emcyCode; o.emcyNode = C.emcyNode;
}

void Lift::onFrame(const CanFrame& f) {
  if (s_mir.left && f.id == s_mir.id && f.ext == s_mir.ext) {
    // Stop at once if the car starts moving or the lift leaves normal service
    const LiftMode md = mode();
    if (s_st.moving || (md != LM_NORMAL && md != LM_OVERLOAD)) s_mir.left = 0;
    else {
      CanFrame o = f;
      for (uint8_t i = 0; i < 8; i++) o.d[i] |= s_mir.bits[i];
      if (Can::send(o)) s_mir.left--;
      else s_mir.left = 0;
    }
  }
  const LiftMap& m = Store::map();
  bool hit = false;
  // Snapshot a few fields so we can tell whether anything CHANGED
  const int16_t  fi0 = s_st.floorIdx;
  const uint8_t  dir0 = s_st.dir, fa0 = s_st.fault;
  const bool     mv0 = s_st.moving, dr0 = s_st.door, fr0 = s_st.fire, in0 = s_st.inspect,
                 ov0 = s_st.overload, os0 = s_st.oos;
  const int16_t  ld0 = s_st.load;
  const uint64_t cc0 = s_st.callCar, cu0 = s_st.callUp, cd0 = s_st.callDn;
  bool up = (s_st.dir == 1), dn = (s_st.dir == 2);
  const uint32_t now = millis();

  if (m.proto == PROTO_CIA417 && coFrame(f, up, dn)) hit = true;

  for (uint8_t s = 0; s < SIG_COUNT; s++) {
    const SigMap& g = m.sig[s];
    if (!(g.flags & MAP_USED)) continue;
    if (g.id != f.id || ((g.flags & MAP_EXT) != 0) != f.ext) continue;
    if (g.byte >= f.dlc) continue;              // frame shorter than the byte we need
    hit = true;
    const uint8_t raw = (uint8_t)(f.d[g.byte] & g.mask);
    int32_t v = raw;

    if (s == SIG_FLOOR) {
      int idx = -1;
      if (g.enc == FENC_ASCII) {
        // Many controllers send the two characters to display ('B','1' / ' ','5').
        // Take that label and look the floor index up in the label table.
        char t[3] = {0, 0, 0};
        uint8_t n = 0;
        for (uint8_t k = 0; k < 2 && g.byte + k < f.dlc; k++) {
          const char c = (char)f.d[g.byte + k];
          if (c > ' ' && c < 0x7F) t[n++] = c;
        }
        v = ((int32_t)f.d[g.byte] << 8) | (g.byte + 1 < f.dlc ? f.d[g.byte + 1] : 0);
        idx = n ? Store::findLabel(t) : -1;
        s_st.floorIdx = (int16_t)idx;
        if (idx >= 0) setLabelFromIdx();
        else          snprintf(s_st.label, LABEL_LEN, "%s", n ? t : "?");
      } else {
        if (g.enc == FENC_BCD) {
          // Broken BCD (nibble > 9) means unreadable — never show a made-up floor
          if ((raw >> 4) > 9 || (raw & 0x0F) > 9) v = -1000;
          else v = (raw >> 4) * 10 + (raw & 0x0F);
        }
        idx = (int)v - g.offset;
        s_st.floorIdx = (v > -1000 && idx >= 0 && idx < Store::cfg().floors) ? (int16_t)idx : -1;
        setLabelFromIdx();
      }
      s_st.rawFloor = v;
    } else if (sigIsBit(s)) {
      bool on = raw != 0;
      if (g.flags & MAP_INV) on = !on;
      v = on;
      switch (s) {
        case SIG_UP:       up = on; break;
        case SIG_DOWN:     dn = on; break;
        case SIG_MOVING:   s_st.moving = on; break;
        case SIG_DOOR:     s_st.door = on; break;
        case SIG_FIRE:     s_st.fire = on; break;
        case SIG_INSPECT:  s_st.inspect = on; break;
        case SIG_OVERLOAD: s_st.overload = on; break;
        case SIG_OOS:      s_st.oos = on; break;
      }
    } else if (s == SIG_FAULT) {
      s_st.fault = raw;
    } else if (s == SIG_LOAD) {
      s_st.load = raw;
    } else if (sigIsBitmap(s)) {
      // offset = bit number of the lowest floor. Negative means the lowest floor
      // sits "below" bit 0 — seen on controllers that number from a basement.
      uint64_t b = readBitmap(f, g.byte);
      if (g.offset > 0)      b >>= g.offset;
      else if (g.offset < 0) b <<= -g.offset;
      if (Store::cfg().floors < 64) b &= ((uint64_t)1 << Store::cfg().floors) - 1;
      if (s == SIG_CALL_CAR)     s_st.callCar = b;
      else if (s == SIG_CALL_UP) s_st.callUp  = b;
      else                       s_st.callDn  = b;
      v = (int32_t)(b & 0x7FFFFFFF);
    }
    s_sigVal[s] = v;
    s_sigAt[s]  = now;
    s_sigHas[s] = true;
  }
  if (!hit) return;

  s_st.dir = up ? 1 : (dn ? 2 : 0);
  s_st.lastAt = now;
  s_st.seen = true;
  if (fi0 != s_st.floorIdx || dir0 != s_st.dir || fa0 != s_st.fault || mv0 != s_st.moving ||
      dr0 != s_st.door || fr0 != s_st.fire || in0 != s_st.inspect || ov0 != s_st.overload ||
      os0 != s_st.oos || ld0 != s_st.load || cc0 != s_st.callCar || cu0 != s_st.callUp ||
      cd0 != s_st.callDn)
    s_st.gen++;
}

const LiftState& Lift::st() { return s_st; }

bool Lift::sigValue(uint8_t s, int32_t& v, uint32_t& age) {
  if (s >= SIG_COUNT || !s_sigHas[s]) return false;
  v = s_sigVal[s];
  age = millis() - s_sigAt[s];
  return true;
}

LiftMode Lift::mode() {
  if (!s_st.linkOk)   return LM_NOLINK;
  if (s_st.fire)      return LM_FIRE;
  if (s_st.fault)     return LM_FAULT;
  if (s_st.inspect)   return LM_INSPECT;
  if (s_st.oos)       return LM_OOS;
  if (s_st.overload)  return LM_OVERLOAD;
  return LM_NORMAL;
}

const char* Lift::modeText(uint8_t m) {
  static const char* const T[LM_COUNT] = {"NO SIGNAL", "FIRE SERVICE", "FAULT", "INSPECTION",
                                          "OUT OF SERVICE", "OVERLOAD", "NORMAL"};
  return m < LM_COUNT ? T[m] : "?";
}
const char* Lift::modeId(uint8_t m) {
  static const char* const T[LM_COUNT] = {"no_link", "fire", "fault", "inspection",
                                          "out_of_service", "overload", "normal"};
  return m < LM_COUNT ? T[m] : "unknown";
}

// ═════════════════════════ Commands ═════════════════════════
const char* Lift::resultText(uint8_t r) {
  static const char* const T[RES_COUNT] = {
    "sent", "no command frame defined", "sending commands is OFF",
    "CAN is LISTEN-ONLY", "remote commands are OFF", "lift not accepting commands now",
    "invalid floor", "too fast", "could not transmit on the bus", "car is moving",
  };
  return r < RES_COUNT ? T[r] : "?";
}

bool Lift::cmdAllowed(uint8_t src, CmdResult* why) {
  CmdResult r = RES_OK;
  if ((src == SRC_WEB || src == SRC_MQTT) && !Store::cfg().allowRemote) r = RES_REMOTE_OFF;
  else if (!simOn() && !Store::cfg().txEnable)                          r = RES_TX_OFF;
  else if (!simOn() && Can::modeActive() != CAN_NORMAL)                 r = RES_LISTEN;
  else {
    // Fire service, inspection, out of service, fault, no signal: send NO calls.
    // In fire service the car must return to the main floor and drop every
    // call; a call slipping in from Home Assistant is the last thing a
    // firefighter needs.
    const LiftMode m = mode();
    if (m != LM_NORMAL && m != LM_OVERLOAD) r = RES_MODE;
  }
  if (why) *why = r;
  return r == RES_OK;
}

CmdResult Lift::command(uint8_t cmd, int floor, uint8_t src) {
  CmdResult r = RES_OK;
  const uint32_t now = millis();
  const Settings& c = Store::cfg();
  const LiftMap&  m = Store::map();
  // CiA 417: calls are built-in (a call request record, as a panel button sends
  // it). Door commands have no panel record we know — they need the map.
  const bool co = m.proto == PROTO_CIA417 && cmd <= CMD_HALL_DN;

  if (cmd >= CMD_COUNT) r = RES_NOT_MAPPED;
  else if (!cmdAllowed(src, &r)) {}
  else if (!co && !(m.cmd[cmd].flags & MAP_USED)) r = RES_NOT_MAPPED;
  else if (cmd <= CMD_HALL_DN && (floor < 0 || floor >= c.floors)) r = RES_BAD_FLOOR;
  else if (cmd == CMD_HALL_UP && floor == c.floors - 1) r = RES_BAD_FLOOR;   // top floor has no up button
  else if (cmd == CMD_HALL_DN && floor == 0)            r = RES_BAD_FLOOR;
  else if (s_lastCmdAt && now - s_lastCmdAt < CMD_MIN_GAP_MS) r = RES_RATE;
  else if (co && (floor + m.coBase < 0 || floor + m.coBase > 254)) r = RES_BAD_FLOOR;
  else if (cmd >= CMD_DOOR_OPEN && s_st.moving) r = RES_MOVING;

  if (r == RES_OK && co) {
    // Pressed now, released 250 ms later — what a real button sends. The call
    // controller answers with the acknowledgement on 0x400+lift-1, which the
    // decoder above turns into the lit call.
    CanFrame f = {};
    f.id   = coCallId();
    f.dlc  = 6;
    f.d[0] = (cmd == CMD_CAR) ? 0x05 : 0x02;
    f.d[1] = (cmd == CMD_CAR) ? 0x00 : (cmd == CMD_HALL_UP ? 0x01 : 0x02);
    f.d[2] = (uint8_t)(1u << (m.coLift - 1));
    f.d[3] = (uint8_t)(floor + m.coBase);
    f.d[4] = 0x11;                              // door A → door A
    f.d[5] = 0x01;                              // on = pressed
    if (!Can::send(f)) r = RES_BUS;
    else {
      s_coRel = f;
      s_coRel.d[5] = 0x00;
      s_coRelAt = now + 250;
    }
  } else if (r == RES_OK && (m.cmd[cmd].flags & MAP_MIRROR)) {
    // Sent with the next frames of that ID (see onFrame); gives up if none comes
    const CmdMap& t = m.cmd[cmd];
    s_mir.id = t.id;
    s_mir.ext = (t.flags & MAP_EXT) != 0;
    memcpy(s_mir.bits, t.data, 8);
    s_mir.left = t.repeat ? t.repeat : 1;
    s_mir.until = now + 1500UL + 400UL * s_mir.left;
  } else if (r == RES_OK) {
    const CmdMap& t = m.cmd[cmd];
    CanFrame f;
    f.id  = t.id + ((t.flags & MAP_IDFLR) && floor >= 0 ? (uint32_t)floor : 0);
    f.ext = (t.flags & MAP_EXT) != 0;
    f.dlc = t.dlc > 8 ? 8 : t.dlc;
    memcpy(f.d, t.data, 8);
    if (t.fenc == CENC_VALUE && t.fbyte < 8) {
      f.d[t.fbyte] = (uint8_t)(floor + t.offset);
    } else if (t.fenc == CENC_BITMAP) {
      const int bit = floor + t.offset;
      const int byte = t.fbyte + bit / 8;
      if (bit >= 0 && byte < 8) f.d[byte] |= (uint8_t)(1u << (bit % 8));
      else r = RES_BAD_FLOOR;
    }
    if (r == RES_OK) {
      const uint8_t rep = t.repeat < 1 ? 1 : (t.repeat > 3 ? 3 : t.repeat);
      for (uint8_t i = 0; i < rep; i++)
        if (!Can::send(f)) { r = RES_BUS; break; }
      if (r == RES_OK && (t.flags & MAP_REL)) {     // like a real button: released 250 ms later
        s_coRel = f;
        memcpy(s_coRel.d, t.data, 8);
        s_coRelAt = now + 250;
      }
    }
  }

  // Keep the result for the CALL screen and for MQTT
  if (cmd < CMD_COUNT) {
    if (cmd <= CMD_HALL_DN)
      snprintf(s_lastCmd, sizeof(s_lastCmd), "%s %s · %s · %s", CMD_NAME[cmd],
               (floor >= 0 && floor < MAX_FLOORS) ? Store::label((uint8_t)floor) : "?",
               SRC_NAME[src % SRC_COUNT], resultText(r));
    else
      snprintf(s_lastCmd, sizeof(s_lastCmd), "%s · %s · %s", CMD_NAME[cmd],
               SRC_NAME[src % SRC_COUNT], resultText(r));
  }
  if (r == RES_OK) {
    s_lastCmdAt = now;
    Store::logAdd(EV_CMD, cmd, (int16_t)floor, src);
  } else if (src == SRC_WEB || src == SRC_MQTT) {
    // Log refused remote commands — the trace of a broken automation, or of
    // someone probing. Rate-limited: a runaway automation firing hundreds of
    // commands must not wash the real log away.
    if (!s_denyLogAt || now - s_denyLogAt > 10000UL) {
      s_denyLogAt = now;
      Store::logAdd(EV_CMD_DENY, (uint8_t)r, (int16_t)floor, src);
    }
  }
  Serial.printf("Cmd  : %s\n", s_lastCmd);
  return r;
}

const char* Lift::lastCmdText() { return s_lastCmd; }
uint32_t    Lift::lastCmdAt()   { return s_lastCmdAt; }

// ═════════════════════════ Templates ═════════════════════════
// EVL-CAN: the project's own minimal protocol — for home-built controllers,
// protocol gateways and the simulator. Full spec in the README.
void Lift::presetClear(LiftMap& m) {
  memset(&m, 0, sizeof(m));
  for (uint8_t i = 0; i < SIG_COUNT; i++) m.sig[i].mask = 0xFF;
  for (uint8_t i = 0; i < CMD_COUNT; i++) { m.cmd[i].dlc = 8; m.cmd[i].repeat = 1; }
  m.preset = PRESET_NONE;
  m.proto  = PROTO_MAP;
  m.coLift = 1;
  m.coNode = 120;          // free in most CiA 417 systems (1 = controller, panels count up from 2)
  m.coBase = 1;
}

// CiA 417: the decoder is built in, the signal map starts empty — signals added to
// it (from the frame inspector) are applied on top, for a maker's own extra frames.
void Lift::presetCia417(LiftMap& m) {
  presetClear(m);
  m.proto  = PROTO_CIA417;
  m.preset = PRESET_CIA417;
}

// Fuji hall-call bus (27.78 kbit/s, 11-bit IDs, 8-byte frames), decoded at the lift
// from three trips (3→5 up, 5→1 down, 1→5 up) with every call type pressed:
//   030  B0 bit7 up · bit6 down · bit5 running · bit4 levelling · bit3 door CLOSED
//        B7 car position, lowest floor = 3 · B4 bit4 in the door zone
//   033  car calls · 034 hall calls up · 035 hall calls down (assumed: the down call
//        seen was answered at once) — bitmaps from B0 bit0 = lowest floor
//   037  B3 bit1 / bit0 = arrival lantern up / down
//   07D  car button pressed (bitmap) · 640+n / 6C0+n hall button up / down at floor n+1
//   07C  car-top board, 5/s: B0 bit1 door-open / bit0 door-close button (held while
//        pressed) · B1 bit7 door fully open · bit6 fully closed
// Not identified yet: fire service, inspection, faults, load.
void Lift::presetFuji(LiftMap& m) {
  presetClear(m);
  auto sig = [&](uint8_t s, uint32_t id, uint8_t byte, uint8_t mask, int8_t off, bool inv) {
    m.sig[s].id = id; m.sig[s].flags = (uint8_t)(MAP_USED | (inv ? MAP_INV : 0)); m.sig[s].byte = byte;
    m.sig[s].mask = mask; m.sig[s].enc = FENC_BIN; m.sig[s].offset = off;
  };
  sig(SIG_FLOOR,    0x030, 7, 0xFF, 3, false);
  sig(SIG_UP,       0x030, 0, 0x80, 0, false);
  sig(SIG_DOWN,     0x030, 0, 0x40, 0, false);
  sig(SIG_MOVING,   0x030, 0, 0x20, 0, false);
  sig(SIG_DOOR,     0x030, 0, 0x08, 0, true);     // the bit says "door closed"
  sig(SIG_CALL_CAR, 0x033, 0, 0xFF, 0, false);
  sig(SIG_CALL_UP,  0x034, 0, 0xFF, 0, false);
  sig(SIG_CALL_DN,  0x035, 0, 0xFF, 0, false);
  auto cmd = [&](uint8_t c, uint32_t id, uint8_t flags) {
    CmdMap& k = m.cmd[c];
    k.id = id; k.flags = (uint8_t)(MAP_USED | MAP_REL | flags); k.dlc = 8;
    memset(k.data, 0, 8);
    k.fbyte = 0; k.fenc = CENC_BITMAP; k.offset = 0; k.repeat = 1;
  };
  cmd(CMD_CAR,     0x07D, 0);                     // car panel: bit = floor
  cmd(CMD_HALL_UP, 0x640, MAP_IDFLR);             // hall board of that floor, up button
  cmd(CMD_HALL_DN, 0x6C0, MAP_IDFLR);             // hall board of that floor, down button
  // Door buttons: car-top frame 07C, B0 bit1 open / bit0 close, held while pressed.
  // Mirrored onto the board's own frames (5 a second): 2 frames = a short press,
  // 75 frames = the open button held 15 s.
  auto door = [&](uint8_t c, uint8_t bit, uint8_t frames) {
    CmdMap& k = m.cmd[c];
    k.id = 0x07C; k.flags = MAP_USED | MAP_MIRROR; k.dlc = 8;
    memset(k.data, 0, 8); k.data[0] = bit;
    k.fbyte = 0; k.fenc = CENC_NONE; k.offset = 0; k.repeat = frames;
  };
  door(CMD_DOOR_OPEN,  0x02, 2);
  door(CMD_DOOR_CLOSE, 0x01, 2);
  door(CMD_DOOR_HOLD,  0x02, 75);
  m.preset = PRESET_FUJI;
}

const char* Lift::protoText() {
  static char b[32];
  const LiftMap& m = Store::map();
  bool extra = false;
  for (uint8_t s = 0; s < SIG_COUNT; s++) if (m.sig[s].flags & MAP_USED) extra = true;
  if (m.proto == PROTO_CIA417) snprintf(b, sizeof(b), "CiA 417 · lift %u%s", m.coLift, extra ? " + map" : "");
  else snprintf(b, sizeof(b), "%s", m.preset == PRESET_FUJI ? "Fuji" : m.preset == PRESET_EVL ? "EVL-CAN"
                                    : extra ? "custom map" : "empty map");
  return b;
}

void Lift::presetEvl(LiftMap& m) {
  presetClear(m);
  auto sig = [&](uint8_t s, uint32_t id, uint8_t byte, uint8_t mask, int8_t off) {
    m.sig[s].id = id; m.sig[s].flags = MAP_USED; m.sig[s].byte = byte;
    m.sig[s].mask = mask; m.sig[s].enc = FENC_BIN; m.sig[s].offset = off;
  };
  sig(SIG_FLOOR,    0x100, 0, 0xFF, 1);   // lowest floor = 1 on the bus
  sig(SIG_UP,       0x100, 1, 0x01, 0);
  sig(SIG_DOWN,     0x100, 1, 0x02, 0);
  sig(SIG_MOVING,   0x100, 1, 0x04, 0);
  sig(SIG_DOOR,     0x100, 1, 0x08, 0);
  sig(SIG_FIRE,     0x100, 2, 0x01, 0);
  sig(SIG_INSPECT,  0x100, 2, 0x02, 0);
  sig(SIG_OVERLOAD, 0x100, 2, 0x04, 0);
  sig(SIG_OOS,      0x100, 2, 0x08, 0);
  sig(SIG_FAULT,    0x100, 3, 0xFF, 0);
  sig(SIG_LOAD,     0x100, 4, 0xFF, 0);
  sig(SIG_CALL_CAR, 0x101, 0, 0xFF, 0);
  sig(SIG_CALL_UP,  0x102, 0, 0xFF, 0);
  sig(SIG_CALL_DN,  0x103, 0, 0xFF, 0);

  auto cmd = [&](uint8_t c, uint8_t code, uint8_t enc) {
    CmdMap& k = m.cmd[c];
    k.id = 0x180; k.flags = MAP_USED; k.dlc = 2;
    memset(k.data, 0, 8);
    k.data[0] = code;
    k.fbyte = 1; k.fenc = enc; k.offset = 1; k.repeat = 1;
  };
  cmd(CMD_CAR,        1, CENC_VALUE);
  cmd(CMD_HALL_UP,    2, CENC_VALUE);
  cmd(CMD_HALL_DN,    3, CENC_VALUE);
  cmd(CMD_DOOR_OPEN,  4, CENC_NONE);
  cmd(CMD_DOOR_CLOSE, 5, CENC_NONE);
  m.preset = PRESET_EVL;
}

// ═════════════════════════ Simulator ═════════════════════════
static bool     s_sim = false;
static struct {
  int      floor;
  uint8_t  dir;            // direction being served
  bool     moving;
  uint8_t  door;           // 0 closed · 1 opening · 2 open · 3 closing
  uint32_t phaseAt;        // millis() when the current phase started
  uint64_t car, up, dn;
  bool     fire, insp, over, oos;
  uint8_t  fault;
  uint8_t  load;
  uint32_t nextTx, nextCall;
} S;

static void simSink(const CanFrame& f) {
  const int nF = Store::cfg().floors;
  // CiA 417 call request from a panel (0x480+node), button pressed
  if (Store::map().proto == PROTO_CIA417) {
    if (f.ext || f.id < 0x481 || f.id > 0x4FF || f.dlc < 6 || !(f.d[5] & 0x01)) return;
    const int fl = (int)f.d[3] - Store::map().coBase;
    if (fl < 0 || fl >= nF) return;
    const uint64_t b = (uint64_t)1 << fl;
    if (f.d[0] == 0x05) S.car |= b;
    if (f.d[0] == 0x02 && (f.d[1] & 0x01)) S.up |= b;
    if (f.d[0] == 0x02 && (f.d[1] & 0x02)) S.dn |= b;
    return;
  }
  // The virtual lift understands EVL-CAN commands: ID 0x180, byte0 = command, byte1 = floor (from 1)
  if (f.id != 0x180 || f.ext || f.dlc < 1) return;
  const int fl = (f.dlc >= 2) ? (int)f.d[1] - 1 : -1;
  switch (f.d[0]) {
    case 1: if (fl >= 0 && fl < nF) S.car |= (uint64_t)1 << fl; break;
    case 2: if (fl >= 0 && fl < nF) S.up  |= (uint64_t)1 << fl; break;
    case 3: if (fl >= 0 && fl < nF) S.dn  |= (uint64_t)1 << fl; break;
    case 4: if (!S.moving && S.door != 2) { S.door = 1; S.phaseAt = millis(); }
            else if (S.door == 2) S.phaseAt = millis();     // hold the door
            break;
    case 5: if (S.door == 2) { S.door = 3; S.phaseAt = millis(); } break;
  }
}

// CiA 417 flavour of the virtual lift: heartbeats of the controller (node 1) and
// the drive (node 2), the drive's status PDO with the velocity, and the call
// controller's records — position, direction, call acknowledgements — sent on
// change, the position also refreshed every second as real controllers do.
static struct { int floor; uint8_t dir; uint64_t car, up, dn; uint32_t hbAt, posAt; } SC;

static void coRec(uint8_t fn, uint8_t sub, int floorIdx, bool on) {
  const LiftMap& m = Store::map();
  CanFrame f = {};
  f.id = 0x400 + m.coLift - 1; f.dlc = 6;
  f.d[0] = fn; f.d[1] = sub; f.d[2] = (uint8_t)(1u << (m.coLift - 1));
  f.d[3] = (uint8_t)(floorIdx + m.coBase); f.d[4] = 0x11; f.d[5] = on ? 1 : 0;
  Can::inject(f);
}
static void coCalls(uint64_t now, uint64_t& was, uint8_t fn, uint8_t sub) {
  const uint64_t ch = now ^ was;
  for (int i = 0; i < 64; i++) if ((ch >> i) & 1) coRec(fn, sub, i, (now >> i) & 1);
  was = now;
}

static void simEmitCo() {
  const uint32_t t = millis();
  const LiftMap& m = Store::map();
  if (timeReached(t, SC.hbAt)) {
    SC.hbAt = t + 1000;
    for (uint8_t n = 1; n <= 2; n++) {
      CanFrame h = {};
      h.id = 0x700 + n; h.dlc = 1; h.d[0] = 0x05;      // operational
      Can::inject(h);
    }
  }
  CanFrame d = {};
  d.id = 0x183 + 0x10 * (m.coLift - 1); d.dlc = 8;
  d.d[0] = 0x37; d.d[1] = 0x06;                        // statusword: operation enabled
  d.d[2] = 0x03;                                       // profile velocity mode
  const int32_t v = S.moving ? (S.dir == 1 ? 1000 : -1000) : 0;
  for (uint8_t i = 0; i < 4; i++) d.d[4 + i] = (uint8_t)((uint32_t)v >> (8 * i));
  Can::inject(d);

  if (S.floor != SC.floor || timeReached(t, SC.posAt)) {
    if (SC.floor >= 0 && SC.floor != S.floor) coRec(0x40, 0, SC.floor, false);
    coRec(0x40, 0, S.floor, true);
    SC.floor = S.floor;
    SC.posAt = t + 1000;
  }
  if (S.dir != SC.dir) {
    coRec(0x42, 0x01, S.floor, S.dir == 1);
    coRec(0x42, 0x02, S.floor, S.dir == 2);
    SC.dir = S.dir;
  }
  coCalls(S.car, SC.car, 0x05, 0x00);
  coCalls(S.up,  SC.up,  0x02, 0x01);
  coCalls(S.dn,  SC.dn,  0x02, 0x02);
}

static void simEmit() {
  if (Store::map().proto == PROTO_CIA417) { simEmitCo(); return; }
  CanFrame f = {};
  f.id = 0x100; f.dlc = 8;
  f.d[0] = (uint8_t)(S.floor + 1);
  f.d[1] = (uint8_t)((S.dir == 1 ? 0x01 : 0) | (S.dir == 2 ? 0x02 : 0) |
                     (S.moving ? 0x04 : 0) | (S.door ? 0x08 : 0));
  f.d[2] = (uint8_t)((S.fire ? 1 : 0) | (S.insp ? 2 : 0) | (S.over ? 4 : 0) | (S.oos ? 8 : 0));
  f.d[3] = S.fault;
  f.d[4] = S.load;
  Can::inject(f);
  const uint64_t bm[3] = {S.car, S.up, S.dn};
  for (uint8_t k = 0; k < 3; k++) {
    CanFrame b = {};
    b.id = 0x101 + k; b.dlc = 8;
    for (uint8_t i = 0; i < 8; i++) b.d[i] = (uint8_t)(bm[k] >> (8 * i));
    Can::inject(b);
  }
}

static bool callsAbove(int f, uint64_t all) { return f + 1 < 64 && (all >> (f + 1)) != 0; }
static bool callsBelow(int f, uint64_t all) { return f > 0 && (all & (((uint64_t)1 << f) - 1)) != 0; }

static void simTick() {
  const uint32_t now = millis();
  const int nF = Store::cfg().floors;
  if (S.floor >= nF) S.floor = nF - 1;

  // Fire service: drop every call, return to the main floor, park with doors open — phase I
  if (S.fire) { S.car = S.up = S.dn = 0; }

  // Invent a hall call every 6–14 s so the demo has some life in it
  if (!S.fire && !S.insp && !S.oos && timeReached(now, S.nextCall)) {
    S.nextCall = now + 6000 + (esp_random() % 8000);
    const int fl = (int)(esp_random() % nF);
    if (fl != S.floor) {
      if (fl == nF - 1)  S.dn |= (uint64_t)1 << fl;
      else if (fl == 0)  S.up |= (uint64_t)1 << fl;
      else               ((esp_random() & 1) ? S.up : S.dn) |= (uint64_t)1 << fl;
    }
    S.load = (uint8_t)(10 + esp_random() % 60);
  }

  const uint64_t all = S.car | S.up | S.dn;
  const uint64_t here = (uint64_t)1 << S.floor;
  const int target = S.fire ? Store::cfg().mainFloor : -1;
  const bool frozen = S.insp || S.oos || S.fault;

  // ── Door ──
  if (S.door == 1 && now - S.phaseAt > 900)  { S.door = 2; S.phaseAt = now; }
  // Overload holds the door; fire service holds it only once at the main floor —
  // holding it anywhere else and the car never gets home.
  if (S.door == 2 && now - S.phaseAt > 3200 && !S.over && !(S.fire && S.floor == target)) {
    S.door = 3; S.phaseAt = now;
  }
  if (S.door == 3 && now - S.phaseAt > 900)  { S.door = 0; S.phaseAt = now; }

  if (S.moving) {
    if (frozen) { S.moving = false; return; }
    if (now - S.phaseAt >= 1600) {            // one floor travelled
      S.phaseAt = now;
      S.floor += (S.dir == 1) ? 1 : -1;
      if (S.floor < 0) S.floor = 0;
      if (S.floor >= nF) S.floor = nF - 1;
      const uint64_t h = (uint64_t)1 << S.floor;
      bool stop = (S.car & h) || S.floor == 0 || S.floor == nF - 1 || S.floor == target;
      if (S.dir == 1 && (S.up & h)) stop = true;
      if (S.dir == 2 && (S.dn & h)) stop = true;
      // Opposite-direction hall call at the end of the run: stop, then reverse
      if (!stop && (S.up & h || S.dn & h)) {
        const uint64_t rest = (S.car | S.up | S.dn) & ~h;
        if (S.dir == 1 && !callsAbove(S.floor, rest)) stop = true;
        if (S.dir == 2 && !callsBelow(S.floor, rest)) stop = true;
      }
      if (S.fire) stop = (S.floor == target);
      if (stop) {
        S.moving = false;
        S.car &= ~h;
        if (S.dir == 1) S.up &= ~h; else S.dn &= ~h;
        if (S.dir == 1 && !callsAbove(S.floor, S.car | S.up | S.dn)) { S.dn &= ~h; S.dir = callsBelow(S.floor, S.car | S.up | S.dn) ? 2 : 0; }
        if (S.dir == 2 && !callsBelow(S.floor, S.car | S.up | S.dn)) { S.up &= ~h; S.dir = callsAbove(S.floor, S.car | S.up | S.dn) ? 1 : 0; }
        S.door = 1; S.phaseAt = now;
      }
    }
    return;
  }

  // ── Stopped, door closed: pick the next job ──
  if (S.door != 0 || frozen) return;
  if (S.fire) {
    if (S.floor != target) { S.dir = S.floor < target ? 1 : 2; S.moving = true; S.phaseAt = now; }
    return;
  }
  if (S.over) return;
  if (all & here) {                             // a call right at this floor
    S.car &= ~here; S.up &= ~here; S.dn &= ~here;
    S.door = 1; S.phaseAt = now;
    return;
  }
  if (S.dir == 1 && callsAbove(S.floor, all))      S.moving = true;
  else if (S.dir == 2 && callsBelow(S.floor, all)) S.moving = true;
  else if (callsAbove(S.floor, all))               { S.dir = 1; S.moving = true; }
  else if (callsBelow(S.floor, all))               { S.dir = 2; S.moving = true; }
  else                                             S.dir = 0;
  if (S.moving) S.phaseAt = now;
}

void Lift::simStart() {
  if (s_sim) return;
  memset(&S, 0, sizeof(S));
  S.floor = Store::cfg().mainFloor;
  S.load = 20;
  S.nextCall = millis() + 2000;
  memset(&SC, 0, sizeof(SC));
  SC.floor = -1;
  SC.dir = 0xFF;
  s_sim = true;
  Can::setSimSink(simSink);       // from here on Can is detached from the real bus
  Serial.printf("Sim  : ON (CAN detached from the bus, emitting %s frames)\n",
                Store::map().proto == PROTO_CIA417 ? "CiA 417" : "EVL-CAN");
}

void Lift::simStop() {
  if (!s_sim) return;
  s_sim = false;
  Can::setSimSink(nullptr);
  Serial.println("Sim  : off");
}

bool Lift::simOn() { return s_sim; }

void Lift::simSet(char w, int v) {
  switch (w) {
    case 'f': S.fire = v; break;
    case 'i': S.insp = v; break;
    case 'o': S.over = v; break;
    case 's': S.oos  = v; break;
    case 'e': S.fault = (uint8_t)v; break;
  }
}

// ═════════════════════════ Loop ═════════════════════════
// Logging is DEBOUNCED: a state must hold for STABLE_LOG_MS before it is
// written. Overload flickers all through rush hour (people step in, step out);
// logging every flicker washes a 200-entry log away in one morning, and readers
// learn to skim past the lines that matter. FIRE SERVICE is logged at once.
struct Deb { bool logged; bool pend; uint32_t since; };
static Deb s_dFire, s_dInsp, s_dOos, s_dOver;
static uint8_t  s_faultLogged = 0, s_faultPend = 0;
static uint32_t s_faultSince = 0;

static void debFlag(Deb& d, bool cur, EvKind on, EvKind off, uint32_t hold) {
  const uint32_t now = millis();
  if (cur == d.logged) { d.pend = false; return; }
  if (!d.pend) { d.pend = true; d.since = now; }
  if (now - d.since >= hold) {
    d.logged = cur; d.pend = false;
    Store::logAdd(cur ? on : off);
  }
}

void Lift::begin() {
  memset(&s_st, 0, sizeof(s_st));
  memset(&C, 0, sizeof(C));
  s_st.floorIdx = -1;
  s_st.rawFloor = INT32_MIN;
  s_st.load = -1;
  snprintf(s_st.label, LABEL_LEN, "--");
}

void Lift::loop() {
  const uint32_t now = millis();

  if (s_sim && timeReached(now, S.nextTx)) {
    S.nextTx = now + 100;                     // EVL-CAN: status frame 10 times a second
    simTick();
    simEmit();
  }
  if (s_mir.left && timeReached(now, s_mir.until)) s_mir.left = 0;   // the frame stopped coming
  if (s_coRelAt && timeReached(now, s_coRelAt)) {   // CiA 417: release the call button
    s_coRelAt = 0;
    Can::send(s_coRel);
  }

  // ── Signal timeout ──
  // Fresh millis() and a SIGNED difference: the simulator above has just set
  // lastAt later than `now`, and the unsigned `now - lastAt` underflowed to
  // ~49 days — the link flapped lost/OK every 100 ms and flooded the log.
  const bool wasOk = s_st.linkOk;
  // UNSIGNED age: lastAt is set from millis() before this fresh read, so it
  // cannot underflow — and a signed age turned "live" again after 24.8 days of silence.
  s_st.linkOk = s_st.seen && (millis() - s_st.lastAt) < Store::cfg().timeoutSec * 1000UL;
  static bool     linkLogged = false;
  static uint32_t linkSince  = 0;
  if (s_st.linkOk != linkLogged) {
    if (!linkSince) linkSince = now ? now : 1;
    if (now - linkSince >= 5000) {
      linkLogged = s_st.linkOk;
      linkSince = 0;
      Store::logAdd(s_st.linkOk ? EV_LINK_OK : EV_LINK_LOST);
    }
  } else linkSince = 0;
  if (wasOk != s_st.linkOk) {
    s_st.gen++;
    if (!s_st.linkOk) snprintf(s_st.label, LABEL_LEN, "--");
    // CiA 417 sends the position only when it changes: back from a short
    // dropout, show the last known floor again instead of "--"
    else if (s_st.floorIdx >= 0) setLabelFromIdx();
  }

  // ── State logging — only while hearing the lift; stale data is never logged ──
  if (s_st.linkOk) {
    debFlag(s_dFire, s_st.fire,     EV_FIRE,     EV_FIRE_CLR,     0);
    debFlag(s_dInsp, s_st.inspect,  EV_INSPECT,  EV_INSPECT_CLR,  STABLE_LOG_MS);
    debFlag(s_dOos,  s_st.oos,      EV_OOS,      EV_OOS_CLR,      STABLE_LOG_MS);
    debFlag(s_dOver, s_st.overload, EV_OVERLOAD, EV_OVERLOAD_CLR, STABLE_LOG_MS);
    if (s_st.fault != s_faultLogged) {
      if (s_st.fault != s_faultPend) { s_faultPend = s_st.fault; s_faultSince = now; }
      else if (now - s_faultSince >= STABLE_LOG_MS) {
        if (s_st.fault) Store::logAdd(EV_FAULT, 0, s_st.fault);
        else            Store::logAdd(EV_FAULT_CLR, 0, s_faultLogged);
        s_faultLogged = s_st.fault;
      }
    } else {
      s_faultPend = s_faultLogged;
    }
  }

  // ── Arrival chime + wake the screen ──
  static bool     wasMoving = false;
  static uint8_t  travelDir = 0;
  static int16_t  lastFloor = -1;
  if (s_st.moving && s_st.dir) travelDir = s_st.dir;
  if (wasMoving && !s_st.moving && s_st.linkOk && s_st.floorIdx >= 0 &&
      !s_st.fire && !s_st.inspect) {
    // A hall lantern announces where the car will go NEXT: the arrow shown on
    // arrival is that direction. No arrow → use the direction just travelled.
    const uint8_t d = s_st.dir ? s_st.dir : travelDir;
    if (Store::cfg().chime) Out::chime(d == 2 ? 2 : 1);
    Display::wake();
  }
  wasMoving = s_st.moving;
  if (s_st.floorIdx != lastFloor) {
    lastFloor = s_st.floorIdx;
    if (s_st.linkOk) Display::wake();      // lift in use → screen bright
  }

  static uint8_t modeWas = 255;
  if (mode() != modeWas) { modeWas = mode(); Display::wake(); }

  // Any sign of use wakes the screen: a call registered or answered, the door,
  // the car starting off, the arrow. People at the landing must see the lift
  // react the moment they press a button, not after the car has moved a floor.
  static uint64_t wCar = 0, wUp = 0, wDn = 0;
  static bool     wDoor = false, wMove = false;
  static uint8_t  wDir = 0;
  if (s_st.callCar != wCar || s_st.callUp != wUp || s_st.callDn != wDn ||
      s_st.door != wDoor || s_st.moving != wMove || s_st.dir != wDir) {
    if (s_st.linkOk) Display::wake();
    wCar = s_st.callCar; wUp = s_st.callUp; wDn = s_st.callDn;
    wDoor = s_st.door; wMove = s_st.moving; wDir = s_st.dir;
  }

  // ── On-board LED ──
  static uint8_t ledWas = 255;
  uint8_t led;
  switch (mode()) {
    case LM_NOLINK: led = Out::LED_NOLINK; break;
    case LM_FIRE:   led = Out::LED_FIRE;   break;
    case LM_NORMAL:
    case LM_OVERLOAD: led = s_sim ? Out::LED_SIM : Out::LED_OK; break;
    default:        led = Out::LED_FAULT;  break;
  }
  if (led != ledWas) { ledWas = led; Out::ledMode((Out::LedMode)led); }
}
