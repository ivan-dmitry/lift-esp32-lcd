// MQTT + Home Assistant (MQTT discovery)
//
// Uses the esp-mqtt client built into the ESP32 core instead of PubSubClient,
// for one reason that is heavy enough: PubSubClient::connect() BLOCKS — with
// the broker down, every reconnect attempt freezes the main loop for seconds,
// the 256-slot CAN queue overflows and the screen stalls. esp-mqtt runs in its
// own task, reconnects by itself, and esp_mqtt_client_enqueue() just drops the
// message into the outbox and returns.
//
// Three rules:
//   · Losing MQTT affects neither the display nor the CAN path.
//   · Commands from HA go through EXACTLY the same locks as panel commands
//     (Lift::command), plus the "remote commands" switch that can only be
//     turned on at the panel.
//   · With that switch OFF the control entities are REMOVED from HA, instead of
//     leaving buttons that are always refused — that would teach users that
//     buttons in HA cannot be trusted.
#include <Preferences.h>
#include "mqtt_client.h"
#include "drivers.h"
#include "store.h"
#include "lift.h"

static Preferences nvsMq;
static bool     s_on = false;
static char     s_host[48] = {0};
static uint16_t s_port = 1883;
static char     s_user[32] = {0};
static char     s_pass[48] = {0};

static esp_mqtt_client_handle_t s_cli = nullptr;
static char     s_uri[72];
static char     s_cid[32];
static char     s_devId[8];                 // last 3 MAC bytes, "a1b2c3"
static char     s_base[32];                 // "elevator/a1b2c3"
static char     s_lwt[48];
static char     s_err[64] = "";
static volatile bool s_conn = false;
static volatile bool s_needDisc = false;
static uint32_t s_sent = 0;

// ───── Command inbox: the MQTT task puts messages in, the main loop takes them out ─────
// Lift::command() is NOT called from the MQTT task: Lift, Store and Can run on
// ONE thread only — the main loop — and keeping it that way means no locks.
struct InMsg { char topic[64]; char data[256]; };   // data fits an announcement URL
static InMsg        s_in[4];
static uint8_t      s_inHead = 0, s_inTail = 0;
static portMUX_TYPE s_mux = portMUX_INITIALIZER_UNLOCKED;

static void mqLoad() {
  nvsMq.begin("mqtt", false);
  s_on = nvsMq.getBool("on", false);
  if (nvsMq.isKey("host")) nvsMq.getString("host", s_host, sizeof(s_host));
  s_port = nvsMq.getUShort("port", 1883);
  if (nvsMq.isKey("user")) nvsMq.getString("user", s_user, sizeof(s_user));
  if (nvsMq.isKey("pass")) nvsMq.getString("pass", s_pass, sizeof(s_pass));
  nvsMq.end();
}
static void mqSave() {
  nvsMq.begin("mqtt", false);
  nvsMq.putBool("on", s_on);
  nvsMq.putString("host", s_host);
  nvsMq.putUShort("port", s_port);
  nvsMq.putString("user", s_user);
  nvsMq.putString("pass", s_pass);
  nvsMq.end();
}

