/**
 ******************************************************************************
 * ReXGlue SDK: GPU trace replay texture staleness check                      *
 ******************************************************************************
 * Copyright 2026 ReXGlue contributors. All rights reserved.                  *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#pragma once

#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <functional>
#include <map>
#include <mutex>
#include <string>
#include <unordered_map>

#include <rex/graphics/trace/texture_probe.h>

namespace rex::memory {
class Memory;
}  // namespace rex::memory

namespace rex::graphics {
class RegisterFile;
class Shader;
}  // namespace rex::graphics

namespace rex::graphics::trace {

// Records what every texture load read (through the texture cache load probe)
// and checks, for every draw, that each bound texture the cache considers
// loaded still matches the guest memory it was loaded from. A texture whose
// guest bytes changed after the load without the cache reloading it is
// stale: the draw sampled old data.
//
// Race mode: at each load window (source read, texture not yet marked up to
// date) the next recorded guest write that changes the texture's memory is
// applied right there, through the same physical write callbacks a guest
// store fires. A cache that marks the texture up to date regardless keeps the
// old bytes; one that notices the write reloads on the next use.
class TextureCheck : public TextureLoadProbe {
 public:
  struct Position {
    uint32_t frame = 0;
    uint32_t draw = 0;
    uint32_t packet = 0;
  };
  // Applies the next recorded write overlapping [address, address + length)
  // that changes guest memory there, if it is at most race_frames ahead.
  // Returns true and fills the write's range and trace frame if one was
  // applied.
  using RaceWriter = std::function<bool(uint32_t address, uint32_t length, uint32_t& write_address,
                                        uint32_t& write_length, uint32_t& write_frame)>;

  TextureCheck(memory::Memory* memory, const std::filesystem::path& out_dir, bool race);
  ~TextureCheck() override;

  void set_race_writer(RaceWriter writer) { race_writer_ = std::move(writer); }
  void set_position(const Position& position) { position_ = position; }

  void OnTextureLoadWindow(const TextureLoadEvent& event) override;

  // Appends ",\"texcheck\":[...]" for the draw to json.
  void CheckDraw(const RegisterFile& regs, Shader* vs, Shader* ps, std::string& json);
  void AddGpuWrittenRange(uint32_t address, uint32_t length);
  // A recorded guest write (trace memory block) of data at address, from
  // trace frame frame: indexes its content hash for the report.
  void NoteWrite(uint32_t address, const void* data, uint32_t length, uint32_t frame);

  std::string SummaryJson() const;
  uint64_t stale_count() const { return stale_; }

 private:
  struct Loaded {
    bool valid = false;
    uint32_t address = 0;
    uint32_t size = 0;
    uint64_t hash = 0;
    Position at;
    uint32_t source = 0;
  };
  struct Entry {
    Loaded base;
    Loaded mips;
    uint32_t race_packet = UINT32_MAX;
    uint32_t race_frame = 0;
    uint32_t race_write_address = 0;
  };
  struct Origin {
    uint32_t address = 0;
    uint32_t size = 0;
    Position at;
    bool written = false;
  };
  struct Key {
    uint32_t base_address, mips_address, format, width, height;
    bool operator==(const Key& o) const {
      return base_address == o.base_address && mips_address == o.mips_address &&
             format == o.format && width == o.width && height == o.height;
    }
  };
  struct KeyHash {
    size_t operator()(const Key& k) const {
      uint64_t h = (uint64_t(k.base_address) << 32) ^ k.mips_address;
      h ^= (uint64_t(k.format) << 58) ^ (uint64_t(k.width) << 20) ^ (uint64_t(k.height) << 40);
      return std::hash<uint64_t>()(h * 0x9E3779B97F4A7C15ull);
    }
  };

  uint64_t HashGuest(uint32_t address, uint32_t size) const;
  bool OverlapsGpuWritten(uint32_t address, uint32_t size) const;
  void NoteOrigin(uint64_t hash, uint32_t address, uint32_t size, const Position& at);
  std::string OriginText(uint64_t hash) const;

  memory::Memory* memory_;
  bool race_;
  RaceWriter race_writer_;
  Position position_;
  std::mutex mutex_;
  std::unordered_map<Key, Entry, KeyHash> entries_;
  std::unordered_map<uint64_t, Origin> origins_;
  std::map<uint32_t, uint32_t> gpu_written_;  // start -> end
  std::FILE* report_ = nullptr;
  uint64_t loads_ = 0;
  uint64_t checked_ = 0;
  uint64_t stale_ = 0;
  uint64_t unloaded_ = 0;
  uint64_t gpu_skipped_ = 0;
  uint64_t race_injections_ = 0;
  uint64_t race_misses_ = 0;
  std::map<std::pair<uint64_t, uint64_t>, uint64_t> stale_distinct_;
};

}  // namespace rex::graphics::trace
