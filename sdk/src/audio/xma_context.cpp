/**
******************************************************************************
* Xenia : Xbox 360 Emulator Research Project                                 *
******************************************************************************
* Copyright 2021 Ben Vanik. All rights reserved.                             *
* Released under the BSD license - see LICENSE in the root for more details. *
******************************************************************************
*
* @modified    Tom Clay, 2026 - Adapted for ReXGlue runtime
*/

#include <algorithm>
#include <cmath>
#include <chrono>
#include <condition_variable>
#include <cstring>
#include <deque>
#include <fstream>
#include <functional>
#include <map>
#include <mutex>
#include <set>
#include <system_error>
#include <thread>
#include <vector>

#include <rex/audio/xma/context.h>
#include <rex/audio/xma/decoder.h>
#include <rex/audio/xma/helpers.h>
#include <rex/dbg.h>
#include <rex/logging.h>
#include <rex/memory/ring_buffer.h>
#include <rex/platform.h>
#include <rex/stream.h>
#include <rex/cvar.h>
#include <rex/filesystem.h>

extern "C" {
#if REX_COMPILER_MSVC
#pragma warning(push)
#pragma warning(disable : 4101 4244 5033)
#endif
#include "libavcodec/avcodec.h"
#include "libavutil/error.h"

#define XXH_INLINE_ALL
#include <xxhash.h>
#if REX_COMPILER_MSVC
#pragma warning(pop)
#endif
}  // extern "C"

// Credits for most of this code goes to:
// https://github.com/koolkdev/libertyv/blob/master/libav_wrapper/xma2dec.c

REXCVAR_DEFINE_BOOL(apu_xma_cache, true, "Audio",
                    "decoded XMA frames are kept by the hash of the frame and of the one before it (the "
                    "decoder's only state between frames) and reused when a sound plays again; checked against a real "
                    "decode (the first 2,000 reuses, then 1 in 256), off for the session after a difference. "
                    "false = every frame decoded");
// 64 MB by default. In a round 15 benchmark a 16 MB cache (8,128 frames) served ~70 % of the frames and
// the decoder used ~15-17 % of a core; at 64 MB it serves ~88-90 % and the decoder uses ~10 %.
REXCVAR_DEFINE_INT32(apu_xma_cache_mb, 64, "Audio", "size of the decoded XMA frame cache, in MB")
    .range(1, 256)
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
REXCVAR_DEFINE_INT32(audio_dump_xma_s, 0, "Audio",
                     "Diagnostic: seconds of each XMA context saved to "
                     "xma_<context>_frames/output_<hz>.wav next to the executable; 0 = none");
REXCVAR_DEFINE_INT32(audio_dump_xma_since_s, 0, "Audio",
                     "Diagnostic: seconds from the first XMA audio before dumping starts");
REXCVAR_DEFINE_INT32(audio_dump_xma_contexts, 12, "Audio",
                     "Diagnostic: most XMA contexts dumped");
REXCVAR_DEFINE_INT32(audio_dump_xma_mb, 32, "Audio",
                     "Diagnostic: most megabytes of XMA audio held before writing");
REXCVAR_DEFINE_INT32(audio_dump_xma_min_s, 2, "Audio",
                     "Diagnostic: shortest XMA sound, in seconds, saved when it ends");

namespace {

// Robotic audio diagnostic: 16-bit big-endian PCM of an XMA context, kept in memory until the
// requested seconds are collected, then written as a little-endian WAV in one go.
struct XmaDump {
  uint32_t sample_hz = 0;
  uint32_t channels = 0;
  std::vector<int16_t> sample_count;
  bool written = false;
  uint32_t sequence = 0;  // segment number in the file name
};

std::mutex dump_xma_mutex;
std::map<std::pair<uint32_t, int>, XmaDump> dumps_xma;  // (context, 0 frames / 1 output)
uint32_t dumped_files_xma = 0;
uint32_t dump_sequence_xma = 0;
uint32_t annotated_xma = 0;
size_t samples_accumulated_xma = 0;  // total of the dumps not yet written
// Contexts currently accumulating (at most audio_dump_xma_contexts): with all of them at once
// the memory cap filled up and none got written.
std::set<uint32_t> dumped_contexts_xma;
bool dump_xma_with_clock = false;
std::chrono::steady_clock::time_point dump_xma_start;

// Written on a separate thread: on the Switch SD the file can take a while, and the caller holds
// the context lock on the game's audio thread.
void WriteDumpXmaInThread(uint32_t ctx_data, int type, const XmaDump& v) {
  const std::string number = std::to_string(ctx_data);
  const auto path_str = rex::filesystem::GetExecutableFolder() /
                    ("xma_" + std::string(number.size() < 3 ? 3 - number.size() : 0, '0') + number + "_" +
                     std::to_string(v.sequence) + (type ? "_output_" : "_frames_") +
                     std::to_string(v.sample_hz) + ".wav");
  std::ofstream file(path_str, std::ios::binary | std::ios::trunc);
  if (!file) {
    return;
  }
  const uint32_t bytes = uint32_t(v.sample_count.size() * 2);
  const auto u32 = [&](uint32_t x) { file.write(reinterpret_cast<const char*>(&x), 4); };
  const auto u16 = [&](uint16_t x) { file.write(reinterpret_cast<const char*>(&x), 2); };
  file.write("RIFF", 4);
  u32(36 + bytes);
  file.write("WAVEfmt ", 8);
  u32(16);
  u16(1);  // PCM
  u16(uint16_t(v.channels));
  u32(v.sample_hz);
  u32(v.sample_hz * v.channels * 2);
  u16(uint16_t(v.channels * 2));
  u16(16);
  file.write("data", 4);
  u32(bytes);
  file.write(reinterpret_cast<const char*>(v.sample_count.data()), bytes);
  REXAPU_INFO("XMA: {} dump of context {}: {} samples at {} Hz, {} channels",
              type ? "output" : "frames", ctx_data, v.sample_count.size() / v.channels,
              v.sample_hz, v.channels);
}

// Dumps are written on a single thread that lives until the process ends: on the Switch SD a file
// can take seconds and the game's audio thread must not wait for that. There is no thread per file:
// on the Switch, std::thread::detach() threw std::system_error (the writer thread had already
// finished) and the game closed. If the thread cannot be created, the file is written immediately.
class DumpWriter {
 public:
  static void Enqueue(std::function<void()> job) {
    static DumpWriter* const writer = Create();  // never destroyed
    if (!writer) {
      job();
      return;
    }
    {
      std::lock_guard<std::mutex> lock(writer->mutex_);
      writer->queue_.push_back(std::move(job));
    }
    writer->warning_.notify_one();
  }

 private:
  DumpWriter() : thread_([this]() { Loop(); }) {}

  static DumpWriter* Create() {
    try {
      return new DumpWriter();
    } catch (const std::system_error&) {
      return nullptr;
    }
  }

  void Loop() {
    for (;;) {
      std::function<void()> job;
      {
        std::unique_lock<std::mutex> lock(mutex_);
        warning_.wait(lock, [this]() { return !queue_.empty(); });
        job = std::move(queue_.front());
        queue_.pop_front();
      }
      job();
    }
  }