// ═════════════════════════ Events (run in the MQTT task) ═════════════════════════
static void onEvent(void*, esp_event_base_t, int32_t id, void* data) {
  esp_mqtt_event_handle_t e = (esp_mqtt_event_handle_t)data;
  switch ((esp_mqtt_event_id_t)id) {
    case MQTT_EVENT_CONNECTED: {
      s_conn = true;
      s_err[0] = 0;
      char t[64];
      snprintf(t, sizeof(t), "%s/cmd/#", s_base);
      esp_mqtt_client_subscribe(e->client, t, 0);
      esp_mqtt_client_subscribe(e->client, "homeassistant/status", 0);
      esp_mqtt_client_publish(e->client, s_lwt, "online", 0, 1, 1);
      s_needDisc = true;
      break;
    }
    case MQTT_EVENT_DISCONNECTED:
      s_conn = false;
      break;
    case MQTT_EVENT_ERROR:
      if (e->error_handle) {
        if (e->error_handle->error_type == MQTT_ERROR_TYPE_CONNECTION_REFUSED) {
          const int rc = e->error_handle->connect_return_code;
          snprintf(s_err, sizeof(s_err), rc >= 4 ? "broker refused: wrong user / password"
                                                 : "broker refused the connection (code %d)", rc);
        } else {
          snprintf(s_err, sizeof(s_err), "cannot reach the broker (socket error %d)",
                   e->error_handle->esp_transport_sock_errno);
        }
      }
      break;
    case MQTT_EVENT_DATA: {
      // Long messages arrive in several pieces: real commands are a few bytes,
      // anything not complete in one piece is ignored.
      if (!e->topic || e->total_data_len != e->data_len) break;
      // A RETAINED command would fire again on every reconnect (WiFi blip,
      // broker restart). Commands are one-shot: retained ones are ignored.
      if (e->retain && e->topic_len > 5) {
        const size_t bl = strlen(s_base);
        if ((size_t)e->topic_len > bl + 5 && !strncmp(e->topic, s_base, bl) && !strncmp(e->topic + bl, "/cmd/", 5)) break;
      }
      if (e->topic_len >= (int)sizeof(InMsg::topic) || e->data_len >= (int)sizeof(InMsg::data)) break;
      portENTER_CRITICAL(&s_mux);
      const uint8_t nx = (uint8_t)((s_inHead + 1) & 3);
      if (nx != s_inTail) {
        memcpy(s_in[s_inHead].topic, e->topic, e->topic_len);
        s_in[s_inHead].topic[e->topic_len] = 0;
        memcpy(s_in[s_inHead].data, e->data, e->data_len);
        s_in[s_inHead].data[e->data_len] = 0;
        s_inHead = nx;
      }
      portEXIT_CRITICAL(&s_mux);
      break;
    }
    default: break;
  }
}

// ═════════════════════════ Publishing ═════════════════════════
static bool pub(const char* topic, const char* payload, bool retain, int qos = 0) {
  if (!s_cli || !s_conn) return false;
  // A growing outbox means the network is choking — do not pile on; RAM is
  // what LVGL needs to build screens.
  if (esp_mqtt_client_get_outbox_size(s_cli) > 8192) return false;
  if (esp_mqtt_client_enqueue(s_cli, topic, payload, 0, qos, retain ? 1 : 0, true) < 0) return false;
  s_sent++;
  return true;
}

// ───── Discovery ─────
enum { E_FLOOR, E_DIR, E_DOOR, E_MOVING, E_FAULT, E_FCODE, E_MODE, E_FIRE, E_LOAD,
       E_LINK, E_FPS, E_RSSI, E_CAR, E_UP, E_DN, E_OPEN, E_CLOSE, E_HOLD, E_COUNT };
static uint8_t s_discI = E_COUNT;          // E_COUNT = nothing being sent

static int devBlock(char* b, size_t n) {
  // The lift name goes into JSON — strip anything that would break the string
  char name[NAME_LEN];
  uint8_t k = 0;
  for (const char* p = Store::cfg().liftName; *p && k < NAME_LEN - 1; p++)
    if (*p != '"' && *p != '\\' && (uint8_t)*p >= 0x20) name[k++] = *p;
  name[k] = 0;
  return snprintf(b, n,
    "\"avty_t\":\"%s\",\"dev\":{\"ids\":[\"elevator_%s\"],\"name\":\"%s\","
    "\"mf\":\"DIY\",\"mdl\":\"ESP32-3248S035 + CAN\",\"sw\":\"" FW_VERSION "\","
    "\"cu\":\"http://%s/\"}",
    s_lwt, s_devId, name, Net::ip().c_str());
}

