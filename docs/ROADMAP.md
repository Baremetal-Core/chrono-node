# Chrono Node – Feature Roadmap

Guiding rules, carried over from milestone 1:
- Offline-first: the DS3231 is always the timekeeper, and the network only corrects it.
- Network code never blocks rendering. Wi-Fi/HTTP run event-driven or in their own task.
- No secrets in git. Credentials live in NVS and are entered on the device or in a portal.
- Memory budget: the ESP32 has no PSRAM (~320 KB RAM) and Wi-Fi+TLS uses ~60–80 KB, so watch heap per milestone.
- One milestone = one branch/PR, a `pio run` that passes, and a hardware check on the CYD.

CYD resources used later: backlight GPIO21 (PWM), LDR GPIO34, RGB LED 4/16/17 (active-low),
speaker GPIO26, touch XPT2046 (CLK25 MOSI32 MISO39 CS33 IRQ36), SD (5/18/19/23).

---

## Decisions
| Topic | Decision |
|-------|----------|
| UI rendering | TFT_eSPI + anti-aliased smooth fonts + sprites (no LVGL) |
| Wi-Fi provisioning | WiFiManager library (captive portal) |
| Weather provider | Open-Meteo (no API key) |
| Tracking | This file; one GitHub issue per milestone, opened when work starts; one branch/PR per milestone |

---

## M2 – Foundation (time model, settings, CI) ✅
- DS3231 stores **UTC**. Local time comes from a POSIX TZ string (e.g. `<+07>-7`, or rules with DST).
- At boot the ESP32 system clock is seeded from the DS3231, with periodic re-seeding (~1 min).
  Rendering uses `localtime_r()`, and the DS3231 stays the source of truth.
- `settings` module on NVS (Preferences), starting with the timezone; later milestones add their own keys.
- Custom 4 MB partition table: 2 × 1.75 MB OTA app slots + 384 KB LittleFS.
- Version and git revision in the banner; GitHub Actions CI runs `pio run` and uploads `chrono-node-fw.bin`.
- Serial: `SET` takes local time and converts it, `TZ <posix>`, `INFO`.
- Note: the RTC currently holds local (+07) time, so re-SET once after this lands.

## M3 – Wi-Fi connectivity & provisioning ✅
- Non-blocking Wi-Fi state machine (event callbacks) with auto-reconnect and backoff.
- Provisioning: if no creds or connect fails → AP + captive portal (`Chrono-Node-XXXX`).
  Uses the WiFiManager library; credentials are stored by the ESP32 Wi-Fi stack in NVS.
- mDNS `chrono-node.local`. Wi-Fi status icon in a status bar.
- Clock keeps ticking normally throughout.

## M4 – NTP sync → DS3231 correction ✅
- SNTP (`pool.ntp.org`, configurable) with a sync callback.
- On sync: measure the DS3231 offset on a second edge and rewrite it (aligned to the second
  boundary) if it is off by > 100 ms. Log the offset and a drift estimate (ppm).
- Re-sync every 6 h plus on reconnect. Show the last-sync age and status (synced / stale / never).
- Clear the DS3231 lost-power flag automatically after a good sync.

## M5 – "Gorgeous" UI system ✅
- TFT_eSPI with anti-aliased smooth fonts compiled into flash (no filesystem upload step) and sprites.
- Custom anti-aliased digit font (e.g. a large Inter/Rajdhani/DSEG variant), seconds as a progress arc or small digits.
- Layout: a big time display, date with weekday, a status bar (Wi-Fi, NTP, indoor temperature from the DS3231's sensor).
- Themes: dark/minimal, accent colours, colour by time-of-day. Smooth digit transitions.
- Turn the always-on RGB LED off (or use it as a subtle status light).

## M6 – Touch & screens ✅ (calibration on this unit pending a stylus)
- XPT2046 driver with a calibration routine stored in NVS.
- Screens: Clock → Weather → Info/Settings. Swipe/tap navigation, on-device settings (12/24h, theme, brightness).

## M7 – Brightness & night mode ✅
- LEDC PWM backlight with gamma correction and smooth fades.
- Day and night brightness levels plus a night schedule, saved in NVS.
- Night theme: black background, pink→magenta digits and dim pink text, switched behind a fade.
- Auto-brightness dropped: the on-board LDR (GPIO34) reads 0 under any light on this unit.

## M8 – Weather ✅
- Open-Meteo API (free, no API key): current conditions, 24 h and 4 days, verified HTTPS + ArduinoJson.
- Location: city lookup (Open-Meteo geocoding) or lat/lon, saved in NVS; default Ho Chi Minh City.
- Weather line on the clock face plus a periodic forecast screen; icons drawn with AA primitives.
  Refresh every 15 min; cached data shown with its age when offline.

## M9 – Web configuration UI
- A small web page at `chrono-node.local` for all settings, Wi-Fi reset, status/diagnostics (heap, RSSI, drift, uptime).
- JSON API (`/api/status`, `/api/settings`) for future integrations.

## M10 – OTA updates & releases
- Firmware upload from the web UI (and ArduinoOTA for dev).
- GitHub Actions: tag `v*` → release with `chrono-node-fw.bin`. Optional: the device checks GitHub releases.
- Rollback on boot failure (app-rollback partition flag).

## M11 – Alarms & sound (optional)
- Alarms stored in NVS and edited via touch/web; speaker on GPIO26 (tones/DAC), snooze by tap.

## M12 – Sensors & smart-home (optional)
- An I2C environment sensor (SHT4x/BME280/SCD41) on the existing GPIO27/22 bus next to the DS3231.
- Optional I2C light sensor (e.g. BH1750) on the same bus for auto-brightness (the CYD LDR is unusable).
- Indoor climate display and history graph. MQTT with Home Assistant auto-discovery.

---

### Suggested order
M2 → M3 → M4 makes it a correct internet clock. M5 makes it gorgeous. M7 makes it livable
at night. M8 adds weather. Then M6, M9, M10, and finally M11 and M12 as extras.
