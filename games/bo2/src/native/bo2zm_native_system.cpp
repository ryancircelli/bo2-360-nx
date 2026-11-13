// Derived from GPL-3.0 code; modified for bo2-zm-nx in 2026.
//
// BO2's own graphics system in its minimal "skeleton" form, plus the bo2zm_renderer switch. See
// bo2zm_native_system.h.
//
// The MMIO range, the register file and its side effects, the vblank and ring threads, the PM4 packet reader and the
// test presentation, trimmed to what keeps the game running without drawing anything.

#include "bo2zm_native_system.h"
#include "render/bo2zm_native_render.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstring>
#include <mutex>
#include <string>
#include <vector>

#include <fmt/format.h>
#include <rex/cvar.h>
#include <rex/graphics/registers.h>
#include <rex/graphics/xenos.h>
#include <rex/kernel/xboxkrnl/video.h>
#include <rex/logging.h>
#include <rex/memory/utils.h>
#include <rex/system/function_dispatcher.h>
#include <rex/system/kernel_state.h>
#include <rex/system/xmemory.h>
#include <rex/system/xobject.h>
#include <rex/system/xthread.h>
#include <rex/system/xtypes.h>
#include <rex/system/xvideo.h>
#include <rex/thread.h>
#include <rex/ui/presenter.h>
#include <rex/ui/vulkan/device.h>
#include <rex/ui/vulkan/presenter.h>
#include <rex/ui/vulkan/provider.h>
#include <rex/ui/windowed_app_context.h>

REXCVAR_DEFINE_STRING(bo2zm_renderer, "xenos", "BO2",
                      "Graphics path: \"xenos\" (GPU emulation plugin), \"native\" (the native renderer) "
                      "or \"skeleton\" (keeps the game running, draws a test color)")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);

REXCVAR_DEFINE_BOOL(bo2zm_native_log_vs_misses, false, "BO2",
                    "Native renderer: log vertex shaders from the ring that the library cannot identify");

REXCVAR_DEFINE_BOOL(bo2zm_native_debug_no_depth, false, "BO2",
                    "Native renderer diagnostics: draw everything without depth or stencil testing")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);

REXCVAR_DEFINE_BOOL(bo2zm_native_debug_magenta, false, "BO2",
                    "Native renderer diagnostics: replace every pixel shader with solid magenta")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);

REXCVAR_DEFINE_BOOL(bo2zm_native_clear_quads, true, "BO2",
                    "Native renderer: run Direct3D's Clear() rectangles as clears (off = drop them, as before)")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);

REXCVAR_DEFINE_INT32(bo2zm_native_report_s, 10, "BO2",
                     "Native renderer: seconds between ring reports in the log (0 = off)");

namespace bo2zm::native {
namespace {

namespace xenos = rex::graphics::xenos;
namespace reg = rex::graphics;
using rex::X_STATUS;
using Clock = std::chrono::steady_clock;
using OutputContext = rex::ui::vulkan::VulkanPresenter::VulkanGuestOutputRefreshContext;

std::atomic<uint64_t> g_swaps{0};

constexpr uint32_t kMmioBase = 0x7FC80000;
constexpr uint32_t kMmioMask = 0xFFFF0000;
constexpr uint32_t kMmioSize = 0x0000FFFF;
constexpr uint32_t kRegisterCount = 0x5003;  // RegisterFile::kRegisterCount

constexpr uint32_t kRegCpRbWptr = 0x01C5;
constexpr uint32_t kRegRbEdramTiming = 0x0F00;
constexpr uint32_t kRegRbBcControl = 0x0F01;
constexpr uint32_t kRegD1ModeVCounter = 0x194C;
constexpr uint32_t kRegD1ModeVblankVlineStatus = 0x1951;
constexpr uint32_t kRegD1ModeViewportSize = 0x1961;

constexpr uint32_t kOutputWidth = 1280;
constexpr uint32_t kOutputHeight = 720;
constexpr uint32_t kFakeOcclusionSamples = 1000;  // as Xenia's query_occlusion_fake_sample_count
constexpr uint16_t kMaxExtent = 2048 >> 3;        // Xenia's kTexture2DCubeMaxWidthHeight >> 3
constexpr int kMaxIndirectDepth = 4;              // nesting limit for indirect buffers
constexpr auto kWaitRegMemMax = std::chrono::milliseconds(200);  // a WAIT_REG_MEM is cut short after this
constexpr auto kWaitRegMemPause = std::chrono::microseconds(50);

// Big-endian words of a command buffer. In the ring the position wraps around
// (the size is a power of two); in an indirect buffer it does not.
struct Reader {
  const uint8_t* base = nullptr;
  uint32_t mask = 0;  // words - 1 in the ring; 0 in a linear buffer
  uint32_t pos = 0;
  uint32_t end = 0;

  uint32_t Pending() const { return mask ? ((end - pos) & mask) : (end - pos); }
  uint32_t Peek() const { return rex::memory::load_and_swap<uint32_t>(base + size_t(pos) * 4); }
  uint32_t Read() {
    const uint32_t value = Peek();
    Advance(1);
    return value;
  }
  void Advance(uint32_t words) { pos = mask ? ((pos + words) & mask) : (pos + words); }
};

bool Compare(uint32_t info, uint32_t value, uint32_t reference) {
  switch (info & 0x7) {
    case 0x0: return false;
    case 0x1: return value < reference;
    case 0x2: return value <= reference;
    case 0x3: return value == reference;
    case 0x4: return value != reference;
    case 0x5: return value >= reference;
    case 0x6: return value > reference;
    default: return true;
  }
}

class NativeGraphicsSystem final : public rex::system::IGraphicsSystem {
 public:
  NativeGraphicsSystem() : registers_(kRegisterCount, 0) {}
  ~NativeGraphicsSystem() override { Shutdown(); }