  std::mutex mutex_;
  std::condition_variable warning_;
  std::deque<std::function<void()>> queue_;
  std::thread thread_;  // last: it starts with the other members already constructed
};

void WriteDumpXma(uint32_t ctx_data, int type, XmaDump& v) {
  XmaDump copy;
  copy.sample_hz = v.sample_hz;
  copy.channels = v.channels;
  copy.sequence = v.sequence;
  copy.sample_count = std::move(v.sample_count);
  DumpWriter::Enqueue([ctx_data, type, copy = std::move(copy)]() {
    WriteDumpXmaInThread(ctx_data, type, copy);
  });
}

void DumpXma(uint32_t ctx_data, int type, uint32_t sample_hz, uint32_t channels, const uint8_t* be,
               size_t bytes) {
  const int32_t seconds = REXCVAR_GET(audio_dump_xma_s);
  if (seconds <= 0 || !sample_hz || !channels) {
    return;
  }
  std::lock_guard<std::mutex> lock(dump_xma_mutex);
  if (!dump_xma_with_clock) {
    dump_xma_with_clock = true;
    dump_xma_start = std::chrono::steady_clock::now();
  }
  if (std::chrono::steady_clock::now() - dump_xma_start <
      std::chrono::seconds(std::max(REXCVAR_GET(audio_dump_xma_since_s), 0))) {
    return;
  }
  if (dumped_files_xma >= 2 * uint32_t(std::max(REXCVAR_GET(audio_dump_xma_contexts), 0))) {
    return;
  }
  if (const auto written = dumps_xma.find({ctx_data, type});
      written != dumps_xma.end() && written->second.written) {
    return;
  }
  if (!dumped_contexts_xma.count(ctx_data)) {
    if (dumped_contexts_xma.size() >=
        size_t(std::max(REXCVAR_GET(audio_dump_xma_contexts), 0))) {
      return;
    }
    dumped_contexts_xma.insert(ctx_data);
  }
  XmaDump& v = dumps_xma[{ctx_data, type}];
  if (!v.sample_count.empty() && (v.sample_hz != sample_hz || v.channels != channels)) {
    // A different format in the same context: start over.
    samples_accumulated_xma -= v.sample_count.size();
    v.sample_count = std::vector<int16_t>();
  }
  if (v.sample_count.empty() && type == 0 && annotated_xma < 64) {
    ++annotated_xma;
    REXAPU_INFO("XMA: context {} with audio at {} Hz, {} channels", ctx_data, sample_hz, channels);
  }
  if (v.sample_count.empty()) {
    v.sequence = ++dump_sequence_xma;
  }
  v.sample_hz = sample_hz;
  v.channels = channels;
  const size_t cap = size_t(std::max(REXCVAR_GET(audio_dump_xma_mb), 1)) * (1024 * 1024 / 2);
  if (samples_accumulated_xma + bytes / 2 > cap) {
    // It does not fit: this context starts over so the dump has no gap.
    samples_accumulated_xma -= v.sample_count.size();
    v.sample_count = std::vector<int16_t>();
    return;
  }
  samples_accumulated_xma += bytes / 2;
  for (size_t i = 0; i + 1 < bytes; i += 2) {
    v.sample_count.push_back(int16_t(uint16_t((uint16_t(be[i]) << 8) | be[i + 1])));
  }
  if (v.sample_count.size() >= size_t(seconds) * sample_hz * channels) {
    ++dumped_files_xma;
    v.written = true;
    samples_accumulated_xma -= v.sample_count.size();
    WriteDumpXma(ctx_data, type, v);  // takes the samples
    v.sample_count = std::vector<int16_t>();
    const auto other = dumps_xma.find({ctx_data, 1 - type});
    if (other == dumps_xma.end() || other->second.written) {
      dumped_contexts_xma.erase(ctx_data);  // it has written its part: frees its slot
    }
  }
}

// Another sound in the context: what was accumulated is no longer continuous with what follows.
void ResetDumpXma(uint32_t ctx_data) {
  if (REXCVAR_GET(audio_dump_xma_s) <= 0) {
    return;
  }
  std::lock_guard<std::mutex> lock(dump_xma_mutex);
  for (int type = 0; type < 2; ++type) {
    const auto it = dumps_xma.find({ctx_data, type});
    if (it != dumps_xma.end() && !it->second.written) {
      XmaDump& v = it->second;
      samples_accumulated_xma -= v.sample_count.size();
      // The sound has ended: if it is long enough it is saved as a segment.
      const size_t min = size_t(std::max(REXCVAR_GET(audio_dump_xma_min_s), 1)) *
                            std::max<uint32_t>(v.sample_hz, 1) * std::max<uint32_t>(v.channels, 1);
      if (v.sample_hz && v.sample_count.size() >= min &&
          dumped_files_xma < 2 * uint32_t(std::max(REXCVAR_GET(audio_dump_xma_contexts), 0))) {
        ++dumped_files_xma;
        WriteDumpXma(ctx_data, type, v);
      }
      dumps_xma.erase(it);
    }
  }
  dumped_contexts_xma.erase(ctx_data);  // the sound has ended: frees its slot
}

}  // namespace

