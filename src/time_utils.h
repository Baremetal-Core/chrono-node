// Chrono Node - UTC <-> local time helpers using the active POSIX TZ.
#pragma once

#include <RTClib.h>
#include <stddef.h>
#include <time.h>

namespace time_utils {

// Converts a UTC DateTime (as stored in the DS3231) to local broken-down time.
void utcToLocal(const DateTime& utc, struct tm& local);

// Converts local wall-clock fields to UTC. Returns false for impossible dates
// (e.g. Feb 30) or times skipped by a DST change.
bool localToUtc(int year, int month, int day, int hour, int minute, int second, DateTime& utc);

// Formats as "YYYY-MM-DD HH:MM:SS +HHMM" in local time.
void formatLocal(const DateTime& utc, char* buf, size_t len);

}  // namespace time_utils