  X_STATUS SetupPresentation(rex::ui::WindowedAppContext* app_context) override {
    if (presenter_) {
      return X_STATUS_SUCCESS;
    }
    app_context_ = app_context;
    if (!provider_) {
      provider_ = rex::ui::vulkan::VulkanProvider::Create(true, true);
      if (!provider_) {
        REXLOG_ERROR("[native] could not create the Vulkan device");
        return X_STATUS_UNSUCCESSFUL;
      }
    }
    auto create = [this]() { presenter_ = provider_->CreatePresenter(); };
    if (app_context_) {
      app_context_->CallInUIThreadSynchronous(create);
    } else {
      create();
    }
    if (!presenter_) {
      REXLOG_ERROR("[native] could not create the presenter");
      return X_STATUS_UNSUCCESSFUL;
    }
    REXLOG_INFO("[native] native graphics system (skeleton): SDK presenter, no Xenos emulation");
    return X_STATUS_SUCCESS;
  }

  X_STATUS SetupGuestGpu(rex::runtime::FunctionDispatcher* dispatcher,
                         rex::system::KernelState* kernel_state) override {
    dispatcher_ = dispatcher;
    kernel_state_ = kernel_state;
    memory_ = dispatcher->memory();
    if (!mmio_registered_) {
      mmio_registered_ =
          memory_->AddVirtualMappedRange(kMmioBase, kMmioMask, kMmioSize, this, &ReadMmio, &WriteMmio);
      if (!mmio_registered_) {
        REXLOG_ERROR("[native] could not register the GPU MMIO range");
        return X_STATUS_UNSUCCESSFUL;
      }
    }
    if (active_.exchange(true)) {
      return X_STATUS_SUCCESS;
    }
    start_ = Clock::now();
    last_report_ = start_;
    vblank_thread_ = rex::system::object_ref<rex::system::XHostThread>(new rex::system::XHostThread(
        kernel_state_, 128 * 1024, 0, [this]() { return VblankLoop(); }));
    vblank_thread_->set_name("GPU VSync (native)");
    vblank_thread_->Create();
    ring_thread_ = rex::system::object_ref<rex::system::XHostThread>(new rex::system::XHostThread(
        kernel_state_, 128 * 1024, 0, [this]() { return RingLoop(); }));
    ring_thread_->set_name("GPU ring (native)");
    ring_thread_->Create();
    return X_STATUS_SUCCESS;
  }

  bool has_presentation() const override { return presenter_ != nullptr; }
  rex::ui::GraphicsProvider* provider() const override { return provider_.get(); }
  rex::ui::Presenter* presenter() const override { return presenter_.get(); }

  void SetInterruptCallback(uint32_t callback, uint32_t user_data) override {
    callback_data_.store(user_data, std::memory_order_release);
    callback_.store(callback, std::memory_order_release);
    REXLOG_INFO("[native] interrupt callback {:08X} ({:08X})", callback, user_data);
  }

  void InitializeRingBuffer(uint32_t ptr, uint32_t size_log2) override {
    // As in CommandProcessor::InitializeRingBuffer: (1 << (size_log2 + 3)) bytes.
    ring_words_.store(uint32_t(1) << (size_log2 + 1), std::memory_order_release);
    ring_base_.store(ptr, std::memory_order_release);
    ring_generation_.fetch_add(1, std::memory_order_acq_rel);
    REXLOG_INFO("[native] ring at {:08X}, {} words", ptr, uint32_t(1) << (size_log2 + 1));
  }

  void EnableReadPointerWriteBack(uint32_t ptr, uint32_t block_size_log2) override {
    (void)block_size_log2;
    read_writeback_.store(ptr, std::memory_order_release);
    REXLOG_INFO("[native] read pointer write-back at {:08X}", ptr);
  }

  void Shutdown() override {
    if (active_.exchange(false)) {
      {
        std::lock_guard<std::mutex> lock(ring_mutex_);
      }
      ring_cv_.notify_all();
      if (ring_thread_) {
        ring_thread_->Wait(0, 0, 0, nullptr);
        ring_thread_.reset();
      }
      if (vblank_thread_) {
        vblank_thread_->Wait(0, 0, 0, nullptr);
        vblank_thread_.reset();
      }
      Report(true);
    }
    DestroyVulkan();  // the framebuffers point to presenter images: before the presenter
    if (presenter_) {
      if (app_context_) {
        app_context_->CallInUIThreadSynchronous([this]() { presenter_.reset(); });
      }
      presenter_.reset();
    }
    provider_.reset();
  }

 private:
  // --- MMIO registers ----------------------------------------------------------

  static uint32_t ReadMmio(void* ppc_context, void* context, uint32_t address) {
    (void)ppc_context;
    return static_cast<NativeGraphicsSystem*>(context)->ReadMmioRegister(address);
  }

  static void WriteMmio(void* ppc_context, void* context, uint32_t address, uint32_t value) {
    (void)ppc_context;
    static_cast<NativeGraphicsSystem*>(context)->WriteMmioRegister(address, value);
  }

  uint32_t ReadMmioRegister(uint32_t address) {
    const uint32_t r = (address & 0xFFFF) / 4;
    switch (r) {
      case kRegRbEdramTiming:
        return 0x08100748;
      case kRegRbBcControl:
        return 0x0000200E;
      case kRegD1ModeVCounter: {
        rex::system::X_VIDEO_MODE mode;
        rex::kernel::xboxkrnl::VdQueryVideoMode(&mode);
        return std::min(uint32_t(mode.display_height), uint32_t(0x0FFF));
      }
      case kRegD1ModeVblankVlineStatus:
        return 1;  // in vblank
      case kRegD1ModeViewportSize: {
        rex::system::X_VIDEO_MODE mode;
        rex::kernel::xboxkrnl::VdQueryVideoMode(&mode);
        const uint32_t width = std::min(uint32_t(mode.display_width), uint32_t(0x0FFF));
        const uint32_t height = std::min(uint32_t(mode.display_height), uint32_t(0x0FFF));
        return (width << 16) | height;
      }
      default:
        return Register(r);
    }
  }