static bool discOne(uint8_t i) {
  static char p[1600];
  char topic[96], dev[300];
  devBlock(dev, sizeof(dev));
  const bool remote = Store::cfg().allowRemote;
  const char* comp = "sensor";
  const char* obj = "";
  int n = 0;
  auto head = [&](const char* name, const char* o) {
    obj = o;
    return snprintf(p, sizeof(p), "{\"name\":\"%s\",\"uniq_id\":\"elevator_%s_%s\",", name, s_devId, o);
  };
  switch (i) {
    case E_FLOOR:
      n = head("Floor", "floor");
      snprintf(p + n, sizeof(p) - n, "\"stat_t\":\"%s/state\",\"val_tpl\":\"{{value_json.floor}}\","
               "\"ic\":\"mdi:elevator\",%s}", s_base, dev);
      break;
    case E_DIR:
      n = head("Direction", "direction");
      snprintf(p + n, sizeof(p) - n, "\"stat_t\":\"%s/state\",\"val_tpl\":\"{{value_json.dir}}\","
               "\"dev_cla\":\"enum\",\"ops\":[\"up\",\"down\",\"stop\"],"
               "\"ic\":\"mdi:swap-vertical\",%s}", s_base, dev);
      break;
    case E_DOOR:
      comp = "binary_sensor";
      n = head("Door", "door");
      snprintf(p + n, sizeof(p) - n, "\"stat_t\":\"%s/state\",\"dev_cla\":\"door\","
               "\"val_tpl\":\"{{'ON' if value_json.door else 'OFF'}}\",%s}", s_base, dev);
      break;
    case E_MOVING:
      comp = "binary_sensor";
      n = head("Moving", "moving");
      snprintf(p + n, sizeof(p) - n, "\"stat_t\":\"%s/state\",\"dev_cla\":\"moving\","
               "\"val_tpl\":\"{{'ON' if value_json.moving else 'OFF'}}\",%s}", s_base, dev);
      break;
    case E_FAULT:
      comp = "binary_sensor";
      n = head("Fault", "fault");
      snprintf(p + n, sizeof(p) - n, "\"stat_t\":\"%s/state\",\"dev_cla\":\"problem\","
               "\"val_tpl\":\"{{'ON' if value_json.fault > 0 else 'OFF'}}\",%s}", s_base, dev);
      break;
    case E_FCODE:
      n = head("Fault code", "fault_code");
      snprintf(p + n, sizeof(p) - n, "\"stat_t\":\"%s/state\",\"val_tpl\":\"{{value_json.fault}}\","
               "\"ic\":\"mdi:alert-circle-outline\",%s}", s_base, dev);
      break;
    case E_MODE:
      n = head("Mode", "mode");
      snprintf(p + n, sizeof(p) - n, "\"stat_t\":\"%s/state\",\"val_tpl\":\"{{value_json.mode}}\","
               "\"dev_cla\":\"enum\",\"ops\":[\"normal\",\"fire\",\"fault\",\"inspection\","
               "\"out_of_service\",\"overload\",\"no_link\"],"
               "\"ic\":\"mdi:elevator-passenger\",%s}", s_base, dev);
      break;
    case E_FIRE:
      comp = "binary_sensor";
      n = head("Fire service", "fire");
      snprintf(p + n, sizeof(p) - n, "\"stat_t\":\"%s/state\",\"dev_cla\":\"safety\","
               "\"val_tpl\":\"{{'ON' if value_json.fire else 'OFF'}}\",%s}", s_base, dev);
      break;
    case E_LOAD:
      obj = "load";
      if (!(Store::map().sig[SIG_LOAD].flags & MAP_USED)) { p[0] = 0; break; }   // no load signal → remove
      n = head("Load", "load");
      snprintf(p + n, sizeof(p) - n, "\"stat_t\":\"%s/state\",\"val_tpl\":\"{{value_json.load}}\","
               "\"unit_of_meas\":\"%%\",\"stat_cla\":\"measurement\",\"ic\":\"mdi:weight\",%s}", s_base, dev);
      break;
    case E_LINK:
      comp = "binary_sensor";
      n = head("CAN signal", "link");
      snprintf(p + n, sizeof(p) - n, "\"stat_t\":\"%s/state\",\"dev_cla\":\"connectivity\","
               "\"ent_cat\":\"diagnostic\",\"val_tpl\":\"{{'ON' if value_json.link else 'OFF'}}\",%s}",
               s_base, dev);
      break;
    case E_FPS:
      n = head("CAN frames per second", "fps");
      snprintf(p + n, sizeof(p) - n, "\"stat_t\":\"%s/state\",\"val_tpl\":\"{{value_json.fps}}\","
               "\"unit_of_meas\":\"frames/s\",\"stat_cla\":\"measurement\",\"ent_cat\":\"diagnostic\","
               "\"ic\":\"mdi:lan\",%s}", s_base, dev);
      break;
    case E_RSSI:
      n = head("WiFi signal", "rssi");
      snprintf(p + n, sizeof(p) - n, "\"stat_t\":\"%s/state\",\"val_tpl\":\"{{value_json.rssi}}\","
               "\"unit_of_meas\":\"dBm\",\"dev_cla\":\"signal_strength\",\"stat_cla\":\"measurement\","
               "\"ent_cat\":\"diagnostic\",%s}", s_base, dev);
      break;
    case E_CAR: case E_UP: case E_DN: {
      comp = "select";
      static const char* const NM[3] = {"Car call", "Hall call up", "Hall call down"};
      static const char* const OB[3] = {"car_call", "hall_up", "hall_down"};
      static const char* const CT[3] = {"car", "up", "dn"};
      const uint8_t k = (uint8_t)(i - E_CAR);
      obj = OB[k];
      if (!remote) { p[0] = 0; break; }
      n = head(NM[k], OB[k]);
      // First option is "-": HA keeps the last selection, and selecting the
      // same floor again sends nothing. After every command the display resets
      // the selection to "-".
      n += snprintf(p + n, sizeof(p) - n, "\"cmd_t\":\"%s/cmd/%s\",\"stat_t\":\"%s/sel\","
                    "\"ic\":\"mdi:gesture-tap-button\",\"ops\":[\"-\"", s_base, CT[k], s_base);
      for (uint8_t f = 0; f < Store::cfg().floors && n < (int)sizeof(p) - 400; f++)
        n += snprintf(p + n, sizeof(p) - n, ",\"%s\"", Store::label(f));
      snprintf(p + n, sizeof(p) - n, "],%s}", dev);
      break;
    }
    case E_OPEN: case E_CLOSE: case E_HOLD: {
      comp = "button";
      static const char* const OB[3] = {"door_open", "door_close", "door_hold"};
      static const char* const NM[3] = {"Door open", "Door close", "Door hold 15 s"};
      static const char* const PL[3] = {"open", "close", "hold"};
      static const char* const IC[3] = {"mdi:elevator-up", "mdi:elevator-down", "mdi:timer-sand"};
      const uint8_t k = (uint8_t)(i - E_OPEN);
      obj = OB[k];
      // Only when the protocol map can send it — a button that only ever answers
      // "no command frame defined" is clutter in Home Assistant
      if (!remote || !(Store::map().cmd[CMD_DOOR_OPEN + k].flags & MAP_USED)) { p[0] = 0; break; }
      n = head(NM[k], obj);
      snprintf(p + n, sizeof(p) - n, "\"cmd_t\":\"%s/cmd/door\",\"pl_prs\":\"%s\","
               "\"ic\":\"%s\",%s}", s_base, PL[k], IC[k], dev);
      break;
    }
  }
  snprintf(topic, sizeof(topic), "homeassistant/%s/elevator_%s/%s/config", comp, s_devId, obj);
  return pub(topic, p, true, 1);   // empty payload + retain = remove the entity from HA
}

