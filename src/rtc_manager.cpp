#include "rtc_manager.h"

#include <Arduino.h>
#include <Wire.h>

#include "config.h"

bool RtcManager::begin() {
  // Always pass explicit pins: SDA=GPIO27, SCL=GPIO22 (CYD left connector).
  bool ok = Wire.begin(config::kI2cSdaPin, config::kI2cSclPin, config::kI2cFrequencyHz);
  if (ok) {
    Serial.printf("[I2C] SDA=GPIO%d SCL=GPIO%d @ %lu Hz\n", config::kI2cSdaPin,
                  config::kI2cSclPin, (unsigned long)config::kI2cFrequencyHz);
  } else {
    Serial.printf("[I2C] ERROR: failed to start bus on SDA=GPIO%d SCL=GPIO%d\n",
                  config::kI2cSdaPin, config::kI2cSclPin);
    return false;
  }
  return probe();
}

bool RtcManager::acknowledges() {
  Wire.beginTransmission(config::kDs3231Address);
  return Wire.endTransmission() == 0;
}

bool RtcManager::probe() {
  if (!acknowledges()) {
    present_ = false;
    return false;
  }
  // RTClib's begin() calls Wire.begin() without pins internally; that is a
  // no-op here because the bus has already been started on GPIO27/GPIO22.
  if (!driverStarted_) {
    driverStarted_ = rtc_.begin(&Wire);
  }
  present_ = driverStarted_;
  return present_;
}

bool RtcManager::lostPower() {
  if (!present_) return false;
  return rtc_.lostPower();
}

bool RtcManager::read(DateTime& out) {
  if (!present_ || !acknowledges()) {
    present_ = false;
    return false;
  }
  out = rtc_.now();
  return out.isValid();
}

bool RtcManager::setTime(const DateTime& dt) {
  if (!present_ || !dt.isValid()) return false;
  rtc_.adjust(dt);  // also clears the oscillator-stop flag
  DateTime check;
  return read(check) && check.isValid();
}