  void WriteMmioRegister(uint32_t address, uint32_t value) {
    const uint32_t r = (address & 0xFFFF) / 4;
    if (r == kRegCpRbWptr) {
      write_pointer_.store(value, std::memory_order_release);
      wptr_writes_.fetch_add(1, std::memory_order_relaxed);
      // Taking the lock for an instant avoids losing the wakeup if the ring
      // thread is between checking and going to sleep.
      {
        std::lock_guard<std::mutex> lock(ring_mutex_);
      }
      ring_cv_.notify_one();
    }
    StoreRegister(r, value);  // no side effects through MMIO (graphics_system.cpp)
  }

  uint32_t Register(uint32_t index) const { return index < kRegisterCount ? registers_[index] : 0; }

  void StoreRegister(uint32_t index, uint32_t value) {
    if (index < kRegisterCount) {
      registers_[index] = value;
    }
  }

  // A write from the ring, with the side effects it has when the GPU processes
  // it (CommandProcessor::WriteRegister). SCRATCH_REG values are copied to
  // memory: D3D writes one and then waits with WAIT_REG_MEM until it shows up at
  // SCRATCH_ADDR (without this, D3D's wait never ends and the game stops).
  void WriteRegister(uint32_t index, uint32_t value) {
    if (index >= kRegisterCount) {
      return;
    }
    if (index == reg::XE_GPU_REG_COHER_STATUS_HOST) {
      value |= UINT32_C(0x80000000);
    }
    registers_[index] = value;
    if (index >= reg::XE_GPU_REG_SCRATCH_REG0 && index <= reg::XE_GPU_REG_SCRATCH_REG7) {
      const uint32_t n = index - reg::XE_GPU_REG_SCRATCH_REG0;
      if ((UINT32_C(1) << n) & registers_[reg::XE_GPU_REG_SCRATCH_UMSK]) {
        const uint32_t address = registers_[reg::XE_GPU_REG_SCRATCH_ADDR] + n * 4;
        rex::memory::store_and_swap<uint32_t>(memory_->TranslatePhysical(address), value);
      }
    }
  }

  // --- GPU memory (the two low bits of the address are the byte order) ----------

  uint32_t ReadMemory(uint32_t address) const {
    uint32_t value;
    std::memcpy(&value, memory_->TranslatePhysical(address & ~uint32_t(0x3)), sizeof(value));
    return xenos::GpuSwap(value, static_cast<xenos::Endian>(address & 0x3));
  }

  void WriteMemory(uint32_t address, uint32_t value) {
    const uint32_t swapped = xenos::GpuSwap(value, static_cast<xenos::Endian>(address & 0x3));
    std::memcpy(memory_->TranslatePhysical(address & ~uint32_t(0x3)), &swapped, sizeof(swapped));
  }

  // --- Threads -------------------------------------------------------------------

  void Interrupt(uint32_t source, uint32_t cpu) {
    const uint32_t callback = callback_.load(std::memory_order_acquire);
    if (!callback || !dispatcher_) {
      return;
    }
    auto* thread = rex::system::XThread::GetCurrentThread();
    if (!thread) {
      return;
    }
    thread->SetActiveCpu(uint8_t(cpu));
    uint64_t args[] = {source, callback_data_.load(std::memory_order_acquire)};
    dispatcher_->ExecuteInterrupt(thread->thread_state(), callback, args, 2);
    interrupts_.fetch_add(1, std::memory_order_relaxed);
  }

  int VblankLoop() {
    rex::system::X_VIDEO_MODE mode;
    rex::kernel::xboxkrnl::VdQueryVideoMode(&mode);
    const double hz = std::max(1.0, double(float(mode.refresh_rate)));
    const auto interval = std::chrono::duration_cast<Clock::duration>(std::chrono::duration<double>(1.0 / hz));
    auto next = Clock::now() + interval;
    while (active_.load(std::memory_order_acquire)) {
      const auto now = Clock::now();
      if (now - next > std::chrono::milliseconds(250)) {
        next = now;  // after a long pause, do not replay hundreds of vblanks
      }
      while (now >= next) {
        counter_.fetch_add(1, std::memory_order_relaxed);
        vblanks_.fetch_add(1, std::memory_order_relaxed);
        Interrupt(0, 2);
        next += interval;
      }
      rex::thread::Sleep(std::chrono::milliseconds(1));
    }
    return 0;
  }

  int RingLoop() {
    uint32_t read = 0;
    uint32_t generation = 0;
    while (active_.load(std::memory_order_acquire)) {
      {
        std::unique_lock<std::mutex> lock(ring_mutex_);
        ring_cv_.wait_for(lock, std::chrono::milliseconds(4), [&] {
          return !active_.load(std::memory_order_acquire) ||
                 write_pointer_.load(std::memory_order_acquire) != read ||
                 ring_generation_.load(std::memory_order_acquire) != generation;
        });
      }
      if (!active_.load(std::memory_order_acquire)) {
        break;
      }
      Report(false);
      const uint32_t gen = ring_generation_.load(std::memory_order_acquire);
      if (gen != generation) {
        generation = gen;
        read = 0;  // InitializeRingBuffer leaves the read pointer at zero
      }
      const uint32_t base = ring_base_.load(std::memory_order_acquire);
      const uint32_t words = ring_words_.load(std::memory_order_acquire);
      if (!base || !words) {
        continue;
      }
      const uint32_t written = write_pointer_.load(std::memory_order_acquire) & (words - 1);
      if (written == read) {
        continue;
      }
      Reader reader;
      reader.base = memory_->TranslatePhysical(base);
      reader.mask = words - 1;
      reader.pos = read;
      reader.end = written;
      while (reader.Pending()) {
        if (!Packet(reader, 0)) {
          break;  // partial packet: the rest comes with the next CP_RB_WPTR
        }
      }
      read = reader.pos;
      // The game's GPU-idle checks (e.g. BO2's sub_828DA6F0) read this back.
      const uint32_t writeback = read_writeback_.load(std::memory_order_acquire);
      if (writeback) {
        rex::memory::store_and_swap<uint32_t>(memory_->TranslatePhysical(writeback), read);
      }
    }
    return 0;
  }