// ───── State ─────
static void pubState() {
  const LiftState& s = Lift::st();
  const LiftMode m = Lift::mode();
  Can::Stats cs; Can::stats(cs);
  char b[400], t[48];
  snprintf(b, sizeof(b),
    "{\"floor\":\"%s\",\"idx\":%d,\"dir\":\"%s\",\"moving\":%s,\"door\":%s,"
    "\"fault\":%u,\"mode\":\"%s\",\"fire\":%s,\"load\":%d,\"link\":%s,"
    "\"fps\":%u,\"rssi\":%d,\"sim\":%s,\"car_calls\":\"%08lX%08lX\"}",
    s.linkOk ? s.label : "--", s.floorIdx,
    s.dir == 1 ? "up" : s.dir == 2 ? "down" : "stop",
    s.moving ? "true" : "false", s.door ? "true" : "false", s.fault,
    Lift::modeId(m), s.fire ? "true" : "false", s.load,
    s.linkOk ? "true" : "false", cs.fps, Net::rssi(), Lift::simOn() ? "true" : "false",
    (unsigned long)(s.callCar >> 32), (unsigned long)(s.callCar & 0xFFFFFFFF));
  snprintf(t, sizeof(t), "%s/state", s_base);
  pub(t, b, true);
}

// ───── Incoming commands ─────
static void handleIn(const InMsg& m) {
  if (!strcmp(m.topic, "homeassistant/status")) {
    // HA just restarted: send configuration + state again
    if (!strcmp(m.data, "online")) s_needDisc = true;
    return;
  }
  const size_t bl = strlen(s_base);
  if (strncmp(m.topic, s_base, bl) || strncmp(m.topic + bl, "/cmd/", 5)) return;
  const char* what = m.topic + bl + 5;
  if (!strcmp(what, "announce")) {
    // payload: "http://host/file.wav" (optionally followed by "?chime=1" options) or "stop"
    if (!strcmp(m.data, "stop")) Audio::stop();
    else if (Store::cfg().audioApi) Audio::playUrl(m.data, strstr(m.data, "chime=1") ? "chime=1" : "");
    return;
  }
  uint8_t cmd = CMD_COUNT;
  int floor = 0;
  if (!strcmp(what, "door")) {
    cmd = !strcmp(m.data, "open") ? CMD_DOOR_OPEN : !strcmp(m.data, "close") ? CMD_DOOR_CLOSE
        : !strcmp(m.data, "hold") ? CMD_DOOR_HOLD : CMD_COUNT;
  } else {
    if (!strcmp(m.data, "-") || !m.data[0]) return;          // empty selection
    cmd = !strcmp(what, "car") ? CMD_CAR : !strcmp(what, "up") ? CMD_HALL_UP
        : !strcmp(what, "dn") ? CMD_HALL_DN : CMD_COUNT;
    floor = Store::findLabel(m.data);
    // "#3" sends the floor index directly, so automations do not depend on labels
    if (floor < 0) floor = (m.data[0] == '#') ? atoi(m.data + 1) : -1;
  }
  if (cmd == CMD_COUNT) return;
  const CmdResult r = Lift::command(cmd, floor, SRC_MQTT);
  char t[64], b[160];
  snprintf(t, sizeof(t), "%s/cmd_result", s_base);
  snprintf(b, sizeof(b), "{\"cmd\":\"%s\",\"floor\":\"%s\",\"ok\":%s,\"msg\":\"%s\"}",
           what, m.data, r == RES_OK ? "true" : "false", Lift::resultText(r));
  pub(t, b, false);
  snprintf(t, sizeof(t), "%s/sel", s_base);
  pub(t, "-", true);
}

