#include <catch2/catch_test_macros.hpp>
#include <rex/types.h>

namespace rex::kernel::xam {
u32 lstrlenW_entry(mapped_u16);
u32 XamIsSystemExperienceTitleId_entry(u32);
u32 XamIsSystemTitleId_entry(u32);
u32 XamIsXbox1TitleId_entry(u32);
}
using namespace rex::kernel::xam;

TEST_CASE("Title classifiers distinguish retail, arcade and dashboard titles", "[xam]") {
  CHECK(XamIsSystemTitleId_entry(0) == 1);
  CHECK(XamIsSystemTitleId_entry(0xFFFE07D1) == 1);
  CHECK(XamIsSystemTitleId_entry(0x584D07D1) == 1);
  CHECK(XamIsSystemTitleId_entry(0x5841089A) == 0);
  CHECK(XamIsSystemTitleId_entry(0x415608C3) == 0);
  CHECK(XamIsSystemTitleId_entry(0x4D5308BC) == 0);
}
TEST_CASE("System experience titles use app publishers and two exact XN ids", "[xam]") {
  CHECK(XamIsSystemExperienceTitleId_entry(0x584807D1) == 1);
  CHECK(XamIsSystemExperienceTitleId_entry(0x584A1234) == 1);
  CHECK(XamIsSystemExperienceTitleId_entry(0x584E07D1) == 1);
  CHECK(XamIsSystemExperienceTitleId_entry(0x584E07D2) == 1);
  CHECK(XamIsSystemExperienceTitleId_entry(0x584E07D3) == 0);
  CHECK(XamIsSystemExperienceTitleId_entry(0x5841089A) == 0);
  CHECK(XamIsSystemExperienceTitleId_entry(0xFFFE07D1) == 0);
}
TEST_CASE("Original Xbox title classification handles version bits and boundary", "[xam]") {
  CHECK(XamIsXbox1TitleId_entry(0x41430006) == 1);
  CHECK(XamIsXbox1TitleId_entry(0x41438006) == 1);
  CHECK(XamIsXbox1TitleId_entry(0x414307CF) == 1);
  CHECK(XamIsXbox1TitleId_entry(0x414307D0) == 0);
  CHECK(XamIsXbox1TitleId_entry(0x4D5308BC) == 0);
  CHECK(XamIsXbox1TitleId_entry(0xFFFE07D1) == 1);
  CHECK(XamIsXbox1TitleId_entry(0xFFFF0001) == 0);
}

TEST_CASE("lstrlenW reads big endian UTF-16 and stops at the first terminator", "[xam]") {
  CHECK(lstrlenW_entry(nullptr) == 0);
  rex::be_u16 text[] = {0x0041, 0x4E2D, 0xD83D, 0xDE00, 0, 0x0042, 0};
  CHECK(lstrlenW_entry(mapped_u16(text, 0x1000)) == 4);
  CHECK(lstrlenW_entry(mapped_u16(text + 4, 0x1008)) == 0);
}