  // --- PM4 packets -----------------------------------------------------------------

  // Consumes a whole packet, or leaves the position untouched if it is not complete yet.
  bool Packet(Reader& reader, int depth) {
    const uint32_t packet = reader.Peek();
    uint32_t count = 0;
    switch (packet >> 30) {
      case 0: count = packet ? ((packet >> 16) & 0x3FFF) + 1 : 0; break;
      case 1: count = 2; break;
      case 2: count = 0; break;
      default: count = ((packet >> 16) & 0x3FFF) + 1; break;
    }
    if (reader.Pending() < count + 1) {
      return false;
    }
    reader.Advance(1);
    ++packets_;
    Reader data = reader;
    reader.Advance(count);
    if (!packet) {
      return true;
    }
    switch (packet >> 30) {
      case 0: {
        const uint32_t index = packet & 0x7FFF;
        const bool single = (packet >> 15) & 0x1;
        for (uint32_t i = 0; i < count; ++i) {
          WriteRegister(single ? index : index + i, data.Read());
        }
        break;
      }
      case 1: {
        const uint32_t value1 = data.Read();
        const uint32_t value2 = data.Read();
        WriteRegister(packet & 0x7FF, value1);
        WriteRegister((packet >> 11) & 0x7FF, value2);
        break;
      }
      case 2:
        break;
      default:
        Type3(data, packet, count, depth);
        break;
    }
    return true;
  }