// ═════════════════════════ Lifecycle ═════════════════════════
static void cliStop() {
  if (!s_cli) return;
  feedLoopWDT();                  // stopping waits for the MQTT task (DNS / connect timeouts)
  esp_mqtt_client_stop(s_cli);
  esp_mqtt_client_destroy(s_cli);
  s_cli = nullptr;
  s_conn = false;
}

static void cliStart() {
  if (s_cli || !s_host[0]) return;
  snprintf(s_uri, sizeof(s_uri), "mqtt://%s:%u", s_host, s_port);
  esp_mqtt_client_config_t c = {};
  c.uri = s_uri;
  c.client_id = s_cid;
  c.username = s_user[0] ? s_user : nullptr;
  c.password = s_pass[0] ? s_pass : nullptr;
  c.lwt_topic = s_lwt;
  c.lwt_msg = "offline";
  c.lwt_qos = 1;
  c.lwt_retain = 1;
  c.keepalive = 30;
  c.buffer_size = 2048;           // the discovery of a 64-floor select is close to 1 KB
  c.out_buffer_size = 2048;
  c.reconnect_timeout_ms = 5000;
  c.network_timeout_ms = 2500;    // the client lock is held this long on a bad link: pub() on the loop waits for it
  c.task_prio = 3;                // below WiFi, above the main loop
  s_cli = esp_mqtt_client_init(&c);
  if (!s_cli) { snprintf(s_err, sizeof(s_err), "client init failed"); return; }
  esp_mqtt_client_register_event(s_cli, (esp_mqtt_event_id_t)ESP_EVENT_ANY_ID, onEvent, nullptr);
  esp_mqtt_client_start(s_cli);
  Serial.printf("MQTT : connecting to %s\n", s_uri);
}

