// Chrono Node - a phrase of the day: Vietnamese proverbs and folk verses
// (tục ngữ, ca dao) and English proverbs, all traditional sayings.
#pragma once

#include <stdint.h>

namespace phrases {

enum class Lang : uint8_t { Vietnamese = 0, English = 1, Both = 2 };

// The phrase for a day (days since 1970-01-01, local). With Both, Vietnamese
// and English take turns day by day.
const char* forDay(int32_t day, Lang lang);

}  // namespace phrases
