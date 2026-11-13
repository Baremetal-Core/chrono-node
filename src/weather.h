// Chrono Node - weather from Open-Meteo (https://open-meteo.com, no API key).
//
// Fetched over HTTPS (certificate-verified) by the network task every 15 min
// while Wi-Fi is connected. Other tasks read a copy with get(). The last data
// is kept while offline and reported with its age.
#pragma once

#include <stddef.h>
#include <stdint.h>

namespace weather {

enum class Icon : uint8_t { Clear, PartlyCloudy, Cloudy, Fog, Drizzle, Rain, Showers, Thunder, Snow };

struct Hour {
  uint32_t time;  // UTC epoch
  int8_t temp;
  uint8_t code;   // WMO weather code
  uint8_t pop;    // precipitation probability %
  bool isDay;
};

struct Day {
  uint32_t time;  // local midnight, UTC epoch
  uint8_t code;
  int8_t tmax;
  int8_t tmin;
  uint8_t pop;
};

constexpr int kHours = 24;
constexpr int kDays = 4;

struct Data {
  bool valid = false;
  uint32_t fetchedUtc = 0;
  float temp = 0;
  float feels = 0;
  uint8_t humidity = 0;
  uint8_t code = 0;
  bool isDay = true;
  Hour hours[kHours] = {};
  int hourCount = 0;
  Day days[kDays] = {};
  int dayCount = 0;
  char place[32] = "";
};

// Call once from setup() before the network task starts.
void begin();

// Network task: call regularly while connected; fetches when due.
void service();

// Copies the latest data. Returns false if no data was ever fetched.
bool get(Data& out);

// Incremented on every successful update; lets the UI redraw only on change.
uint32_t version();

// Age of the data in seconds (UINT32_MAX if none or the clock isn't set).
uint32_t ageSeconds();

// Asynchronous requests handled by the network task.
void requestUpdate();
void requestCity(const char* name);  // geocode, save the location, then update

const char* describe(uint8_t code);  // e.g. "Light drizzle"
Icon iconFor(uint8_t code);

void printStatus();

}  // namespace weather
