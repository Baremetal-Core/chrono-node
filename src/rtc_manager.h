// Chrono Node - DS3231 RTC access over I2C.
#pragma once

#include <RTClib.h>

class RtcManager {
 public:
  // Starts I2C on the configured pins and attempts to detect the DS3231.
  // Returns true if the RTC was found.
  bool begin();

  // Re-checks whether the DS3231 acknowledges on the bus.
  // Used to recover when the RTC is connected after boot.
  bool probe();

  bool isPresent() const { return present_; }

  // True if the DS3231 oscillator-stop flag is set (time is not trustworthy).
  bool lostPower();

  // Reads the current DS3231 time. Returns false (and marks the RTC as missing)
  // if the device does not respond, or false if the time read is invalid.
  bool read(DateTime& out);

  // Writes a new UTC time to the DS3231 and clears the oscillator-stop flag.
  bool setTime(const DateTime& dt);

  // DS3231 die temperature (0.25 °C resolution, about ±3 °C accuracy).
  bool temperatureC(float& out);

 private:
  bool acknowledges();

  RTC_DS3231 rtc_;
  bool present_ = false;
  bool driverStarted_ = false;
};