  void Type3(Reader& data, uint32_t packet, uint32_t words, int depth) {
    const uint32_t opcode = (packet >> 8) & 0x7F;
    ++opcodes_[opcode];
    if ((packet & 0x1) && (!(bin_select_ & bin_mask_) || opcode == xenos::PM4_XE_SWAP)) {
      return;  // predicate fails; predicated Swaps are always dropped
    }
    switch (opcode) {
      case xenos::PM4_INTERRUPT: {
        if (words < 1) break;
        const uint32_t cpus = data.Read();
        for (uint32_t cpu = 0; cpu < 6; ++cpu) {
          if (cpus & (1u << cpu)) {
            Interrupt(1, cpu);
          }
        }
        break;
      }
      case xenos::PM4_XE_SWAP: {
        // VdSwap writes "SWAP", the physical address of the front buffer and its size.
        if (words >= 4) {
          data.Read();
          swap_frontbuffer_ = data.Read();
          swap_width_ = data.Read();
          swap_height_ = data.Read();
        }
        Present();
        break;
      }
      case xenos::PM4_INDIRECT_BUFFER:
      case xenos::PM4_INDIRECT_BUFFER_PFD: {
        if (words < 2) break;
        const uint32_t address = data.Read() & 0x1FFFFFFF;
        const uint32_t length = data.Read() & 0xFFFFF;
        if (depth < kMaxIndirectDepth) {
          IndirectBuffer(address, length, depth + 1);
        }
        break;
      }
      case xenos::PM4_WAIT_REG_MEM: {
        if (words < 5) break;
        const uint32_t info = data.Read();
        const uint32_t poll = data.Read();
        const uint32_t reference = data.Read();
        const uint32_t mask = data.Read();
        const auto limit = Clock::now() + kWaitRegMemMax;
        for (;;) {
          if (!(info & 0x10) && poll == reg::XE_GPU_REG_COHER_STATUS_HOST &&
              (Register(poll) & UINT32_C(0x80000000))) {
            StoreRegister(poll, 0);  // MakeCoherent: no shared memory to synchronize
          }
          const uint32_t value = (info & 0x10) ? ReadMemory(poll) : Register(poll);
          if (Compare(info, value & mask, reference)) {
            break;
          }
          // Without a GPU some conditions never come true: cut the wait short so the ring does not hang.
          if (Clock::now() >= limit || !active_.load(std::memory_order_acquire)) {
            if (wait_timeouts_++ == 0) {
              REXLOG_WARN("[native] WAIT_REG_MEM not met ({} {:08X} ref {:08X} mask {:08X})",
                          (info & 0x10) ? "memory" : "register", poll, reference, mask);
            }
            break;
          }
          rex::thread::Sleep(kWaitRegMemPause);
        }
        break;
      }
      case xenos::PM4_REG_RMW: {
        if (words < 3) break;
        const uint32_t info = data.Read();
        const uint32_t and_value = data.Read();
        const uint32_t or_value = data.Read();
        uint32_t value = Register(info & 0x1FFF);
        value &= ((info >> 31) & 0x1) ? Register(and_value & 0x1FFF) : and_value;
        value |= ((info >> 30) & 0x1) ? Register(or_value & 0x1FFF) : or_value;
        WriteRegister(info & 0x1FFF, value);
        break;
      }
      case xenos::PM4_REG_TO_MEM: {
        if (words < 2) break;
        const uint32_t index = data.Read();
        const uint32_t address = data.Read();
        WriteMemory(address, Register(index));
        break;
      }
      case xenos::PM4_MEM_WRITE: {
        if (words < 1) break;
        uint32_t address = data.Read();
        for (uint32_t i = 1; i < words; ++i) {
          WriteMemory(address, data.Read());
          address += 4;
        }
        break;
      }
      case xenos::PM4_COND_WRITE: {
        if (words < 6) break;
        const uint32_t info = data.Read();
        const uint32_t poll = data.Read();
        const uint32_t reference = data.Read();
        const uint32_t mask = data.Read();
        const uint32_t target = data.Read();
        const uint32_t value = data.Read();
        const uint32_t current = (info & 0x10) ? ReadMemory(poll) : Register(poll);
        if (Compare(info, current & mask, reference)) {
          if (info & 0x100) {
            WriteMemory(target, value);
          } else {
            WriteRegister(target, value);
          }
        }
        break;
      }
      case xenos::PM4_EVENT_WRITE:
        if (words < 1) break;
        WriteRegister(reg::XE_GPU_REG_VGT_EVENT_INITIATOR, data.Read() & 0x3F);
        break;
      case xenos::PM4_EVENT_WRITE_SHD: {
        if (words < 3) break;
        const uint32_t initiator = data.Read();
        const uint32_t address = data.Read();
        const uint32_t value = data.Read();
        WriteRegister(reg::XE_GPU_REG_VGT_EVENT_INITIATOR, initiator & 0x3F);
        WriteMemory(address, ((initiator >> 31) & 0x1) ? counter_.load(std::memory_order_relaxed) : value);
        break;
      }
      case xenos::PM4_EVENT_WRITE_EXT: {
        if (words < 2) break;
        const uint32_t initiator = data.Read();
        const uint32_t address = data.Read();
        WriteRegister(reg::XE_GPU_REG_VGT_EVENT_INITIATOR, initiator & 0x3F);
        // Screen extent of the previous draw: the maximum is faked.
        const uint16_t extent[] = {0, kMaxExtent, 0, kMaxExtent, 0, 1};
        uint8_t* target = memory_->TranslatePhysical(address & ~uint32_t(0x3));
        for (size_t i = 0; i < std::size(extent); ++i) {
          rex::memory::store_and_swap<uint16_t>(target + i * 2, extent[i]);
        }
        break;
      }
      case xenos::PM4_EVENT_WRITE_ZPD: {
        if (words < 1) break;
        WriteRegister(reg::XE_GPU_REG_VGT_EVENT_INITIATOR, data.Read() & 0x3F);
        const uint32_t address = Register(reg::XE_GPU_REG_RB_SAMPLE_COUNT_ADDR);
        if (address) {
          OcclusionQuery(address);
        }
        break;
      }
      case xenos::PM4_SET_CONSTANT: {
        if (words < 1) break;
        const uint32_t type_index = data.Read();
        const uint32_t base = ConstantBase(type_index);
        if (base != UINT32_MAX) {
          const uint32_t index = base + (type_index & 0x7FF);
          for (uint32_t i = 1; i < words; ++i) {
            WriteRegister(index + i - 1, data.Read());
          }
        }
        break;
      }
      case xenos::PM4_SET_CONSTANT2:
      case xenos::PM4_SET_SHADER_CONSTANTS: {
        // Same format: first value = register index, the rest is written consecutively.
        if (words < 1) break;
        const uint32_t index = data.Read() & 0xFFFF;
        for (uint32_t i = 1; i < words; ++i) {
          WriteRegister(index + i - 1, data.Read());
        }
        break;
      }
      case xenos::PM4_LOAD_ALU_CONSTANT: {
        // Registers loaded from guest memory instead of from the ring.
        if (words < 3) break;
        const uint32_t address = data.Read() & 0x3FFFFFFF;
        const uint32_t type_index = data.Read();
        const uint32_t amount = data.Read() & 0xFFF;
        const uint32_t base = ConstantBase(type_index);
        if (base != UINT32_MAX) {
          const uint8_t* source = memory_->TranslatePhysical(address);
          const uint32_t index = base + (type_index & 0x7FF);
          for (uint32_t i = 0; i < amount; ++i) {
            WriteRegister(index + i, rex::memory::load_and_swap<uint32_t>(source + size_t(i) * 4));
          }
        }
        break;
      }
      case xenos::PM4_SET_BIN_MASK_LO:
        if (words < 1) break;
        bin_mask_ = (bin_mask_ & 0xFFFFFFFF00000000ull) | data.Read();
        break;
      case xenos::PM4_SET_BIN_MASK_HI:
        if (words < 1) break;
        bin_mask_ = (bin_mask_ & 0xFFFFFFFFull) | (uint64_t(data.Read()) << 32);
        break;
      case xenos::PM4_SET_BIN_SELECT_LO:
        if (words < 1) break;
        bin_select_ = (bin_select_ & 0xFFFFFFFF00000000ull) | data.Read();
        break;
      case xenos::PM4_SET_BIN_SELECT_HI:
        if (words < 1) break;
        bin_select_ = (bin_select_ & 0xFFFFFFFFull) | (uint64_t(data.Read()) << 32);
        break;
      case xenos::PM4_SET_BIN_MASK: {
        if (words < 2) break;
        const uint64_t high = data.Read();
        const uint64_t low = data.Read();
        bin_mask_ = (high << 32) | low;
        break;
      }
      case xenos::PM4_SET_BIN_SELECT: {
        if (words < 2) break;
        const uint64_t high = data.Read();
        const uint64_t low = data.Read();
        bin_select_ = (high << 32) | low;
        break;
      }
      case xenos::PM4_VIZ_QUERY: {
        // As the emulated path: when the query ends it is reported as visible.
        if (words < 1) break;
        const uint32_t value = data.Read();
        const uint32_t id = value & 0x3F;
        if (!(value & 0x100)) {
          WriteRegister(reg::XE_GPU_REG_VGT_EVENT_INITIATOR, xenos::VIZQUERY_START);
        } else {
          WriteRegister(reg::XE_GPU_REG_VGT_EVENT_INITIATOR, xenos::VIZQUERY_END);
          const uint32_t status = id < 32 ? uint32_t(reg::XE_GPU_REG_PA_SC_VIZ_QUERY_STATUS_0)
                                          : uint32_t(reg::XE_GPU_REG_PA_SC_VIZ_QUERY_STATUS_1);
          StoreRegister(status, Register(status) | (UINT32_C(1) << (id < 32 ? id : id - 32)));
        }
        break;
      }
      case xenos::PM4_DRAW_INDX:
      case xenos::PM4_DRAW_INDX_2: {
        // DRAW_INDX is preceded by the visibility query word.
        const uint32_t skip = opcode == xenos::PM4_DRAW_INDX ? 1 : 0;
        if (words < skip + 1) break;
        if (skip) {
          data.Read();
        }
        const uint32_t initiator = data.Read();
        WriteRegister(reg::XE_GPU_REG_VGT_DRAW_INITIATOR, initiator);
        if (((initiator >> 6) & 0x3) == uint32_t(xenos::SourceSelect::kDMA)) {
          if (words < skip + 3) break;
          WriteRegister(reg::XE_GPU_REG_VGT_DMA_BASE, data.Read());
          WriteRegister(reg::XE_GPU_REG_VGT_DMA_SIZE, data.Read());
        }
        // A draw with RB_MODECONTROL in copy mode is a resolve and/or a clear.
        if ((Register(reg::XE_GPU_REG_RB_MODECONTROL) & 0x7) == uint32_t(xenos::EdramMode::kCopy)) {
          ++resolves_;
        } else {
          ++draws_;
        }
        break;
      }
      default:
        break;  // shader loads, invalidations...: only the full renderer handles these
    }
  }

