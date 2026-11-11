#include "web_server.h"

#include <Arduino.h>
#include <ArduinoJson.h>
#include <ESPmDNS.h>
#include <HTTPClient.h>
#include <WebServer.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <freertos/queue.h>
#include <sys/time.h>

#include "certs/ca_bundle.h"
#include "clock_sync.h"
#include "config.h"
#include "net_manager.h"
#include "settings.h"
#include "weather.h"
#include "web_page.h"

namespace web {
namespace {

constexpr size_t kCommandLen = 96;
constexpr int kQueueDepth = 8;

WebServer* server = nullptr;
QueueHandle_t commands = nullptr;
portMUX_TYPE stateLock = portMUX_INITIALIZER_UNLOCKED;
ClockState clockState = {};

// Console commands the web UI may run. Blocking/diagnostic ones (SHOT, TEST,
// TOUCH) are excluded.
const char* const kAllowed[] = {"TZ ",          "NTP SERVER ", "NTP SYNC",   "BRIGHT ",
                                "NIGHT ",       "FORMAT ",     "WEATHER LOC ", "WEATHER CYCLE ",
                                "WEATHER UPDATE", "SETEPOCH ", "WIFI PORTAL", "WIFI FORGET",
                                "REBOOT",       "SCREEN "};

bool allowed(const String& cmd) {
  if (cmd.length() == 0 || cmd.length() >= kCommandLen || cmd.indexOf('\n') >= 0 ||
      cmd.indexOf('\r') >= 0) {
    return false;
  }
  for (const char* prefix : kAllowed) {
    if (cmd.startsWith(prefix) || cmd == String(prefix)) return true;
  }
  return false;
}

void sendJson(JsonDocument& doc) {
  String out;
  serializeJson(doc, out);
  server->sendHeader("Cache-Control", "no-store");
  server->send(200, "application/json", out);
}

void handleRoot() {
  server->sendHeader("Cache-Control", "no-store");
  server->send_P(200, "text/html", kWebPage);
}

void handleStatus() {
  JsonDocument doc;
  doc["firmware"] = config::kFirmwareVersion;
  doc["revision"] = config::kGitRevision;
  doc["uptime_s"] = millis() / 1000;
  doc["heap_free"] = ESP.getFreeHeap();
  doc["heap_min"] = ESP.getMinFreeHeap();

  struct timeval tv;
  gettimeofday(&tv, nullptr);
  doc["utc"] = (uint32_t)tv.tv_sec;
  struct tm local;
  localtime_r(&tv.tv_sec, &local);
  char buf[40];
  strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M:%S", &local);
  doc["local_time"] = buf;

  JsonObject wifi = doc["wifi"].to<JsonObject>();
  char ssid[33], ip[16];
  net::ssid(ssid, sizeof(ssid));
  net::ip(ip, sizeof(ip));
  wifi["ssid"] = ssid;
  wifi["ip"] = ip;
  wifi["rssi"] = net::rssi();
  wifi["host"] = String(config::kHostname) + ".local";

  char ntp[16];
  clock_sync::statusText(ntp, sizeof(ntp));
  doc["ntp"] = ntp;

  ClockState cs;
  portENTER_CRITICAL(&stateLock);
  cs = clockState;
  portEXIT_CRITICAL(&stateLock);
  JsonObject rtc = doc["rtc"].to<JsonObject>();
  rtc["present"] = cs.rtcPresent;
  rtc["lost_power"] = cs.rtcLostPower;
  if (cs.hasTemp) rtc["temp_c"] = serialized(String(cs.tempC, 2));
  doc["night"] = cs.night;
  doc["screen"] = cs.screen;

  weather::Data wx;
  if (weather::get(wx)) {
    JsonObject w = doc["weather"].to<JsonObject>();
    w["place"] = wx.place;
    w["temp_c"] = serialized(String(wx.temp, 1));
    w["condition"] = weather::describe(wx.code);
    w["age_s"] = weather::ageSeconds();
    if (wx.dayCount) {
      w["high_c"] = wx.days[0].tmax;
      w["low_c"] = wx.days[0].tmin;
      w["rain_pct"] = wx.days[0].pop;
    }
  }
  sendJson(doc);
}

void handleSettings() {
  JsonDocument doc;
  doc["timezone"] = settings::timezone();
  doc["ntp_server"] = settings::ntpServer();
  doc["brightness_day"] = settings::brightnessDay();
  doc["brightness_night"] = settings::brightnessNight();
  JsonObject night = doc["night"].to<JsonObject>();
  night["enabled"] = settings::nightEnabled();
  char hm[8];
  snprintf(hm, sizeof(hm), "%02u:%02u", settings::nightStart() / 60, settings::nightStart() % 60);
  night["start"] = hm;
  snprintf(hm, sizeof(hm), "%02u:%02u", settings::nightEnd() / 60, settings::nightEnd() % 60);
  night["end"] = hm;
  doc["hour12"] = settings::hour12();
  doc["forecast_cycle"] = settings::weatherCycle();
  JsonObject loc = doc["location"].to<JsonObject>();
  loc["name"] = settings::place();
  loc["lat"] = serialized(String(settings::latitude(), 4));
  loc["lon"] = serialized(String(settings::longitude(), 4));
  JsonArray recents = doc["recent_locations"].to<JsonArray>();
  for (int i = 0; i < settings::recentLocationCount(); ++i) {
    char name[32];
    float la, lo;
    if (!settings::recentLocation(i, name, sizeof(name), la, lo)) continue;
    JsonObject r = recents.add<JsonObject>();
    r["name"] = name;
    r["lat"] = serialized(String(la, 4));
    r["lon"] = serialized(String(lo, 4));
  }
  sendJson(doc);
}

String urlEncode(const String& s) {
  String out;
  for (char c : s) {
    if (isalnum(static_cast<unsigned char>(c)) || c == '-' || c == '.' || c == '_') {
      out += c;
    } else {
      char enc[4];
      snprintf(enc, sizeof(enc), "%%%02X", static_cast<unsigned char>(c));
      out += enc;
    }
  }
  return out;
}

void handleGeocode() {
  String q = server->arg("q");
  q.trim();
  if (q.length() < 2 || q.length() > 60) {
    server->send(400, "application/json", "{\"error\":\"query must be 2-60 characters\"}");
    return;
  }
  String url = "https://geocoding-api.open-meteo.com/v1/search?count=5&language=en&name=" + urlEncode(q);
  if (server->hasArg("country")) url += "&countryCode=" + urlEncode(server->arg("country"));

  WiFiClientSecure client;
  client.setCACertBundle(ca_bundle);
  client.setTimeout(10);
  HTTPClient http;
  http.useHTTP10(true);
  http.setTimeout(10000);
  if (!http.begin(client, url) || http.GET() != HTTP_CODE_OK) {
    http.end();
    server->send(502, "application/json", "{\"error\":\"geocoding service unavailable\"}");
    return;
  }
  JsonDocument filter;
  JsonObject f = filter["results"][0].to<JsonObject>();
  for (const char* k : {"name", "latitude", "longitude", "country", "country_code", "admin1",
                        "admin2", "population"}) {
    f[k] = true;
  }
  JsonDocument in;
  DeserializationError err = deserializeJson(in, http.getStream(), DeserializationOption::Filter(filter));
  http.end();
  if (err) {
    server->send(502, "application/json", "{\"error\":\"bad geocoding response\"}");
    return;
  }
  JsonDocument out;
  JsonArray results = out["results"].to<JsonArray>();
  for (JsonObject r : in["results"].as<JsonArray>()) results.add(r);
  sendJson(out);
}

void handleCommand() {
  String cmd = server->arg("plain");
  cmd.trim();
  if (!allowed(cmd)) {
    server->send(400, "application/json", "{\"error\":\"command not allowed\"}");
    return;
  }
  char buf[kCommandLen];
  strlcpy(buf, cmd.c_str(), sizeof(buf));
  if (xQueueSend(commands, buf, pdMS_TO_TICKS(100)) != pdTRUE) {
    server->send(503, "application/json", "{\"error\":\"busy, try again\"}");
    return;
  }
  Serial.printf("[WEB] %s: %s\n", server->client().remoteIP().toString().c_str(), buf);
  server->send(202, "application/json", "{\"queued\":true}");
}

}  // namespace

void start() {
  if (!commands) commands = xQueueCreate(kQueueDepth, kCommandLen);
  if (server) return;
  server = new WebServer(80);
  server->on("/", HTTP_GET, handleRoot);
  server->on("/api/status", HTTP_GET, handleStatus);
  server->on("/api/settings", HTTP_GET, handleSettings);
  server->on("/api/geocode", HTTP_GET, handleGeocode);
  server->on("/api/command", HTTP_POST, handleCommand);
  server->onNotFound([] { server->send(404, "text/plain", "Not found"); });
  server->begin();
  MDNS.addService("http", "tcp", 80);
  Serial.printf("[WEB] Settings at http://%s.local (http://%s)\n", config::kHostname,
                WiFi.localIP().toString().c_str());
}

void stop() {
  if (!server) return;
  server->stop();
  delete server;
  server = nullptr;
}

void loop() {
  if (server) server->handleClient();
}

bool nextCommand(char* buf, size_t len) {
  if (!commands) return false;
  char tmp[kCommandLen];
  if (xQueueReceive(commands, tmp, 0) != pdTRUE) return false;
  strlcpy(buf, tmp, len);
  return true;
}

void publishClockState(const ClockState& state) {
  portENTER_CRITICAL(&stateLock);
  clockState = state;
  portEXIT_CRITICAL(&stateLock);
}

}  // namespace web