void Mqtt::begin() {
  mqLoad();
  const uint64_t mac = ESP.getEfuseMac();
  // The last three MAC bytes, in the order printed on the module label
  snprintf(s_devId, sizeof(s_devId), "%02x%02x%02x",
           (uint8_t)(mac >> 24), (uint8_t)(mac >> 32), (uint8_t)(mac >> 40));
  snprintf(s_base, sizeof(s_base), "elevator/%s", s_devId);
  snprintf(s_lwt, sizeof(s_lwt), "%s/avail", s_base);
  snprintf(s_cid, sizeof(s_cid), "elevator-%s", s_devId);
}

void Mqtt::loop() {
  const uint32_t now = millis();
  // ── Only once joined to the site WiFi. In AP mode there is no way out. ──
  if (s_on && Net::staConnected() && !s_cli) cliStart();
  if (!s_on && s_cli) cliStop();
  if (!s_cli) return;

  // ── Incoming ──
  InMsg m;
  bool got = false;
  portENTER_CRITICAL(&s_mux);
  if (s_inTail != s_inHead) { m = s_in[s_inTail]; s_inTail = (uint8_t)((s_inTail + 1) & 3); got = true; }
  portEXIT_CRITICAL(&s_mux);
  if (got) handleIn(m);

  if (!s_conn) return;

  // ── Discovery: ONE entity per loop pass, not 17 messages at once ──
  if (s_needDisc) { s_needDisc = false; s_discI = 0; }
  if (s_discI < E_COUNT) {
    // Next entity only once this one is queued: a full outbox must not drop a
    // config (or the empty "remove" message for the call entities)
    if (discOne(s_discI)) s_discI++;
    if (s_discI == E_COUNT) {
      char t[48];
      snprintf(t, sizeof(t), "%s/sel", s_base);
      pub(t, "-", true);
      pubState();
    }
    return;
  }

  // ── State: on change (at most 4 times a second) + a 30 s heartbeat ──
  static uint32_t lastGen = 0xFFFFFFFF, lastAt = 0;
  static uint8_t  lastMode = 255;
  static bool     lastLink = false;
  const LiftState& s = Lift::st();
  const uint8_t md = Lift::mode();
  const bool changed = s.gen != lastGen || md != lastMode || s.linkOk != lastLink;
  if ((changed && now - lastAt >= 250) || now - lastAt >= 30000) {
    lastGen = s.gen; lastMode = md; lastLink = s.linkOk; lastAt = now;
    pubState();
  }
}

bool Mqtt::on()        { return s_on; }
bool Mqtt::connected() { return s_cli && s_conn; }
const char* Mqtt::host() { return s_host; }
uint16_t    Mqtt::port() { return s_port; }
const char* Mqtt::user() { return s_user; }
bool        Mqtt::hasPass() { return s_pass[0] != 0; }
const char* Mqtt::baseTopic() { return s_base; }
const char* Mqtt::lastError() { return s_err; }
uint32_t    Mqtt::sentCount() { return s_sent; }
void        Mqtt::rediscover() { s_needDisc = true; }

void Mqtt::setEnabled(bool on) {
  if (s_on == on) return;
  s_on = on;
  mqSave();
  if (!on) cliStop();
}

void Mqtt::setServer(const char* host, uint16_t port) {
  strncpy(s_host, host, sizeof(s_host) - 1); s_host[sizeof(s_host) - 1] = 0;
  s_port = port ? port : 1883;
  mqSave();
  cliStop();                       // reconnect to the new address on the next loop pass
}

// pass = nullptr: keep the old password (the web page never sends it back out)
void Mqtt::setAuth(const char* user, const char* pass) {
  strncpy(s_user, user, sizeof(s_user) - 1); s_user[sizeof(s_user) - 1] = 0;
  if (pass) { strncpy(s_pass, pass, sizeof(s_pass) - 1); s_pass[sizeof(s_pass) - 1] = 0; }
  mqSave();
  cliStop();
}
