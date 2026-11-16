// Chrono Node - a phrase of the day: Vietnamese proverbs and folk verses
// (tục ngữ, ca dao) and English proverbs, all traditional sayings.
#pragma once

#include <stdint.h>

namespace phrases {

enum class Lang : uint8_t { Vietnamese = 0, English = 1, Both = 2, Off = 3 };

// The day's phrases (days since 1970-01-01, local): the Vietnamese one, then
// the English one, as `lang` asks. Returns how many (0-2).
int forDay(int32_t day, Lang lang, const char* out[2]);

}  // namespace phrases
