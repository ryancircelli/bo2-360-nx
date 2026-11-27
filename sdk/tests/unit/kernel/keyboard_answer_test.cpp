// FillKeyboardAnswer: the no-UI XamShowKeyboardUI answer (Fallout 3 birth-scene name prompt looped on a swapped name).
#include <catch2/catch_test_macros.hpp>

#include <rex/kernel/xam/text_input.h>

#include <string>

namespace {
// Guest text as the game stores it: big-endian UTF-16.
std::u16string BigEndian(std::u16string s) {
  for (auto& c : s) c = static_cast<char16_t>((c >> 8) | (c << 8));
  return s;
}
}  // namespace

TEST_CASE("keyboard answer keeps the game's default text unswapped") {
  const std::u16string guest = BigEndian(u"Bob");
  char16_t buffer[8];
  rex::kernel::xam::FillKeyboardAnswer(buffer, 8, guest, "Gamer");
  CHECK(std::u16string(buffer) == guest);
}

TEST_CASE("keyboard answer with no default gives the fallback, big-endian") {
  char16_t buffer[8];
  rex::kernel::xam::FillKeyboardAnswer(buffer, 8, {}, "Gamer");
  CHECK(std::u16string(buffer) == BigEndian(u"Gamer"));
}

TEST_CASE("keyboard answer truncates to the buffer with a NUL") {
  char16_t buffer[4];
  rex::kernel::xam::FillKeyboardAnswer(buffer, 4, {}, "Gamertag");
  CHECK(std::u16string(buffer) == BigEndian(u"Gam"));
}