  static uint32_t ConstantBase(uint32_t type_index) {
    switch ((type_index >> 16) & 0xFF) {
      case 0: return 0x4000;  // ALU
      case 1: return 0x4800;  // fetch
      case 2: return 0x4900;  // bool
      case 3: return 0x4908;  // loop
      case 4: return 0x2000;  // registers
      default: return UINT32_MAX;
    }
  }

  // Occlusion query results: a faked "visible" count, as the emulated path does
  // (D3D marks pending results with 0xFFFFFEED).
  void OcclusionQuery(uint32_t address) {
    using Counts = xenos::xe_gpu_depth_sample_counts;
    constexpr uint32_t kMark = 0xEDFEFFFF;  // 0xFFFFFEED written big-endian by D3D, read as little-endian
    auto* counts = memory_->TranslatePhysical<Counts*>(address);
    const bool marked = uint32_t(counts->ZPass_A) == kMark || uint32_t(counts->ZPass_B) == kMark ||
                        uint32_t(counts->ZFail_A) == kMark || uint32_t(counts->ZFail_B) == kMark;
    std::memset(counts, 0, sizeof(*counts));
    if (marked) {
      counts->ZPass_A = kFakeOcclusionSamples;
      counts->Total_A = kFakeOcclusionSamples;
    }
    ++occlusion_queries_;
  }

  void IndirectBuffer(uint32_t address, uint32_t words, int depth) {
    ++indirect_buffers_;
    Reader reader;
    reader.base = memory_->TranslatePhysical(address);
    reader.pos = 0;
    reader.end = words;
    while (reader.Pending()) {
      if (!Packet(reader, depth)) {
        break;
      }
    }
  }

  // --- Test presentation --------------------------------------------------------------

  void Present() {
    const uint64_t swap = ++swaps_;
    g_swaps.fetch_add(1, std::memory_order_relaxed);
    counter_.fetch_add(1, std::memory_order_relaxed);
    if (!presenter_) {
      return;
    }
    presenter_->RefreshGuestOutput(kOutputWidth, kOutputHeight, kOutputWidth, kOutputHeight,
                                   [this, swap](rex::ui::Presenter::GuestOutputRefreshContext& context) {
                                     return ClearOutput(static_cast<OutputContext&>(context), swap);
                                   });
  }

