// Chrono Node - Vietnamese holidays and festivals for the clock's date line.
#pragma once

#include <stdint.h>

#include "lunar.h"

namespace holidays {

struct Holiday {
  bool lunar;     // day/month of the lunar calendar (never a leap month)
  uint8_t day;
  uint8_t month;
  const char* name;  // UTF-8, upper case for the date line
};

// Today's holiday (solar date d/m, lunar date l), or nullptr.
const Holiday* today(int day, int month, const lunar::Date& l);

// All of them, in table order (for the console).
const Holiday* all(int& count);

}  // namespace holidays
