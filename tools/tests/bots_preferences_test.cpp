// Exercise the production lobby bot manager with isolated guest/controller state.
#include "../../games/bo2/src/bo2zm_bots.h"
#include "../../core/bo2zm_settings.h"
#include <rex/ppc.h>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <stdexcept>
#include <thread>
#include <vector>
#include <cstring>
#ifdef _WIN32
#include <windows.h>
#else
#include <sys/mman.h>
#endif
extern int32_t bo2zm_test_bot_count;
namespace {
std::filesystem::path root;
bool zombies = true;
std::vector<std::string> published;
void Check(bool ok, const char* text) { if (!ok) throw std::runtime_error(text); }
std::string Read(const std::filesystem::path& path) {
  std::ifstream file(path, std::ios::binary);
  return {std::istreambuf_iterator<char>(file), {}};
}
struct Guest {
  uint8_t* base;
  Guest() {
#ifdef _WIN32
    base = static_cast<uint8_t*>(VirtualAlloc(nullptr, size_t(1) << 32, MEM_RESERVE, PAGE_NOACCESS));
    Check(base && VirtualAlloc(base + 0x837F0000, 0x10000, MEM_COMMIT, PAGE_READWRITE), "guest fixture mapping");
#else
    base = static_cast<uint8_t*>(mmap(nullptr, size_t(1) << 32, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0));
    Check(base != MAP_FAILED && mprotect(base + 0x837F0000, 0x10000, PROT_READ | PROT_WRITE) == 0, "guest mapping");
#endif
  }
  ~Guest() {
#ifdef _WIN32
    VirtualFree(base, 0, MEM_RELEASE);
#else
    munmap(base, size_t(1) << 32);
#endif
  }
  void Word(uint32_t address, uint32_t value) {
    value = __builtin_bswap32(value); std::memcpy(base + address, &value, 4);
  }
  void Humans(int count) {
    for (uint32_t i = 0; i < 4; ++i) {
      Word(0x837F6A28 + i * 20, i < uint32_t(count) ? 1 : 0);
      Word(0x837F6A28 + i * 20 + 4, i);
      Word(0x837F6A28 + i * 20 + 8, i);
    }
  }
};
}
namespace bo2zm::mode { bool IsZombies() { return zombies; } }
namespace bo2zm::launch { std::filesystem::path SharedUserRoot(const std::filesystem::path&) { return root; } }
namespace bo2zm::autoexec {
void ExecuteCommands(PPCContext&, uint8_t*, std::string_view text, int) { published.emplace_back(text); }
}
int main(int argc, char** argv) {
  try {
    Check(argc == 2, "temporary user root required"); root = argv[1];
    using namespace bo2zm::bots;
    const Context town{"zm_transit", "zstandard", "town"}, farm{"zm_transit", "zstandard", "farm"};
    const auto file = root / "bo2zm_settings.txt";
    Check(Requested(town) == 0 && !std::filesystem::exists(root), "startup did not default OFF");
    Check(!Command("unrelated", ""), "unrelated command consumed");
    for (int humans = 1; humans <= 4; ++humans)
      Check(preferences::ClampToHumans(3, humans) == 4 - humans, "pure four-total bound");
    Check(preferences::MaximumForHumans(0) == 0 && preferences::MaximumForHumans(5) == 0,
          "invalid human counts did not fail closed");
    PPCContext ctx;
#if defined(BO2_GAME_CAMPAIGN)
    Command("bo2zm_bots_open", ""); Command("bo2zm_bots_count", "zm_transit zstandard town 3");
    EveryFrame(ctx, nullptr);
    Check(Requested(town) == 0 && published.empty() && !std::filesystem::exists(root), "Campaign bot side effects");
#else
    Guest guest; guest.Humans(1);
    EveryFrame(ctx, guest.base);
    Check(HumanCount(guest.base) == 1, "primary human count");
    Command("bo2zm_bots_context", "zm_transit zstandard town");
    Command("bo2zm_bots_count", "zm_transit zstandard town 3");
    Check(Requested(town) == 0, "missing pack enabled bots");
    SetAvailability(true, "Ready");
    bo2zm::settings::SetManyAt(file, {{"bots_count_default", "3"}, {*preferences::Key(town), "3"}, {"unrelated", "keep"}});
    const auto original = Read(file);
    Command("bo2zm_bots_open", "");
    Check(Requested(town) == 0, "old saved selection activated on lobby open");
    Command("bo2zm_bots_count", "zm_transit zstandard town 3");
    Check(Requested(town) == 3 && Requested(farm) == 0, "explicit choice/context isolation");
    for (int humans = 2; humans <= 4; ++humans) {
      guest.Humans(humans); EveryFrame(ctx, guest.base);
      Check(HumanCount(guest.base) == humans && Requested(town) == 4 - humans, "joining human did not reduce bots");
      Check(published.back().find("bo2zm_bots_max_ui " + std::to_string(4 - humans)) != std::string::npos,
            "UI maximum stale");
      Command("bo2zm_bots_count", "zm_transit zstandard town 3");
      Check(Requested(town) == 4 - humans, "stale selector bypassed human guard");
    }
    guest.Humans(1); EveryFrame(ctx, guest.base);
    Check(Requested(town) == 0, "leaving human unexpectedly restored bots");
    Command("bo2zm_bots_count", "zm_transit zstandard town 3");
    Check(Requested(town, 2) == 2 && Requested(town, 3) == 1 && Requested(town, 4) == 0,
          "server's fresh human count did not override stale UI count");
    Command("bo2zm_bots_context", "zm_transit zstandard town");
    Check(Requested(town) == 3, "ordinary repaint reset choice");
    Command("bo2zm_bots_open", ""); Check(Requested(town) == 0, "reopening same lobby kept choice");
    Command("bo2zm_bots_count", "zm_transit zstandard town 2");
    Command("bo2zm_bots_context", "zm_transit zstandard farm");
    Check(Requested(farm) == 0 && Requested(town) == 0, "map change inherited stale selection");
    // A System Link lobby hosted here reports local and remote players; the larger count fills slots.
    guest.Humans(1); EveryFrame(ctx, guest.base);
    Command("bo2zm_bots_open", ""); Command("bo2zm_bots_count", "zm_transit zstandard town 3");
    Check(Humans(guest.base) == 1 && Requested(town, Humans(guest.base)) == 3, "no lobby report changed the count");
    Check(Command("bo2zm_bots_humans", "2"), "lobby player count not consumed");
    EveryFrame(ctx, guest.base);
    Check(Humans(guest.base) == 2 && Requested(town, Humans(guest.base)) == 2 && Requested(town) == 2,
          "remote player did not reduce bots");
    Check(published.back().find("bo2zm_bots_max_ui 2") != std::string::npos, "UI maximum ignored remote player");
    Command("bo2zm_bots_humans", "4"); EveryFrame(ctx, guest.base);
    Check(Requested(town, Humans(guest.base)) == 0, "full System Link lobby kept bots");
    for (const char* bad : {"0", "5", "", "2 3", "-1", "x"}) {
      Command("bo2zm_bots_humans", bad);
      Check(Humans(guest.base) == 4, "invalid lobby player count accepted");
    }
    guest.Humans(3); Command("bo2zm_bots_humans", "1");
    Check(Humans(guest.base) == 3, "lobby report hid local players");
    Command("bo2zm_bots_open", ""); guest.Humans(1);
    Check(Humans(guest.base) == 1, "a new lobby kept the old lobby's players");
    EveryFrame(ctx, guest.base);
    for (int override = 0; override <= 3; ++override) {
      bo2zm_test_bot_count = override;
      for (int humans = 1; humans <= 4; ++humans)
        Check(Requested(town, humans) == preferences::ClampToHumans(override, humans), "test override exceeded total");
    }
    bo2zm_test_bot_count = 4;
    Check(Requested(farm) == 0, "retired four-bot experiment remained enabled");
    bo2zm_test_bot_count = -1;
    guest.Humans(2); guest.Word(0x837F6A28 + 20 + 8, 0);
    Check(HumanCount(guest.base) == 4, "duplicate controller mapping not fail-closed");
    Check(Read(file) == original, "lobby bot actions rewrote saved preferences");
    zombies = false; Command("bo2zm_bots_open", "");
    Check(Requested(town) == 0 && Read(file) == original, "MP bot side effects");
    zombies = true;
    bo2zm::settings::SetAt(file, "script_mod", "Legacy");
    Check(MigrateLegacySelection("Legacy"), "legacy selection migration failed");
    Check(bo2zm::settings::GetAt(file, "unrelated") == "keep" && Requested(town) == 0,
          "migration enabled bots or damaged unrelated settings");
#endif
    std::cout << "Lobby OFF lifecycle, human-slot bounds, stale UI/server guard and mode isolation passed\n";
    return 0;
  } catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
