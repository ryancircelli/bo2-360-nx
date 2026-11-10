// Derived from GPL-3.0 code; modified for bo2-zm-nx in 2026.
//
// Sub-allocation pool for the native renderer's textures.
// The rationale, the measurements and the safety invariant are in bo2zm_native_texture_pool.h.

#include "bo2zm_native_texture_pool.h"

#if BO2ZM_NATIVE_METAL
#include "metal/bo2zm_gpu_metal.h"
#endif

#include <rex/cvar.h>
#include <rex/logging.h>
#include <rex/ui/vulkan/util.h>

#include <algorithm>
#include <string>

REXCVAR_DEFINE_BOOL(bo2zm_native_texture_pool, true, "BO2",
                    "Native renderer: textures take pieces of large memory blocks instead of each asking for a "
                    "dedicated allocation. On Horizon each dedicated allocation costs ~1.9 ms of CPU (an nvMapCreate, "
                    "TWO address reservations and TWO mappings), and the image does not change. Turning it off goes "
                    "back to one allocation per texture")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);

REXCVAR_DEFINE_INT32(bo2zm_native_texture_pool_slab_mb, 32, "BO2",
                     "Native renderer: MB of each large block of the texture pool. Larger = fewer allocations from the "
                     "system, but each new block costs a memset of that size")
    .range(4, 256)
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);

namespace bo2zm::native {
namespace {

uint32_t AlignUp(uint32_t value, uint32_t step) {
  if (step <= 1) {
    return value;
  }
  return ((value + step - 1) / step) * step;
}

bool BitBusy(const std::vector<uint64_t>& map, uint32_t i) {
  return ((map[i >> 6] >> (i & 63)) & 1ull) != 0ull;
}

// Returns true if [start, start+n) is entirely free. Otherwise it stores in `first_busy`
// the first used unit it found, so the search can jump past it.
bool RangeFree(const std::vector<uint64_t>& map, uint32_t start, uint32_t n,
                uint32_t& first_busy) {
  for (uint32_t i = start; i < start + n; ++i) {
    if (BitBusy(map, i)) {
      first_busy = i;
      return false;
    }
  }
  return true;
}

void MarkRange(std::vector<uint64_t>& map, uint32_t start, uint32_t n, bool busy) {
  for (uint32_t i = start; i < start + n; ++i) {
    if (busy) {
      map[i >> 6] |= 1ull << (i & 63);
    } else {
      map[i >> 6] &= ~(1ull << (i & 63));
    }
  }
}

/*
 * First fit, without a rover.
 *
 * With 32 MB blocks there are 512 units, so a full pass is at most a few hundred bit checks,
 * and jumping past the first used unit keeps it linear. At ten or twenty textures per second
 * this does not show up in measurements; a rover would only add a second pass and more places
 * to get it wrong.
 */
bool FindGap(const std::vector<uint64_t>& map, uint32_t units_totals, uint32_t n,
                 uint32_t step, uint32_t& start_out) {
  if (n == 0 || n > units_totals) {
    return false;
  }
  uint32_t start = 0;
  while (start + n <= units_totals) {
    uint32_t busy = 0;
    if (RangeFree(map, start, n, busy)) {
      start_out = start;
      return true;
    }
    start = AlignUp(busy + 1, step);
  }
  return false;
}

// The largest contiguous free range, in units. It is the fragmentation measure in the report.
uint32_t LargerGap(const std::vector<uint64_t>& map, uint32_t units_totals) {
  uint32_t best = 0;
  uint32_t run_len = 0;
  for (uint32_t i = 0; i < units_totals; ++i) {
    if (BitBusy(map, i)) {
      run_len = 0;
    } else {
      ++run_len;
      best = std::max(best, run_len);
    }
  }
  return best;
}

}  // namespace

TexturePool::~TexturePool() { Finish(); }

namespace {

class VulkanSlabs final : public SlabMemory {
 public:
  explicit VulkanSlabs(const rex::ui::vulkan::VulkanDevice* vulkan_device)
      : vulkan_device_(vulkan_device), device_(vulkan_device->device()) {}

