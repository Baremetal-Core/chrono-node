#include "home.h"

#include <ArduinoJson.h>
#include <HTTPClient.h>
#include <WiFiClient.h>
#include <WiFiClientSecure.h>

#include <atomic>
#include <memory>

#include "certs/ca_bundle.h"
#include "mem_lease.h"
#include "settings.h"

namespace home {
namespace {

constexpr int kMax = config::kHaMaxDevices;

SemaphoreHandle_t mutex = nullptr;
Device devs[kMax] = {};
int devCount = 0;
char pendingFrom[kMax][20] = {};  // the state when it was switched
uint32_t pendingSinceMs[kMax] = {};
std::atomic<uint32_t> ver{0};
std::atomic<bool> active{false};
std::atomic<uint32_t> webSeenMs{0};  // the web page is showing the states
std::atomic<bool> pollNow{false};
std::atomic<uint32_t> switchWanted{0};  // bit per device
uint32_t lastPollMs = 0;
uint32_t pollGapMs = config::kHaPollMs;
char lastError[80] = "";
bool giveUp = false;  // the last request got no answer, or the token was refused

bool startsWith(const char* s, const char* prefix) { return strncmp(s, prefix, strlen(prefix)) == 0; }

Kind kindOf(const char* entity) {
  if (startsWith(entity, "scene.") || startsWith(entity, "script.")) return Kind::Run;
  if (startsWith(entity, "climate.") || startsWith(entity, "media_player.")) return Kind::OnOff;
  return Kind::Toggle;
}

bool switchable(const char* entity) {
  static const char* const kDomains[] = {"switch.", "light.",   "fan.",     "input_boolean.",
                                         "scene.",  "script.",  "climate.", "media_player."};
  for (const char* d : kDomains) {
    if (startsWith(entity, d)) return true;
  }
  return false;
}

// Holds the memory lease only for https (TLS needs the room).
struct LeaseIf {
  bool on;
  explicit LeaseIf(bool b) : on(b) {
    if (on) mem_lease::acquire();
  }
  ~LeaseIf() {
    if (on) mem_lease::release();
  }
};

// A response body in memory, for the read callbacks.
class MemStream : public Stream {
 public:
  explicit MemStream(const String& s) : s_(s) {}
  int available() override { return s_.length() - i_; }
  int read() override { return i_ < s_.length() ? static_cast<uint8_t>(s_[i_++]) : -1; }
  int peek() override { return i_ < s_.length() ? static_cast<uint8_t>(s_[i_]) : -1; }
  size_t write(uint8_t) override { return 0; }

 private:
  const String& s_;
  size_t i_ = 0;
};

// Keeps the first `cap` bytes of a response body. The rest is read and
// dropped, so a kept-alive connection is ready for the next request.
class BodySink : public Stream {
 public:
  explicit BodySink(size_t cap) : cap_(cap) {}
  size_t write(uint8_t c) override { return write(&c, 1); }
  size_t write(const uint8_t* p, size_t n) override {
    for (size_t i = 0; i < n; ++i) {
      if (text.length() < cap_) {
        text += static_cast<char>(p[i]);
      } else {
        overflow = true;
      }
    }
    return n;
  }
  int available() override { return 0; }
  int read() override { return -1; }
  int peek() override { return -1; }
  String text;
  bool overflow = false;

