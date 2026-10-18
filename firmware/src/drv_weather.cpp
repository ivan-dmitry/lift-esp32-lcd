// Weather for the main screen — current temperature + condition from Open-Meteo
// (free, no account, no API key). Location: set on /cfg, or found once from the
// internet address (ip-api.com, city level). Runs in its own task: an HTTP
// request may take seconds and must never hold up the CAN path or the display.
#include <Arduino.h>
#include <WiFi.h>
#include <HTTPClient.h>
#include <Preferences.h>
#include "drivers.h"

static Weather::Now  s_now = {};
static Weather::Cfg  s_cfg = {true, 0, 0};
static char          s_city[32] = "";
static portMUX_TYPE  s_mux = portMUX_INITIALIZER_UNLOCKED;
static volatile bool s_kick = false;          // fetch again now (settings changed)

static void cfgLoad() {
  Preferences p;
  p.begin("wx", true);
  s_cfg.on  = p.getBool("on", true);
  s_cfg.lat = p.getFloat("lat", 0);
  s_cfg.lon = p.getFloat("lon", 0);
  p.end();
}
static void cfgSave() {
  Preferences p;
  p.begin("wx", false);
  p.putBool("on", s_cfg.on);
  p.putFloat("lat", s_cfg.lat);
  p.putFloat("lon", s_cfg.lon);
  p.end();
}

// Tiny JSON helpers: the answers are small and flat enough for strstr
static bool jNum(const String& s, const char* key, int from, float& out) {
  char k[40];
  snprintf(k, sizeof(k), "\"%s\":", key);
  const int i = s.indexOf(k, from);
  if (i < 0) return false;
  out = atof(s.c_str() + i + strlen(k));
  return true;
}
static bool jStr(const String& s, const char* key, char* out, size_t n) {
  char k[40];
  snprintf(k, sizeof(k), "\"%s\":\"", key);
  const int i = s.indexOf(k);
  if (i < 0) return false;
  const int a = i + strlen(k), b = s.indexOf('"', a);
  if (b < 0) return false;
  snprintf(out, n, "%s", s.substring(a, b).c_str());
  return true;
}

static bool get(const char* url, String& body) {
  WiFiClient c;
  HTTPClient h;
  h.setConnectTimeout(5000);
  h.setTimeout(6000);
  if (!h.begin(c, url)) return false;
  const int code = h.GET();
  if (code == 200) body = h.getString();
  h.end();
  return code == 200;
}

// Coarse location from the public internet address (city level), once
static bool locate(float& lat, float& lon) {
  String b;
  if (!get("http://ip-api.com/json/?fields=status,city,lat,lon", b)) return false;
  if (b.indexOf("\"success\"") < 0) return false;
  float la = 0, lo = 0;
  if (!jNum(b, "lat", 0, la) || !jNum(b, "lon", 0, lo)) return false;
  lat = la; lon = lo;
  char city[32] = "";
  jStr(b, "city", city, sizeof(city));
  portENTER_CRITICAL(&s_mux);
  strlcpy(s_city, city, sizeof(s_city));
  portEXIT_CRITICAL(&s_mux);
  Serial.printf("WX   : located %s (%.2f, %.2f)\n", city, la, lo);
  return true;
}

static bool fetch(float lat, float lon) {
  char url[200];
  snprintf(url, sizeof(url),
           "http://api.open-meteo.com/v1/forecast?latitude=%.3f&longitude=%.3f"
           "&current=temperature_2m,weather_code,is_day&timezone=auto", lat, lon);
  String b;
  if (!get(url, b)) return false;
  // Values live in "current":{…}; "current_units" comes first with the same keys
  const int cur = b.indexOf("\"current\":{");
  if (cur < 0) return false;
  float t = 0, code = 0, day = 1;
  if (!jNum(b, "temperature_2m", cur, t) || !jNum(b, "weather_code", cur, code)) return false;
  jNum(b, "is_day", cur, day);
  portENTER_CRITICAL(&s_mux);
  s_now.valid = true;
  s_now.tempC = t;
  s_now.code  = (int16_t)code;
  s_now.isDay = day >= 0.5f;
  s_now.at    = millis();
  s_now.gen++;
  portEXIT_CRITICAL(&s_mux);
  Serial.printf("WX   : %.1f C, code %d, %s\n", t, (int)code, day >= 0.5f ? "day" : "night");
  return true;
}

static void task(void*) {
  float lat = 0, lon = 0;
  bool located = false;
  uint32_t nextAt = 0;
  for (;;) {
    vTaskDelay(pdMS_TO_TICKS(1000));
    if (!s_cfg.on || !Net::staConnected()) continue;
    if (s_kick) { s_kick = false; nextAt = 0; located = false; }
    if (nextAt && !timeReached(millis(), nextAt)) continue;
    bool ok = false;
    if (s_cfg.lat != 0 || s_cfg.lon != 0) { lat = s_cfg.lat; lon = s_cfg.lon; located = true; }
    else if (!located) located = locate(lat, lon);
    if (located) ok = fetch(lat, lon);
    // Every 15 min; after a failure try again in 2 min
    nextAt = millis() + (ok ? 15UL * 60000UL : 2UL * 60000UL);
    if (!nextAt) nextAt = 1;
    // Older than an hour = no longer "now": hide it rather than show stale weather
    portENTER_CRITICAL(&s_mux);
    if (s_now.valid && millis() - s_now.at > 3600000UL) { s_now.valid = false; s_now.gen++; }
    portEXIT_CRITICAL(&s_mux);
  }
}

void Weather::begin() {
  cfgLoad();
  xTaskCreatePinnedToCore(task, "weather", 6144, nullptr, 1, nullptr, 0);
}

Weather::Now Weather::now() {
  portENTER_CRITICAL(&s_mux);
  Now n = s_now;
  if (!s_cfg.on) n.valid = false;
  portEXIT_CRITICAL(&s_mux);
  return n;
}

Weather::Cfg Weather::cfg() { return s_cfg; }

void Weather::setCfg(const Cfg& c) {
  s_cfg = c;
  cfgSave();
  s_kick = true;
  portENTER_CRITICAL(&s_mux);
  s_now.gen++;
  portEXIT_CRITICAL(&s_mux);
}

void Weather::city(char* out, size_t n) {
  portENTER_CRITICAL(&s_mux);
  strlcpy(out, s_city, n);
  portEXIT_CRITICAL(&s_mux);
}
