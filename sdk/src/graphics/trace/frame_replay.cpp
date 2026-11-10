/**
 * Offline replay of a .xtr trace with per-draw render target readback.
 * See frame_replay.h for attribution.
 *
 * Readback is backend agnostic: after a draw, the replayer temporarily
 * programs a resolve (EDRAM copy) of each written render target into a
 * scratch region of guest memory, in k_32_32_32_32_FLOAT for color and in
 * the raw 32 bpp depth format for depth, executes it through the normal
 * draw path with readback_resolve=full, and restores the registers it
 * touched. Every backend that implements resolves therefore produces
 * directly comparable data.
 */

#include "frame_replay.h"
#if defined(FABLE2_NO_CONVERTER)
#include <rex/graphics/trace_writer.h>
#endif

#include <algorithm>
#include <bit>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <future>
#include <map>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

#include <zlib.h>

#include <fmt/format.h>

#include <rex/cvar.h>
#include <rex/graphics/command_processor.h>
#include <rex/graphics/graphics_system.h>
#include <rex/graphics/pipeline/shader/shader.h>
#include <rex/graphics/pipeline/texture/util.h>
#include <rex/graphics/register_file.h>
#include <rex/graphics/registers.h>
#include <rex/graphics/trace/texture_probe.h>
#include <rex/graphics/trace/trace_protocol.h>
#include <rex/graphics/util/draw.h>
#include <rex/graphics/xenos.h>
#include <rex/hash.h>
#include <rex/logging.h>
#include <rex/system/xmemory.h>
#include <rex/thread/mutex.h>

#include "texture_check.h"
#include "trace_access.h"