  bool ClearOutput(OutputContext& context, uint64_t swap) {
    const rex::ui::vulkan::VulkanDevice* device_info = provider_->vulkan_device();
    const auto& dfn = device_info->functions();
    const VkDevice device = device_info->device();

    if (render_pass_ == VK_NULL_HANDLE) {
      VkAttachmentDescription attachment{};
      attachment.format = rex::ui::vulkan::VulkanPresenter::kGuestOutputFormat;
      attachment.samples = VK_SAMPLE_COUNT_1_BIT;
      attachment.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
      attachment.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
      attachment.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
      attachment.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
      attachment.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
      attachment.finalLayout = rex::ui::vulkan::VulkanPresenter::kGuestOutputInternalLayout;
      VkAttachmentReference reference{0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
      VkSubpassDescription subpass{};
      subpass.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
      subpass.colorAttachmentCount = 1;
      subpass.pColorAttachments = &reference;
      VkRenderPassCreateInfo info{};
      info.sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO;
      info.attachmentCount = 1;
      info.pAttachments = &attachment;
      info.subpassCount = 1;
      info.pSubpasses = &subpass;
      if (dfn.vkCreateRenderPass(device, &info, nullptr, &render_pass_) != VK_SUCCESS) {
        render_pass_ = VK_NULL_HANDLE;
        return false;
      }
    }

    if (pool_ == VK_NULL_HANDLE) {
      VkCommandPoolCreateInfo info{};
      info.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
      info.queueFamilyIndex = device_info->queue_family_graphics_compute();
      if (dfn.vkCreateCommandPool(device, &info, nullptr, &pool_) != VK_SUCCESS) {
        pool_ = VK_NULL_HANDLE;
        return false;
      }
      VkCommandBufferAllocateInfo allocate{};
      allocate.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
      allocate.commandPool = pool_;
      allocate.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
      allocate.commandBufferCount = 1;
      if (dfn.vkAllocateCommandBuffers(device, &allocate, &commands_) != VK_SUCCESS) {
        commands_ = VK_NULL_HANDLE;
      }
      VkFenceCreateInfo fence_info{};
      fence_info.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
      if (dfn.vkCreateFence(device, &fence_info, nullptr, &fence_) != VK_SUCCESS) {
        fence_ = VK_NULL_HANDLE;
      }
    }
    if (commands_ == VK_NULL_HANDLE || fence_ == VK_NULL_HANDLE) {
      return false;
    }

    // One command buffer: wait for the previous submission before reusing it.
    if (fence_pending_) {
      dfn.vkWaitForFences(device, 1, &fence_, VK_TRUE, UINT64_MAX);
      dfn.vkResetFences(device, 1, &fence_);
      fence_pending_ = false;
    }
    dfn.vkResetCommandPool(device, pool_, 0);

    VkFramebuffer framebuffer = VK_NULL_HANDLE;
    for (const Framebuffer& f : framebuffers_) {
      if (f.framebuffer != VK_NULL_HANDLE && f.version == context.image_version()) {
        framebuffer = f.framebuffer;
      }
    }
    if (framebuffer == VK_NULL_HANDLE) {
      Framebuffer& f = framebuffers_[next_framebuffer_];
      next_framebuffer_ = (next_framebuffer_ + 1) % framebuffers_.size();
      if (f.framebuffer != VK_NULL_HANDLE) {
        dfn.vkDestroyFramebuffer(device, f.framebuffer, nullptr);
        f.framebuffer = VK_NULL_HANDLE;
      }
      VkImageView view = context.image_view();
      VkFramebufferCreateInfo info{};
      info.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
      info.renderPass = render_pass_;
      info.attachmentCount = 1;
      info.pAttachments = &view;
      info.width = kOutputWidth;
      info.height = kOutputHeight;
      info.layers = 1;
      if (dfn.vkCreateFramebuffer(device, &info, nullptr, &f.framebuffer) != VK_SUCCESS) {
        f.framebuffer = VK_NULL_HANDLE;
        return false;
      }
      f.version = context.image_version();
      framebuffer = f.framebuffer;
    }

    VkCommandBufferBeginInfo begin{};
    begin.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    if (dfn.vkBeginCommandBuffer(commands_, &begin) != VK_SUCCESS) {
      return false;
    }
    // Test color: green goes up and down with each Swap.
    const float phase = float(swap % 240) / 239.0f;
    VkClearValue color{};
    color.color.float32[0] = 0.05f;
    color.color.float32[1] = 0.10f + 0.40f * phase;
    color.color.float32[2] = 0.35f;
    color.color.float32[3] = 1.0f;
    VkRenderPassBeginInfo pass{};
    pass.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
    pass.renderPass = render_pass_;
    pass.framebuffer = framebuffer;
    pass.renderArea.extent.width = kOutputWidth;
    pass.renderArea.extent.height = kOutputHeight;
    pass.clearValueCount = 1;
    pass.pClearValues = &color;
    dfn.vkCmdBeginRenderPass(commands_, &pass, VK_SUBPASS_CONTENTS_INLINE);
    dfn.vkCmdEndRenderPass(commands_);
    if (dfn.vkEndCommandBuffer(commands_) != VK_SUCCESS) {
      return false;
    }
    VkSubmitInfo submit{};
    submit.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    submit.commandBufferCount = 1;
    submit.pCommandBuffers = &commands_;
    {
      const auto queue = device_info->AcquireQueue(device_info->queue_family_graphics_compute(), 0);
      if (dfn.vkQueueSubmit(queue.queue(), 1, &submit, fence_) != VK_SUCCESS) {
        return false;
      }
    }
    fence_pending_ = true;
    context.SetIs8bpc(true);
    return true;
  }

  void DestroyVulkan() {
    if (!provider_ || !provider_->vulkan_device()) {
      return;
    }
    const rex::ui::vulkan::VulkanDevice* device_info = provider_->vulkan_device();
    const auto& dfn = device_info->functions();
    const VkDevice device = device_info->device();
    if (fence_ != VK_NULL_HANDLE) {
      if (fence_pending_) {
        dfn.vkWaitForFences(device, 1, &fence_, VK_TRUE, UINT64_MAX);
        fence_pending_ = false;
      }
      dfn.vkDestroyFence(device, fence_, nullptr);
      fence_ = VK_NULL_HANDLE;
    }
    for (Framebuffer& f : framebuffers_) {
      if (f.framebuffer != VK_NULL_HANDLE) {
        dfn.vkDestroyFramebuffer(device, f.framebuffer, nullptr);
        f = Framebuffer{};
      }
    }
    if (pool_ != VK_NULL_HANDLE) {
      dfn.vkDestroyCommandPool(device, pool_, nullptr);
      pool_ = VK_NULL_HANDLE;
      commands_ = VK_NULL_HANDLE;
    }
    if (render_pass_ != VK_NULL_HANDLE) {
      dfn.vkDestroyRenderPass(device, render_pass_, nullptr);
      render_pass_ = VK_NULL_HANDLE;
    }
  }

  // --- Report --------------------------------------------------------------------------

  // Ring thread only (plus once at shutdown, after it stopped).
  void Report(bool final) {
    const int32_t seconds = REXCVAR_GET(bo2zm_native_report_s);
    const auto now = Clock::now();
    if (!final && (seconds <= 0 || now - last_report_ < std::chrono::seconds(seconds))) {
      return;
    }
    const double window = std::chrono::duration<double>(now - last_report_).count();
    last_report_ = now;
    const auto per_s = [window](uint64_t now_value, uint64_t& before) {
      const double rate = window > 0 ? double(now_value - before) / window : 0.0;
      before = now_value;
      return rate;
    };
    const uint64_t swaps = swaps_;
    const double swaps_per_s = per_s(swaps, last_.swaps);
    const double draws_per_frame = swaps - last_.frames_at_draws
                                       ? double(draws_ - last_.draws) / double(swaps - last_.frames_at_draws)
                                       : 0.0;
    const double resolves_per_frame =
        swaps - last_.frames_at_draws ? double(resolves_ - last_.resolves) / double(swaps - last_.frames_at_draws)
                                      : 0.0;
    last_.draws = draws_;
    last_.resolves = resolves_;
    last_.frames_at_draws = swaps;
    REXLOG_INFO(
        "[native] {:.0f} s: {} swaps ({:.1f}/s) | per frame: {:.1f} draws, {:.1f} resolves | packets {:.0f}/s, "
        "indirect buffers {:.0f}/s, interrupts {:.0f}/s, vblanks {:.0f}/s, CP_RB_WPTR writes {:.0f}/s | "
        "WAIT_REG_MEM timeouts {}, occlusion queries {}",
        std::chrono::duration<double>(now - start_).count(), swaps, swaps_per_s, draws_per_frame,
        resolves_per_frame, per_s(packets_, last_.packets), per_s(indirect_buffers_, last_.indirect),
        per_s(interrupts_.load(std::memory_order_relaxed), last_.interrupts),
        per_s(vblanks_.load(std::memory_order_relaxed), last_.vblanks),
        per_s(wptr_writes_.load(std::memory_order_relaxed), last_.wptr), wait_timeouts_, occlusion_queries_);
    if (final) {
      std::string seen;
      for (uint32_t op = 0; op < opcodes_.size(); ++op) {
        if (opcodes_[op]) {
          seen += fmt::format(" {:02X}:{}", op, opcodes_[op]);
        }
      }
      REXLOG_INFO("[native] type-3 opcodes seen (opcode:count):{}", seen);
    }
  }

  // --- State ------------------------------------------------------------------------------

  rex::ui::WindowedAppContext* app_context_ = nullptr;
  std::unique_ptr<rex::ui::vulkan::VulkanProvider> provider_;
  std::unique_ptr<rex::ui::Presenter> presenter_;
  rex::runtime::FunctionDispatcher* dispatcher_ = nullptr;
  rex::system::KernelState* kernel_state_ = nullptr;
  rex::memory::Memory* memory_ = nullptr;
  bool mmio_registered_ = false;

  std::atomic<bool> active_{false};
  rex::system::object_ref<rex::system::XHostThread> vblank_thread_;
  rex::system::object_ref<rex::system::XHostThread> ring_thread_;
  std::mutex ring_mutex_;
  std::condition_variable ring_cv_;

  std::atomic<uint32_t> callback_{0};
  std::atomic<uint32_t> callback_data_{0};
  std::atomic<uint32_t> ring_base_{0};
  std::atomic<uint32_t> ring_words_{0};
  std::atomic<uint32_t> ring_generation_{0};
  std::atomic<uint32_t> write_pointer_{0};
  std::atomic<uint32_t> read_writeback_{0};
  std::atomic<uint32_t> counter_{0};  // vblanks + swaps, for EVENT_WRITE_SHD

  std::vector<uint32_t> registers_;
  uint64_t bin_mask_ = ~uint64_t(0);
  uint64_t bin_select_ = ~uint64_t(0);
  uint32_t swap_frontbuffer_ = 0;
  uint32_t swap_width_ = 0;
  uint32_t swap_height_ = 0;

  // Vulkan test output.
  struct Framebuffer {
    VkFramebuffer framebuffer = VK_NULL_HANDLE;
    uint64_t version = 0;
  };
  VkRenderPass render_pass_ = VK_NULL_HANDLE;
  VkCommandPool pool_ = VK_NULL_HANDLE;
  VkCommandBuffer commands_ = VK_NULL_HANDLE;
  VkFence fence_ = VK_NULL_HANDLE;
  bool fence_pending_ = false;
  std::array<Framebuffer, 3> framebuffers_{};
  size_t next_framebuffer_ = 0;

  // Counters. Plain ones are written by the ring thread only.
  uint64_t swaps_ = 0;
  uint64_t packets_ = 0;
  uint64_t draws_ = 0;
  uint64_t resolves_ = 0;
  uint64_t indirect_buffers_ = 0;
  uint64_t wait_timeouts_ = 0;
  uint64_t occlusion_queries_ = 0;
  std::array<uint64_t, 128> opcodes_{};
  std::atomic<uint64_t> interrupts_{0};
  std::atomic<uint64_t> vblanks_{0};
  std::atomic<uint64_t> wptr_writes_{0};
  Clock::time_point start_{};
  Clock::time_point last_report_{};
  struct {
    uint64_t swaps = 0, packets = 0, indirect = 0, interrupts = 0, vblanks = 0, wptr = 0;
    uint64_t draws = 0, resolves = 0, frames_at_draws = 0;
  } last_;
};

}  // namespace

// bo2zm_renderer is init-only, so it is read once. PortedRendererActive is asked on every poll of the Backend thread's
// ring wait (bo2zm_waits.cpp, sub_828DA6F0), and a string comparison each time was about 2 % of the Backend's running
// samples on the PC.
bool Active() {
  static const bool value = [] {
    const std::string& renderer = REXCVAR_GET(bo2zm_renderer);
    return renderer == "native" || renderer == "skeleton";
  }();
  return value;
}

bool PortedRendererActive() {
  static const bool value = REXCVAR_GET(bo2zm_renderer) == "native";
  return value;
}

bool DebugMagenta() {
  static const bool value = REXCVAR_GET(bo2zm_native_debug_magenta);
  return value;
}

bool DebugNoDepth() {
  static const bool value = REXCVAR_GET(bo2zm_native_debug_no_depth);
  return value;
}

bool ClearQuads() {
  static const bool value = REXCVAR_GET(bo2zm_native_clear_quads);
  return value;
}

bool LogVertexShaderMisses() {
  return REXCVAR_GET(bo2zm_native_log_vs_misses);
}

uint64_t Swaps() {
  return g_swaps.load(std::memory_order_relaxed);
}

std::unique_ptr<rex::system::IGraphicsSystem> CreateGraphicsSystem() {
  if (PortedRendererActive()) {
    return bo2zm::native::CreateSystemGraphics();
  }
  return std::make_unique<NativeGraphicsSystem>();
}

}  // namespace bo2zm::native