namespace rex::audio {

using stream::BitStream;

const uint32_t XmaContext::kBitsPerPacketHeader;
const uint32_t XmaContext::kOutputMaxSizeBytes;

XmaContext::XmaContext()
    : work_completion_event_(rex::thread::Event::CreateAutoResetEvent(false)) {}

XmaContext::~XmaContext() {
  if (av_context_) {
    avcodec_free_context(&av_context_);
  }
  if (av_frame_) {
    av_frame_free(&av_frame_);
  }
}

int XmaContext::Setup(uint32_t id, memory::Memory* memory, uint32_t guest_ptr) {
  id_ = id;
  memory_ = memory;
  guest_ptr_ = guest_ptr;

  // Allocate ffmpeg stuff:
  av_packet_ = av_packet_alloc();
  assert_not_null(av_packet_);
  av_packet_->buf = av_buffer_alloc(128 * 1024);

  // find the XMA2 audio decoder
  av_codec_ = avcodec_find_decoder(AV_CODEC_ID_XMAFRAMES);
  if (!av_codec_) {
    REXAPU_ERROR("XmaContext {}: Codec not found", id);
    return 1;
  }

  av_context_ = avcodec_alloc_context3(av_codec_);
  if (!av_context_) {
    REXAPU_ERROR("XmaContext {}: Couldn't allocate context", id);
    return 1;
  }

  // Initialize these to 0. They'll actually be set later.
  av_context_->channels = 0;
  av_context_->sample_rate = 0;

  av_frame_ = av_frame_alloc();
  if (!av_frame_) {
    REXAPU_ERROR("XmaContext {}: Couldn't allocate frame", id);
    return 1;
  }

  // FYI: We're purposely not opening the codec here. That is done later.
  return 0;
}

/*
 * Silence watchdog, measurement only.
 *
 * In one recording, after braking in a corner, the engine acceleration sound takes ~2.5 s to come
 * back, coinciding with a zone load. If the voice that goes quiet is XMA, it shows here: every time
 * the game enables the context with an output buffer pending and the pass produces no PCM, a silence
 * starts (or continues); when it produces again, if the silence lasted 50 ms or more, a log line
 * gives the context, how long it lasted, how many empty passes there were and why (1 only flush
 * without subframes, 2 no input, 3 no progress, 4 error). Contexts that never sound again (end of a
 * sound) write nothing. It does not change anything that is decoded. At most 200 lines per run.
 */
void XmaContext::AnnotateProduction(bool produced, uint8_t reason, const XMA_CONTEXT_DATA& data) {
  static std::atomic<uint32_t> lines{0};
  const auto now = std::chrono::steady_clock::now();
  if (!produced) {
    if (!in_silence_) {
      in_silence_ = true;
      silence_since_ = now;
      silence_passes_ = 0;
    }
    ++silence_passes_;
    silence_reason_ = reason;
    return;
  }
  if (!in_silence_) {
    return;
  }
  in_silence_ = false;
  const double ms = std::chrono::duration<double, std::milli>(now - silence_since_).count();
  if (ms >= 50.0 && lines.fetch_add(1, std::memory_order_relaxed) < 200) {
    REXLOG_INFO("[xma] silence: context {} produced nothing for {:.0f} ms and plays again ({} empty passes, "
                "last reason {}: 1 flush only, 2 no input, 3 no progress, 4 error; inputs {}{}, current buffer {}, "
                "loops {})",
                id_, ms, silence_passes_, silence_reason_, uint32_t(data.input_buffer_0_valid),
                uint32_t(data.input_buffer_1_valid), uint32_t(data.current_buffer), uint32_t(data.loop_count));
  }
}

bool XmaContext::Work() {
  if (!is_allocated() || !is_enabled()) {
    return false;
  }

  std::lock_guard<std::mutex> lock(lock_);
  // The immediate work and the XMA thread may have seen the same notification.
  // Only one must consume it; Disable/Release also use this lock.
  if (!is_allocated() || !is_enabled()) {
    return false;
  }
  set_is_enabled(false);

  auto context_ptr = memory()->TranslateVirtual(guest_ptr());
  XMA_CONTEXT_DATA data(context_ptr);
  const XMA_CONTEXT_DATA initial_data = data;

  // A read offset this decoder did not store means the game set a decode position, so drop what
  // is left of the frame it was decoding (xenia-canary a4ef31cdf).
  if (data.input_buffer_read_offset != last_stored_read_offset_) {
    current_frame_remaining_subframes_ = 0;
  }

  if (!data.output_buffer_valid) {
    return true;
  }

  // Avoid modulo by zero and accesses outside the guest buffer.
  if (!data.output_buffer_block_count ||
      data.output_buffer_read_offset >= data.output_buffer_block_count ||
      data.output_buffer_write_offset >= data.output_buffer_block_count) {
    data.error_status = 4;
    data.output_buffer_valid = 0;
    StoreContextMerged(data, initial_data, context_ptr);
    return true;
  }

  memory::RingBuffer output_rb = PrepareOutputRingBuffer(&data);
  uint32_t output_bytes_written = 0;

  // Consume-only context: no input, just drain remaining subframes.
  if (data.IsConsumeOnlyContext()) {
    if (current_frame_remaining_subframes_ == 0) {
      return true;
    }
    // Flush all the PCM that fits, even if the packet size was set to zero.
    // Consume returns zero when it has to wait for free space.
    while (current_frame_remaining_subframes_) {
      const uint32_t written = Consume(&output_rb, &data);
      if (!written) {
        break;
      }
      output_bytes_written += written;
    }
    data.output_buffer_write_offset = output_rb.write_offset() / kOutputBytesPerBlock;
    if (output_bytes_written && output_rb.empty()) {
      data.output_buffer_valid = 0;
    }
    StoreContextMerged(data, initial_data, context_ptr);
    AnnotateProduction(output_bytes_written != 0, 1, data);  // Measurement only
    return true;
  }

  // Minimum free blocks needed before attempting a decode.
  // Use subframe_decode_count (clamped to 1) instead of full frame size.
  const uint32_t effective_sdc = std::max(static_cast<uint32_t>(1), data.subframe_decode_count);
  // Re-read each pass: Decode() clears output_buffer_padding once it has skipped a decode
  // position's leading subframes.
  const auto minimum_subframe_decode_count = [&]() {
    return static_cast<int32_t>(effective_sdc) + static_cast<int32_t>(data.output_buffer_padding);
  };

  if (minimum_subframe_decode_count() > remaining_subframe_blocks_in_output_buffer_) {
    StoreContextMerged(data, initial_data, context_ptr);
    AnnotateProduction(true, 0, data);  // output full, not silence
    return true;
  }

  uint8_t silence_reason = 0;  // Measurement only
  while (remaining_subframe_blocks_in_output_buffer_ >= minimum_subframe_decode_count()) {
    const uint32_t previous_offset = data.input_buffer_read_offset;
    const uint32_t previous_buffer = data.current_buffer;
    const auto previous_valid = data.input_buffer_0_valid | (data.input_buffer_1_valid << 1);
    const uint32_t previous_loop_count = data.loop_count;
    const uint8_t previous_subframes = current_frame_remaining_subframes_;
    Decode(&data);
    const uint32_t written = Consume(&output_rb, &data);
    output_bytes_written += written;

    // The last packet can leave several decoded subframes. If this returns with
    // PCM pending, the consumer may wait for a full block without advancing the
    // read position or re-enabling this context.
    if ((!data.IsAnyInputBufferValid() && !current_frame_remaining_subframes_) ||
        data.error_status == 4) {
      silence_reason = data.error_status == 4 ? 4 : 2;
      break;
    }
    // A frame's continuation may be missing while the producer refills the other
    // buffer. Keep the state and release the lock until the next Enable, without
    // spinning here or turning an input wait into an error.
    if (!written && previous_offset == data.input_buffer_read_offset &&
        previous_buffer == data.current_buffer &&
        previous_valid == (data.input_buffer_0_valid | (data.input_buffer_1_valid << 1)) &&
        previous_loop_count == data.loop_count &&
        previous_subframes == current_frame_remaining_subframes_) {
      silence_reason = 3;
      break;
    }
  }

  data.output_buffer_write_offset = output_rb.write_offset() / kOutputBytesPerBlock;

  // Equal pointers mean full only if PCM was written this time.
  if (output_bytes_written && output_rb.empty()) {
    data.output_buffer_valid = 0;
  }

  StoreContextMerged(data, initial_data, context_ptr);
  AnnotateProduction(output_bytes_written != 0, silence_reason, data);  // Measurement only
  return true;
}

void XmaContext::Enable() {
  std::lock_guard<std::mutex> lock(lock_);
  set_is_enabled(true);
}

bool XmaContext::Block(bool poll) {
  if (!lock_.try_lock()) {
    if (poll) {
      return false;
    }
    lock_.lock();
  }
  lock_.unlock();
  return true;
}

void XmaContext::Clear() {
  std::lock_guard<std::mutex> lock(lock_);
  REXAPU_NOISY_DEBUG("XmaContext: reset context {}", id());

  auto context_ptr = memory()->TranslateVirtual(guest_ptr());
  XMA_CONTEXT_DATA data(context_ptr);
  ClearLocked(&data);
  data.Store(context_ptr);
}

void XmaContext::ClearLocked(XMA_CONTEXT_DATA* data) {
  data->input_buffer_0_valid = 0;
  data->input_buffer_1_valid = 0;
  data->output_buffer_valid = 0;

  data->input_buffer_read_offset = kBitsPerPacketHeader;
  data->output_buffer_read_offset = 0;
  data->output_buffer_write_offset = 0;

  ResetDecoderState();
}

/*
 * The decoded XMA frame cache (apu_xma_cache).
 *
 * Without it the XMA worker used 17 % of a core on the console, ~11 % of it inside FFmpeg (avcodec_send_packet and
 * what it calls), decoding the same zombie, weapon and ambience sounds again each time they play. The xmaframes
 * decoder (FFmpeg wmaprodec.c, xmaframes_decode_packet) starts each frame's bits afresh (save_bits without append) and
 * carries one thing from frame to frame: the second half of the last IMDCT (channel[].out) and the last block length
 * (prev_block_len), both set by the frame it decoded; skip_frame is 0 for XMA and packet_loss is only read on the
 * WMA Pro packet path. So a frame's PCM is a function of its bits, the bits of the frame before it (nothing before a
 * fresh decoder), the rate and the channel count; the cache key is the hash of those four.
 * A frame served from the cache is not given to the decoder, so its state falls one frame behind; before the next
 * real decode the last served frame is fed to it and its output dropped (CatchUp), which leaves the state that frame
 * would have left. A real decode is compared with the cached PCM for the first 2,000 reuses and then 1 in 256 (after
 * a catch-up when there was one); a difference switches the cache off for the session and is logged.
 * Two ways per set, the one used less recently replaced: a direct-mapped 4 MB table served only ~33 % of the frames,
 * and a third of its hit runs ended in a frame evicted before the sound came back.
 */
namespace {

class FrameCache {
 public:
  static constexpr uint32_t kEntryBytes = XmaContext::kBytesPerFrameChannel * 2;
  struct Entry {
    uint64_t key = 0;  // 0 = empty
    uint32_t bytes = 0;
    uint8_t pcm[kEntryBytes];
  };