namespace rex::graphics::trace {

namespace {

constexpr uint32_t kDefaultScratchAddress = 0x1C000000;
constexpr uint32_t kScratchSize = 32 * 1024 * 1024;
// Layout inside the scratch region.
constexpr uint32_t kScratchPm4Offset = 0;
constexpr uint32_t kScratchVertexOffset = 0x100;
constexpr uint32_t kScratchImageOffset = 0x1000;

struct NamedRegister {
  uint32_t index;
  const char* name;
  bool is_float;
};

#define REX_TRACE_REG(name) {XE_GPU_REG_##name, #name, false}
#define REX_TRACE_REG_F(name) {XE_GPU_REG_##name, #name, true}
const NamedRegister kStateRegisters[] = {
    REX_TRACE_REG(RB_MODECONTROL),
    REX_TRACE_REG(RB_SURFACE_INFO),
    REX_TRACE_REG(RB_COLOR_INFO),
    REX_TRACE_REG(RB_COLOR1_INFO),
    REX_TRACE_REG(RB_COLOR2_INFO),
    REX_TRACE_REG(RB_COLOR3_INFO),
    REX_TRACE_REG(RB_DEPTH_INFO),
    REX_TRACE_REG(RB_COLOR_MASK),
    REX_TRACE_REG(RB_COLORCONTROL),
    REX_TRACE_REG(RB_DEPTHCONTROL),
    REX_TRACE_REG(RB_STENCILREFMASK),
    REX_TRACE_REG(RB_STENCILREFMASK_BF),
    REX_TRACE_REG(RB_BLENDCONTROL0),
    REX_TRACE_REG(RB_BLENDCONTROL1),
    REX_TRACE_REG(RB_BLENDCONTROL2),
    REX_TRACE_REG(RB_BLENDCONTROL3),
    REX_TRACE_REG_F(RB_BLEND_RED),
    REX_TRACE_REG_F(RB_BLEND_GREEN),
    REX_TRACE_REG_F(RB_BLEND_BLUE),
    REX_TRACE_REG_F(RB_BLEND_ALPHA),
    REX_TRACE_REG_F(RB_ALPHA_REF),
    REX_TRACE_REG(RB_EDRAM_INFO),
    REX_TRACE_REG(PA_SU_SC_MODE_CNTL),
    REX_TRACE_REG_F(PA_SU_POLY_OFFSET_FRONT_SCALE),
    REX_TRACE_REG_F(PA_SU_POLY_OFFSET_FRONT_OFFSET),
    REX_TRACE_REG_F(PA_SU_POLY_OFFSET_BACK_SCALE),
    REX_TRACE_REG_F(PA_SU_POLY_OFFSET_BACK_OFFSET),
    REX_TRACE_REG(PA_SU_VTX_CNTL),
    REX_TRACE_REG(PA_SU_POINT_SIZE),
    REX_TRACE_REG(PA_SU_LINE_CNTL),
    REX_TRACE_REG(PA_CL_VTE_CNTL),
    REX_TRACE_REG(PA_CL_CLIP_CNTL),
    REX_TRACE_REG_F(PA_CL_VPORT_XSCALE),
    REX_TRACE_REG_F(PA_CL_VPORT_XOFFSET),
    REX_TRACE_REG_F(PA_CL_VPORT_YSCALE),
    REX_TRACE_REG_F(PA_CL_VPORT_YOFFSET),
    REX_TRACE_REG_F(PA_CL_VPORT_ZSCALE),
    REX_TRACE_REG_F(PA_CL_VPORT_ZOFFSET),
    REX_TRACE_REG(PA_SC_WINDOW_OFFSET),
    REX_TRACE_REG(PA_SC_WINDOW_SCISSOR_TL),
    REX_TRACE_REG(PA_SC_WINDOW_SCISSOR_BR),
    REX_TRACE_REG(PA_SC_SCREEN_SCISSOR_TL),
    REX_TRACE_REG(PA_SC_SCREEN_SCISSOR_BR),
    REX_TRACE_REG(PA_SC_AA_CONFIG),
    REX_TRACE_REG(PA_SC_AA_MASK),
    REX_TRACE_REG(PA_SC_LINE_CNTL),
    REX_TRACE_REG(PA_SC_VIZ_QUERY),
    REX_TRACE_REG(VGT_DRAW_INITIATOR),
    REX_TRACE_REG(VGT_DMA_BASE),
    REX_TRACE_REG(VGT_DMA_SIZE),
    REX_TRACE_REG(VGT_INDX_OFFSET),
    REX_TRACE_REG(VGT_MIN_VTX_INDX),
    REX_TRACE_REG(VGT_MAX_VTX_INDX),
    REX_TRACE_REG(VGT_MULTI_PRIM_IB_RESET_INDX),
    REX_TRACE_REG(SQ_PROGRAM_CNTL),
    REX_TRACE_REG(SQ_CONTEXT_MISC),
    REX_TRACE_REG(RB_COPY_CONTROL),
    REX_TRACE_REG(RB_COPY_DEST_BASE),
    REX_TRACE_REG(RB_COPY_DEST_PITCH),
    REX_TRACE_REG(RB_COPY_DEST_INFO),
    REX_TRACE_REG(RB_DEPTH_CLEAR),
    REX_TRACE_REG(RB_COLOR_CLEAR),
};
#undef REX_TRACE_REG
#undef REX_TRACE_REG_F

// Registers the readback resolve overrides.
const uint32_t kReadbackRegisters[] = {
    XE_GPU_REG_RB_MODECONTROL,       XE_GPU_REG_RB_COPY_CONTROL,
    XE_GPU_REG_RB_COPY_DEST_BASE,    XE_GPU_REG_RB_COPY_DEST_PITCH,
    XE_GPU_REG_RB_COPY_DEST_INFO,    XE_GPU_REG_SHADER_CONSTANT_FETCH_00_0,
    XE_GPU_REG_SHADER_CONSTANT_FETCH_00_1, XE_GPU_REG_PA_SU_SC_MODE_CNTL,
    XE_GPU_REG_PA_SC_VIZ_QUERY,      XE_GPU_REG_VGT_DRAW_INITIATOR,
};

std::string JsonEscape(std::string_view s) {
  std::string out;
  for (char c : s) {
    if (c == '"' || c == '\\') {
      out.push_back('\\');
      out.push_back(c);
    } else if (uint8_t(c) < 0x20) {
      out += fmt::format("\\u{:04x}", uint32_t(uint8_t(c)));
    } else {
      out.push_back(c);
    }
  }
  return out;
}

std::string JsonFloat(float f) {
  if (!std::isfinite(f)) {
    return fmt::format("\"{}\"", f);
  }
  return fmt::format("{}", f);
}

// Minimal .npy writer (format version 1.0).
bool WriteNpy(const std::filesystem::path& path, const char* descr, uint32_t height,
              uint32_t width, uint32_t channels, const void* data, size_t bytes) {
  std::string header =
      channels > 1
          ? fmt::format("{{'descr': '{}', 'fortran_order': False, 'shape': ({}, {}, {}), }}", descr,
                        height, width, channels)
          : fmt::format("{{'descr': '{}', 'fortran_order': False, 'shape': ({}, {}), }}", descr,
                        height, width);
  size_t total = 10 + header.size() + 1;
  size_t pad = (64 - total % 64) % 64;
  header.append(pad, ' ');
  header.push_back('\n');
  std::FILE* f = std::fopen(path.string().c_str(), "wb");
  if (!f) {
    return false;
  }
  const uint8_t magic[8] = {0x93, 'N', 'U', 'M', 'P', 'Y', 1, 0};
  uint16_t header_len = uint16_t(header.size());
  std::fwrite(magic, 1, sizeof(magic), f);
  std::fwrite(&header_len, 1, 2, f);
  std::fwrite(header.data(), 1, header.size(), f);
  std::fwrite(data, 1, bytes, f);
  std::fclose(f);
  return true;
}

// Collects draws for readback. When the plugin was started with capture
// enabled (REX_GPU_TRACE_* environment), forwards every event to the capture
// so a replayed trace can be captured again (round trip test of the capture
// path without a game).
class ReplayHooks : public TraceHooks {
 public:
  bool is_replaying() const override { return true; }
  bool is_recording() const override { return chain && !suppress && chain->is_recording(); }
  void OnPrimaryBufferStart(uint32_t base_ptr, uint32_t count) override {
    if (forward()) chain->OnPrimaryBufferStart(base_ptr, count);
  }
  void OnPrimaryBufferEnd() override {
    if (forward()) chain->OnPrimaryBufferEnd();
  }
  void OnIndirectBufferStart(uint32_t base_ptr, uint32_t count) override {
    if (forward()) chain->OnIndirectBufferStart(base_ptr, count);
  }
  void OnIndirectBufferEnd() override {
    if (forward()) chain->OnIndirectBufferEnd();
  }
  void OnPacketStart(const uint8_t* ring_base, uint32_t ring_capacity, uint32_t read_offset,
                     uint32_t dword_count) override {
    if (forward()) chain->OnPacketStart(ring_base, ring_capacity, read_offset, dword_count);
  }
  void OnPacketEnd() override {
    if (forward()) chain->OnPacketEnd();
  }
  void OnMemoryRead(uint32_t physical_address, uint32_t length) override {
    if (forward()) chain->OnMemoryRead(physical_address, length);
  }
  void OnDrawEnd(CommandProcessor& cp, const DrawEvent& draw) override {
    if (suppress) {
      return;
    }
    if (chain) chain->OnDrawEnd(cp, draw);
    pending_draws.push_back(draw);
  }
  void OnSwap(CommandProcessor& cp, uint32_t frontbuffer_ptr, uint32_t width,
              uint32_t height) override {
    if (forward()) chain->OnSwap(cp, frontbuffer_ptr, width, height);
  }
  bool forward() const { return chain && !suppress; }
  TraceHooks* chain = nullptr;
  bool suppress = false;
  std::vector<DrawEvent> pending_draws;
};

struct TargetImage {
  std::string key;
  uint32_t width = 0;
  uint32_t height = 0;
  uint32_t channels = 0;
  std::vector<uint32_t> data;  // float32 bits or raw uint32 depth.
};

class Replayer {
 public:
  Replayer(GraphicsSystem* graphics_system, const RexGpuTraceReplayOptions& options)
      : gs_(graphics_system),
        cp_(*graphics_system->command_processor()),
        memory_(graphics_system->memory()),
        options_(options),
        out_dir_(options.output_dir ? options.output_dir : "replay_out") {}

  int Run(const std::vector<uint8_t>& trace);

 private:
  bool ExecuteCommands(const std::vector<uint8_t>& trace);
  void WriteMemory(uint32_t address, const void* data, uint32_t length, bool from_trace = true);
  // Trace memory blocks (kMemoryRead), in trace order, for race mode.
  struct WriteRecord {
    size_t data_offset;
    uint32_t address;
    uint32_t length;
    uint32_t encoded_length;
    uint32_t frame;
    MemoryEncodingFormat encoding;
    bool applied_early;
  };
  void ScanWrites(const std::vector<uint8_t>& trace);
  const uint8_t* DecodeRecord(const uint8_t* trace, const WriteRecord& record);
  bool DecodeBlock(MemoryEncodingFormat encoding, const uint8_t* data, uint32_t encoded_length,
                   uint32_t decoded_length, std::vector<uint8_t>& out);
  // Applies a trace memory block. In race mode, bytes still owned by a block
  // applied early (later in the trace) are left alone, and only the changed
  // span fires the physical write callbacks.
  void ApplyRecordedWrite(uint32_t record_index, const uint8_t* data);
  bool RaceWrite(uint32_t address, uint32_t length, uint32_t& write_address,
                 uint32_t& write_length, uint32_t& write_frame);
  void RestoreRegisters(uint32_t first, uint32_t count, const uint32_t* values);
  void HandleDraws();
  void RecordDraw(const DrawEvent& draw);
  bool ReadbackTarget(int source, uint32_t width, uint32_t height, TargetImage& image);
  void EmitTarget(const std::string& label, TargetImage& image, std::string& json);
  uint32_t ScratchAddress() const;

