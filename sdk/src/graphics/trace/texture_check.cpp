/**
 ******************************************************************************
 * ReXGlue SDK: GPU trace replay texture staleness check                      *
 ******************************************************************************
 * Copyright 2026 ReXGlue contributors. All rights reserved.                  *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include "texture_check.h"

#include <algorithm>

#include <fmt/format.h>

#include <rex/graphics/pipeline/shader/shader.h>
#include <rex/graphics/pipeline/texture/info.h>
#include <rex/graphics/pipeline/texture/util.h>
#include <rex/graphics/register_file.h>
#include <rex/graphics/registers.h>
#include <rex/graphics/xenos.h>
#include <rex/hash.h>
#include <rex/logging.h>
#include <rex/system/xmemory.h>

namespace rex::graphics::trace {

TextureCheck::TextureCheck(memory::Memory* memory, const std::filesystem::path& out_dir,
                           bool race)
    : memory_(memory), race_(race) {
  report_ = std::fopen((out_dir / "texcheck.jsonl").string().c_str(), "wb");
}

TextureCheck::~TextureCheck() {
  if (report_) {
    std::fclose(report_);
  }
  if (TextureLoadProbeSlot() == this) {
    TextureLoadProbeSlot() = nullptr;
  }
}

uint64_t TextureCheck::HashGuest(uint32_t address, uint32_t size) const {
  address &= 0x1FFFFFFF;
  size = uint32_t(std::min<uint64_t>(size, 0x20000000ull - address));
  return XXH3_64bits(memory_->TranslatePhysical(address), size);
}

void TextureCheck::NoteOrigin(uint64_t hash, uint32_t address, uint32_t size,
                              const Position& at) {
  origins_.try_emplace(hash, Origin{address, size, at});
}

std::string TextureCheck::OriginText(uint64_t hash) const {
  auto it = origins_.find(hash);
  if (it == origins_.end()) {
    return "content never seen in guest memory";
  }
  if (it->second.written) {
    return fmt::format("guest data written to 0x{:08X}+{} in frame {}", it->second.address,
                       it->second.size, it->second.at.frame);
  }
  return fmt::format("content of 0x{:08X}+{} first loaded at frame {} draw {}", it->second.address,
                     it->second.size, it->second.at.frame, it->second.at.draw);
}

void TextureCheck::NoteWrite(uint32_t address, const void* data, uint32_t length,
                             uint32_t frame) {
  if (length < 256) {
    return;
  }
  uint64_t hash = XXH3_64bits(data, length);
  std::lock_guard<std::mutex> lock(mutex_);
  Origin origin{address & 0x1FFFFFFF, length, {frame, position_.draw, position_.packet}, true};
  origins_.try_emplace(hash, origin);
}

void TextureCheck::AddGpuWrittenRange(uint32_t address, uint32_t length) {
  if (!length) {
    return;
  }
  std::lock_guard<std::mutex> lock(mutex_);
  address &= 0x1FFFFFFF;
  uint32_t start = address;
  uint32_t end = address + length;
  auto it = gpu_written_.upper_bound(start);
  if (it != gpu_written_.begin()) {
    auto prev = std::prev(it);
    if (prev->second >= start) {
      start = prev->first;
      end = std::max(end, prev->second);
      it = gpu_written_.erase(prev);
    }
  }
  while (it != gpu_written_.end() && it->first <= end) {
    end = std::max(end, it->second);
    it = gpu_written_.erase(it);
  }
  gpu_written_.emplace(start, end);
}

bool TextureCheck::OverlapsGpuWritten(uint32_t address, uint32_t size) const {
  if (!size) {
    return false;
  }
  address &= 0x1FFFFFFF;
  auto it = gpu_written_.upper_bound(address);
  if (it != gpu_written_.begin()) {
    auto prev = std::prev(it);
    if (prev->second > address) {
      return true;
    }
  }
  return it != gpu_written_.end() && it->first < address + size;
}

void TextureCheck::OnTextureLoadWindow(const TextureLoadEvent& event) {
  std::unique_lock<std::mutex> lock(mutex_);
  ++loads_;
  Key key{event.base_address & 0x1FFFFFFF, event.mips_address & 0x1FFFFFFF,
          uint32_t(GetBaseFormat(xenos::TextureFormat(event.format))), event.width,
          event.height};
  Entry& entry = entries_[key];
  // What the load read: the guest bytes now. The resident path uploaded the
  // invalid pages of these ranges to the shared memory buffer right before.
  auto record = [&](Loaded& loaded, uint32_t address, uint32_t size) {
    loaded.valid = true;
    loaded.address = address & 0x1FFFFFFF;
    loaded.size = size;
    loaded.hash = HashGuest(address, size);
    loaded.at = position_;
    loaded.source = event.source;
    NoteOrigin(loaded.hash, loaded.address, size, position_);
  };
  if (event.load_base && event.base_size) {
    record(entry.base, event.base_address, event.base_size);
  }
  if (event.load_mips && event.mips_size) {
    record(entry.mips, event.mips_address, event.mips_size);
  }
  if (!race_ || !race_writer_ || entry.race_packet == position_.packet || event.scaled_resolve) {
    return;
  }
  // Race mode: the guest write lands now, after the copy and before the
  // texture is marked up to date. One write per texture per packet (the
  // CPU-source path retries its copy when it sees one).
  struct Range {
    uint32_t address, size;
  } ranges[2] = {{event.base_address, event.base_size}, {event.mips_address, event.mips_size}};
  RaceWriter writer = race_writer_;
  for (const Range& r : ranges) {
    if (!r.size || OverlapsGpuWritten(r.address, r.size)) {
      continue;
    }
    uint32_t write_address = 0, write_length = 0, write_frame = 0;
    // The write fires the cache's callbacks, which may take the cache's own
    // locks and call back into nothing here, but keep our mutex free anyway.
    lock.unlock();
    bool applied = writer(r.address & 0x1FFFFFFF, r.size, write_address, write_length,
                          write_frame);
    lock.lock();
    if (applied) {
      Entry& e = entries_[key];
      e.race_packet = position_.packet;
      e.race_frame = write_frame;
      e.race_write_address = write_address;
      ++race_injections_;
      if (report_) {
        std::fprintf(report_,
                     "{\"kind\":\"race\",\"frame\":%u,\"draw\":%u,\"texture\":\"0x%08X\","
                     "\"mips\":\"0x%08X\",\"fmt\":%u,\"w\":%u,\"h\":%u,\"source\":\"%s\","
                     "\"write\":\"0x%08X\",\"write_bytes\":%u,\"write_frame\":%u}\n",
                     position_.frame, position_.draw, key.base_address, key.mips_address,
                     key.format, key.width, key.height, event.source ? "cpu" : "resident",
                     write_address, write_length, write_frame);
      }
      break;
    }
    ++race_misses_;
  }
}

void TextureCheck::CheckDraw(const RegisterFile& regs, Shader* vs, Shader* ps,
                             std::string& json) {
  std::lock_guard<std::mutex> lock(mutex_);
  json += ",\"texcheck\":[";
  bool first = true;
  uint32_t seen_mask = 0;
  for (Shader* shader : {vs, ps}) {
    if (!shader) {
      continue;
    }
    for (const auto& binding : shader->texture_bindings()) {
      uint32_t slot = binding.fetch_constant;
      if (slot >= 32 || (seen_mask & (1u << slot))) {
        continue;
      }
      seen_mask |= 1u << slot;
      xenos::xe_gpu_texture_fetch_t fetch = regs.GetTextureFetch(slot);
      if (fetch.type != xenos::FetchConstantType::kTexture) {
        continue;
      }
      uint32_t width_minus_1, height_minus_1, depth_minus_1;
      uint32_t base_page, mip_page, mip_max_level;
      texture_util::GetSubresourcesFromFetchConstant(fetch, &width_minus_1, &height_minus_1,
                                                     &depth_minus_1, &base_page, &mip_page,
                                                     nullptr, &mip_max_level);
      if (!base_page && !mip_page) {
        continue;
      }
      Key key{base_page << 12, mip_page << 12, uint32_t(GetBaseFormat(fetch.format)),
              width_minus_1 + 1, height_minus_1 + 1};
      ++checked_;
      const char* state = "ok";
      auto it = entries_.find(key);
      uint64_t cur_base = 0, cur_mips = 0;
      bool stale_base = false, stale_mips = false;
      const Entry* entry = it != entries_.end() ? &it->second : nullptr;
      if (!entry) {
        state = "unloaded";
        ++unloaded_;
      } else {
        bool gpu = (entry->base.valid &&
                    OverlapsGpuWritten(entry->base.address, entry->base.size)) ||
                   (entry->mips.valid &&
                    OverlapsGpuWritten(entry->mips.address, entry->mips.size));
        if (gpu) {
          state = "gpu";
          ++gpu_skipped_;
        } else {
          if (entry->base.valid && base_page) {
            cur_base = HashGuest(entry->base.address, entry->base.size);
            stale_base = cur_base != entry->base.hash;
          }
          if (entry->mips.valid && mip_page && mip_max_level) {
            cur_mips = HashGuest(entry->mips.address, entry->mips.size);
            stale_mips = cur_mips != entry->mips.hash;
          }
          if ((stale_base || stale_mips) && entry->race_packet == position_.packet) {
            // The write was injected while this draw's load was in flight:
            // the draw itself legitimately used the bytes from before it.
            state = "race";
            stale_base = stale_mips = false;
          } else if (stale_base || stale_mips) {
            state = "stale";
          }
        }
      }
      if (!first) {
        json += ",";
      }
      first = false;
      json += fmt::format(
          "{{\"fetch\":{},\"stage\":\"{}\",\"base\":\"0x{:08X}\",\"mips\":\"0x{:08X}\","
          "\"fmt\":{},\"w\":{},\"h\":{},\"state\":\"{}\"",
          slot, shader == vs ? "vs" : "ps", key.base_address, key.mips_address, key.format,
          key.width, key.height, state);
      if (entry && entry->base.valid) {
        json += fmt::format(",\"base_loaded\":\"{:016X}\",\"base_loaded_at\":[{},{}]",
                            entry->base.hash, entry->base.at.frame, entry->base.at.draw);
        if (cur_base) {
          json += fmt::format(",\"base_guest\":\"{:016X}\"", cur_base);
        }
      }
      if (entry && entry->mips.valid) {
        json += fmt::format(",\"mips_loaded\":\"{:016X}\",\"mips_loaded_at\":[{},{}]",
                            entry->mips.hash, entry->mips.at.frame, entry->mips.at.draw);
        if (cur_mips) {
          json += fmt::format(",\"mips_guest\":\"{:016X}\"", cur_mips);
        }
      }
      json += "}";
      if (!(stale_base || stale_mips)) {
        continue;
      }
      ++stale_;
      for (int part = 0; part < 2; ++part) {
        bool stale = part ? stale_mips : stale_base;
        if (!stale) {
          continue;
        }
        const Loaded& loaded = part ? entry->mips : entry->base;
        uint64_t expected = part ? cur_mips : cur_base;
        uint64_t& count = stale_distinct_[{loaded.hash, expected}];
        ++count;
        if (!report_) {
          continue;
        }
        std::string line = fmt::format(
            "frame {} draw {} fetch slot {}: sampled stale texture at 0x{:08X} fmt {} {}x{} {} "
            "0x{:08X}+{} (got hash {:016X} = {}, loaded at frame {} draw {} via {}; expected "
            "hash {:016X} = {})",
            position_.frame, position_.draw, slot, key.base_address, key.format, key.width,
            key.height, part ? "mips" : "base", loaded.address, loaded.size, loaded.hash,
            OriginText(loaded.hash), loaded.at.frame, loaded.at.draw,
            loaded.source ? "cpu" : "resident", expected, OriginText(expected));
        std::fprintf(report_,
                     "{\"kind\":\"stale\",\"frame\":%u,\"draw\":%u,\"fetch\":%u,\"stage\":\"%s\","
                     "\"texture\":\"0x%08X\",\"mips\":\"0x%08X\",\"fmt\":%u,\"w\":%u,\"h\":%u,"
                     "\"part\":\"%s\",\"got\":\"%016llX\",\"expected\":\"%016llX\","
                     "\"loaded_at\":[%u,%u],\"repeat\":%llu,\"text\":\"%s\"}\n",
                     position_.frame, position_.draw, slot, shader == vs ? "vs" : "ps",
                     key.base_address, key.mips_address, key.format, key.width, key.height,
                     part ? "mips" : "base", (unsigned long long)loaded.hash,
                     (unsigned long long)expected, loaded.at.frame, loaded.at.draw,
                     (unsigned long long)count, line.c_str());
      }
    }
  }
  json += "]";
}

std::string TextureCheck::SummaryJson() const {
  return fmt::format(
      "{{\"race\":{},\"loads\":{},\"checked\":{},\"stale\":{},\"stale_distinct\":{},"
      "\"unloaded\":{},\"gpu_written_skipped\":{},\"race_injections\":{},\"race_misses\":{}}}",
      race_ ? "true" : "false", loads_, checked_, stale_, stale_distinct_.size(), unloaded_,
      gpu_skipped_, race_injections_, race_misses_);
}

}  // namespace rex::graphics::trace