 private:
  size_t cap_;
};

// Plain-http requests share one kept-alive connection. A new connection per
// request would leave each one in TIME_WAIT for 2 minutes on the clock:
// polling 6 devices every few seconds keeps lwIP's 16 TCP slots full, and new
// connections, the web page's too, get reset.
WiFiClient keptClient;
HTTPClient keptHttp;
std::atomic<bool> dropKept{false};  // the address changed

// One request to Home Assistant. `body` nullptr: GET, else POST. On 200/201
// `read` gets the response body and returns success. `streamed`: a big body
// (the device list) streams through its own connection instead of memory.
template <typename Fn>
bool request(const String& path, const char* body, Fn&& read, bool streamed = false) {
  String url = settings::haUrl();
  while (url.endsWith("/")) url.remove(url.length() - 1);
  url += path;
  const bool https = url.startsWith("https://");
  const bool keep = !https && !streamed;
  if (dropKept.exchange(false)) keptClient.stop();
  LeaseIf lease(https);
  WiFiClient plain;
  std::unique_ptr<WiFiClientSecure> secure;
  HTTPClient oneOff;
  HTTPClient& http = keep ? keptHttp : oneOff;
  http.setConnectTimeout(config::kHaTimeoutMs);
  http.setTimeout(config::kHaTimeoutMs);
  http.setReuse(keep);
  http.useHTTP10(!keep);  // one-off: no chunked encoding, the body streams as is
  bool ok;
  if (https) {
    secure.reset(new WiFiClientSecure);
    secure->setCACertBundle(ca_bundle);
    ok = http.begin(*secure, url);
  } else {
    ok = http.begin(keep ? keptClient : plain, url);
  }
  giveUp = !ok;
  if (!ok) {
    snprintf(lastError, sizeof(lastError), "bad address %s", settings::haUrl());
    return false;
  }
  http.addHeader("Authorization", String("Bearer ") + settings::haToken());
  http.addHeader("Content-Type", "application/json");
  const bool reused = keep && keptClient.connected();
  int code = body ? http.POST(reinterpret_cast<uint8_t*>(const_cast<char*>(body)), strlen(body)) : http.GET();
  if (code < 0 && code != HTTPC_ERROR_CONNECTION_REFUSED && reused && !body) {
    // Home Assistant may have just closed the idle connection: once more on a
    // new one (only reads; a switch could have gone through).
    keptClient.stop();
    code = http.GET();
  }
  giveUp = code < 0 || code == 401;
  if (code != 200 && code != 201) {
    if (code == 401) {
      snprintf(lastError, sizeof(lastError), "HTTP 401: the access token was refused");
    } else if (code < 0) {
      snprintf(lastError, sizeof(lastError), "%s", HTTPClient::errorToString(code).c_str());
    } else {
      snprintf(lastError, sizeof(lastError), "HTTP %d for %s", code, path.c_str());
    }
    if (keep) keptClient.stop();  // its unread body would garble the next response
    http.end();
    return false;
  }
  lastError[0] = '\0';
  if (!keep) {
    WiFiClient& stream = *http.getStreamPtr();
    stream.setTimeout((config::kHaTimeoutMs + 999) / 1000);  // seconds
    ok = read(stream);
    http.end();
    return ok;
  }
  BodySink sink(config::kHaBodyMaxBytes);
  if (http.getSize() > 0) sink.text.reserve(min<int>(http.getSize(), config::kHaBodyMaxBytes));
  const int n = http.writeToStream(&sink);  // the whole body, chunked or not
  http.end();
  if (n < 0) {
    snprintf(lastError, sizeof(lastError), "%s", HTTPClient::errorToString(n).c_str());
    keptClient.stop();
    return false;
  }
  if (sink.overflow) {
    snprintf(lastError, sizeof(lastError), "answer too big for %s", path.c_str());
    return false;
  }
  MemStream in(sink.text);
  return read(in);
}

void setState(int i, const char* state, const char* name) {
  const uint32_t now = millis();
  xSemaphoreTake(mutex, portMAX_DELAY);
  Device& d = devs[i];
  bool changed = strcmp(d.state, state) != 0;
  strlcpy(d.state, state, sizeof(d.state));
  if (name[0] && strcmp(d.name, name) != 0) {
    strlcpy(d.name, name, sizeof(d.name));
    changed = true;
  }
  if (d.pending && (strcmp(state, pendingFrom[i]) != 0 || d.kind == Kind::Run ||
                    now - pendingSinceMs[i] > config::kHaPendingMs)) {
    d.pending = false;
    changed = true;
  }
  xSemaphoreGive(mutex);
  if (changed) ++ver;
}

void readState(int i) {
  char entity[64];
  xSemaphoreTake(mutex, portMAX_DELAY);
  strlcpy(entity, devs[i].entity, sizeof(entity));
  xSemaphoreGive(mutex);
  JsonDocument filter;
  filter["state"] = true;
  filter["attributes"]["friendly_name"] = true;
  JsonDocument doc;
  const bool ok = request(String("/api/states/") + entity, nullptr, [&](Stream& s) {
    return !deserializeJson(doc, s, DeserializationOption::Filter(filter));
  });
  setState(i, ok ? (doc["state"] | "") : "unavailable", doc["attributes"]["friendly_name"] | "");
}

void doSwitch(int i) {
  Device d;
  xSemaphoreTake(mutex, portMAX_DELAY);
  if (i >= devCount) {
    xSemaphoreGive(mutex);
    return;
  }
  d = devs[i];
  xSemaphoreGive(mutex);
  char domain[24];
  strlcpy(domain, d.entity, min<size_t>(sizeof(domain), strcspn(d.entity, ".") + 1));
  const char* service = d.kind == Kind::Toggle ? "toggle"
                        : d.kind == Kind::Run  ? "turn_on"
                        : isOn(d)              ? "turn_off"
                                               : "turn_on";
  char body[96];
  snprintf(body, sizeof(body), "{\"entity_id\":\"%s\"}", d.entity);
  const bool ok = request(String("/api/services/") + domain + "/" + service, body, [](Stream&) { return true; });
  Serial.printf("[HOME] %s.%s %s: %s\n", domain, service, d.entity, ok ? "sent" : lastError);
  if (!ok) {
    xSemaphoreTake(mutex, portMAX_DELAY);
    devs[i].pending = false;
    xSemaphoreGive(mutex);
    ++ver;
  }
}

}  // namespace

void begin() {
  if (!mutex) mutex = xSemaphoreCreateMutex();
  dropKept = true;
  Device next[kMax] = {};
  int n = 0;
  const char* p = settings::haEntities();
  while (*p && n < kMax) {
    const size_t len = strcspn(p, ",");
    if (len > 0 && len < sizeof(next[n].entity)) {
      Device& d = next[n];
      memcpy(d.entity, p, len);
      d.entity[len] = '\0';
      if (switchable(d.entity)) {
        strlcpy(d.name, strchr(d.entity, '.') + 1, sizeof(d.name));
        d.kind = kindOf(d.entity);
        ++n;
      }
    }
    p += len;
    if (*p == ',') ++p;
  }
  xSemaphoreTake(mutex, portMAX_DELAY);
  memcpy(devs, next, sizeof(devs));
  devCount = n;
  xSemaphoreGive(mutex);
  pollNow = true;
  ++ver;
}

bool configured() { return settings::haUrl()[0] && settings::haToken()[0] && devCount > 0; }

void service() {
  if (!configured()) return;
  const uint32_t sw = switchWanted.exchange(0);
  if (sw) {
    for (int i = 0; i < config::kHaMaxDevices; ++i) {
      if (sw & (1u << i)) doSwitch(i);
    }
    lastPollMs = millis() - config::kHaPollMs + 400;  // read the new state shortly
    pollGapMs = config::kHaPollMs;
  }
  const uint32_t now = millis();
  bool anyPending = false;
  xSemaphoreTake(mutex, portMAX_DELAY);
  for (int i = 0; i < devCount; ++i) anyPending |= devs[i].pending;
  xSemaphoreGive(mutex);
  const bool watched = active || anyPending || now - webSeenMs < 10000;
  const bool due = pollNow.exchange(false) || (watched && now - lastPollMs >= pollGapMs);
  if (!due) return;
  for (int i = 0; i < devCount; ++i) {
    readState(i);
    if (giveUp) {
      // Home Assistant is down, the address is wrong or the token was refused:
      // the other devices would fail the same way (after a timeout).
      for (int j = i + 1; j < devCount; ++j) setState(j, "unavailable", "");
      break;
    }
  }
  // From the end of the poll; wait longer while Home Assistant can't be used,
  // so the web page (to fix the address) and the weather still get their turn.
  lastPollMs = millis();
  pollGapMs = giveUp ? config::kHaRetryMs : config::kHaPollMs;
}

String lastErrorText() { return lastError; }

int devices(Device* out) {
  if (!mutex) return 0;
  xSemaphoreTake(mutex, portMAX_DELAY);
  const int n = devCount;
  memcpy(out, devs, sizeof(Device) * n);
  xSemaphoreGive(mutex);
  return n;
}

uint32_t version() { return ver; }

void keepFresh() { webSeenMs = millis(); }

void setActive(bool shown) {
  if (shown && !active) pollNow = true;
  active = shown;
}

bool requestSwitch(int index) {
  if (!mutex || index < 0) return false;
  xSemaphoreTake(mutex, portMAX_DELAY);
  const bool ok = index < devCount && !devs[index].pending;
  if (ok) {
    devs[index].pending = true;
    devs[index].target = !isOn(devs[index]);
    strlcpy(pendingFrom[index], devs[index].state, sizeof(pendingFrom[index]));
    pendingSinceMs[index] = millis();
  }
  xSemaphoreGive(mutex);
  if (!ok) return false;
  switchWanted.fetch_or(1u << index);
  ++ver;
  return true;
}

bool isOn(const Device& d) {
  static const char* const kOff[] = {"off", "standby", "unavailable", "unknown", "idle", ""};
  if (d.kind == Kind::Run) return false;
  for (const char* s : kOff) {
    if (strcmp(d.state, s) == 0) return false;
  }
  return true;
}

bool shownOn(const Device& d) { return d.pending && d.kind != Kind::Run ? d.target : isOn(d); }

bool listEntities(String& json, String& error) {
  if (!settings::haUrl()[0] || !settings::haToken()[0]) {
    error = "set the Home Assistant address and access token first";
    return false;
  }
  JsonDocument filter;
  filter["entity_id"] = true;
  filter["attributes"]["friendly_name"] = true;
  int n = 0;
  json = "[";
  // /api/states lists every entity (it can be hundreds of KB): read it one
  // element at a time and keep only the switchable ones.
  const bool ok = request("/api/states", nullptr, [&](Stream& s) {
    if (!s.find("[")) return false;
    do {
      JsonDocument doc;
      if (deserializeJson(doc, s, DeserializationOption::Filter(filter))) return false;
      const char* id = doc["entity_id"] | "";
      if (!switchable(id) || n >= 300) continue;
      JsonDocument item;
      item["id"] = id;
      item["name"] = doc["attributes"]["friendly_name"] | id;
      String one;
      serializeJson(item, one);  // (serializing into `json` would replace it)
      if (n++) json += ",";
      json += one;
    } while (s.findUntil(",", "]"));
    return true;
  }, true);
  json += "]";
  if (!ok) error = lastError[0] ? lastError : "couldn't read the device list";
  return ok;
}

void printStatus() {
  Serial.printf("[HOME] Home Assistant %s, token %s, %d device(s)%s%s\n",
                settings::haUrl()[0] ? settings::haUrl() : "(not set)",
                settings::haToken()[0] ? "set" : "not set", devCount,
                lastError[0] ? ", last error: " : "", lastError);
  Device d[kMax];
  const int n = devices(d);
  for (int i = 0; i < n; ++i) {
    Serial.printf("[HOME]   %d %-32s %-20s %s%s\n", i + 1, d[i].entity, d[i].name,
                  d[i].state[0] ? d[i].state : "?", d[i].pending ? " (switching)" : "");
  }
}

}  // namespace home