  GraphicsSystem* gs_;
  CommandProcessor& cp_;
  memory::Memory* memory_;
  RexGpuTraceReplayOptions options_;
  std::filesystem::path out_dir_;
  ReplayHooks hooks_;
  std::FILE* draws_json_ = nullptr;
  uint32_t draw_index_ = 0;
  uint32_t frame_index_ = 0;
  uint32_t packet_index_ = 0;
  uint32_t current_packet_base_ = 0;
  uint32_t scratch_ = kDefaultScratchAddress;
  bool scratch_conflict_ = false;
  std::map<std::string, TargetImage> previous_;
  uint32_t readbacks_ = 0;
  uint32_t failed_readbacks_ = 0;
  std::unique_ptr<TextureCheck> texcheck_;
  const uint8_t* trace_data_ = nullptr;
  std::vector<WriteRecord> records_;
  // 64 KiB guest page -> indices of records touching it, ascending.
  std::unordered_map<uint32_t, std::vector<uint32_t>> page_records_;
  uint32_t next_record_ = 0;
  std::vector<uint32_t> early_pending_;
  std::vector<uint8_t> decode_buffer_;
  std::vector<uint8_t> race_buffer_;
};

constexpr uint32_t kRecordPageShift = 16;

bool Replayer::DecodeBlock(MemoryEncodingFormat encoding, const uint8_t* data,
                           uint32_t encoded_length, uint32_t decoded_length,
                           std::vector<uint8_t>& out) {
  if (encoding != MemoryEncodingFormat::kZlib) {
    return false;
  }
  if (out.size() < decoded_length) {
    out.resize(decoded_length);
  }
  uLongf length = decoded_length;
  return uncompress(out.data(), &length, data, encoded_length) == Z_OK &&
         length == decoded_length;
}

const uint8_t* Replayer::DecodeRecord(const uint8_t* trace, const WriteRecord& record) {
  const uint8_t* data = trace + record.data_offset;
  if (record.encoding == MemoryEncodingFormat::kNone) {
    return data;
  }
  if (!DecodeBlock(record.encoding, data, record.encoded_length, record.length, race_buffer_)) {
    return nullptr;
  }
  return race_buffer_.data();
}

void Replayer::ScanWrites(const std::vector<uint8_t>& trace) {
  const uint8_t* base = trace.data();
  const uint8_t* ptr = base + sizeof(TraceHeader);
  const uint8_t* end = base + trace.size();
  uint32_t frame = 0;
  while (size_t(end - ptr) >= sizeof(uint32_t)) {
    TraceCommandType type;
    std::memcpy(&type, ptr, sizeof(type));
    size_t step = 0;
    switch (type) {
      case TraceCommandType::kPrimaryBufferStart:
        step = sizeof(PrimaryBufferStartCommand);
        break;
      case TraceCommandType::kPrimaryBufferEnd:
        step = sizeof(PrimaryBufferEndCommand);
        break;
      case TraceCommandType::kIndirectBufferStart:
        step = sizeof(IndirectBufferStartCommand);
        break;
      case TraceCommandType::kIndirectBufferEnd:
        step = sizeof(IndirectBufferEndCommand);
        break;
      case TraceCommandType::kPacketStart:
        step = sizeof(PacketStartCommand) +
               size_t(reinterpret_cast<const PacketStartCommand*>(ptr)->count) * 4;
        break;
      case TraceCommandType::kPacketEnd:
        step = sizeof(PacketEndCommand);
        break;
      case TraceCommandType::kMemoryRead: {
        auto cmd = reinterpret_cast<const MemoryCommand*>(ptr);
        uint32_t index = uint32_t(records_.size());
        records_.push_back({size_t(ptr + sizeof(*cmd) - base), cmd->base_ptr & 0x1FFFFFFF,
                            cmd->decoded_length, cmd->encoded_length, frame,
                            cmd->encoding_format, false});
        if (cmd->decoded_length) {
          uint32_t a = cmd->base_ptr & 0x1FFFFFFF;
          for (uint32_t page = a >> kRecordPageShift;
               page <= (a + cmd->decoded_length - 1) >> kRecordPageShift; ++page) {
            page_records_[page].push_back(index);
          }
        }
        step = sizeof(*cmd) + cmd->encoded_length;
        break;
      }
      case TraceCommandType::kMemoryWrite:
        step = sizeof(MemoryCommand) + reinterpret_cast<const MemoryCommand*>(ptr)->encoded_length;
        break;
      case TraceCommandType::kEdramSnapshot:
        step = sizeof(EdramSnapshotCommand) +
               reinterpret_cast<const EdramSnapshotCommand*>(ptr)->encoded_length;
        break;
      case TraceCommandType::kEvent:
        if (reinterpret_cast<const EventCommand*>(ptr)->event_type ==
            EventCommand::Type::kSwap) {
          ++frame;
        }
        step = sizeof(EventCommand);
        break;
      case TraceCommandType::kRegisters:
        step = sizeof(RegistersCommand) +
               reinterpret_cast<const RegistersCommand*>(ptr)->encoded_length;
        break;
      case TraceCommandType::kGammaRamp:
        step = sizeof(GammaRampCommand) +
               reinterpret_cast<const GammaRampCommand*>(ptr)->encoded_length;
        break;
      default:
        return;
    }
    if (size_t(end - ptr) < step) {
      return;
    }
    ptr += step;
  }
}

void Replayer::ApplyRecordedWrite(uint32_t record_index, const uint8_t* data) {
  WriteRecord& record = records_[record_index];
  if (texcheck_) {
    texcheck_->NoteWrite(record.address, data, record.length, record.frame);
  }
  if (!(options_.flags & kRexGpuTraceReplayRace)) {
    WriteMemory(record.address, data, record.length);
    return;
  }
  early_pending_.erase(std::remove(early_pending_.begin(), early_pending_.end(), record_index),
                       early_pending_.end());
  if (record.applied_early) {
    return;
  }
  // Split around bytes owned by blocks applied early (they are newer).
  std::vector<std::pair<uint32_t, uint32_t>> spans{{record.address,
                                                    record.address + record.length}};
  for (uint32_t other : early_pending_) {
    const WriteRecord& o = records_[other];
    if (other < record_index) {
      continue;
    }
    std::vector<std::pair<uint32_t, uint32_t>> next;
    for (auto [s0, s1] : spans) {
      uint32_t o0 = o.address, o1 = o.address + o.length;
      if (o1 <= s0 || o0 >= s1) {
        next.push_back({s0, s1});
        continue;
      }
      if (s0 < o0) {
        next.push_back({s0, o0});
      }
      if (o1 < s1) {
        next.push_back({o1, s1});
      }
    }
    spans.swap(next);
  }
  for (auto [s0, s1] : spans) {
    // Only the changed bytes are guest stores; identical bytes must not fire
    // callbacks, or they would heal a texture the cache got wrong.
    const uint8_t* src = data + (s0 - record.address);
    const uint8_t* dst = memory_->TranslatePhysical(s0);
    uint32_t n = s1 - s0;
    uint32_t first = 0;
    while (first < n && src[first] == dst[first]) {
      ++first;
    }
    if (first == n) {
      continue;
    }
    uint32_t last = n;
    while (last > first && src[last - 1] == dst[last - 1]) {
      --last;
    }
    WriteMemory(s0 + first, src + first, last - first);
  }
}

bool Replayer::RaceWrite(uint32_t address, uint32_t length, uint32_t& write_address,
                         uint32_t& write_length, uint32_t& write_frame) {
  uint32_t window = options_.race_frames ? options_.race_frames : 1;
  uint32_t best = UINT32_MAX;
  for (uint32_t page = address >> kRecordPageShift;
       page <= (address + length - 1) >> kRecordPageShift; ++page) {
    auto it = page_records_.find(page);
    if (it == page_records_.end()) {
      continue;
    }
    const auto& list = it->second;
    for (auto r = std::lower_bound(list.begin(), list.end(), next_record_); r != list.end();
         ++r) {
      if (*r >= best) {
        break;
      }
      const WriteRecord& record = records_[*r];
      if (record.frame > frame_index_ + window) {
        break;
      }
      if (record.applied_early || record.address >= address + length ||
          record.address + record.length <= address) {
        continue;
      }
      // Must change the texture's bytes.
      const uint8_t* data = DecodeRecord(trace_data_, record);
      if (!data) {
        continue;
      }
      uint32_t o0 = std::max(address, record.address);
      uint32_t o1 = std::min(address + length, record.address + record.length);
      if (std::memcmp(data + (o0 - record.address), memory_->TranslatePhysical(o0), o1 - o0)) {
        best = *r;
        break;
      }
    }
  }
  if (best == UINT32_MAX) {
    return false;
  }
  WriteRecord& record = records_[best];
  const uint8_t* data = DecodeRecord(trace_data_, record);
  // Apply as if at its trace position (changed span only), then mark it so
  // the trace position skips it.
  uint32_t saved = next_record_;
  ApplyRecordedWrite(best, data);
  next_record_ = saved;
  record.applied_early = true;
  early_pending_.push_back(best);
  write_address = record.address;
  write_length = record.length;
  write_frame = record.frame;
  return true;
}

uint32_t Replayer::ScratchAddress() const { return scratch_; }

void Replayer::WriteMemory(uint32_t address, const void* data, uint32_t length,
                           bool from_trace) {
  address &= 0x1FFFFFFF;
  if (!length) {
    return;
  }
  if (from_trace && address < scratch_ + kScratchSize && address + length > scratch_) {
    scratch_conflict_ = true;
  }
  // Invalidate GPU caches (and unprotect watched pages) before writing, as
  // the guest CPU would have.
  memory_->TriggerPhysicalMemoryCallbacks(rex::thread::global_critical_region::AcquireDirect(),
                                          0xA0000000u | address, length, true, false);
  std::memcpy(memory_->TranslatePhysical(address), data, length);
}

void Replayer::RestoreRegisters(uint32_t first, uint32_t count, const uint32_t* values) {
  RegisterFile& regs = TraceAccess::registers(cp_);
  count = std::min<uint32_t>(count, RegisterFile::kRegisterCount - std::min<uint32_t>(
                                                                     first, RegisterFile::kRegisterCount));
  std::memcpy(&regs.values[first], values, count * sizeof(uint32_t));
  // Route the shader constant and fetch constant ranges through
  // WriteRegister so backends mark their constant buffers and bindings dirty.
  uint32_t lo = std::max<uint32_t>(first, XE_GPU_REG_SHADER_CONSTANT_000_X);
  uint32_t hi = std::min<uint32_t>(first + count, XE_GPU_REG_SHADER_CONSTANT_LOOP_31 + 1);
  for (uint32_t i = lo; i < hi; ++i) {
    TraceAccess::WriteRegister(cp_, i, regs.values[i]);
  }
}

int Replayer::Run(const std::vector<uint8_t>& trace) {
  std::error_code ec;
  std::filesystem::create_directories(out_dir_ / "draws", ec);
  if (ec) {
    REXGPU_ERROR("replay: cannot create {}: {}", out_dir_.string(), ec.message());
    return 1;
  }
  if (options_.scratch_address) {
    scratch_ = options_.scratch_address & 0x1FFFFFFF;
  }
  draws_json_ = std::fopen((out_dir_ / "draws.jsonl").string().c_str(), "wb");
  if (!draws_json_) {
    return 1;
  }

  trace_data_ = trace.data();
  if (options_.flags & (kRexGpuTraceReplayTexCheck | kRexGpuTraceReplayRace)) {
    bool race = (options_.flags & kRexGpuTraceReplayRace) != 0;
    texcheck_ = std::make_unique<TextureCheck>(memory_, out_dir_, race);
    if (race) {
      ScanWrites(trace);
      texcheck_->set_race_writer([this](uint32_t address, uint32_t length,
                                        uint32_t& write_address, uint32_t& write_length,
                                        uint32_t& write_frame) {
        return RaceWrite(address, length, write_address, write_length, write_frame);
      });
    }
    TextureLoadProbeSlot() = texcheck_.get();
  }

  hooks_.chain = cp_.trace_hooks();
  cp_.set_trace_hooks(&hooks_);
  bool ok = ExecuteCommands(trace);
  TextureLoadProbeSlot() = nullptr;
  cp_.set_trace_hooks(hooks_.chain);
  std::fclose(draws_json_);
  draws_json_ = nullptr;

  const auto* header = reinterpret_cast<const TraceHeader*>(trace.data());
  std::string summary = fmt::format(
      "{{\n  \"trace\": \"{}\",\n  \"backend\": \"{}\",\n  \"title_id\": \"{:08X}\",\n"
      "  \"frames\": {},\n  \"draws\": {},\n  \"readbacks\": {},\n  \"failed_readbacks\": {},\n"
      "  \"scratch_address\": \"0x{:08X}\",\n  \"scratch_conflict\": {},\n  \"ok\": {},\n"
      "  \"texcheck\": {}\n}}\n",
      JsonEscape(options_.trace_path ? options_.trace_path : ""),
      JsonEscape(options_.backend_label ? options_.backend_label : ""), header->title_id,
      frame_index_, draw_index_, readbacks_, failed_readbacks_, scratch_,
      scratch_conflict_ ? "true" : "false", ok ? "true" : "false",
      texcheck_ ? texcheck_->SummaryJson() : std::string("null"));
  std::ofstream(out_dir_ / "summary.json") << summary;
  if (texcheck_) {
    REXGPU_INFO("replay: texcheck {}", texcheck_->SummaryJson());
  }
  REXGPU_INFO("replay: {} frame(s), {} draw(s), {} readback(s) ({} failed) -> {}", frame_index_,
              draw_index_, readbacks_, failed_readbacks_, out_dir_.string());
  if (scratch_conflict_) {
    REXGPU_WARN("replay: trace touched the scratch region 0x{:08X}; pass a different one",
                scratch_);
  }
  return ok ? 0 : 1;
}

bool Replayer::ExecuteCommands(const std::vector<uint8_t>& trace) {
  const uint8_t* ptr = trace.data() + sizeof(TraceHeader);
  const uint8_t* end = trace.data() + trace.size();
  const PacketStartCommand* pending_packet = nullptr;
  auto need = [&](size_t bytes) { return size_t(end - ptr) >= bytes; };

  while (ptr < end) {
    if (!need(sizeof(uint32_t))) {
      return false;
    }
    TraceCommandType type;
    std::memcpy(&type, ptr, sizeof(type));
    switch (type) {
      case TraceCommandType::kPrimaryBufferStart:
        ptr += sizeof(PrimaryBufferStartCommand);
        break;
      case TraceCommandType::kPrimaryBufferEnd:
        ptr += sizeof(PrimaryBufferEndCommand);
        break;
      case TraceCommandType::kIndirectBufferStart:
        ptr += sizeof(IndirectBufferStartCommand);
        break;
      case TraceCommandType::kIndirectBufferEnd:
        ptr += sizeof(IndirectBufferEndCommand);
        break;
      case TraceCommandType::kPacketStart: {
        if (!need(sizeof(PacketStartCommand))) {
          return false;
        }
        auto cmd = reinterpret_cast<const PacketStartCommand*>(ptr);
        ptr += sizeof(*cmd);
        size_t bytes = size_t(cmd->count) * sizeof(uint32_t);
        if (!need(bytes)) {
          return false;
        }
        std::memcpy(memory_->TranslatePhysical(cmd->base_ptr), ptr, bytes);
        ptr += bytes;
        pending_packet = cmd;
        break;
      }
      case TraceCommandType::kPacketEnd: {
        ptr += sizeof(PacketEndCommand);
        if (!pending_packet || !pending_packet->count) {
          // Packet containing other packets (indirect buffer); its contents
          // were executed individually. Empty packets come from broken
          // captures and would execute garbage.
          pending_packet = nullptr;
          break;
        }
        current_packet_base_ = pending_packet->base_ptr;
        if (texcheck_) {
          texcheck_->set_position({frame_index_, draw_index_, packet_index_});
        }
        cp_.ExecutePacket(pending_packet->base_ptr, pending_packet->count);
        pending_packet = nullptr;
        ++packet_index_;
        HandleDraws();
        break;
      }
      case TraceCommandType::kMemoryRead: {
        if (!need(sizeof(MemoryCommand))) {
          return false;
        }
        auto cmd = reinterpret_cast<const MemoryCommand*>(ptr);
        ptr += sizeof(*cmd);
        if (!need(cmd->encoded_length)) {
          return false;
        }
        const uint8_t* data = ptr;
        if (cmd->encoding_format != MemoryEncodingFormat::kNone) {
          if (!DecodeBlock(cmd->encoding_format, ptr, cmd->encoded_length, cmd->decoded_length,
                           decode_buffer_)) {
            REXGPU_ERROR("replay: cannot decode memory block (encoding {})",
                         uint32_t(cmd->encoding_format));
            return false;
          }
          data = decode_buffer_.data();
        }
        if (texcheck_) {
          if (options_.flags & kRexGpuTraceReplayRace) {
            ApplyRecordedWrite(next_record_, data);
          } else {
            texcheck_->NoteWrite(cmd->base_ptr, data, cmd->decoded_length, frame_index_);
            WriteMemory(cmd->base_ptr, data, cmd->decoded_length);
          }
        } else {
          WriteMemory(cmd->base_ptr, data, cmd->decoded_length);
        }
        ++next_record_;
        ptr += cmd->encoded_length;
        break;
      }
      case TraceCommandType::kMemoryWrite: {
        if (!need(sizeof(MemoryCommand))) {
          return false;
        }
        auto cmd = reinterpret_cast<const MemoryCommand*>(ptr);
        ptr += sizeof(*cmd) + cmd->encoded_length;
        break;
      }
      case TraceCommandType::kEdramSnapshot: {
        // Not produced by the ReXGlue writer; skip.
        if (!need(sizeof(EdramSnapshotCommand))) {
          return false;
        }
        auto cmd = reinterpret_cast<const EdramSnapshotCommand*>(ptr);
        ptr += sizeof(*cmd) + cmd->encoded_length;
        break;
      }
      case TraceCommandType::kEvent: {
        if (!need(sizeof(EventCommand))) {
          return false;
        }
        auto cmd = reinterpret_cast<const EventCommand*>(ptr);
        ptr += sizeof(*cmd);
        if (cmd->event_type == EventCommand::Type::kSwap) {
          ++frame_index_;
          if (options_.max_frames && frame_index_ >= options_.max_frames) {
            return true;
          }
        }
        break;
      }
      case TraceCommandType::kRegisters: {
        if (!need(sizeof(RegistersCommand))) {
          return false;
        }
        auto cmd = reinterpret_cast<const RegistersCommand*>(ptr);
        ptr += sizeof(*cmd);
        if (!need(cmd->encoded_length) || cmd->encoding_format != MemoryEncodingFormat::kNone) {
          return false;
        }
        std::vector<uint32_t> values(cmd->register_count);
        std::memcpy(values.data(), ptr, values.size() * sizeof(uint32_t));
        RestoreRegisters(cmd->first_register, cmd->register_count, values.data());
        ptr += cmd->encoded_length;
        break;
      }
      case TraceCommandType::kGammaRamp: {
        if (!need(sizeof(GammaRampCommand))) {
          return false;
        }
        auto cmd = reinterpret_cast<const GammaRampCommand*>(ptr);
        ptr += sizeof(*cmd);
        if (!need(cmd->encoded_length)) {
          return false;
        }
        if (cmd->encoding_format == MemoryEncodingFormat::kNone &&
            cmd->encoded_length == TraceAccess::kGammaRampBytes) {
          TraceAccess::WriteGammaRamp(cp_, ptr, cmd->rw_component);
        }
        ptr += cmd->encoded_length;
        break;
      }
      default:
        REXGPU_ERROR("replay: unknown trace command {} at offset {}", uint32_t(type),
                     size_t(ptr - trace.data()));
        return false;
    }
  }
  return true;
}

void Replayer::HandleDraws() {
  if (hooks_.pending_draws.empty()) {
    return;
  }
  std::vector<DrawEvent> draws;
  draws.swap(hooks_.pending_draws);
  for (const DrawEvent& draw : draws) {
    RecordDraw(draw);
  }
}

bool Replayer::ReadbackTarget(int source, uint32_t width, uint32_t height, TargetImage& image) {
  RegisterFile& regs = TraceAccess::registers(cp_);
  bool is_depth = source >= int(xenos::kMaxColorRenderTargets);
  uint32_t saved[std::size(kReadbackRegisters)];
  for (size_t i = 0; i < std::size(kReadbackRegisters); ++i) {
    saved[i] = regs.values[kReadbackRegisters[i]];
  }

  uint32_t scratch = ScratchAddress();
  uint32_t pitch_aligned = (width + 31) & ~31u;
  uint32_t height_aligned = (height + 31) & ~31u;
  uint32_t bpp_log2 = is_depth ? 2 : 4;
  uint32_t image_bytes = (pitch_aligned * height_aligned) << bpp_log2;
  if (kScratchImageOffset + image_bytes > kScratchSize) {
    return false;
  }
  uint32_t image_address = scratch + kScratchImageOffset;

  // Rectangle vertices: three float2, host endian (fetch endian kNone).
  float vertices[6] = {0.0f, 0.0f, float(width), 0.0f, float(width), float(height)};
  WriteMemory(scratch + kScratchVertexOffset, vertices, sizeof(vertices), false);
  // DRAW_INDX_2, auto index rectangle list of 3 vertices.
  uint32_t initiator = uint32_t(xenos::PrimitiveType::kRectangleList) |
                       (uint32_t(xenos::SourceSelect::kAutoIndex) << 6) | (3u << 16);
  uint32_t pm4[2] = {
      std::byteswap((3u << 30) | (uint32_t(xenos::PM4_DRAW_INDX_2) << 8)),
      std::byteswap(initiator),
  };
  WriteMemory(scratch + kScratchPm4Offset, pm4, sizeof(pm4), false);
  // Poison the destination so a dropped resolve is detectable.
  memory_->TriggerPhysicalMemoryCallbacks(rex::thread::global_critical_region::AcquireDirect(),
                                          0xA0000000u | image_address, image_bytes, true, false);
  std::memset(memory_->TranslatePhysical(image_address), 0xFF, image_bytes);

  auto write = [&](uint32_t index, uint32_t value) { TraceAccess::WriteRegister(cp_, index, value); };
  reg::RB_MODECONTROL modecontrol;
  modecontrol.value = saved[0];
  modecontrol.edram_mode = xenos::EdramMode::kCopy;
  write(XE_GPU_REG_RB_MODECONTROL, modecontrol.value);
  reg::RB_COPY_CONTROL copy_control;
  copy_control.value = 0;
  copy_control.copy_src_select = uint32_t(source);
  copy_control.copy_sample_select = xenos::CopySampleSelect::k0;
  copy_control.copy_command = is_depth ? xenos::CopyCommand::kRaw : xenos::CopyCommand::kConvert;
  write(XE_GPU_REG_RB_COPY_CONTROL, copy_control.value);
  write(XE_GPU_REG_RB_COPY_DEST_BASE, image_address);
  reg::RB_COPY_DEST_PITCH dest_pitch;
  dest_pitch.value = 0;
  dest_pitch.copy_dest_pitch = pitch_aligned;
  dest_pitch.copy_dest_height = height_aligned;
  write(XE_GPU_REG_RB_COPY_DEST_PITCH, dest_pitch.value);
  reg::RB_COPY_DEST_INFO dest_info;
  dest_info.value = 0;
  dest_info.copy_dest_endian = xenos::Endian128::kNone;
  dest_info.copy_dest_format =
      is_depth ? xenos::ColorFormat::k_8_8_8_8 : xenos::ColorFormat::k_32_32_32_32_FLOAT;
  write(XE_GPU_REG_RB_COPY_DEST_INFO, dest_info.value);
  xenos::xe_gpu_vertex_fetch_t fetch = {};
  fetch.type = xenos::FetchConstantType::kVertex;
  fetch.address = (scratch + kScratchVertexOffset) >> 2;
  fetch.endian = xenos::Endian::kNone;
  fetch.size = 6;
  uint32_t fetch_dwords[2];
  std::memcpy(fetch_dwords, &fetch, sizeof(fetch_dwords));
  write(XE_GPU_REG_SHADER_CONSTANT_FETCH_00_0, fetch_dwords[0]);
  write(XE_GPU_REG_SHADER_CONSTANT_FETCH_00_1, fetch_dwords[1]);
  reg::PA_SU_SC_MODE_CNTL sc_mode;
  sc_mode.value = saved[7];
  sc_mode.vtx_window_offset_enable = 0;
  write(XE_GPU_REG_PA_SU_SC_MODE_CNTL, sc_mode.value);
  write(XE_GPU_REG_PA_SC_VIZ_QUERY, 0);

  hooks_.suppress = true;
  cp_.ExecutePacket(scratch + kScratchPm4Offset, 2);
  cp_.TraceReadbackGuestMemory(image_address, image_bytes);
  hooks_.suppress = false;

  for (size_t i = 0; i < std::size(kReadbackRegisters); ++i) {
    write(kReadbackRegisters[i], saved[i]);
  }

  // Untile.
  image.width = width;
  image.height = height;
  image.channels = is_depth ? 1 : 4;
  image.data.resize(size_t(width) * height * image.channels);
  const uint8_t* src = memory_->TranslatePhysical(image_address);
  bool all_poison = true;
  for (uint32_t y = 0; y < height; ++y) {
    for (uint32_t x = 0; x < width; ++x) {
      int32_t offset = texture_util::GetTiledOffset2D(int32_t(x), int32_t(y), pitch_aligned,
                                                      bpp_log2);
      uint32_t* dst = &image.data[(size_t(y) * width + x) * image.channels];
      std::memcpy(dst, src + offset, image.channels * sizeof(uint32_t));
      for (uint32_t c = 0; c < image.channels; ++c) {
        if (dst[c] != 0xFFFFFFFFu) {
          all_poison = false;
        }
      }
    }
  }
  return !all_poison;
}

void Replayer::EmitTarget(const std::string& label, TargetImage& image, std::string& json) {
  bool full = (options_.flags & kRexGpuTraceReplayFullImages) != 0;
  uint64_t hash = XXH3_64bits(image.data.data(), image.data.size() * sizeof(uint32_t));
  auto prev_it = previous_.find(label);
  bool have_prev = prev_it != previous_.end() && prev_it->second.key == image.key &&
                   prev_it->second.width == image.width && prev_it->second.height == image.height &&
                   prev_it->second.channels == image.channels;
  uint32_t x0 = 0, y0 = 0, x1 = image.width, y1 = image.height;
  bool changed = true;
  if (have_prev && !full) {
    // Bounding box of changed pixels relative to the previous readback of the
    // same surface.
    const auto& prev = prev_it->second.data;
    uint32_t bx0 = image.width, by0 = image.height, bx1 = 0, by1 = 0;
    size_t row = size_t(image.width) * image.channels;
    for (uint32_t y = 0; y < image.height; ++y) {
      const uint32_t* a = &image.data[y * row];
      const uint32_t* b = &prev[y * row];
      if (!std::memcmp(a, b, row * sizeof(uint32_t))) {
        continue;
      }
      by0 = std::min(by0, y);
      by1 = y + 1;
      for (uint32_t x = 0; x < image.width; ++x) {
        if (std::memcmp(a + x * image.channels, b + x * image.channels,
                        image.channels * sizeof(uint32_t))) {
          bx0 = std::min(bx0, x);
          bx1 = std::max(bx1, x + 1);
        }
      }
    }
    if (by1 == 0) {
      changed = false;
      x0 = y0 = x1 = y1 = 0;
    } else {
      x0 = bx0;
      y0 = by0;
      x1 = bx1;
      y1 = by1;
    }
  }
  std::string file;
  if (changed) {
    uint32_t w = x1 - x0, h = y1 - y0;
    std::vector<uint32_t> crop(size_t(w) * h * image.channels);
    for (uint32_t y = 0; y < h; ++y) {
      std::memcpy(&crop[size_t(y) * w * image.channels],
                  &image.data[(size_t(y + y0) * image.width + x0) * image.channels],
                  size_t(w) * image.channels * sizeof(uint32_t));
    }
    file = fmt::format("draws/{:06}_{}.npy", draw_index_, label);
    WriteNpy(out_dir_ / file, image.channels == 4 ? "<f4" : "<u4", h, w, image.channels,
             crop.data(), crop.size() * sizeof(uint32_t));
  }
  json += fmt::format(
      "{{\"target\":\"{}\",\"key\":\"{}\",\"width\":{},\"height\":{},\"channels\":{},"
      "\"format\":\"{}\",\"hash\":\"{:016X}\",\"base\":{},\"file\":{},\"rect\":[{},{},{},{}]}}",
      label, image.key, image.width, image.height, image.channels,
      image.channels == 4 ? "rgba32f" : "depth_raw_u32", hash, have_prev && !full ? "\"delta\"" : "\"full\"",
      file.empty() ? "null" : "\"" + file + "\"", x0, y0, x1 - x0, y1 - y0);
  previous_[label] = image;
}

void Replayer::RecordDraw(const DrawEvent& draw) {
  RegisterFile& regs = TraceAccess::registers(cp_);
  uint32_t index = draw_index_;
  auto modecontrol = regs.Get<reg::RB_MODECONTROL>();
  Shader* vs = cp_.active_vertex_shader();
  Shader* ps = cp_.active_pixel_shader();

  std::string json = fmt::format(
      "{{\"draw\":{},\"frame\":{},\"packet\":{},\"packet_base\":\"0x{:08X}\","
      "\"prim_type\":{},\"source_select\":{},\"index_count\":{},\"indexed\":{},"
      "\"index_format\":{},\"index_base\":\"0x{:08X}\",\"index_bytes\":{},\"succeeded\":{},"
      "\"edram_mode\":{},\"is_resolve\":{},",
      index, frame_index_, packet_index_, current_packet_base_, uint32_t(draw.prim_type),
      uint32_t(draw.source_select), draw.index_count, draw.is_indexed ? "true" : "false",
      uint32_t(draw.index_format), draw.index_guest_base, draw.index_length_bytes,
      draw.succeeded ? "true" : "false", uint32_t(modecontrol.edram_mode),
      modecontrol.edram_mode == xenos::EdramMode::kCopy ? "true" : "false");
  json += vs ? fmt::format("\"vs\":\"{:016X}\",", vs->ucode_data_hash()) : "\"vs\":null,";
  json += ps ? fmt::format("\"ps\":\"{:016X}\",", ps->ucode_data_hash()) : "\"ps\":null,";

  json += "\"regs\":{";
  bool first = true;
  for (const NamedRegister& r : kStateRegisters) {
    uint32_t v = regs.values[r.index];
    if (!first) {
      json += ",";
    }
    first = false;
    if (r.is_float) {
      json += fmt::format("\"{}\":{}", r.name, JsonFloat(std::bit_cast<float>(v)));
    } else {
      json += fmt::format("\"{}\":\"0x{:08X}\"", r.name, v);
    }
  }
  json += "},\"textures\":[";
  first = true;
  if (ps || vs) {
    std::map<uint32_t, bool> seen;
    for (Shader* shader : {vs, ps}) {
      if (!shader) {
        continue;
      }
      for (const auto& binding : shader->texture_bindings()) {
        if (seen[binding.fetch_constant]) {
          continue;
        }
        seen[binding.fetch_constant] = true;
        const uint32_t* f = &regs.values[XE_GPU_REG_SHADER_CONSTANT_FETCH_00_0 +
                                         binding.fetch_constant * 6];
        xenos::xe_gpu_texture_fetch_t fetch = regs.GetTextureFetch(binding.fetch_constant);
        if (!first) {
          json += ",";
        }
        first = false;
        json += fmt::format(
            "{{\"stage\":\"{}\",\"fetch\":{},\"dwords\":[\"0x{:08X}\",\"0x{:08X}\",\"0x{:08X}\","
            "\"0x{:08X}\",\"0x{:08X}\",\"0x{:08X}\"],\"format\":{},\"base\":\"0x{:08X}\"}}",
            shader == vs ? "vs" : "ps", binding.fetch_constant, f[0], f[1], f[2], f[3], f[4], f[5],
            uint32_t(fetch.format), uint32_t(fetch.base_address) << 12);
      }
    }
  }
  json += "],\"vertex_fetch\":[";
  if (vs) {
    first = true;
    std::map<uint32_t, bool> seen;
    for (const auto& binding : vs->vertex_bindings()) {
      if (seen[binding.fetch_constant]) {
        continue;
      }
      seen[binding.fetch_constant] = true;
      xenos::xe_gpu_vertex_fetch_t fetch = regs.GetVertexFetch(binding.fetch_constant);
      if (!first) {
        json += ",";
      }
      first = false;
      json += fmt::format("{{\"fetch\":{},\"address\":\"0x{:08X}\",\"bytes\":{},\"stride\":{}}}",
                          binding.fetch_constant, uint32_t(fetch.address) << 2,
                          uint32_t(fetch.size) << 2, binding.stride_words * 4);
    }
  }
  json += "],\"targets\":[";

  bool in_window = !options_.draw_count || (index >= options_.first_draw &&
                                            index - options_.first_draw < options_.draw_count);
  bool first_target = true;
  if (in_window && draw.succeeded && modecontrol.edram_mode == xenos::EdramMode::kColorDepth) {
    auto surface_info = regs.Get<reg::RB_SURFACE_INFO>();
    auto scissor_br = regs.Get<reg::PA_SC_WINDOW_SCISSOR_BR>();
    uint32_t pitch = surface_info.surface_pitch;
    uint32_t width = std::min<uint32_t>(scissor_br.br_x, pitch);
    uint32_t height = scissor_br.br_y;
    uint32_t sx = surface_info.msaa_samples >= xenos::MsaaSamples::k4X ? 2 : 1;
    uint32_t sy = surface_info.msaa_samples >= xenos::MsaaSamples::k2X ? 2 : 1;

    auto clamp_height = [&](uint32_t base_tiles, bool is_64bpp) {
      uint32_t tile_w = is_64bpp ? 40 : 80;
      uint32_t tiles_per_row = (pitch * sx + tile_w - 1) / tile_w;
      if (!tiles_per_row || base_tiles >= 2048) {
        return 0u;
      }
      uint32_t rows = (2048 - base_tiles) / tiles_per_row;
      return std::min(height, rows * 16 / sy);
    };

    auto color_mask = regs[XE_GPU_REG_RB_COLOR_MASK];
    if ((options_.flags & kRexGpuTraceReplayReadbackColor) && ps && width) {
      for (uint32_t rt = 0; rt < xenos::kMaxColorRenderTargets; ++rt) {
        if (!ps->writes_color_target(rt) || !((color_mask >> (4 * rt)) & 0xF)) {
          continue;
        }
        reg::RB_COLOR_INFO color_info;
        color_info.value = regs.values[reg::RB_COLOR_INFO::rt_register_indices[rt]];
        uint32_t h = clamp_height(
            color_info.color_base,
            xenos::IsColorRenderTargetFormat64bpp(color_info.color_format));
        if (!h) {
          continue;
        }
        TargetImage image;
        image.key = fmt::format("c{}_{}_{}_{}_{}", rt, uint32_t(color_info.color_base),
                                uint32_t(color_info.color_format), pitch,
                                uint32_t(surface_info.msaa_samples));
        ++readbacks_;
        if (!ReadbackTarget(int(rt), width, h, image)) {
          ++failed_readbacks_;
          continue;
        }
        if (!first_target) {
          json += ",";
        }
        first_target = false;
        EmitTarget(fmt::format("rt{}", rt), image, json);
      }
    }
    auto depthcontrol = regs.Get<reg::RB_DEPTHCONTROL>();
    if ((options_.flags & kRexGpuTraceReplayReadbackDepth) && width &&
        (depthcontrol.z_enable || depthcontrol.stencil_enable)) {
      auto depth_info = regs.Get<reg::RB_DEPTH_INFO>();
      uint32_t h = clamp_height(depth_info.depth_base, false);
      if (h) {
        TargetImage image;
        image.key = fmt::format("d_{}_{}_{}_{}", uint32_t(depth_info.depth_base),
                                uint32_t(depth_info.depth_format), pitch,
                                uint32_t(surface_info.msaa_samples));
        ++readbacks_;
        if (ReadbackTarget(int(xenos::kMaxColorRenderTargets), width, h, image)) {
          if (!first_target) {
            json += ",";
          }
          first_target = false;
          EmitTarget("depth", image, json);
        } else {
          ++failed_readbacks_;
        }
      }
    }
  }
  json += "]";
  if (texcheck_) {
    if (modecontrol.edram_mode == xenos::EdramMode::kCopy) {
      draw_util::ResolveInfo info;
#if defined(FABLE2_NO_CONVERTER)
      // Built into rexgpu-metal: the plugin's private SDK copy keeps Xenia's
      // TraceWriter argument (a no-op in release builds).
      static ::rex::graphics::TraceWriter unused_trace_writer(nullptr);
      if (draw_util::GetResolveInfo(regs, *memory_, unused_trace_writer, 1, 1, false, false, info) &&
#else
      if (draw_util::GetResolveInfo(regs, *memory_, 1, 1, false, false, info) &&
#endif
          info.copy_dest_extent_length) {
        texcheck_->AddGpuWrittenRange(info.copy_dest_extent_start, info.copy_dest_extent_length);
      }
    } else {
      texcheck_->CheckDraw(regs, vs, ps, json);
    }
  }
  json += "}\n";
  std::fwrite(json.data(), 1, json.size(), draws_json_);
  ++draw_index_;
}

}  // namespace

int ReplayTrace(GraphicsSystem* graphics_system, const RexGpuTraceReplayOptions& options) {
  if (!graphics_system || !graphics_system->command_processor() || !options.trace_path) {
    return 1;
  }
  std::ifstream in(options.trace_path, std::ios::binary);
  if (!in) {
    REXGPU_ERROR("replay: cannot open {}", options.trace_path);
    return 1;
  }
  std::vector<uint8_t> trace((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
  if (trace.size() < sizeof(TraceHeader)) {
    return 1;
  }
  const auto* header = reinterpret_cast<const TraceHeader*>(trace.data());
  if (header->version != kTraceFormatVersion) {
    REXGPU_ERROR("replay: trace version {} unsupported (want {})", header->version,
                 kTraceFormatVersion);
    return 1;
  }

  // Resolved data must reach guest memory synchronously for readback.
  rex::cvar::SetFlagByName("readback_resolve", "full");
  // Deterministic output: never skip a draw while its pipeline compiles.
  rex::cvar::SetFlagByName("async_shader_compilation", "false");

  memory::Memory* memory = graphics_system->memory();
  auto heap = memory->LookupHeapByType(true, 64 * 1024);
  heap->AllocFixed(heap->heap_base(), heap->heap_size(), heap->page_size(),
                   memory::kMemoryAllocationReserve | memory::kMemoryAllocationCommit,
                   memory::kMemoryProtectRead | memory::kMemoryProtectWrite);

  std::promise<int> done;
  auto result = done.get_future();
  graphics_system->command_processor()->CallInThread([&]() {
    Replayer replayer(graphics_system, options);
    done.set_value(replayer.Run(trace));
  });
  return result.get();
}

}  // namespace rex::graphics::trace