  bool Choose(uint32_t& type_out, gpu::Size& alignment_view_out) override {
    /*
     * A sample image with the same usage and tiling as the real textures, only to ask the
     * driver which memory types it accepts and with what alignment. It is destroyed right
     * away; it does not allocate memory.
     */
    const auto& dfn = vulkan_device_->functions();
    VkImageCreateInfo info{};
    info.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    info.imageType = VK_IMAGE_TYPE_2D;
    info.format = VK_FORMAT_R8G8B8A8_UNORM;
    info.extent = {256, 256, 1};
    info.mipLevels = 1;
    info.arrayLayers = 1;
    info.samples = VK_SAMPLE_COUNT_1_BIT;
    info.tiling = VK_IMAGE_TILING_OPTIMAL;
    info.usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    info.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;

    VkImage sample = VK_NULL_HANDLE;
    if (dfn.vkCreateImage(device_, &info, nullptr, &sample) != VK_SUCCESS) {
      return false;
    }
    VkMemoryRequirements requirements{};
    dfn.vkGetImageMemoryRequirements(device_, sample, &requirements);
    dfn.vkDestroyImage(device_, sample, nullptr);

    // The same criterion CreateDedicatedAllocationImage uses for textures, so the slab lands
    // in the same memory type the dedicated allocation would.
    const uint32_t type = rex::ui::vulkan::util::ChooseMemoryType(
        vulkan_device_->memory_types(), requirements.memoryTypeBits,
        rex::ui::vulkan::util::MemoryPurpose::kDeviceLocal);
    if (type == UINT32_MAX) {
      return false;
    }
    type_out = type;
    alignment_view_out = requirements.alignment;
    return true;
  }

  gpu::Memory Allocate(gpu::Size bytes, uint32_t type) override {
    /*
     * Without VkMemoryDedicatedAllocateInfo on purpose: that way the nvkmd_mem is created with
     * pte_kind = 0 and tile_mode = 0, its layout.valid stays false
     * (nvkmd/switch/nvkmd_switch_dev.c:787-795) and the block accepts aliased images with any
     * pte_kind (horizon/nouveau_horizon_memory.c:1056-1073).
     */
    VkMemoryAllocateInfo reserve{};
    reserve.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    reserve.allocationSize = bytes;
    reserve.memoryTypeIndex = type;

    VkDeviceMemory guest_memory = VK_NULL_HANDLE;
    if (vulkan_device_->functions().vkAllocateMemory(device_, &reserve, nullptr, &guest_memory) != VK_SUCCESS) {
      return nullptr;
    }
    return guest_memory;
  }

  void Release(gpu::Memory memory) override {
    vulkan_device_->functions().vkFreeMemory(device_, memory, nullptr);
  }

 private:
  const rex::ui::vulkan::VulkanDevice* vulkan_device_;
  VkDevice device_;
};

}  // namespace

std::unique_ptr<SlabMemory> VulkanSlabMemory(const rex::ui::vulkan::VulkanDevice* vulkan_device) {
  return std::make_unique<VulkanSlabs>(vulkan_device);
}

#if BO2ZM_NATIVE_METAL
namespace {

class MetalSlabs final : public SlabMemory {
 public:
  explicit MetalSlabs(gpu::metal::Device& device) : device_(device) {}

  bool Choose(uint32_t& type_out, gpu::Size& alignment_view_out) override {
    // The same sample texture as the Vulkan side asks about.
    gpu::ImageDesc sample;
    sample.format = VK_FORMAT_R8G8B8A8_UNORM;
    sample.width = 256;
    sample.height = 256;
    const gpu::Requirements requirements = device_.TextureRequirements(sample);
    if (requirements.size == 0) {
      return false;
    }
    type_out = 0;
    alignment_view_out = requirements.alignment;
    return true;
  }

  gpu::Memory Allocate(gpu::Size bytes, uint32_t) override { return device_.CreateHeap(bytes); }