  // nullptr when off: the setting, a failed allocation, or a difference seen by the check.
  static FrameCache* IfOn();

  // On a hit copies the PCM to out; *check: decode anyway and compare (Checked).
  bool Find(uint64_t key, uint32_t bytes, uint8_t* out, bool* check) {
    std::lock_guard<std::mutex> lock(mutex_);
    const uint64_t set = key % sets_;
    for (uint32_t way = 0; way < 2; ++way) {
      const Entry& e = entries_[set * 2 + way];
      if (e.key == key && e.bytes == bytes) {
        std::memcpy(out, e.pcm, bytes);
        victim_[set] = uint8_t(way ^ 1);
        const uint64_t n = ++hits_ever_;
        *check = n <= 2000 || (n & 255) == 0;
        return true;
      }
    }
    return false;
  }

  void Store(uint64_t key, uint32_t bytes, const uint8_t* pcm) {
    std::lock_guard<std::mutex> lock(mutex_);
    const uint64_t set = key % sets_;
    uint32_t way = victim_[set];
    if (entries_[set * 2 + (way ^ 1)].key == key) {
      way ^= 1;
    }
    Entry& e = entries_[set * 2 + way];
    e.key = key;
    e.bytes = bytes;
    std::memcpy(e.pcm, pcm, bytes);
    victim_[set] = uint8_t(way ^ 1);
  }

  static std::atomic<bool> failed;
  // Counts for the 10-s report.
  static std::atomic<uint64_t> frames, served, checks, different, stored, catch_ups;

 private:
  std::mutex mutex_;
  std::unique_ptr<Entry[]> entries_;
  std::unique_ptr<uint8_t[]> victim_;  // per set: the way to replace next
  uint64_t sets_ = 1;
  uint64_t hits_ever_ = 0;

