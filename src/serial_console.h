// Chrono Node - minimal serial console for development.
//
// Commands (case-insensitive, terminated by Enter):
//   SET YYYY-MM-DD HH:MM:SS   set the DS3231 from local wall-clock time (stored as UTC)
//   GET                       print the current time (local and UTC)
//   TZ [posix]                show or set the timezone, e.g. TZ <+07>-7
//   INFO                      firmware, memory and RTC diagnostics
//   WIFI                      Wi-Fi status
//   WIFI PORTAL               open the Wi-Fi setup portal
//   WIFI FORGET               erase saved Wi-Fi credentials and open the portal
//   NTP                       NTP sync status
//   NTP SYNC                  sync with the NTP server now
//   NTP SERVER <host>         set and save the NTP server, then sync
//   BRIGHT [DAY|NIGHT <0-100>] show or set backlight levels (saved)
//   NIGHT [ON|OFF|HH:MM HH:MM] show or set the night mode schedule (saved)
//   WEATHER                   weather status and forecast
//   WEATHER UPDATE|SHOW       fetch now / show the forecast screen for 30 s
//   WEATHER CITY <name>       look up and save a location (Open-Meteo geocoding)
//   WEATHER LOC <lat> <lon> [name]  save a location by coordinates
//   WEATHER CYCLE ON|OFF      periodic forecast screen
//   FORMAT 12|24              12- or 24-hour clock (saved)
//   BANNER                    list banner messages and settings
//   BANNER ADD <text>         add a message (UTF-8, up to 120 bytes; max 5)
//   BANNER SET <n> <text>     replace message n;  BANNER DEL <n>;  BANNER CLEAR
//   BANNER ON|OFF             enable/disable;  BANNER EVERY <min> (0 = continuous)
//   BANNER SPEED <px/s>       scroll speed (20-200);  BANNER SHOW: scroll the next one now
//   TOUCH                     print touch readings for 10 s
//   TOUCH CAL                 on-screen touch calibration (saved)
//   SCREEN <name>             go to CLOCK, FORECAST, INFO or SETTINGS
//   SETEPOCH <unix>           set the DS3231 from a UTC Unix time (used by the web UI)
//   REBOOT                    restart the clock
//   OTA                       firmware partitions and update state
//   TEST                      colour test pattern for 20 s (panel variant check)
//   TEST PALETTE              the five day-cycle palettes side by side for 30 s
//   SHOT                      dump the screen (tools/screenshot.py turns it into a PNG)
//   HELP                      list commands
#pragma once

class DisplayManager;
class RtcManager;

// Non-blocking; call from loop(). Returns true if the RTC time, timezone,
// brightness or night schedule changed.
bool serialConsolePoll(RtcManager& rtc, DisplayManager& display);

// Runs one command line as if typed (used by the web UI). Main task only.
bool serialConsoleExecute(RtcManager& rtc, DisplayManager& display, const char* command);

void serialConsolePrintHelp();
