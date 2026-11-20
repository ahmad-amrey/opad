// opad::parse_double (opad/util.hpp): decimal numbers read the same on every platform and in every locale. On macOS
// before Xcode 26 std::from_chars has no floating point, so this runs its strtod_l path there (CI's macOS job).
#include <clocale>
#include <cmath>
#include <string>

#include "check.hpp"
#include "opad/util.hpp"

namespace {
// The number and how much of the text it took; taken -1: not a number.
std::pair<double, int> read(const std::string& s) {
  double v = -999;
  const char* end = opad::parse_double(s.data(), s.data() + s.size(), v);
  return {v, end ? int(end - s.data()) : -1};
}
}  // namespace

TEST(numbers_are_read_whole_or_up_to_what_follows) {
  CHECK(read("1.5") == std::make_pair(1.5, 3));
  CHECK(read("-0.25") == std::make_pair(-0.25, 5));
  CHECK(read(".5") == std::make_pair(0.5, 2));
  CHECK(read("3") == std::make_pair(3.0, 1));
  CHECK(read("1e3") == std::make_pair(1000.0, 3));
  CHECK(read("2.5E-1") == std::make_pair(0.25, 6));
  CHECK(read("7,8") == std::make_pair(7.0, 1));   // a comma ends it (a point list "x,y")
  CHECK(read("12 34") == std::make_pair(12.0, 2));
  CHECK(read("4mm") == std::make_pair(4.0, 1));
}

TEST(what_is_not_a_number_is_refused_as_from_chars_does) {
  CHECK(read("").second == -1);
  CHECK(read("abc").second == -1);
  CHECK(read(" 1").second == -1);   // no leading blanks
  CHECK(read("+1").second == -1);   // no plus sign (callers skip it themselves)
  CHECK(read("0x10").second == 1);  // not hexadecimal: the 0 alone
}

TEST(the_locale_does_not_change_the_decimal_point) {
  // A locale with a decimal comma, where the machine has one (de_DE, fr_FR...); skipped silently where it has none.
  for (const char* name : {"de_DE.UTF-8", "de_DE.utf8", "fr_FR.UTF-8", "German_Germany.1252"})
    if (std::setlocale(LC_ALL, name)) break;
  CHECK(read("1.5") == std::make_pair(1.5, 3));
  CHECK(read("1,5") == std::make_pair(1.0, 1));
  std::setlocale(LC_ALL, "C");
}

CHECK_MAIN()
