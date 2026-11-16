#include "holidays.h"

namespace holidays {
namespace {

constexpr Holiday kHolidays[] = {
    {false, 1, 1, "TẾT DƯƠNG LỊCH"},
    {false, 14, 2, "VALENTINE"},
    {false, 8, 3, "QUỐC TẾ PHỤ NỮ"},
    {false, 1, 5, "QUỐC TẾ LAO ĐỘNG"},
    {false, 1, 6, "QUỐC TẾ THIẾU NHI"},
    {false, 20, 10, "PHỤ NỮ VIỆT NAM"},
    {false, 20, 11, "NHÀ GIÁO VIỆT NAM"},
    {false, 24, 12, "ĐÊM GIÁNG SINH"},
    {false, 25, 12, "GIÁNG SINH"},
    {true, 15, 1, "RẰM THÁNG GIÊNG"},
    {true, 10, 3, "GIỖ TỔ HÙNG VƯƠNG"},
    {true, 15, 4, "PHẬT ĐẢN"},
    {true, 5, 5, "TẾT ĐOAN NGỌ"},
    {true, 15, 7, "VU LAN"},
    {true, 15, 8, "TRUNG THU"},
    {true, 23, 12, "ÔNG CÔNG ÔNG TÁO"},
};

}  // namespace

const Holiday* today(int day, int month, const lunar::Date& l) {
  for (const Holiday& h : kHolidays) {
    if (h.lunar ? (!l.leap && l.day == h.day && l.month == h.month)
                : (day == h.day && month == h.month)) {
      return &h;
    }
  }
  return nullptr;
}

const Holiday* all(int& count) {
  count = sizeof(kHolidays) / sizeof(kHolidays[0]);
  return kHolidays;
}

}  // namespace holidays
