/**
 * @file        rex/system/game_patches.h
 * @brief       Xenia-canary style game patch files (cheats, FOV, HUD ...) for every game
 *
 * @copyright   Copyright (c) 2026
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 *
 * Reads `<TITLEID> - <name>.patch.toml` files in the xenia-canary format unchanged (title_id, module hash,
 * [[patch]] entries with be8/be16/be32/be64/f32/f64/string/u16string/array writes at guest addresses). Written
 * from the format's behaviour, not from canary's code. A file applies to a loaded module when its title id and one
 * of its hashes match; the hash is the one canary computes (XXH3-64 over the module's code pages), so the hashes
 * in community files match our modules.
 *
 * A recompiled game runs host code generated at build time, so a write into the game's CODE pages changes nothing
 * at run time. Each patch is classified:
 *  - data: every write is outside the code pages. Applied and reverted live.
 *  - code: some write is inside the code pages. Works only when the app was built with the patch (codegen emits
 *    the patched and the original instruction behind a switch and registers it with RegisterBuiltIn); otherwise it
 *    is listed as unavailable with the reason.
 * Every patch of every file for the running title is listed (Guide page "Patches / Cheats"); unavailable ones
 * carry a one-line reason. Enabled state is saved per title and module hash.
 */
#pragma once

#include <atomic>
#include <cstdint>
#include <filesystem>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace rex::memory {
class Memory;
}
namespace rex::runtime {
class XexModule;
}

namespace rex::patches {

struct Write {
  uint32_t address = 0;
  std::vector<uint8_t> bytes;  // guest byte order, exactly what is stored
};

struct Patch {
  std::string name;
  std::string desc;
  std::string author;
  bool default_on = false;  // is_enabled in the file
  std::vector<Write> writes;
  std::string parse_error;  // a malformed write: the ones before it are kept (canary does the same)
};

struct File {
  std::string path;
  std::string title_name;
  uint32_t title_id = 0;
  std::vector<uint64_t> hashes;
  std::vector<Patch> patches;
};

// Canary file name rule: 8 hex digits first, ".patch.toml" last.
bool IsPatchFileName(std::string_view filename);
// Parses one patch file. nullopt (and *error) when canary would skip the whole file.
std::optional<File> ParseFile(std::string_view toml_text, std::string* error = nullptr);

// XXH3-64, the hash canary uses for modules (over the range described in game_patches.cpp).
uint64_t HashCode(const uint8_t* bytes, size_t size);

enum class Kind { kData, kCode };

struct Entry {
  size_t id = 0;  // pass to SetEnabled
  std::string title;  // patch name
  std::string desc;
  std::string author;
  std::string file;   // file name only
  Kind kind = Kind::kData;
  bool available = false;
  bool built_in = false;  // code patch compiled into this app
  bool on = false;
  std::string reason;  // why it is not available (empty when available)
};

// Folders searched for patch files (non-existing ones are fine) and the folder the enabled state is kept in.
// Nothing is loaded until this is called, so tools that load modules without it (codegen) are untouched.
/// apply = false (build tools): classify only, never write guest memory.
void Configure(std::vector<std::filesystem::path> dirs, std::filesystem::path state_dir, bool apply = true);
bool Configured();

// Called by the loader once a module's image is in memory (title update applied), before the game runs.
// Computes the hash, matches files, applies the enabled data patches.
void OnModuleLoaded(memory::Memory* memory, const runtime::XexModule& xex, uint32_t title_id);

// Everything to show: one entry per patch of every file whose title id matches a loaded module.
std::vector<Entry> List();
// Turns a patch on or off now and saves the choice. False (and *why) when it cannot be applied.
bool SetEnabled(size_t id, bool on, std::string* why = nullptr);
// "<module> <TITLEID> <HASH>" lines for the loaded modules (Guide info, logs).
std::vector<std::string> ModuleSummary();

// Code patches compiled into the app (codegen-generated table). `flag` is read by the generated code.
/// For the recompiler: every matched patch that changes code, as whole new instruction words (address -> word,
/// big-endian value) built over the loaded image. Call after the executable module loaded.
struct BuildPatch {
  std::string name;
  uint32_t title_id = 0;
  uint64_t module_hash = 0;
  std::map<uint32_t, uint32_t> words;
};
std::vector<BuildPatch> CodePatchesForBuild();

void RegisterBuiltIn(uint32_t title_id, uint64_t module_hash, std::string_view patch_name,
                     std::atomic<bool>* flag);

}  // namespace rex::patches