  static FrameCache* Make() {
    const uint64_t bytes = uint64_t(std::clamp(REXCVAR_GET(apu_xma_cache_mb), 1, 256)) << 20;
    const uint64_t sets = std::max<uint64_t>(1, bytes / (2 * sizeof(Entry)));
    auto* cache = new (std::nothrow) FrameCache;
    if (cache) {
      cache->entries_.reset(new (std::nothrow) Entry[sets * 2]);
      cache->victim_.reset(new (std::nothrow) uint8_t[sets]());
    }
    if (!cache || !cache->entries_ || !cache->victim_) {
      delete cache;
      failed.store(true, std::memory_order_relaxed);
      REXLOG_ERROR("[xma] frame cache: could not allocate {} MB; every frame is decoded", bytes >> 20);
      return nullptr;
    }
    cache->sets_ = sets;
    REXLOG_INFO("[xma] frame cache: {} frames in {} sets of 2 ({:.1f} MB)", sets * 2, sets,
                double(sets * 2 * sizeof(Entry)) / (1 << 20));
    return cache;
  }
};

std::atomic<bool> FrameCache::failed{false};
std::atomic<uint64_t> FrameCache::frames{0}, FrameCache::served{0}, FrameCache::checks{0},
    FrameCache::different{0}, FrameCache::stored{0}, FrameCache::catch_ups{0};

FrameCache* FrameCache::IfOn() {
  if (!REXCVAR_GET(apu_xma_cache) || failed.load(std::memory_order_relaxed)) {
    return nullptr;
  }
  static FrameCache* const cache = Make();
  return cache;
}

uint64_t FrameKey(uint64_t previous, uint64_t frame, uint32_t rate_id, bool stereo) {
  const uint64_t words[3] = {previous, frame, (uint64_t(rate_id) << 1) | uint64_t(stereo)};
  const uint64_t key = XXH3_64bits(words, sizeof(words));
  return key ? key : 1;
}

// Every 10 s, from the thread that decodes first after that.
void ReportFrameCache() {
  static std::atomic<int64_t> last{0};
  const int64_t now = std::chrono::duration_cast<std::chrono::nanoseconds>(
                          std::chrono::steady_clock::now().time_since_epoch())
                          .count();
  int64_t l = last.load(std::memory_order_relaxed);
  if (l == 0) {
    last.compare_exchange_strong(l, now, std::memory_order_relaxed);
    return;
  }
  if (now - l < 10'000'000'000ll || !last.compare_exchange_strong(l, now, std::memory_order_relaxed)) {
    return;
  }
  const uint64_t frames = FrameCache::frames.exchange(0, std::memory_order_relaxed);
  const uint64_t served = FrameCache::served.exchange(0, std::memory_order_relaxed);
  const uint64_t checks = FrameCache::checks.exchange(0, std::memory_order_relaxed);
  const uint64_t different = FrameCache::different.exchange(0, std::memory_order_relaxed);
  const uint64_t stored = FrameCache::stored.exchange(0, std::memory_order_relaxed);
  const uint64_t catch_ups = FrameCache::catch_ups.exchange(0, std::memory_order_relaxed);
  REXLOG_INFO("[xma] frame cache ({}), last 10 s: {} frames, {} served from the cache ({:.1f} %), {} decoded and "
              "checked against it ({} different), {} decoded and kept, {} catch-up decodes",
              FrameCache::failed.load() ? "OFF after a difference" : (REXCVAR_GET(apu_xma_cache) ? "on" : "off"),
              frames, served, frames ? 100.0 * double(served) / double(frames) : 0.0, checks, different, stored,
              catch_ups);
}

}  // namespace

// Feeds the decoder the last frame served from the cache, so its state is the one that frame leaves.
void XmaContext::CatchUp() {
  if (!decoder_behind_) {
    return;
  }
  decoder_behind_ = false;
  uint8_t* const frame = av_packet_->data;
  const int size = av_packet_->size;
  av_packet_->data = catch_up_frame_.data();
  av_packet_->size = int(catch_up_size_);
  DecodePacket(av_context_, av_packet_, av_frame_);  // output dropped
  av_packet_->data = frame;
  av_packet_->size = size;
  FrameCache::catch_ups.fetch_add(1, std::memory_order_relaxed);
}

// Decodes the frame in av_packet_ into raw_frame_ (big-endian PCM), from the cache when it can.
bool XmaContext::DecodeCached(const XMA_CONTEXT_DATA* data) {
  const bool stereo = bool(data->is_stereo);
  const uint32_t pcm_bytes = kBytesPerFrameChannel << uint32_t(stereo);
  const uint64_t frame_hash = XXH3_64bits(av_packet_->data, size_t(av_packet_->size)) | 1;
  const uint64_t key = FrameKey(previous_frame_hash_, frame_hash, uint32_t(data->sample_rate), stereo);
  previous_frame_hash_ = frame_hash;
  FrameCache::frames.fetch_add(1, std::memory_order_relaxed);
  ReportFrameCache();

  FrameCache* const cache = FrameCache::IfOn();
  bool check = false;
  std::array<uint8_t, FrameCache::kEntryBytes> cached;
  if (cache && cache->Find(key, pcm_bytes, raw_frame_.data(), &check)) {
    if (!check) {
      std::memcpy(catch_up_frame_.data(), av_packet_->data, size_t(av_packet_->size));
      catch_up_size_ = uint32_t(av_packet_->size);
      decoder_behind_ = true;
      FrameCache::served.fetch_add(1, std::memory_order_relaxed);
      return true;
    }
    std::memcpy(cached.data(), raw_frame_.data(), pcm_bytes);
  }

  CatchUp();
  if (!DecodePacket(av_context_, av_packet_, av_frame_)) {
    return false;
  }
  ConvertFrame(reinterpret_cast<const uint8_t**>(&av_frame_->data), stereo, raw_frame_.data());
  if (cache) {
    if (check) {
      FrameCache::checks.fetch_add(1, std::memory_order_relaxed);
      if (std::memcmp(cached.data(), raw_frame_.data(), pcm_bytes) != 0) {
        FrameCache::different.fetch_add(1, std::memory_order_relaxed);
        if (!FrameCache::failed.exchange(true)) {
          REXLOG_ERROR("[xma] frame cache: a reused frame differs from its decode (context {}, key {:016X}); the "
                       "cache is off for the rest of the session",
                       id(), key);
        }
      }
    } else {
      cache->Store(key, pcm_bytes, raw_frame_.data());
      FrameCache::stored.fetch_add(1, std::memory_order_relaxed);
    }
  }
  return true;
}

void XmaContext::ResetDecoderState() {
  ResetDumpXma(id());  // diagnostic audio_dump_xma_s
  previous_frame_hash_ = 0;  // the frame cache's chain starts again with the decoder
  decoder_behind_ = false;
  // A freed or re-initialized context is a new logical stream, so the previous
  // wave's MDCT overlap-add tail must not survive into frame 0 of the next one.
  // avcodec_flush_buffers() cannot drop it: ff_xmaframes_decoder declares no
  // flush callback, so the call never reaches the code clearing channel[].out.
  // Invalidating the cached format makes PrepareDecoder reopen the codec on the
  // next decode, which does discard the history.
  if (av_context_) {
    av_context_->sample_rate = 0;
    av_context_->channels = 0;
  }
  raw_frame_.fill(0);
  in_silence_ = false;  // a new sound does not inherit the previous one's silence
  current_frame_remaining_subframes_ = 0;
  loop_frame_output_limit_ = 0;
  loop_start_skip_pending_ = false;
  last_stored_read_offset_ = UINT32_MAX;
}

void XmaContext::Disable() {
  std::lock_guard<std::mutex> lock(lock_);
  set_is_enabled(false);
}

void XmaContext::Release() {
  std::lock_guard<std::mutex> lock(lock_);
  assert_true(is_allocated());

  set_is_allocated(false);
  ResetDecoderState();
  auto context_ptr = memory()->TranslateVirtual(guest_ptr());
  std::memset(context_ptr, 0, sizeof(XMA_CONTEXT_DATA));
}

void XmaContext::SwapInputBuffer(XMA_CONTEXT_DATA* data) {
  if (data->current_buffer == 0) {
    data->input_buffer_0_valid = 0;
  } else {
    data->input_buffer_1_valid = 0;
  }
  data->current_buffer ^= 1;
  data->input_buffer_read_offset = kBitsPerPacketHeader;
}

void XmaContext::UpdateLoopStatus(XMA_CONTEXT_DATA* data) {
  if (!IsLoopEnd(*data)) {
    return;
  }

  const uint32_t loop_start = std::max(kBitsPerPacketHeader, data->loop_start);

  data->input_buffer_read_offset = loop_start;
  loop_start_skip_pending_ = true;

  if (data->loop_count < 255) {
    data->loop_count--;
  }
}

int XmaContext::GetSampleRate(int id) {
  return kIdToSampleRate[std::min(id, 3)];
}

int16_t XmaContext::GetPacketNumber(size_t size, size_t bit_offset) {
  if (bit_offset < kBitsPerPacketHeader) {
    assert_always();
    return -1;
  }
  if (bit_offset >= (size << 3)) {
    assert_always();
    return -1;
  }
  size_t byte_offset = bit_offset >> 3;
  size_t packet_number = byte_offset / kBytesPerPacket;
  return static_cast<int16_t>(packet_number);
}

uint32_t XmaContext::GetCurrentInputBufferSize(XMA_CONTEXT_DATA* data) {
  return data->GetCurrentInputBufferPacketCount() * kBytesPerPacket;
}

uint8_t* XmaContext::GetCurrentInputBuffer(XMA_CONTEXT_DATA* data) {
  return memory()->TranslatePhysical(data->GetCurrentInputBufferAddress());
}

uint32_t XmaContext::GetAmountOfBitsToRead(uint32_t remaining_stream_bits, uint32_t frame_size) {
  return std::min(remaining_stream_bits, frame_size);
}

const uint8_t* XmaContext::GetNextPacket(XMA_CONTEXT_DATA* data, uint32_t next_packet_index,
                                         uint32_t current_input_packet_count) {
  uint8_t buffer_index = data->current_buffer;
  if (next_packet_index >= current_input_packet_count) {
    buffer_index ^= 1;
    next_packet_index -= current_input_packet_count;
  }

  if (!data->IsInputBufferValid(buffer_index) ||
      next_packet_index >= data->GetInputBufferPacketCount(buffer_index)) {
    return nullptr;
  }

  const uint32_t next_buffer_address = data->GetInputBufferAddress(buffer_index);
  if (!next_buffer_address) {
    REXAPU_ERROR("XmaContext {}: Buffer marked valid but has null pointer!", id());
    return nullptr;
  }

  // The skip between packets may continue inside the second buffer.
  // Always reading its start mixes the continuation with another audio stream.
  return memory()->TranslatePhysical(next_buffer_address) + next_packet_index * kBytesPerPacket;
}

void XmaContext::AdvanceInputPacket(XMA_CONTEXT_DATA* data, uint32_t next_packet_index) {
  // At most the two available buffers are consumed. Keep pending skips when
  // crossing them and search after packets with no new frames.
  for (unsigned buffer = 0; buffer < 2; ++buffer) {
    if (!data->IsCurrentInputBufferValid()) {
      return;
    }
    const uint32_t count = data->GetCurrentInputBufferPacketCount();
    if (!data->GetCurrentInputBufferAddress()) {
      data->error_status = 4;
      return;
    }
    const uint32_t offset =
        xma::FindNextFrameStart(GetCurrentInputBuffer(data), next_packet_index, count);
    if (offset != UINT32_MAX) {
      data->input_buffer_read_offset = offset;
      return;
    }
    next_packet_index -= count;  // a skip chain past this buffer continues in the next one
    SwapInputBuffer(data);
  }
}

memory::RingBuffer XmaContext::PrepareOutputRingBuffer(XMA_CONTEXT_DATA* data) {
  const uint32_t output_capacity = data->output_buffer_block_count * kOutputBytesPerBlock;
  const uint32_t output_read_offset = data->output_buffer_read_offset * kOutputBytesPerBlock;
  const uint32_t output_write_offset = data->output_buffer_write_offset * kOutputBytesPerBlock;

  if (output_capacity > kOutputMaxSizeBytes) {
    REXAPU_WARN(
        "XmaContext {}: Output buffer exceeds expected size! "
        "(Actual: {} Max: {})",
        id(), output_capacity, kOutputMaxSizeBytes);
  }

  uint8_t* output_buffer = memory()->TranslatePhysical(data->output_buffer_ptr);

  memory::RingBuffer output_rb(output_buffer, output_capacity);
  output_rb.set_read_offset(output_read_offset);
  output_rb.set_write_offset(output_write_offset);
  remaining_subframe_blocks_in_output_buffer_ =
      static_cast<int32_t>(output_rb.write_count()) / kOutputBytesPerBlock;

  return output_rb;
}

kPacketInfo XmaContext::GetPacketInfo(uint8_t* packet, uint32_t frame_offset) {
  kPacketInfo packet_info = {};

  const uint32_t first_frame_offset = xma::GetPacketFrameOffset(packet);
  BitStream stream(packet, kBitsPerPacket);
  stream.SetOffset(first_frame_offset);

  if (frame_offset < first_frame_offset) {
    packet_info.current_frame_ = 0;
    packet_info.current_frame_size_ = first_frame_offset - frame_offset;
  }

  while (true) {
    if (stream.BitsRemaining() < kBitsPerFrameHeader) {
      // A header that starts here may continue in the next packet. Counting it
      // avoids marking the previous frame as the last one and skipping this one.
      // Decode will join both fragments to get its full size.
      if (stream.BitsRemaining()) {
        if (stream.offset_bits() == frame_offset) {
          packet_info.current_frame_ = packet_info.frame_count_;
        }
        packet_info.frame_count_++;
      }
      break;
    }

    const uint64_t frame_size = stream.Peek(kBitsPerFrameHeader);
    if (frame_size == 0 || frame_size == xma::kMaxFrameLength) {
      break;
    }

    if (stream.offset_bits() == frame_offset) {
      packet_info.current_frame_ = packet_info.frame_count_;
      packet_info.current_frame_size_ = static_cast<uint32_t>(frame_size);
    }

    packet_info.frame_count_++;

    if (frame_size > stream.BitsRemaining()) {
      break;
    }

    stream.Advance(frame_size - 1);

    if (stream.Read(1) == 0) {
      break;
    }
  }

  if (xma::IsPacketXma2Type(packet)) {
    const uint8_t xma2_frame_count = xma::GetPacketFrameCount(packet);
    if (xma2_frame_count > packet_info.frame_count_) {
      if (packet_info.current_frame_size_ == 0) {
        packet_info.current_frame_ = packet_info.frame_count_;
      }
      packet_info.frame_count_ = xma2_frame_count;
    }
  }
  return packet_info;
}

void XmaContext::StoreContextMerged(const XMA_CONTEXT_DATA& data,
                                    const XMA_CONTEXT_DATA& initial_data, uint8_t* context_ptr) {
  XMA_CONTEXT_DATA fresh(context_ptr);

  fresh.loop_count = data.loop_count;
  fresh.output_buffer_write_offset = data.output_buffer_write_offset;
  if (initial_data.input_buffer_0_valid && !data.input_buffer_0_valid) {
    fresh.input_buffer_0_valid = 0;
  }
  if (initial_data.input_buffer_1_valid && !data.input_buffer_1_valid) {
    fresh.input_buffer_1_valid = 0;
  }

  if (initial_data.output_buffer_valid && !data.output_buffer_valid) {
    fresh.output_buffer_valid = 0;
  }
  // The decoder clears the decode position's start subframe once it has skipped them.
  if (initial_data.output_buffer_padding && !data.output_buffer_padding) {
    fresh.output_buffer_padding = 0;
  }

  fresh.input_buffer_read_offset = data.input_buffer_read_offset;
  fresh.error_status = data.error_status;
  fresh.current_buffer = data.current_buffer;
  fresh.output_buffer_read_offset = data.output_buffer_read_offset;

  fresh.Store(context_ptr);
  last_stored_read_offset_ = fresh.input_buffer_read_offset;
}

uint32_t XmaContext::Consume(memory::RingBuffer* output_rb, const XMA_CONTEXT_DATA* data) {
  if (!current_frame_remaining_subframes_) {
    return 0;
  }

  if (loop_frame_output_limit_ > 0) {
    const uint8_t total_subframes = (kBytesPerFrameChannel / kOutputBytesPerBlock)
                                    << data->is_stereo;
    const uint8_t consumed = total_subframes - current_frame_remaining_subframes_;
    if (consumed >= loop_frame_output_limit_) {
      remaining_subframe_blocks_in_output_buffer_ -= data->output_buffer_padding;
      current_frame_remaining_subframes_ = 0;
      loop_frame_output_limit_ = 0;
      return 0;
    }
  }

  const uint8_t effective_sdc = std::max(static_cast<uint32_t>(1), data->subframe_decode_count);
  int8_t subframes_to_write = std::min(static_cast<int8_t>(current_frame_remaining_subframes_),
                                       static_cast<int8_t>(effective_sdc));

  if (loop_frame_output_limit_ > 0) {
    const uint8_t total_subframes = (kBytesPerFrameChannel / kOutputBytesPerBlock)
                                    << data->is_stereo;
    const uint8_t consumed = total_subframes - current_frame_remaining_subframes_;
    const int8_t remaining_until_limit = static_cast<int8_t>(loop_frame_output_limit_ - consumed);
    if (subframes_to_write > remaining_until_limit) {
      subframes_to_write = remaining_until_limit;
    }
  }

  // The no-input path also goes through here. Do not drop samples that do not
  // fit: keep them until the consumer frees space.
  if (remaining_subframe_blocks_in_output_buffer_ < subframes_to_write ||
      output_rb->write_count() < uint32_t(subframes_to_write) * kOutputBytesPerBlock) {
    return 0;
  }

  const int8_t raw_frame_read_offset =
      ((kBytesPerFrameChannel / kOutputBytesPerBlock) << data->is_stereo) -
      current_frame_remaining_subframes_;

  const uint32_t written = static_cast<uint32_t>(output_rb->Write(
      raw_frame_.data() + (kOutputBytesPerBlock * raw_frame_read_offset),
      subframes_to_write * kOutputBytesPerBlock));
  // Diagnostic (audio_dump_xma_s): the blocks as they reach the game's buffer.
  DumpXma(id(), 1, GetSampleRate(data->sample_rate), data->is_stereo ? 2 : 1,
            raw_frame_.data() + (kOutputBytesPerBlock * raw_frame_read_offset), written);

  const int8_t headroom = (current_frame_remaining_subframes_ - subframes_to_write == 0)
                              ? data->output_buffer_padding
                              : 0;

  remaining_subframe_blocks_in_output_buffer_ -= subframes_to_write + headroom;
  current_frame_remaining_subframes_ -= subframes_to_write;
  return written;
}

int XmaContext::PrepareDecoder(int sample_rate, bool is_two_channel) {
  sample_rate = GetSampleRate(sample_rate);

  uint32_t channels = is_two_channel ? 2 : 1;
  if (av_context_->sample_rate != sample_rate ||
      av_context_->channels != static_cast<int>(channels)) {
    REXAPU_NOISY_DEBUG("XmaContext {}: Codec reinit: rate {} -> {}, channels {} -> {}", id(),
                       av_context_->sample_rate, sample_rate, av_context_->channels, channels);
    avcodec_free_context(&av_context_);
    av_context_ = avcodec_alloc_context3(av_codec_);

    av_context_->sample_rate = sample_rate;
    av_context_->channels = channels;
    // No AV_CODEC_FLAG2_SKIP_MANUAL (Xenia sets it): with it, every decoded frame got a SKIP_SAMPLES side data
    // block (a side data struct and a buffer allocated, then freed by av_frame_unref), which nothing reads. Without
    // it FFmpeg would skip or discard samples itself only on packet side data or discard flags, which these packets
    // never carry, or on a decoder skip, which the xmaframes decoder never sets: on the PC, 540,000 frames of a
    // Town run all had skip 0 and discard 0. Same output, a few allocations fewer per frame (they contended for
    // the global malloc lock with the renderer on the Switch).

    if (avcodec_open2(av_context_, av_codec_, NULL) < 0) {
      REXAPU_ERROR("XmaContext: Failed to reopen FFmpeg context");
      return -1;
    }
    return 1;
  }
  return 0;
}

void XmaContext::PreparePacket(uint32_t frame_size, uint32_t frame_padding) {
  av_packet_->data = xma_frame_.data();
  av_packet_->size = static_cast<int>(1 + ((frame_padding + frame_size) / 8) +
                                      (((frame_padding + frame_size) % 8) ? 1 : 0));

  auto padding_end = av_packet_->size * 8 - (8 + frame_padding + frame_size);
  assert_true(padding_end < 8);
  xma_frame_[0] = ((frame_padding & 7) << 5) | ((padding_end & 7) << 2);
}

bool XmaContext::DecodePacket(AVCodecContext* av_context, const AVPacket* av_packet,
                              AVFrame* av_frame) {
  auto ret = avcodec_send_packet(av_context, av_packet);
  if (ret < 0) {
    char errbuf[AV_ERROR_MAX_STRING_SIZE];
    av_strerror(ret, errbuf, sizeof(errbuf));
    REXAPU_ERROR("XmaContext {}: Error sending packet for decoding: {} ({})", id(), errbuf, ret);
    return false;
  }
  ret = avcodec_receive_frame(av_context, av_frame);

  if (ret == AVERROR(EAGAIN)) {
    return false;
  }
  if (ret < 0) {
    char errbuf[AV_ERROR_MAX_STRING_SIZE];
    av_strerror(ret, errbuf, sizeof(errbuf));
    REXAPU_ERROR("XmaContext {}: Error during decoding: {} ({})", id(), errbuf, ret);
    return false;
  }
  return true;
}

void XmaContext::Decode(XMA_CONTEXT_DATA* data) {
  SCOPE_profile_cpu_f("apu");

  if (!data->IsAnyInputBufferValid()) {
    return;
  }

  if (current_frame_remaining_subframes_ > 0) {
    return;
  }

  if (!data->IsCurrentInputBufferValid()) {
    SwapInputBuffer(data);
    if (!data->IsCurrentInputBufferValid()) {
      return;
    }
  }

  uint8_t* current_input_buffer = GetCurrentInputBuffer(data);

  input_buffer_.fill(0);

  // Loop-end frame: decode it here (output limited to loop_subframe_end),
  // jump to loop_start afterwards in the next-offset step.
  const bool is_loop_end_frame = IsLoopEnd(*data);

  if (!data->output_buffer_block_count) {
    REXAPU_ERROR("XmaContext {}: Error - Received 0 for output_buffer_block_count!", id());
    return;
  }

  if (data->input_buffer_read_offset < kBitsPerPacketHeader) {
    data->input_buffer_read_offset = kBitsPerPacketHeader;
  }

  const uint32_t current_input_size = GetCurrentInputBufferSize(data);
  const uint32_t current_input_packet_count = current_input_size / kBytesPerPacket;

  const int16_t packet_index = GetPacketNumber(current_input_size, data->input_buffer_read_offset);

  if (packet_index == -1) {
    REXAPU_ERROR("XmaContext {}: Invalid packet index. Input read offset: {}", id(),
                 static_cast<uint32_t>(data->input_buffer_read_offset));
    // Work must publish the error and release the lock. Without this state, it
    // repeated Decode forever with the same offset outside the input.
    data->error_status = 4;
    return;
  }

  uint8_t* packet = current_input_buffer + (packet_index * kBytesPerPacket);
  const uint32_t packet_first_frame_offset = xma::GetPacketFrameOffset(packet);
  uint32_t relative_offset = data->input_buffer_read_offset % kBitsPerPacket;

  if (relative_offset < packet_first_frame_offset) {
    data->input_buffer_read_offset = (packet_index * kBitsPerPacket) + packet_first_frame_offset;
    relative_offset = packet_first_frame_offset;
  }

  const uint8_t skip_count = xma::GetPacketSkipCount(packet);

  // Full packet skip (0xFF) -- no new frames begin in this packet.
  if (skip_count == 0xFF) {
    AdvanceInputPacket(data, packet_index + 1);
    return;
  }

  kPacketInfo packet_info = GetPacketInfo(packet, relative_offset);
  const uint32_t packet_to_skip = skip_count + 1;
  const uint32_t next_packet_index = packet_index + packet_to_skip;

  // Frame header split across packet boundary.
  if (packet_info.current_frame_size_ == 0) {
    const uint8_t* next_packet = GetNextPacket(data, next_packet_index, current_input_packet_count);
    if (!next_packet) {
      // A split header still belongs to the current buffer. Do not release it
      // before the producer publishes its continuation in the other buffer.
      if (kBitsPerPacket - relative_offset < kBitsPerFrameHeader &&
          next_packet_index >= current_input_packet_count &&
          !data->IsInputBufferValid(data->current_buffer ^ 1)) {
        return;
      }
      SwapInputBuffer(data);
      return;
    }
    std::memcpy(input_buffer_.data(), packet + kBytesPerPacketHeader, kBytesPerPacketData);
    std::memcpy(input_buffer_.data() + kBytesPerPacketData, next_packet + kBytesPerPacketHeader,
                kBytesPerPacketData);

    BitStream combined(input_buffer_.data(), (kBitsPerPacket - kBitsPerPacketHeader) * 2);
    combined.SetOffset(relative_offset - kBitsPerPacketHeader);

    uint64_t frame_size = combined.Peek(kBitsPerFrameHeader);
    if (frame_size == xma::kMaxFrameLength) {
      data->error_status = 4;
      return;
    }
    packet_info.current_frame_size_ = static_cast<uint32_t>(frame_size);
  }

  BitStream stream(current_input_buffer, (packet_index + 1) * kBitsPerPacket);
  stream.SetOffset(data->input_buffer_read_offset);

  const uint64_t bits_to_copy = GetAmountOfBitsToRead(static_cast<uint32_t>(stream.BitsRemaining()),
                                                      packet_info.current_frame_size_);

  if (bits_to_copy == 0) {
    REXAPU_ERROR("XmaContext {}: There are no bits to copy!", id());
    SwapInputBuffer(data);
    return;
  }

  if (packet_info.isLastFrameInPacket()) {
    if (stream.BitsRemaining() < packet_info.current_frame_size_) {
      const uint8_t* next_packet =
          GetNextPacket(data, next_packet_index, current_input_packet_count);
      if (!next_packet) {
        if (next_packet_index >= current_input_packet_count &&
            !data->IsInputBufferValid(data->current_buffer ^ 1)) {
          return;
        }
        data->error_status = 4;
        return;
      }
      std::memcpy(input_buffer_.data() + kBytesPerPacketData, next_packet + kBytesPerPacketHeader,
                  kBytesPerPacketData);
    }
  }

  std::memcpy(input_buffer_.data(), packet + kBytesPerPacketHeader, kBytesPerPacketData);

  stream = BitStream(input_buffer_.data(), (kBitsPerPacket - kBitsPerPacketHeader) * 2);
  stream.SetOffset(relative_offset - kBitsPerPacketHeader);

  xma_frame_.fill(0);

  const uint32_t padding_start =
      static_cast<uint8_t>(stream.Copy(xma_frame_.data() + 1, packet_info.current_frame_size_));

  raw_frame_.fill(0);

  const int prepared = PrepareDecoder(data->sample_rate, bool(data->is_stereo));
  if (prepared < 0) {
    data->error_status = 4;
    return;
  }
  if (prepared > 0) {
    // A fresh decoder: no frame before this one, nothing to catch up (the frame cache's key).
    previous_frame_hash_ = 0;
    decoder_behind_ = false;
  }
  PreparePacket(packet_info.current_frame_size_, padding_start);
  const bool decoded = DecodeCached(data);  // the frame cache, or DecodePacket and ConvertFrame
  if (decoded) {
    // Diagnostic (audio_dump_xma_s): the whole frame as it comes out of FFmpeg.
    DumpXma(id(), 0, GetSampleRate(data->sample_rate), data->is_stereo ? 2 : 1, raw_frame_.data(),
              size_t(kBytesPerFrameChannel) << data->is_stereo);

    // A game drops the samples it does not want at the start of each sound in its own code.
    // Deliver the decoder's whole block: shifting it by 192 samples and holding
    // its tail until the next frame also dropped a final block of 512. The
    // consumer ran out of input and waited forever.
    current_frame_remaining_subframes_ = 4 << data->is_stereo;
    loop_frame_output_limit_ =
        is_loop_end_frame ? static_cast<uint8_t>((data->loop_subframe_end + 1) << data->is_stereo)
                          : 0;
    const uint8_t decoded_start_skip =
        loop_start_skip_pending_ ? static_cast<uint8_t>(data->loop_subframe_skip << data->is_stereo)
                                 : 0;
    loop_start_skip_pending_ = false;
    current_frame_remaining_subframes_ -=
        std::min(decoded_start_skip, current_frame_remaining_subframes_);

    // Decode position start: skip the leading subframes the game asked for, then clear the count.
    if (data->output_buffer_padding) {
      const uint8_t skip = data->output_buffer_padding << data->is_stereo;
      if (skip < current_frame_remaining_subframes_) {
        REXAPU_INFO("XmaContext {}: decode position, skipping {} leading subframes", id(), skip);
        current_frame_remaining_subframes_ -= skip;
      }
      data->output_buffer_padding = 0;
    }
  }

  // Compute where to go next.
  if (is_loop_end_frame) {
    UpdateLoopStatus(data);
    return;
  }

  if (!packet_info.isLastFrameInPacket()) {
    const uint32_t next_frame_offset =
        (data->input_buffer_read_offset + bits_to_copy) % kBitsPerPacket;
    data->input_buffer_read_offset = (packet_index * kBitsPerPacket) + next_frame_offset;
    return;
  }

  AdvanceInputPacket(data, next_packet_index);
}

void XmaContext::ConvertFrame(const uint8_t** samples, bool is_two_channel,
                              uint8_t* output_buffer) {
  // Loop through every sample, convert and drop it into the output array.
  // If more than one channel, we need to interleave the samples from each
  // channel next to each other. Always saturate because FFmpeg output is
  // not limited to [-1, 1] (for example 1.095 as seen in 5454082B).
  constexpr float scale = (1 << 15) - 1;
  auto out = reinterpret_cast<int16_t*>(output_buffer);

  // For testing of vectorized versions, stereo audio is common in 4D5307E6,
  // since the first menu frame; the intro cutscene also has more than 2
  // channels.
#if REX_ARCH_AMD64
  static_assert(kSamplesPerFrame % 8 == 0);
  const auto in_channel_0 = reinterpret_cast<const float*>(samples[0]);
  const __m128 scale_mm = _mm_set1_ps(scale);
  if (is_two_channel) {
    const auto in_channel_1 = reinterpret_cast<const float*>(samples[1]);
    const __m128i shufmask = _mm_set_epi8(14, 15, 6, 7, 12, 13, 4, 5, 10, 11, 2, 3, 8, 9, 0, 1);
    for (uint32_t i = 0; i < kSamplesPerFrame; i += 4) {
      // Load 8 samples, 4 for each channel.
      __m128 in_mm0 = _mm_loadu_ps(&in_channel_0[i]);
      __m128 in_mm1 = _mm_loadu_ps(&in_channel_1[i]);
      // Rescale.
      in_mm0 = _mm_mul_ps(in_mm0, scale_mm);
      in_mm1 = _mm_mul_ps(in_mm1, scale_mm);
      // Cast to int32.
      __m128i out_mm0 = _mm_cvtps_epi32(in_mm0);
      __m128i out_mm1 = _mm_cvtps_epi32(in_mm1);
      // Saturated cast and pack to int16.
      __m128i out_mm = _mm_packs_epi32(out_mm0, out_mm1);
      // Interleave channels and byte swap.
      out_mm = _mm_shuffle_epi8(out_mm, shufmask);
      // Store, as [out + i * 4] movdqu.
      _mm_storeu_si128(reinterpret_cast<__m128i*>(&out[i * 2]), out_mm);
    }
  } else {
    const __m128i shufmask = _mm_set_epi8(14, 15, 12, 13, 10, 11, 8, 9, 6, 7, 4, 5, 2, 3, 0, 1);
    for (uint32_t i = 0; i < kSamplesPerFrame; i += 8) {
      // Load 8 samples, as [in_channel_0 + i * 4] and
      // [in_channel_0 + i * 4 + 16] movups.
      __m128 in_mm0 = _mm_loadu_ps(&in_channel_0[i]);
      __m128 in_mm1 = _mm_loadu_ps(&in_channel_0[i + 4]);
      // Rescale.
      in_mm0 = _mm_mul_ps(in_mm0, scale_mm);
      in_mm1 = _mm_mul_ps(in_mm1, scale_mm);
      // Cast to int32.
      __m128i out_mm0 = _mm_cvtps_epi32(in_mm0);
      __m128i out_mm1 = _mm_cvtps_epi32(in_mm1);
      // Saturated cast and pack to int16.
      __m128i out_mm = _mm_packs_epi32(out_mm0, out_mm1);
      // Byte swap.
      out_mm = _mm_shuffle_epi8(out_mm, shufmask);
      // Store, as [out + i * 2] movdqu.
      _mm_storeu_si128(reinterpret_cast<__m128i*>(&out[i]), out_mm);
    }
  }
#else
  uint32_t o = 0;
  for (uint32_t i = 0; i < kSamplesPerFrame; i++) {
    for (uint32_t j = 0; j <= uint32_t(is_two_channel); j++) {
      // Select the appropriate array based on the current channel.
      auto in = reinterpret_cast<const float*>(samples[j]);

      // Raw samples sometimes aren't within [-1, 1]
      float scaled_sample = rex::clamp_float(in[i], -1.0f, 1.0f) * scale;

      // Round like _mm_cvtps_epi32 on x86 (xenia-canary 2372a5410); truncating
      // biased every sample toward zero on ARM.
      auto sample = static_cast<int16_t>(std::lrintf(scaled_sample));
      out[o++] = rex::byte_swap(sample);
    }
  }
#endif
}

}  // namespace rex::audio