  void Release(gpu::Memory memory) override { gpu::metal::Device::ReleaseHeap(memory); }

  // Metal places textures at 128-byte alignment (256 B to 4 KiB in check_backend's measurement), so 1 KiB units
  // waste 0.2% over a grid of the game's texture shapes where 4 KiB units waste 0.9% and 64 KiB ones 20%.
  gpu::Size Unit() const override { return 1024; }

 private:
  gpu::metal::Device& device_;
};

}  // namespace

std::unique_ptr<SlabMemory> MetalSlabMemory(gpu::metal::Device& device) {
  return std::make_unique<MetalSlabs>(device);
}
#endif

bool TexturePool::Start(const rex::ui::vulkan::VulkanDevice* vulkan_device, int32_t mb_cache_max) {
  return Start(vulkan_device ? VulkanSlabMemory(vulkan_device) : nullptr, mb_cache_max);
}

bool TexturePool::Start(std::unique_ptr<SlabMemory> memory, int32_t mb_cache_max) {
  if (!REXCVAR_GET(bo2zm_native_texture_pool)) {
    REXLOG_INFO("[native] C3: texture pool (bo2zm_native_texture_pool) = no; each texture will ask for its dedicated "
                "allocation, as before");
    return false;
  }
  if (!memory) {
    return false;
  }
  memory_ = std::move(memory);

  gpu::Size alignment_view = 0;
  if (!memory_->Choose(type_memory_, alignment_view)) {
    REXLOG_WARN("[native] C3: texture pool: there is no valid local memory type; continuing with a dedicated allocation "
                "per texture");
    memory_.reset();
    return false;
  }

  const int32_t slab_mb = std::max<int32_t>(REXCVAR_GET(bo2zm_native_texture_pool_slab_mb), 4);
  slab_bytes_ = uint64_t(slab_mb) << 20;
  unit_ = memory_->Unit();
  slab_units_ = uint32_t(slab_bytes_ / unit_);

  /*
   * How much to prewarm and how far to grow.
   *
   * The texture cache's limit counts the textures' own bytes, but here each texture takes at least
   * one 64 KB unit (what vkGetImageMemoryRequirements asks for). In a match the cache is held at
   * 75 % of its 512 MB (384 MB), and with BO2's many small textures that fills about 540 MB of the
   * pool. A cap of one slab above the limit (544 MB) therefore left the pool full for the whole
   * match: new textures kept falling back to the dedicated path at about 1.9 ms each on the ring
   * thread (on an OLED, the count went from about 950 to 2,000 within four minutes, 27 a second by
   * the end), a small prewarm of 256 MB sent about 1,050 of the map's loading textures there as
   * well, and the slabs added while playing (one every 120 frames, 9 per match) each cost a 32 MB
   * memset on the ring thread.
   *
   * So the pool is prewarmed to one and a quarter times the limit and capped at one and a half
   * times the limit plus one slab, for the rounding. With 544 MB prewarmed no texture took the
   * dedicated path in a match, and the pool ended at 640 MB after three slabs added while playing,
   * 577 MB of it in use.
   */
  const int32_t mb_max = mb_cache_max > 0 ? mb_cache_max : 384;
  const int32_t mb_initial = mb_max * 5 / 4;
  const uint64_t bytes_cap = (uint64_t(mb_max) * 3 / 2 << 20) + slab_bytes_;
  slabs_cap_ = uint32_t(std::min<uint64_t>((bytes_cap + slab_bytes_ - 1) / slab_bytes_, kMaxSlabs));
  const uint32_t slabs_initial =
      uint32_t(std::min<uint64_t>(((uint64_t(mb_initial) << 20) + slab_bytes_ - 1) / slab_bytes_, slabs_cap_));

  active_ = true;  // CreateSlab needs it set
  for (uint32_t i = 0; i < slabs_initial; ++i) {
    if (!CreateSlab(false)) {
      break;
    }
  }
  if (slabs_.empty()) {
    REXLOG_WARN("[native] C3: texture pool: could not create even one {} MB block; continuing with a dedicated "
                "allocation per texture",
                slab_mb);
    active_ = false;
    memory_.reset();
    return false;
  }

  REXLOG_INFO("[native] C3: texture pool ON: {} blocks of {} MB prewarmed ({} MB), cap {} blocks ({} MB); memory type "
              "{}, unit {} KB, alignment the driver asks for {} KB",
              slabs_.size(), slab_mb, (slabs_.size() * slab_bytes_) >> 20, slabs_cap_,
              (uint64_t(slabs_cap_) * slab_bytes_) >> 20, type_memory_, unit_ >> 10,
              alignment_view >> 10);
  return true;
}

bool TexturePool::CreateSlab(bool in_hot) {
  if (!active_ || slabs_.size() >= slabs_cap_ || slabs_.size() >= kMaxSlabs) {
    return false;
  }
  const gpu::Memory guest_memory = memory_->Allocate(slab_bytes_, type_memory_);
  if (guest_memory == nullptr) {
    ++slabs_failed_;
    return false;
  }

  Slab slab;
  slab.guest_memory = guest_memory;
  slab.units = slab_units_;
  slab.occupied.assign((slab_units_ + 63) / 64, 0ull);
  slab.run_length.assign(slab_units_, 0u);
  slabs_.push_back(std::move(slab));
  if (in_hot) {
    ++slabs_in_hot_;
    REXLOG_INFO("[native] C3: texture pool: new block {} while running; {} MB allocated so far",
                slabs_.size() - 1, (slabs_.size() * slab_bytes_) >> 20);
  }
  return true;
}

void TexturePool::Finish() {
  if (memory_) {
    for (Slab& slab : slabs_) {
      if (slab.guest_memory != nullptr) {
        memory_->Release(slab.guest_memory);
      }
    }
  }
  slabs_.clear();
  active_ = false;
  memory_.reset();
}

bool TexturePool::Reserve(const gpu::Requirements& requirements, gpu::Memory& memory_out, gpu::Size& offset_out,
                          uint32_t& block_out) {
  if (!active_) {
    return false;
  }
  // The slab's type must be among the ones this image accepts. On NVK for Tegra it always
  // is, but if some format said otherwise, falling back to the dedicated path is better than
  // binding something invalid.
  if (((requirements.types >> type_memory_) & 1u) == 0u) {
    return false;
  }
  if (requirements.size == 0) {
    return false;
  }

  const uint64_t units64 = (requirements.size + unit_ - 1) / unit_;
  if (units64 > slab_units_) {
    ++gaps_failed_;
    return false;  // does not fit even in an empty block: use the dedicated path
  }
  const uint32_t units = uint32_t(units64);

  /*
   * The alignment is honored exactly; it is not assumed to be 64 KiB.
   *
   * For TILING_OPTIMAL on Switch the driver asks for at least 64 KiB
   * (nvk_image.c:1192-1197), but a 3D texture can ask for up to 512 KB because the level-0
   * tile size includes z_log2 (nouveau/nil/image.rs:430). Assuming 64 KiB would make the
   * bind fail on a driver assert (nvk_image.c:1692), a rare and late failure. Converted to
   * units, the alignment is the step used to search for candidate starts.
   */
  const uint64_t alignment = std::max<uint64_t>(requirements.alignment, unit_);
  const uint32_t step = uint32_t(std::max<uint64_t>(1, alignment / unit_));

  for (uint32_t s = 0; s < slabs_.size(); ++s) {
    Slab& slab = slabs_[s];
    uint32_t start = 0;
    if (!FindGap(slab.occupied, slab.units, units, step, start)) {
      continue;
    }
    MarkRange(slab.occupied, start, units, true);
    slab.run_length[start] = units;
    slab.units_in_use += units;

    memory_out = slab.guest_memory;
    offset_out = gpu::Size(start) * unit_;
    block_out = (s << 24) | start;
    ++textures_live_;
    ++textures_placed_;
    return true;
  }

  ++gaps_failed_;
  return false;
}

void TexturePool::Free(uint32_t block) {
  if (block == kBlockPoolInvalid || !active_) {
    return;
  }
  const uint32_t s = block >> 24;
  const uint32_t start = block & 0x00FFFFFFu;
  if (s >= slabs_.size()) {
    REXLOG_ERROR("[native] C3: texture pool: block {:08X} with a large block that does not exist", block);
    return;
  }
  Slab& slab = slabs_[s];
  if (start >= slab.units || slab.run_length[start] == 0u) {
    REXLOG_ERROR("[native] C3: texture pool: block {:08X} that was not the start of anything (double free?)",
                 block);
    return;
  }
  const uint32_t units = slab.run_length[start];
  MarkRange(slab.occupied, start, units, false);
  slab.run_length[start] = 0u;
  slab.units_in_use -= std::min(slab.units_in_use, units);
  if (textures_live_ > 0) {
    --textures_live_;
  }
}

void TexturePool::PerFrame(uint64_t frame) {
  if (!active_ || slabs_.size() >= slabs_cap_) {
    return;
  }
  if (frame < last_growth_ + kFramesBetweenSlabs) {
    return;
  }
  uint64_t free = 0;
  for (const Slab& slab : slabs_) {
    free += uint64_t(slab.units - slab.units_in_use) * unit_;
  }
  if (free >= kSlackBytes) {
    return;
  }
  last_growth_ = frame;
  CreateSlab(true);
}

TexturePoolState TexturePool::State() const {
  TexturePoolState e;
  e.active = active_;
  e.type_memory = type_memory_;
  e.slabs = uint32_t(slabs_.size());
  e.bytes_reserved = uint64_t(slabs_.size()) * slab_bytes_;
  uint32_t larger = 0;
  for (const Slab& slab : slabs_) {
    e.bytes_in_use += uint64_t(slab.units_in_use) * unit_;
    larger = std::max(larger, LargerGap(slab.occupied, slab.units));
  }
  e.bytes_free = e.bytes_reserved - e.bytes_in_use;
  e.bytes_larger_gap = uint64_t(larger) * unit_;
  e.textures_live = textures_live_;
  e.textures_placed = textures_placed_;
  e.textures_dedicated = textures_dedicated_;
  e.gaps_failed = gaps_failed_;
  e.slabs_in_hot = slabs_in_hot_;
  e.slabs_failed = slabs_failed_;
  return e;
}

std::string TexturePool::Summary() const {
  if (!active_) {
    return std::string("texture pool off (") + std::to_string(textures_dedicated_) +
           " textures with a dedicated allocation)";
  }
  const TexturePoolState e = State();
  /*
   * Fragmentation is what needs watching: if plenty is free but the largest free range is
   * small, large textures start falling back to the dedicated path and the savings vanish
   * without anything failing. That is why both figures are printed together.
   */
  const uint64_t frag = e.bytes_free > 0 ? 100u - (e.bytes_larger_gap * 100u / e.bytes_free) : 0u;
  return std::string("texture pool: ") + std::to_string(e.slabs) + " blocks (" +
         std::to_string(e.bytes_reserved >> 20) + " MB), " + std::to_string(e.bytes_in_use >> 20) +
         " MB in use by " + std::to_string(e.textures_live) + " textures; free " +
         std::to_string(e.bytes_free >> 20) + " MB with the largest gap in " +
         std::to_string(e.bytes_larger_gap >> 20) + " MB (fragmentation " + std::to_string(frag) +
         " %); " + std::to_string(e.textures_placed) + " placed and " +
         std::to_string(e.textures_dedicated) + " to the dedicated path (" + std::to_string(e.gaps_failed) +
         " without a gap); blocks while running " + std::to_string(e.slabs_in_hot) + ", denied " +
         std::to_string(e.slabs_failed);
}

}  // namespace bo2zm::native
