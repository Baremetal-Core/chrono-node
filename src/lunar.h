// Chrono Node - Vietnamese lunar calendar (Âm lịch).
//
// Lunar years 1900-2100 come from a table generated from NASA JPL's DE440s
// ephemeris (src/lunar_table.h, tools/gen_lunar_table.py). Outside it, the
// standard astronomical method is calculated: months start on the day of the
// new moon (Meeus' truncated series), month 11 always contains the winter
// solstice, and in a 13-month year the first month without a major solar term
// is the leap month. Days are counted in UTC+7. The calculation emulates
// doubles on the ESP32 (~1 ms): call it once a day.
#pragma once

#include <stddef.h>

namespace lunar {

struct Date {
  int day;    // 1-30
  int month;  // 1-12
  int year;   // e.g. 2026 (the year of Tết that started it)
  bool leap;  // a leap month ("tháng nhuận")
};

Date fromSolar(int day, int month, int year);

// Days from the given solar date to the next Tết (lunar 1/1); 0 on Tết itself.
int daysToTet(int day, int month, int year);

// The year's name in the 60-year cycle, e.g. "Bính Ngọ" for 2026.
void yearName(int lunarYear, char* buf, size_t len);

}  // namespace lunar
