#include "time_utils.h"

namespace time_utils {

void utcToLocal(const DateTime& utc, struct tm& local) {
  time_t t = static_cast<time_t>(utc.unixtime());
  localtime_r(&t, &local);
}

bool localToUtc(int year, int month, int day, int hour, int minute, int second, DateTime& utc) {
  struct tm tm = {};
  tm.tm_year = year - 1900;
  tm.tm_mon = month - 1;
  tm.tm_mday = day;
  tm.tm_hour = hour;
  tm.tm_min = minute;
  tm.tm_sec = second;
  tm.tm_isdst = -1;
  time_t t = mktime(&tm);
  if (t == static_cast<time_t>(-1)) return false;

  // Reject input that does not round-trip (e.g. Feb 30, or a time skipped by DST).
  struct tm check;
  localtime_r(&t, &check);
  if (check.tm_year != year - 1900 || check.tm_mon != month - 1 || check.tm_mday != day ||
      check.tm_hour != hour || check.tm_min != minute || check.tm_sec != second) {
    return false;
  }
  utc = DateTime(static_cast<uint32_t>(t));
  return utc.isValid() && utc.year() >= 2000 && utc.year() <= 2099;
}

void formatLocal(const DateTime& utc, char* buf, size_t len) {
  struct tm local;
  utcToLocal(utc, local);
  strftime(buf, len, "%Y-%m-%d %H:%M:%S %z", &local);
}

}  // namespace time_utils
