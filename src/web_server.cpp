#include "web_server.h"

#include <Arduino.h>
#include <ArduinoJson.h>
#include <ESPmDNS.h>
#include <LittleFS.h>
#include <HTTPClient.h>
#include <Update.h>
#include <WebServer.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <freertos/queue.h>
#include <sys/time.h>

#include "certs/ca_bundle.h"
#include "clock_sync.h"
#include "config.h"
#include "net_manager.h"
#include "ota.h"
#include "photos.h"
#include "settings.h"
#include "ui_theme.h"
#include "weather.h"
#include "web_page.h"
#include "yield_stream.h"

namespace web {
namespace {

constexpr size_t kCommandLen = 200;
constexpr int kQueueDepth = 8;

WebServer* server = nullptr;
QueueHandle_t commands = nullptr;
portMUX_TYPE stateLock = portMUX_INITIALIZER_UNLOCKED;
ClockState clockState = {};

// Console commands the web UI may run. Blocking/diagnostic ones (SHOT, TEST,
// TOUCH) are excluded.
const char* const kAllowed[] = {"TZ ",          "NTP SERVER ", "NTP SYNC",   "BRIGHT ",
                                "NIGHT ",       "FORMAT ",     "WEATHER LOC ", "WEATHER CYCLE ", "WEATHER FORGET ",
                                "WEATHER UPDATE", "SETEPOCH ", "WIFI PORTAL", "WIFI FORGET",
                                "REBOOT",       "SCREEN ",     "BANNER",     "PHOTO",
                                "TIMEOUT",      "PREVIEW",     "COLOR",      "EFFECT",
                                "TRANSITION"};

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
  doc["heap_total"] = ESP.getHeapSize();
  doc["heap_free"] = ESP.getFreeHeap();
  doc["heap_min"] = ESP.getMinFreeHeap();
  doc["release_repo"] = config::kReleaseRepo;
  doc["release_asset"] = config::kReleaseAsset;
  JsonObject otaState = doc["ota"].to<JsonObject>();
  otaState["progress"] = ota::progress();
  char otaError[80];
  if (ota::recentError(otaError, sizeof(otaError))) otaState["error"] = otaError;
  const ota::FlashLayout fl = ota::flashLayout();
  JsonObject flash = doc["flash"].to<JsonObject>();
  flash["chip"] = fl.chip;
  flash["system"] = fl.system();
  flash["firmware"] = fl.firmware;
  flash["slot"] = fl.slot;
  flash["update_slot"] = fl.updateSlot;
  flash["photos_total"] = fl.data;
  flash["photos_used"] = photos::usedBytes();

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

String hexColor(uint16_t c) {
  char buf[8];
  snprintf(buf, sizeof(buf), "#%02X%02X%02X", (c >> 11) << 3, ((c >> 5) & 0x3F) << 2, (c & 0x1F) << 3);
  return buf;
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
  JsonObject colors = doc["colors"].to<JsonObject>();
  const uint8_t styleIndex = settings::colorStyle() % static_cast<uint8_t>(theme::Style::Count);
  colors["style"] = theme::styleKey(static_cast<theme::Style>(styleIndex));
  const uint16_t customTop = settings::customColors() >> 16, customBottom = settings::customColors() & 0xFFFF;
  JsonArray custom = colors["custom"].to<JsonArray>();
  custom.add(hexColor(customTop));
  custom.add(hexColor(customBottom));
  colors["speed"] = theme::speedKey(static_cast<theme::Speed>(settings::colorSpeed()));
  colors["mix_every"] = settings::mixEveryMinutes();
  doc["transition"] = theme::transitionKey(static_cast<theme::Transition>(settings::transition()));
  JsonObject fx = colors["effects"].to<JsonObject>();
  fx["shine"] = (settings::effects() & theme::kFxShine) != 0;
  fx["pulse"] = (settings::effects() & theme::kFxPulse) != 0;
  fx["glow"] = (settings::effects() & theme::kFxGlow) != 0;
  JsonArray styles = colors["styles"].to<JsonArray>();
  struct tm local;
  time_t nowT = time(nullptr);
  localtime_r(&nowT, &local);
  for (int i = 0; i < static_cast<int>(theme::Style::Count); ++i) {
    const theme::Style st = static_cast<theme::Style>(i);
    JsonObject o = styles.add<JsonObject>();
    o["key"] = theme::styleKey(st);
    o["name"] = theme::styleName(st);
    o["animated"] = theme::styleAnimated(st);
    theme::Palette p;
    bool has = theme::presetPalette(st, p);
    if (st == theme::Style::TimeOfDay) {
      p = theme::paletteForMinute(local.tm_hour * 60 + local.tm_min);
      has = true;
    } else if (st == theme::Style::Custom) {
      p = {customTop, customBottom};
      has = true;
    }
    if (has) {
      o["top"] = hexColor(p.top);
      o["bottom"] = hexColor(p.bottom);
    }
  }
  JsonObject timeouts = doc["timeouts"].to<JsonObject>();
  timeouts["forecast_s"] = settings::forecastShowSeconds();
  timeouts["photo_s"] = settings::photoShowSeconds();
  timeouts["idle_s"] = settings::screenIdleSeconds();
  JsonObject banner = doc["banner"].to<JsonObject>();
  banner["enabled"] = settings::bannerEnabled();
  banner["every_min"] = settings::bannerEveryMinutes();  // default for new messages
  banner["speed"] = settings::bannerSpeed();
  JsonArray msgs = banner["messages"].to<JsonArray>();
  for (int i = 0; i < settings::bannerCount(); ++i) {
    JsonObject m = msgs.add<JsonObject>();
    m["text"] = settings::bannerText(i);
    m["every_min"] = settings::bannerEveryMinutes(i);
  }
  JsonObject ph = doc["photos"].to<JsonObject>();
  ph["available"] = photos::available();
  ph["every_min"] = settings::photoEveryMinutes();
  ph["max"] = config::kPhotoSlots;
  ph["max_bytes"] = config::kPhotoMaxBytes;
  ph["used_bytes"] = photos::usedBytes();
  ph["total_bytes"] = photos::totalBytes();
  JsonArray items = ph["items"].to<JsonArray>();
  int slots[config::kPhotoSlots];
  const int n = photos::list(slots, config::kPhotoSlots);
  for (int i = 0; i < n; ++i) {
    JsonObject it = items.add<JsonObject>();
    it["slot"] = slots[i];
    it["bytes"] = photos::sizeOf(slots[i]);
  }
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
  YieldingStream body(http.getStream());
  DeserializationError err = deserializeJson(in, body, DeserializationOption::Filter(filter));
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

// Photo upload: a JPEG (already cropped and compressed by the page) goes
// into a free slot. The page then asks the clock to show it.
int photoSaved = 0;
String photoError;

void handlePhotoUpload() {
  HTTPUpload& up = server->upload();
  switch (up.status) {
    case UPLOAD_FILE_START:
      photoSaved = 0;
      photoError = "";
      photos::uploadBegin(photoError);
      break;
    case UPLOAD_FILE_WRITE:
      if (photoError.length()) return;
      photos::uploadWrite(up.buf, up.currentSize, photoError);
      break;
    case UPLOAD_FILE_END:
      if (photoError.length()) return;
      photoSaved = photos::uploadFinish(photoError);
      break;
    case UPLOAD_FILE_ABORTED:
      photos::uploadAbort();
      photoError = "upload aborted";
      break;
  }
}

void handlePhotoDone() {
  JsonDocument doc;
  if (photoSaved) {
    doc["ok"] = true;
    doc["slot"] = photoSaved;
    Serial.printf("[WEB] %s: photo %d uploaded\n", server->client().remoteIP().toString().c_str(),
                  photoSaved);
  } else {
    doc["error"] = photoError.length() ? photoError : String("no file received");
  }
  String out;
  serializeJson(doc, out);
  server->send(photoSaved ? 200 : 400, "application/json", out);
  photoSaved = 0;
  photoError = "";
}

void handlePhotoGet() {
  const int slot = server->arg("n").toInt();
  File f;
  if (photos::exists(slot)) f = LittleFS.open(photos::path(slot), "r");
  if (!f) {
    server->send(404, "text/plain", "No such photo");
    return;
  }
  server->sendHeader("Cache-Control", "no-cache");
  server->streamFile(f, "image/jpeg");
  f.close();
}

// Firmware upload: PIN check, then stream into the inactive OTA slot.
bool uploadOk = false;
bool uploadAuthorized = false;
String uploadError;
size_t uploadTotal = 0;

void handleOtaPin() {
  uint32_t p = ota::newPin();
  Serial.printf("[OTA] Update PIN %04lu requested from %s (valid 2 min)\n", (unsigned long)p,
                server->client().remoteIP().toString().c_str());
  server->send(200, "application/json", "{\"pin_shown_on_clock\":true}");
}

// Install a GitHub release: the clock downloads it itself. Needs the PIN too,
// so nobody else on the network can switch the firmware version.
void handleOtaRelease() {
  const String tag = server->arg("tag");
  if (!ota::checkPin(strtoul(server->arg("pin").c_str(), nullptr, 10))) {
    server->send(403, "application/json", "{\"error\":\"wrong or expired code\"}");
    return;
  }
  if (!ota::requestRelease(tag.c_str())) {
    server->send(400, "application/json", "{\"error\":\"invalid release or an update is running\"}");
    return;
  }
  Serial.printf("[OTA] %s asked to install release %s\n",
                server->client().remoteIP().toString().c_str(), tag.c_str());
  server->send(202, "application/json", "{\"started\":true}");
}

void handleOtaUpload() {
  HTTPUpload& up = server->upload();
  switch (up.status) {
    case UPLOAD_FILE_START: {
      uploadOk = false;
      uploadError = "";
      uploadTotal = server->hasHeader("Content-Length") ? server->header("Content-Length").toInt() : 0;
      uploadAuthorized = ota::checkPin(strtoul(server->arg("pin").c_str(), nullptr, 10));
      if (!uploadAuthorized) {
        uploadError = "wrong or expired PIN";
        return;
      }
      Serial.printf("[OTA] Receiving %s\n", up.filename.c_str());
      ota::setProgress(0);
      if (!Update.begin(UPDATE_SIZE_UNKNOWN, U_FLASH)) uploadError = Update.errorString();
      break;
    }
    case UPLOAD_FILE_WRITE:
      if (!uploadAuthorized || uploadError.length()) return;
      if (Update.write(up.buf, up.currentSize) != up.currentSize) {
        uploadError = Update.errorString();
        Update.abort();
      } else if (uploadTotal > 0) {
        ota::setProgress(min<int>(99, Update.progress() * 100 / uploadTotal));
      }
      break;
    case UPLOAD_FILE_END:
      if (!uploadAuthorized || uploadError.length()) return;
      if (Update.end(true)) {
        uploadOk = true;
        Serial.printf("[OTA] Received %u bytes, image verified\n", up.totalSize);
      } else {
        uploadError = Update.errorString();
      }
      break;
    case UPLOAD_FILE_ABORTED:
      Update.abort();
      uploadError = "upload aborted";
      break;
  }
}

void handleOtaDone() {
  ota::setProgress(-1);
  if (!uploadOk) {
    Serial.printf("[OTA] Update failed: %s\n", uploadError.c_str());
    String body = "{\"error\":\"" + uploadError + "\"}";
    server->send(uploadAuthorized ? 500 : 403, "application/json", body);
    return;
  }
  server->send(200, "application/json", "{\"ok\":true,\"rebooting\":true}");
  Serial.println("[OTA] Rebooting into the new firmware");
  Serial.flush();
  delay(500);
  ESP.restart();
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
  server->on("/api/photo", HTTP_GET, handlePhotoGet);
  server->on("/api/photo", HTTP_POST, handlePhotoDone, handlePhotoUpload);
  server->on("/api/ota/pin", HTTP_POST, handleOtaPin);
  server->on("/api/ota/release", HTTP_POST, handleOtaRelease);
  server->on("/api/ota", HTTP_POST, handleOtaDone, handleOtaUpload);
  static const char* headers[] = {"Content-Length"};
  server->collectHeaders(headers, 1);
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
