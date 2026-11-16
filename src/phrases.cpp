#include "phrases.h"

namespace phrases {
namespace {

const char* const kVietnamese[] = {
    "Có công mài sắt, có ngày nên kim.",
    "Uống nước nhớ nguồn.",
    "Ăn quả nhớ kẻ trồng cây.",
    "Một cây làm chẳng nên non, ba cây chụm lại nên hòn núi cao.",
    "Đi một ngày đàng, học một sàng khôn.",
    "Thất bại là mẹ thành công.",
    "Không thầy đố mày làm nên.",
    "Học thầy không tày học bạn.",
    "Lá lành đùm lá rách.",
    "Gần mực thì đen, gần đèn thì sáng.",
    "Tốt gỗ hơn tốt nước sơn.",
    "Cái nết đánh chết cái đẹp.",
    "Nước chảy đá mòn.",
    "Tích tiểu thành đại.",
    "Muốn biết phải hỏi, muốn giỏi phải học.",
    "Chớ thấy sóng cả mà ngã tay chèo.",
    "Ai ơi giữ chí cho bền, dù ai xoay hướng đổi nền mặc ai.",
    "Lời nói chẳng mất tiền mua, lựa lời mà nói cho vừa lòng nhau.",
    "Một con ngựa đau, cả tàu bỏ cỏ.",
    "Bầu ơi thương lấy bí cùng, tuy rằng khác giống nhưng chung một giàn.",
    "Nhiễu điều phủ lấy giá gương, người trong một nước phải thương nhau cùng.",
    "Công cha như núi Thái Sơn, nghĩa mẹ như nước trong nguồn chảy ra.",
    "Có chí thì nên.",
    "Kiến tha lâu cũng đầy tổ.",
    "Ăn có nhai, nói có nghĩ.",
    "Uốn lưỡi bảy lần trước khi nói.",
    "Đói cho sạch, rách cho thơm.",
    "Thương người như thể thương thân.",
    "Một miếng khi đói bằng một gói khi no.",
    "Hay học thì sang, hay làm thì có.",
    "Tiên học lễ, hậu học văn.",
    "Mưa dầm thấm lâu.",
    "Chậm mà chắc.",
    "Cẩn tắc vô áy náy.",
    "Ở hiền gặp lành.",
    "Gieo gió gặt bão.",
    "Sông có khúc, người có lúc.",
    "Sau cơn mưa trời lại sáng.",
    "Trăm hay không bằng tay quen.",
    "Trăm nghe không bằng một thấy.",
    "Có làm thì mới có ăn.",
    "Học ăn, học nói, học gói, học mở.",
    "Ăn trông nồi, ngồi trông hướng.",
    "Chị ngã em nâng.",
    "Anh em như thể tay chân.",
    "Bán anh em xa, mua láng giềng gần.",
    "Một điều nhịn, chín điều lành.",
    "Nói chín thì nên làm mười.",
    "Có đi mới đến, có học mới hay.",
    "Thuốc đắng dã tật, sự thật mất lòng.",
    "Cây ngay không sợ chết đứng.",
    "Giấy rách phải giữ lấy lề.",
    "Ếch ngồi đáy giếng coi trời bằng vung.",
    "Chim có tổ, người có tông.",
    "Lửa thử vàng, gian nan thử sức.",
    "Năng nhặt chặt bị.",
    "Thua keo này, bày keo khác.",
    "Hữu xạ tự nhiên hương.",
    "Biết thì thưa thốt, không biết thì dựa cột mà nghe.",
    "Một chữ cũng là thầy, nửa chữ cũng là thầy.",
};

const char* const kEnglish[] = {
    "Actions speak louder than words.",
    "A journey of a thousand miles begins with a single step.",
    "Where there's a will, there's a way.",
    "Practice makes perfect.",
    "Rome wasn't built in a day.",
    "The early bird catches the worm.",
    "Better late than never.",
    "Every cloud has a silver lining.",
    "Slow and steady wins the race.",
    "Honesty is the best policy.",
    "Knowledge is power.",
    "A friend in need is a friend indeed.",
    "Two heads are better than one.",
    "Don't count your chickens before they hatch.",
    "Look before you leap.",
    "Many hands make light work.",
    "An apple a day keeps the doctor away.",
    "Time and tide wait for no man.",
    "Good things come to those who wait.",
    "Fortune favours the bold.",
    "Laughter is the best medicine.",
    "Patience is a virtue.",
    "Don't judge a book by its cover.",
    "Kindness costs nothing.",
    "Little by little, one travels far.",
    "Tomorrow is another day.",
    "Home is where the heart is.",
    "No pain, no gain.",
    "Necessity is the mother of invention.",
    "When in Rome, do as the Romans do.",
    "The best things in life are free.",
    "You reap what you sow.",
    "Still waters run deep.",
    "A smooth sea never made a skilled sailor.",
    "Fall seven times, stand up eight.",
    "Learning is a treasure that follows its owner everywhere.",
    "Measure twice, cut once.",
    "Make hay while the sun shines.",
    "A stitch in time saves nine.",
    "Haste makes waste.",
    "It is never too late to learn.",
    "Birds of a feather flock together.",
    "One good turn deserves another.",
    "Silence is golden.",
    "The best time to plant a tree was twenty years ago; the second best is now.",
    "Count your blessings.",
    "Good things take time.",
    "Strike while the iron is hot.",
    "Hope for the best, prepare for the worst.",
    "Easy does it.",
    "The squeaky wheel gets the grease.",
    "Variety is the spice of life.",
    "Every dog has its day.",
    "There's no place like home.",
    "Great oaks from little acorns grow.",
    "A watched pot never boils.",
    "Don't put all your eggs in one basket.",
    "If at first you don't succeed, try, try again.",
    "The darkest hour is just before the dawn.",
    "The bamboo that bends is stronger than the oak that resists.",
};

template <typename T, int N>
const char* pick(T (&list)[N], int32_t n) {
  return list[((n % N) + N) % N];
}

}  // namespace

const char* forDay(int32_t day, Lang lang) {
  switch (lang) {
    case Lang::Vietnamese:
      return pick(kVietnamese, day);
    case Lang::English:
      return pick(kEnglish, day);
    case Lang::Both:
    default:
      // Even days Vietnamese, odd days English; each list goes in order.
      return day % 2 == 0 ? pick(kVietnamese, day / 2) : pick(kEnglish, day / 2);
  }
}

}  // namespace phrases
