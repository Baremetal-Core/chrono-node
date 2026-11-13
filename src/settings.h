// Chrono Node - persistent settings stored in ESP32 NVS.
#pragma once

#include <stddef.h>
#include <stdint.h>

namespace settings {

// Loads settings from NVS and applies the timezone. Call once at boot.
void begin();

// Current POSIX TZ string, for example "<+07>-7".
const char* timezone();

// Validates, saves and applies a POSIX TZ string. Returns false if rejected.
bool setTimezone(const char* posixTz);

// NTP server host name, for example "pool.ntp.org".
const char* ntpServer();

// Validates and saves an NTP host name. Returns false if rejected.
bool setNtpServer(const char* host);

// Backlight levels in percent (0-100).
uint8_t brightnessDay();
uint8_t brightnessNight();
bool setBrightness(bool night, uint8_t percent);

// Night mode schedule in minutes after local midnight; the window may wrap
// past midnight (e.g. 22:00-06:30).
bool nightEnabled();
uint16_t nightStart();
uint16_t nightEnd();
bool setNightEnabled(bool enabled);
bool setNightWindow(uint16_t startMinute, uint16_t endMinute);

// True if night mode applies at `minuteOfDay` with the current schedule.
bool isNightAt(int minuteOfDay);

// Weather location (decimal degrees) and its display name.
float latitude();
float longitude();
const char* place();
// Saves the location. With `remember`, it's also put first in the recent list.
bool setLocation(float lat, float lon, const char* name, bool remember = true);

// Recently used weather locations (newest first), for the settings screen.
constexpr int kMaxRecentLocations = 5;
int recentLocationCount();
bool recentLocation(int index, char* name, size_t nameLen, float& lat, float& lon);

// 12-hour clock (with AM/PM) instead of 24-hour.
bool hour12();
bool setHour12(bool enabled);

// Scrolling banner messages (UTF-8), shown in rotation on the clock face.
int bannerCount();
const char* bannerText(int index);
bool addBanner(const char* text);
bool setBanner(int index, const char* text);
bool removeBanner(int index);
bool clearBanners();
bool bannerEnabled();
bool setBannerEnabled(bool enabled);
// Each message's period in minutes (0 = continuous). Index -1 is the default
// for new messages; setting it also sets every message.
uint8_t bannerEveryMinutes(int index = -1);
bool setBannerEveryMinutes(int index, uint8_t minutes);
// Changes whenever the messages or their periods change.
uint32_t bannerVersion();
uint8_t bannerSpeed();         // pixels per second
bool setBannerSpeed(uint8_t pxPerSecond);

// How long the forecast and photos stay up when they come up by themselves,
// and how long other screens wait without touch before the clock returns.
constexpr uint8_t kMinShowS = 3, kMaxShowS = 120;
constexpr uint16_t kMinIdleS = 5, kMaxIdleS = 600;
uint8_t forecastShowSeconds();
bool setForecastShowSeconds(uint8_t s);
uint8_t photoShowSeconds();
bool setPhotoShowSeconds(uint8_t s);
uint16_t screenIdleSeconds();
bool setScreenIdleSeconds(uint16_t s);

// Clock face colours: style (theme::Style), the custom style's two colours
// (RGB565, top << 16 | bottom) and effect flags (theme::kFx*).
uint8_t colorStyle();
bool setColorStyle(uint8_t style);
uint32_t customColors();
bool setCustomColors(uint16_t top, uint16_t bottom);
uint8_t effects();
bool setEffects(uint8_t fx);

// Animated colour speed (theme::Speed), the Mix style's minutes per style
// (1-60) and the screen change animation (theme::Transition).
uint8_t colorSpeed();
bool setColorSpeed(uint8_t speed);
uint8_t mixEveryMinutes();
bool setMixEveryMinutes(uint8_t minutes);
uint8_t transition();
bool setTransition(uint8_t t);

// Photo slideshow period in minutes (0 = off).
uint8_t photoEveryMinutes();
bool setPhotoEveryMinutes(uint8_t minutes);

// Periodic forecast screen on/off.
bool weatherCycle();
bool setWeatherCycle(bool enabled);

}  // namespace settings
