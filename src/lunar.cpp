#include "lunar.h"

#include <math.h>
#include <stdio.h>

#include "lunar_table.h"

namespace lunar {
namespace {

constexpr double kTimeZone = 7.0;  // Vietnam
constexpr double kDr = M_PI / 180;
constexpr double kSynodicMonth = 29.530588853;
constexpr double kNewMoon1900 = 2415021.076998695;  // JD of a reference new moon

long julianDay(int d, int m, int y) {
  const int a = (14 - m) / 12;
  const long yy = y + 4800 - a;
  const int mm = m + 12 * a - 3;
  return d + (153 * mm + 2) / 5 + 365 * yy + yy / 4 - yy / 100 + yy / 400 - 32045;
}

// The k-th new moon after 1900-01-01 as a local day number.
long newMoonDay(long k) {
  const double t = k / 1236.85, t2 = t * t, t3 = t2 * t;
  double jd = 2415020.75933 + 29.53058868 * k + 0.0001178 * t2 - 0.000000155 * t3;
  jd += 0.00033 * sin((166.56 + 132.87 * t - 0.009173 * t2) * kDr);
  const double m = 359.2242 + 29.10535608 * k - 0.0000333 * t2 - 0.00000347 * t3;  // sun anomaly
  const double mp = 306.0253 + 385.81691806 * k + 0.0107306 * t2 + 0.00001236 * t3;  // moon
  const double f = 21.2964 + 390.67050646 * k - 0.0016528 * t2 - 0.00000239 * t3;  // latitude
  double c = (0.1734 - 0.000393 * t) * sin(m * kDr) + 0.0021 * sin(2 * kDr * m);
  c += -0.4068 * sin(mp * kDr) + 0.0161 * sin(kDr * 2 * mp) - 0.0004 * sin(kDr * 3 * mp);
  c += 0.0104 * sin(kDr * 2 * f) - 0.0051 * sin(kDr * (m + mp)) - 0.0074 * sin(kDr * (m - mp));
  c += 0.0004 * sin(kDr * (2 * f + m)) - 0.0004 * sin(kDr * (2 * f - m));
  c += -0.0006 * sin(kDr * (2 * f + mp)) + 0.0010 * sin(kDr * (2 * f - mp));
  c += 0.0005 * sin(kDr * (2 * mp + m));
  const double deltaT = t < -11 ? 0.001 + 0.000839 * t + 0.0002261 * t2 - 0.00000845 * t3 -
                                      0.000000081 * t * t3
                                : -0.000278 + 0.000265 * t + 0.000262 * t2;
  return static_cast<long>(floor(jd + c - deltaT + 0.5 + kTimeZone / 24));
}

// Which of the 12 major solar terms (30° of sun longitude each) the day is in.
int solarTerm(long day) {
  const double t = (day - 0.5 - kTimeZone / 24 - 2451545.0) / 36525, t2 = t * t;
  const double m = 357.52910 + 35999.05030 * t - 0.0001559 * t2 - 0.00000048 * t * t2;
  const double l0 = 280.46645 + 36000.76983 * t + 0.0003032 * t2;
  const double dl = (1.914600 - 0.004817 * t - 0.000014 * t2) * sin(kDr * m) +
                    (0.019993 - 0.000101 * t) * sin(kDr * 2 * m) + 0.000290 * sin(kDr * 3 * m);
  double l = (l0 + dl) * kDr;
  l -= 2 * M_PI * floor(l / (2 * M_PI));
  return static_cast<int>(floor(l / M_PI * 6));
}

// Start of the lunar month 11 (the one with the winter solstice) of a year.
long month11(int year) {
  const long k = static_cast<long>(floor((julianDay(31, 12, year) - 2415021) / kSynodicMonth));
  const long nm = newMoonDay(k);
  return solarTerm(nm) >= 9 ? newMoonDay(k - 1) : nm;
}

// In a 13-month year starting at month 11 `a11`: the offset of the leap month.
int leapMonthOffset(long a11) {
  const long k = static_cast<long>(floor((a11 - kNewMoon1900) / kSynodicMonth + 0.5));
  int i = 1;
  int arc = solarTerm(newMoonDay(k + i));
  int last;
  do {
    last = arc;
    ++i;
    arc = solarTerm(newMoonDay(k + i));
  } while (arc != last && i < 14);
  return i - 1;
}

// Day number of lunar d/m/y (not leap).
long toDayNumber(int d, int m, int y) {
  long a11, b11;
  if (m < 11) {
    a11 = month11(y - 1);
    b11 = month11(y);
  } else {
    a11 = month11(y);
    b11 = month11(y + 1);
  }
  const long k = static_cast<long>(floor(0.5 + (a11 - kNewMoon1900) / kSynodicMonth));
  int off = m - 11;
  if (off < 0) off += 12;
  if (b11 - a11 > 365 && off >= leapMonthOffset(a11)) ++off;
  return newMoonDay(k + off) + d - 1;
}

bool inTable(int lunarYear) { return lunarYear >= kTableFirstYear && lunarYear <= kTableLastYear; }

// Day number of Tết (lunar 1/1) of a year in the table.
long tetDay(int lunarYear) {
  return julianDay(1, 1, lunarYear) + ((kTable[lunarYear - kTableFirstYear] >> 17) & 0x3F);
}

// The lunar date from the table; false outside it.
bool fromTable(long dayNumber, int year, Date& out) {
  int ly = year;
  if (!inTable(ly) || dayNumber < tetDay(ly)) --ly;
  if (!inTable(ly) || dayNumber < tetDay(ly)) return false;
  const uint32_t e = kTable[ly - kTableFirstYear];
  const int leapMonth = (e >> 13) & 0xF;
  long start = tetDay(ly);
  int month = 1;
  bool leap = false;
  for (int i = 0; i < (leapMonth ? 13 : 12); ++i) {
    const int length = (e >> i) & 1 ? 30 : 29;
    if (dayNumber < start + length) {
      out = {static_cast<int>(dayNumber - start + 1), month, ly, leap};
      return true;
    }
    start += length;
    if (!leap && month == leapMonth) {
      leap = true;  // the leap month repeats this month's number
    } else {
      leap = false;
      ++month;
    }
  }
  return false;  // after the table's last year
}

// The astronomical calculation, for dates outside the table.
Date fromSolarFormula(long dayNumber, int year) {
  const long k = static_cast<long>(floor((dayNumber - kNewMoon1900) / kSynodicMonth));
  long monthStart = newMoonDay(k + 1);
  if (monthStart > dayNumber) monthStart = newMoonDay(k);
  long a11 = month11(year), b11 = a11;
  Date out = {};
  if (a11 >= monthStart) {
    out.year = year;
    a11 = month11(year - 1);
  } else {
    out.year = year + 1;
    b11 = month11(year + 1);
  }
  out.day = static_cast<int>(dayNumber - monthStart + 1);
  const int diff = static_cast<int>((monthStart - a11) / 29);
  out.month = diff + 11;
  if (b11 - a11 > 365) {
    const int leapDiff = leapMonthOffset(a11);
    if (diff >= leapDiff) {
      out.month = diff + 10;
      out.leap = diff == leapDiff;
    }
  }
  if (out.month > 12) out.month -= 12;
  if (out.month >= 11 && diff < 4) out.year -= 1;
  return out;
}

}  // namespace

Date fromSolar(int day, int month, int year) {
  const long dayNumber = julianDay(day, month, year);
  Date out;
  if (fromTable(dayNumber, year, out)) return out;
  return fromSolarFormula(dayNumber, year);
}

int daysToTet(int day, int month, int year) {
  const Date l = fromSolar(day, month, year);
  if (l.month == 1 && l.day == 1 && !l.leap) return 0;
  const long today = julianDay(day, month, year);
  const long next = inTable(l.year + 1) ? tetDay(l.year + 1) : toDayNumber(1, 1, l.year + 1);
  return static_cast<int>(next - today);
}

void yearName(int lunarYear, char* buf, size_t len) {
  static const char* const kStems[] = {"Giáp", "Ất",   "Bính", "Đinh", "Mậu",
                                       "Kỷ",   "Canh", "Tân",  "Nhâm", "Quý"};
  static const char* const kBranches[] = {"Tý",  "Sửu", "Dần",  "Mão",  "Thìn", "Tỵ",
                                          "Ngọ", "Mùi", "Thân", "Dậu", "Tuất", "Hợi"};
  snprintf(buf, len, "%s %s", kStems[(lunarYear + 6) % 10], kBranches[(lunarYear + 8) % 12]);
}

}  // namespace lunar
