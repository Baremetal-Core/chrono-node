#include "weather.h"

#include <Arduino.h>
#include <ArduinoJson.h>
#include <HTTPClient.h>
#include <WiFiClientSecure.h>
#include <freertos/semphr.h>
#include <math.h>
#include <sys/time.h>

#include <atomic>

#include "certs/ca_bundle.h"
#include "config.h"
#include "settings.h"
#include "yield_stream.h"

namespace weather {
namespace {

SemaphoreHandle_t mutex = nullptr;
Data latest;
std::atomic<uint32_t> ver{0};
std::atomic<bool> updateRequested{false};
std::atomic<bool> cityRequested{false};
char cityName[64] = "";  // written before cityRequested is set

uint32_t lastAttemptMs = 0;
uint32_t lastSuccessMs = 0;
bool everAttempted = false;
uint32_t failures = 0;
char lastError[64] = "";

uint32_t nowUtc() {
  struct timeval tv;
  gettimeofday(&tv, nullptr);
  return tv.tv_sec > 1700000000 ? static_cast<uint32_t>(tv.tv_sec) : 0;
}

int8_t roundTemp(float t) { return static_cast<int8_t>(lroundf(t)); }

// Opens an HTTPS GET; on success the body can be read from http.getStream().
bool httpsGet(WiFiClientSecure& client, HTTPClient& http, const String& url) {
  client.setCACertBundle(ca_bundle);
  client.setTimeout(10);
  http.setTimeout(10000);
  http.setConnectTimeout(8000);
  http.useHTTP10(true);  // no chunked encoding, so the JSON can be parsed from the stream
  if (!http.begin(client, url)) {
    snprintf(lastError, sizeof(lastError), "bad URL");
    return false;
  }
  int code = http.GET();
  if (code != HTTP_CODE_OK) {
    snprintf(lastError, sizeof(lastError), "HTTP %d %s", code,
             code < 0 ? HTTPClient::errorToString(code).c_str() : "");
    http.end();
    return false;
  }
  return true;
}

bool fetch() {
  char url[512];
  snprintf(url, sizeof(url),
           "https://api.open-meteo.com/v1/forecast?latitude=%.4f&longitude=%.4f"
           "&current=temperature_2m,relative_humidity_2m,apparent_temperature,weather_code,is_day"
           "&hourly=temperature_2m,precipitation_probability,weather_code,is_day"
           "&daily=weather_code,temperature_2m_max,temperature_2m_min,precipitation_probability_max"
           "&timezone=auto&timeformat=unixtime&forecast_days=%d&forecast_hours=%d",
           settings::latitude(), settings::longitude(), kDays, kHours);

  WiFiClientSecure client;
  HTTPClient http;
  if (!httpsGet(client, http, url)) return false;

  JsonDocument doc;
  YieldingStream body(http.getStream());
  DeserializationError err = deserializeJson(doc, body);
  http.end();
  if (err) {
    snprintf(lastError, sizeof(lastError), "JSON: %s", err.c_str());
    return false;
  }
  JsonObject cur = doc["current"];
  if (cur.isNull()) {
    snprintf(lastError, sizeof(lastError), "unexpected response");
    return false;
  }

  Data d;
  d.valid = true;
  d.fetchedUtc = nowUtc();
  d.temp = cur["temperature_2m"] | 0.0f;
  d.feels = cur["apparent_temperature"] | 0.0f;
  d.humidity = cur["relative_humidity_2m"] | 0;
  d.code = cur["weather_code"] | 0;
  d.isDay = (cur["is_day"] | 1) != 0;

  JsonObject h = doc["hourly"];
  JsonArray ht = h["time"];
  for (size_t i = 0; i < ht.size() && d.hourCount < kHours; ++i) {
    Hour& o = d.hours[d.hourCount++];
    o.time = ht[i];
    o.temp = roundTemp(h["temperature_2m"][i] | 0.0f);
    o.code = h["weather_code"][i] | 0;
    o.pop = h["precipitation_probability"][i] | 0;
    o.isDay = (h["is_day"][i] | 1) != 0;
  }

  JsonObject dl = doc["daily"];
  JsonArray dt = dl["time"];
  for (size_t i = 0; i < dt.size() && d.dayCount < kDays; ++i) {
    Day& o = d.days[d.dayCount++];
    o.time = dt[i];
    o.code = dl["weather_code"][i] | 0;
    o.tmax = roundTemp(dl["temperature_2m_max"][i] | 0.0f);
    o.tmin = roundTemp(dl["temperature_2m_min"][i] | 0.0f);
    o.pop = dl["precipitation_probability_max"][i] | 0;
  }
  strlcpy(d.place, settings::place(), sizeof(d.place));

  xSemaphoreTake(mutex, portMAX_DELAY);
  latest = d;
  xSemaphoreGive(mutex);
  ver++;
  return true;
}

void geocode() {
  String name(cityName);
  String q;
  for (char c : name) {
    if (isalnum(static_cast<unsigned char>(c)) || c == '-' || c == '.') {
      q += c;
    } else {
      char enc[4];
      snprintf(enc, sizeof(enc), "%%%02X", static_cast<unsigned char>(c));
      q += enc;
    }
  }
  WiFiClientSecure client;
  HTTPClient http;
  String url = "https://geocoding-api.open-meteo.com/v1/search?count=1&language=en&name=" + q;
  if (!httpsGet(client, http, url)) {
    Serial.printf("[WEATHER] City lookup failed: %s\n", lastError);
    return;
  }
  JsonDocument doc;
  YieldingStream body(http.getStream());
  DeserializationError err = deserializeJson(doc, body);
  http.end();
  JsonObject r = doc["results"][0];
  if (err || r.isNull()) {
    Serial.printf("[WEATHER] City \"%s\" not found\n", cityName);
    return;
  }
  const char* found = r["name"] | cityName;
  float lat = r["latitude"] | 0.0f;
  float lon = r["longitude"] | 0.0f;
  if (!settings::setLocation(lat, lon, found)) {
    Serial.println("[WEATHER] ERROR: could not save location");
    return;
  }
  Serial.printf("[WEATHER] Location: %s, %s (%.3f, %.3f)\n", found, (const char*)(r["country"] | ""),
                lat, lon);
  updateRequested = true;
}

}  // namespace

void begin() { mutex = xSemaphoreCreateMutex(); }

void service() {
  if (cityRequested.exchange(false)) geocode();

  const uint32_t now = millis();
  bool due;
  if (updateRequested.exchange(false)) {
    due = true;
  } else if (!everAttempted) {
    due = true;
  } else if (lastSuccessMs != 0 && failures == 0) {
    due = now - lastSuccessMs >= config::kWeatherRefreshMs;
  } else {
    // Retry after failures with backoff: 1, 2, 4 ... up to 15 min.
    uint32_t wait = config::kWeatherRetryMs << (failures > 4 ? 4 : failures - 1);
    if (wait > config::kWeatherRefreshMs) wait = config::kWeatherRefreshMs;
    due = now - lastAttemptMs >= wait;
  }
  if (!due || nowUtc() == 0) return;  // TLS needs a valid clock

  everAttempted = true;
  lastAttemptMs = now;
  if (fetch()) {
    lastSuccessMs = now;
    failures = 0;
    lastError[0] = '\0';
    Data d;
    get(d);
    Serial.printf("[WEATHER] %s: %.1f C, %s, humidity %u%%, today %d-%d C, rain %u%%\n", d.place,
                  d.temp, describe(d.code), d.humidity, d.dayCount ? d.days[0].tmin : 0,
                  d.dayCount ? d.days[0].tmax : 0, d.dayCount ? d.days[0].pop : 0);
  } else {
    ++failures;
    Serial.printf("[WEATHER] Update failed: %s\n", lastError);
  }
}

bool get(Data& out) {
  if (!mutex) return false;
  xSemaphoreTake(mutex, portMAX_DELAY);
  out = latest;
  xSemaphoreGive(mutex);
  return out.valid;
}

uint32_t version() { return ver; }

uint32_t ageSeconds() {
  Data d;
  uint32_t now = nowUtc();
  if (!get(d) || now == 0 || d.fetchedUtc == 0) return UINT32_MAX;
  return now > d.fetchedUtc ? now - d.fetchedUtc : 0;
}

void requestUpdate() { updateRequested = true; }

void requestCity(const char* name) {
  strlcpy(cityName, name, sizeof(cityName));
  cityRequested = true;
}

const char* describe(uint8_t code) {
  switch (code) {
    case 0: return "Clear";
    case 1: return "Mainly clear";
    case 2: return "Partly cloudy";
    case 3: return "Overcast";
    case 45: case 48: return "Fog";
    case 51: return "Light drizzle";
    case 53: return "Drizzle";
    case 55: return "Heavy drizzle";
    case 56: case 57: return "Freezing drizzle";
    case 61: return "Light rain";
    case 63: return "Rain";
    case 65: return "Heavy rain";
    case 66: case 67: return "Freezing rain";
    case 71: return "Light snow";
    case 73: return "Snow";
    case 75: return "Heavy snow";
    case 77: return "Snow grains";
    case 80: return "Light showers";
    case 81: return "Showers";
    case 82: return "Heavy showers";
    case 85: case 86: return "Snow showers";
    case 95: return "Thunderstorm";
    case 96: case 99: return "Thunderstorm, hail";
    default: return "Unknown";
  }
}

Icon iconFor(uint8_t code) {
  if (code == 0) return Icon::Clear;
  if (code <= 2) return Icon::PartlyCloudy;
  if (code == 3) return Icon::Cloudy;
  if (code == 45 || code == 48) return Icon::Fog;
  if (code >= 51 && code <= 57) return Icon::Drizzle;
  if (code >= 61 && code <= 67) return Icon::Rain;
  if (code >= 71 && code <= 77) return Icon::Snow;
  if (code >= 80 && code <= 82) return Icon::Showers;
  if (code == 85 || code == 86) return Icon::Snow;
  if (code >= 95) return Icon::Thunder;
  return Icon::Cloudy;
}

void printStatus() {
  Serial.printf("[WEATHER] Location %s (%.3f, %.3f), forecast screen %s\n", settings::place(),
                settings::latitude(), settings::longitude(), settings::weatherCycle() ? "on" : "off");
  Data d;
  if (!get(d)) {
    Serial.printf("[WEATHER] No data yet%s%s\n", lastError[0] ? ", last error: " : "", lastError);
    return;
  }
  uint32_t age = ageSeconds();
  Serial.printf("[WEATHER] Now %.1f C (feels %.1f), %s, humidity %u%%, updated %lus ago%s%s\n",
                d.temp, d.feels, describe(d.code), d.humidity, (unsigned long)age,
                lastError[0] ? ", last error: " : "", lastError);
  for (int i = 0; i < d.dayCount; ++i) {
    struct tm t;
    time_t tt = d.days[i].time;
    localtime_r(&tt, &t);
    Serial.printf("[WEATHER]   %04d-%02d-%02d  %3d..%3d C  rain %3u%%  %s\n", t.tm_year + 1900,
                  t.tm_mon + 1, t.tm_mday, d.days[i].tmin, d.days[i].tmax, d.days[i].pop,
                  describe(d.days[i].code));
  }
}

}  // namespace weather
