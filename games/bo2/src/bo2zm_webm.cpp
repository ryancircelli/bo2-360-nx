// The game's WebM video decoder (libvpx's VP8 decoder in TU18 default_mp.xex), which plays the DLC maps' loading
// videos (zm_*_load.webm). Their frames used to come out wrong on the PC and the Switch alike, with either renderer:
// the code generator's vpkuwus/vpkuhus wrote the destination while still reading it. The test switches here are the
// ones that found it, off for play: the decoder on one thread, only its first frames, no loop filter, and its IDCT,
// DC-only add and six-tap predictors checked against libvpx's reference.
//
// Recompiled, libvpx is too slow on the Switch (its VMX128 code is emulated): Buried's video played at ~18 FPS and
// held the game's own frame rate there. With bo2zm_native_vp8 the SDK's FFmpeg decodes the frames instead (VP8 with
// NEON on the Switch, config_bo2zm_vp8.h). libvpx still decodes each video's first frame, which allocates its frame
// buffers; after that, every frame FFmpeg decodes is copied into libvpx's frame_to_show buffer and libvpx is skipped,
// so vp8_decode (sub_824C3DD0) and the game's player see what libvpx would have given them. The player
// (sub_824B0FA8) decodes, gets the frame and copies its planes into the video texture on one thread, so writing the
// same buffer every frame is safe.
//
// sub_824BFC78 is libvpx's vp8_decoder_create_threads: it allocates pbi->de_thread_data, pbi->mb_row_di,
// pbi->h_event_start_decoding and pbi->h_decoding_thread (its error strings at 0x8205D69C-0x8205D718). r3 = pbi:
// max_threads at +25404 (clamped to 8 here), the multithreaded flag at +25400 (set when more than one thread is
// asked for), the worker count at +25412 (max_threads - 1). The game asks for 4.
//
// sub_824BC388 is libvpx's vp8dx_receive_compressed_data, the only caller of vp8_decode_frame (sub_824B88F0, its
// strings "Invalid frame width" etc. at 0x8205D464-0x8205D4D8): one call per frame. After decoding and swapping the
// frame buffers (sub_824BC188) it runs sub_8264C5F0(cm, xd) when cm+17552 (pc->filter_level) is not zero:
// vp8_loop_filter_frame. Its arguments: r3 = pbi, r4 = the frame's size, r5 = its data, r6 = the time stamp (64 bits).
// At its end: pbi+25504 (ready_for_new_data) = 0, pbi+25496 (last_time_stamp) = the time stamp; its VP8_COMMON starts
// at pbi+4368 (the error struct, its first member).
//
// sub_824BC690 is vp8dx_get_raw_frame(pbi, sd, ...): none when ready_for_new_data is 1 or show_frame (pbi+18464) is
// 0; else ready_for_new_data = 1 and the 60-byte YV12_BUFFER_CONFIG at frame_to_show (pointer at pbi+18120) is copied
// to sd, with Width and Height from pbi+18096 and +18100. The fields, from yuvconfig2image at the end of sub_824C3DD0:
// y_width +0, y_height +4, y_stride +8, uv_stride +20, y_buffer +24, u_buffer +28, v_buffer +32.
//
// sub_824BC040 is vp8dx_remove_decompressor(pbi), called by vp8_destroy (sub_824C3C98, ctx+264 = pbi) and by
// sub_824BC740 when creating a decoder fails.

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

#include <rex/cvar.h>
#include <rex/hook.h>
#include <rex/logging.h>

extern "C" {
#include <libavcodec/avcodec.h>
}

REXCVAR_DEFINE_BOOL(bo2zm_test_vp8_one_thread, false, "BO2",
                    "Test: the game's VP8 decoder (WebM videos) decodes on its own thread only, with no workers. "
                    "false = as many threads as the game asks for")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
REXCVAR_DEFINE_INT32(bo2zm_test_vp8_frames, 0, "BO2",
                     "Test: the game's VP8 decoder decodes only its first N frames of the session; later frames return "
                     "at once with nothing decoded, so the video's textures keep what was decoded. 0 = every frame")
    .range(0, 1000000)
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);

REXCVAR_DEFINE_BOOL(bo2zm_native_vp8, true, "BO2",
                    "The game's WebM videos (the DLC maps' loading videos) are decoded by FFmpeg's VP8 decoder (NEON on "
                    "the Switch) instead of the game's own libvpx, which decodes only each video's first frame. "
                    "false = the game's libvpx for every frame")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
REXCVAR_DEFINE_INT32(bo2zm_native_vp8_threads, 1, "BO2",
                     "With bo2zm_native_vp8: FFmpeg's slice threads per video. 1 = all on the game's video thread")
    .range(1, 4)
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
REXCVAR_DEFINE_BOOL(bo2zm_native_vp8_check, false, "BO2",
                    "Test, with bo2zm_native_vp8: libvpx still decodes and shows every frame, FFmpeg decodes the same "
                    "frames and each one is compared with libvpx's (counts and mismatches logged, [webm])")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);

namespace {
// VP8D_COMP (pbi) and YV12_BUFFER_CONFIG offsets: see the top of this file.
constexpr uint32_t kPbiFrameToShow = 18120, kPbiShowFrame = 18464, kPbiLastTimeStamp = 25496,
                   kPbiReadyForNewData = 25504;
constexpr uint32_t kYv12Width = 0, kYv12Height = 4, kYv12YStride = 8, kYv12UvStride = 20, kYv12Y = 24, kYv12U = 28,
                   kYv12V = 32;

uint32_t Load32(const uint8_t* p) {
  uint32_t v;
  std::memcpy(&v, p, 4);
  return __builtin_bswap32(v);
}
void Store32(uint8_t* p, uint32_t v) {
  v = __builtin_bswap32(v);
  std::memcpy(p, &v, 4);
}
void Store64(uint8_t* p, uint64_t v) {
  v = __builtin_bswap64(v);
  std::memcpy(p, &v, 8);
}

// One video: its FFmpeg decoder, keyed by libvpx's pbi.
struct Vp8Video {
  AVCodecContext* codec = nullptr;
  AVPacket* packet = nullptr;
  AVFrame* frame = nullptr;
  std::vector<uint8_t> data;  // the frame's bytes, then FFmpeg's zeroed padding
  uint64_t frames = 0, shown = 0, failed = 0, checked = 0, differ = 0;
  double decode_ms = 0, decode_max_ms = 0, copy_ms = 0;

  ~Vp8Video() {
    avcodec_free_context(&codec);
    av_packet_free(&packet);
    av_frame_free(&frame);
  }
};

std::mutex g_videos_mutex;
std::unordered_map<uint32_t, std::unique_ptr<Vp8Video>> g_videos;

Vp8Video* FindVideo(uint32_t pbi) {
  std::lock_guard<std::mutex> lock(g_videos_mutex);
  auto it = g_videos.find(pbi);
  if (it != g_videos.end()) {
    return it->second.get();
  }
  const AVCodec* vp8 = avcodec_find_decoder(AV_CODEC_ID_VP8);
  auto video = std::make_unique<Vp8Video>();
  if (vp8) {
    video->codec = avcodec_alloc_context3(vp8);
    video->packet = av_packet_alloc();
    video->frame = av_frame_alloc();
  }
  int err = -1;
  if (video->codec && video->packet && video->frame) {
    video->codec->thread_count = REXCVAR_GET(bo2zm_native_vp8_threads);
    video->codec->thread_type = FF_THREAD_SLICE;  // frame threads would hand each frame back later
    err = avcodec_open2(video->codec, vp8, nullptr);
  }
  if (err < 0) {
    REXLOG_ERROR("[webm] FFmpeg's VP8 decoder did not open ({}): this video stays on the game's libvpx", err);
    g_videos[pbi] = nullptr;
    return nullptr;
  }
  REXLOG_INFO("[webm] video {:08X}: decoded by FFmpeg's VP8 decoder, {} thread(s){}", pbi,
              video->codec->thread_count, REXCVAR_GET(bo2zm_native_vp8_check) ? " (checked against libvpx)" : "");
  return (g_videos[pbi] = std::move(video)).get();
}

void LogVideo(uint32_t pbi, const Vp8Video& v, const char* when) {
  const double frames = double(std::max<uint64_t>(v.frames, 1));
  REXLOG_INFO("[webm] video {:08X} {}: {} frames ({} shown, {} failed), FFmpeg {:.2f} ms a frame (worst {:.2f}), "
              "copy {:.2f} ms; checked against libvpx {} ({} different)",
              pbi, when, v.frames, v.shown, v.failed, v.decode_ms / frames, v.decode_max_ms, v.copy_ms / frames,
              v.checked, v.differ);
}

// One frame in: 1 with v.frame holding the picture, 0 for a frame VP8 does not show (an alt-ref), < 0 on an error.
int Decode(Vp8Video& v, const uint8_t* src, uint32_t size) {
  v.data.assign(src, src + size);
  v.data.resize(size_t(size) + AV_INPUT_BUFFER_PADDING_SIZE, 0);
  v.packet->data = v.data.data();
  v.packet->size = int(size);
  int err = avcodec_send_packet(v.codec, v.packet);
  if (err < 0) {
    return err;
  }
  err = avcodec_receive_frame(v.codec, v.frame);
  return err == AVERROR(EAGAIN) ? 0 : err < 0 ? err : 1;
}

struct Planes {
  uint8_t* plane[3];
  uint32_t stride[3];
  int width, height;
};

Planes LibvpxFrame(uint8_t* base, uint32_t yv12) {
  const uint8_t* const p = base + yv12;
  const uint32_t uv_stride = Load32(p + kYv12UvStride);
  return {{base + Load32(p + kYv12Y), base + Load32(p + kYv12U), base + Load32(p + kYv12V)},
          {Load32(p + kYv12YStride), uv_stride, uv_stride},
          int(Load32(p + kYv12Width)),
          int(Load32(p + kYv12Height))};
}

// Calls fn(plane, row, ffmpeg row, libvpx row, bytes) for every row both frames have.
template <typename Fn>
void ForEachRow(const AVFrame& f, const Planes& to, Fn&& fn) {
  for (int i = 0; i < 3; ++i) {
    const int shift = i ? 1 : 0;
    const int w = (std::min(f.width, to.width) + shift) >> shift;
    const int h = (std::min(f.height, to.height) + shift) >> shift;
    for (int r = 0; r < h; ++r) {
      fn(i, r, f.data[i] + ptrdiff_t(r) * f.linesize[i], to.plane[i] + size_t(r) * to.stride[i], size_t(w));
    }
  }
}

void Check(uint32_t pbi, Vp8Video& v, const Planes& libvpx) {
  uint64_t bytes = 0;
  int first_plane = -1, first_row = -1;
  ForEachRow(*v.frame, libvpx, [&](int plane, int row, const uint8_t* ours, const uint8_t* theirs, size_t n) {
    for (size_t c = 0; c < n; ++c) {
      if (ours[c] != theirs[c]) {
        ++bytes;
        if (first_plane < 0) {
          first_plane = plane;
          first_row = row;
        }
      }
    }
  });
  ++v.checked;
  if (bytes && ++v.differ <= 8) {
    REXLOG_WARN("[webm] video {:08X} frame {}: FFmpeg and libvpx differ in {} bytes, the first in plane {} row {}", pbi,
                v.frames, bytes, first_plane, first_row);
  }
}
}  // namespace

REX_EXTERN(__imp__sub_824BC388);
REX_HOOK_RAW(sub_824BC388) {
  static const int32_t limit = REXCVAR_GET(bo2zm_test_vp8_frames);
  static int32_t frames = 0;
  if (limit > 0 && frames >= limit) {
    ctx.r3.u64 = 0;
    return;
  }
  if (limit > 0 && ++frames == limit) {
    REXLOG_INFO("[webm] VP8 decoder: frame {} decoded, the later ones are skipped (bo2zm_test_vp8_frames)", frames);
  }
  static const bool native = REXCVAR_GET(bo2zm_native_vp8);
  Vp8Video* const v = native ? FindVideo(ctx.r3.u32) : nullptr;
  if (!v) {
    __imp__sub_824BC388(ctx, base);
    return;
  }
  static const bool check = REXCVAR_GET(bo2zm_native_vp8_check);
  const uint32_t pbi = ctx.r3.u32, size = ctx.r4.u32, src = ctx.r5.u32;
  const uint64_t time_stamp = ctx.r6.u64;
  uint8_t* const p = base + pbi;
  const auto start = std::chrono::steady_clock::now();
  if (check || Load32(p + kPbiFrameToShow) == 0) {
    // libvpx decodes this frame (each video's first: it allocates libvpx's frame buffers); FFmpeg decodes it too,
    // to have its own reference frames.
    __imp__sub_824BC388(ctx, base);
    const auto decoded = std::chrono::steady_clock::now();
    const int got = Decode(*v, base + src, size);
    v->decode_ms += std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - decoded).count();
    ++v->frames;
    if (got == 1) {
      ++v->shown;
      if (check && ctx.r3.u32 == 0 && Load32(p + kPbiShowFrame) != 0 && Load32(p + kPbiFrameToShow) != 0) {
        Check(pbi, *v, LibvpxFrame(base, Load32(p + kPbiFrameToShow)));
      }
    } else if (got < 0) {
      ++v->failed;
    }
    if (v->frames % 300 == 0) {
      LogVideo(pbi, *v, "so far");
    }
    return;
  }
  const int got = Decode(*v, base + src, size);
  const auto decoded = std::chrono::steady_clock::now();
  const double ms = std::chrono::duration<double, std::milli>(decoded - start).count();
  v->decode_ms += ms;
  v->decode_max_ms = std::max(v->decode_max_ms, ms);
  ++v->frames;
  if (got == 1) {
    ++v->shown;
    ForEachRow(*v->frame, LibvpxFrame(base, Load32(p + kPbiFrameToShow)),
               [](int, int, const uint8_t* from, uint8_t* to, size_t n) { std::memcpy(to, from, n); });
    v->copy_ms += std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - decoded).count();
  } else if (got < 0 && ++v->failed <= 8) {
    REXLOG_WARN("[webm] video {:08X} frame {}: FFmpeg's VP8 decoder failed ({}), no picture", pbi, v->frames, got);
  }
  // As libvpx leaves it: a new frame is ready, shown or not.
  Store32(p + kPbiShowFrame, got == 1 ? 1 : 0);
  Store32(p + kPbiReadyForNewData, 0);
  Store64(p + kPbiLastTimeStamp, time_stamp);
  ctx.r3.u64 = 0;
  if (v->frames % 300 == 0) {
    LogVideo(pbi, *v, "so far");
  }
}

REX_EXTERN(__imp__sub_824BC040);
REX_HOOK_RAW(sub_824BC040) {
  {
    std::lock_guard<std::mutex> lock(g_videos_mutex);
    auto it = g_videos.find(ctx.r3.u32);
    if (it != g_videos.end()) {
      if (it->second) {
        LogVideo(it->first, *it->second, "closed");
      }
      g_videos.erase(it);
    }
  }
  __imp__sub_824BC040(ctx, base);
}

REXCVAR_DEFINE_BOOL(bo2zm_test_vp8_no_loop_filter, false, "BO2",
                    "Test: the game's VP8 decoder skips its loop filter (vp8_loop_filter_frame, sub_8264C5F0). false = "
                    "the game's own")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);

REX_EXTERN(__imp__sub_8264C5F0);
REX_HOOK_RAW(sub_8264C5F0) {
  if (REXCVAR_GET(bo2zm_test_vp8_no_loop_filter)) {
    return;
  }
  __imp__sub_8264C5F0(ctx, base);
}

REXCVAR_DEFINE_BOOL(bo2zm_test_vp8_check_idct, false, "BO2",
                    "Test: every call of the game's 4x4 inverse DCT (sub_8261EB58, VMX128 floating point) is compared "
                    "with libvpx's integer reference (vp8_short_idct4x4llm_c), and mismatches are logged ([webm])")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);

namespace {
int16_t LoadS16(const uint8_t* p) {
  return int16_t(uint16_t(p[0]) << 8 | p[1]);
}

// libvpx's vp8_short_idct4x4llm_c: 16 coefficients in, a 4x4 block out with shorts pitch / 2 apart (the first
// pass's rows are kept as shorts, as libvpx keeps them in the output).
void ReferenceIdct(const int16_t in[16], int16_t out[16]) {
  constexpr int kCos = 20091, kSin = 35468;
  int16_t mid[16];
  for (int i = 0; i < 4; ++i) {
    const int a1 = in[i] + in[8 + i], b1 = in[i] - in[8 + i];
    int t1 = (in[4 + i] * kSin) >> 16, t2 = in[12 + i] + ((in[12 + i] * kCos) >> 16);
    const int c1 = t1 - t2;
    t1 = in[4 + i] + ((in[4 + i] * kCos) >> 16);
    t2 = (in[12 + i] * kSin) >> 16;
    const int d1 = t1 + t2;
    mid[i] = int16_t(a1 + d1);
    mid[12 + i] = int16_t(a1 - d1);
    mid[4 + i] = int16_t(b1 + c1);
    mid[8 + i] = int16_t(b1 - c1);
  }
  for (int r = 0; r < 4; ++r) {
    const int16_t* ip = mid + 4 * r;
    const int a1 = ip[0] + ip[2], b1 = ip[0] - ip[2];
    int t1 = (ip[1] * kSin) >> 16, t2 = ip[3] + ((ip[3] * kCos) >> 16);
    const int c1 = t1 - t2;
    t1 = ip[1] + ((ip[1] * kCos) >> 16);
    t2 = (ip[3] * kSin) >> 16;
    const int d1 = t1 + t2;
    out[4 * r + 0] = int16_t((a1 + d1 + 4) >> 3);
    out[4 * r + 3] = int16_t((a1 - d1 + 4) >> 3);
    out[4 * r + 1] = int16_t((b1 + c1 + 4) >> 3);
    out[4 * r + 2] = int16_t((b1 - c1 + 4) >> 3);
  }
}

std::atomic<uint64_t> g_idct_calls{0}, g_idct_bad{0};
}  // namespace

namespace {
std::atomic<uint64_t> g_dc_calls{0}, g_dc_bad{0};
}  // namespace

// sub_8261EB58: libvpx's 4x4 inverse DCT (vp8_short_idct4x4llm, reached through the thunk sub_8264B9C0) in VMX128
// floating point: r3 = 16 coefficients, r4 = output, r5 = its pitch in bytes. The output is two 16-byte stores when
// r4 is 16-byte aligned and r5 is 8, else stores left/right (stvlx/stvrx) row by row.
REX_EXTERN(__imp__sub_8261EB58);
REX_HOOK_RAW(sub_8261EB58) {
  static const bool check = REXCVAR_GET(bo2zm_test_vp8_check_idct);
  if (!check) {
    __imp__sub_8261EB58(ctx, base);
    return;
  }
  const uint32_t in_addr = ctx.r3.u32, out_addr = ctx.r4.u32, pitch = ctx.r5.u32;
  int16_t in[16], want[16], got[16];
  for (int i = 0; i < 16; ++i) {
    in[i] = LoadS16(base + in_addr + 2 * i);
  }
  __imp__sub_8261EB58(ctx, base);
  ReferenceIdct(in, want);
  for (int r = 0; r < 4; ++r) {
    for (int c = 0; c < 4; ++c) {
      got[4 * r + c] = LoadS16(base + out_addr + r * pitch + 2 * c);
    }
  }
  const uint64_t n = g_idct_calls.fetch_add(1, std::memory_order_relaxed) + 1;
  if (std::memcmp(got, want, sizeof(got)) != 0) {
    const uint64_t bad = g_idct_bad.fetch_add(1, std::memory_order_relaxed) + 1;
    if (bad <= 12) {
      std::string text;
      for (int i = 0; i < 16; ++i) {
        text += std::to_string(in[i]) + (i == 15 ? "" : ",");
      }
      text += " | got ";
      for (int i = 0; i < 16; ++i) {
        text += std::to_string(got[i]) + (i == 15 ? "" : ",");
      }
      text += " | want ";
      for (int i = 0; i < 16; ++i) {
        text += std::to_string(want[i]) + (i == 15 ? "" : ",");
      }
      REXLOG_WARN("[webm] IDCT mismatch {} of {} calls: out {:08X} (16-aligned {}), pitch {}: in {}", bad, n, out_addr,
                  (out_addr & 15) == 0 ? "yes" : "no", pitch, text);
    }
  }
  if (n % 20000 == 0) {
    REXLOG_INFO("[webm] IDCT checked: {} calls, {} different", n, g_idct_bad.load(std::memory_order_relaxed));
  }
}

// sub_8261EAB8: vp8_dc_only_idct_add in VMX128: r3 = the DC coefficient (a short), r4 = prediction (pitch r6),
// r5 = destination (stride r7); every one of the 4x4 pixels gets clamp(pred + ((dc + 4) >> 3)). It keeps the value
// below the stack pointer (sth -16(r1)) and reads it back with lvlx/lvrx.
REX_EXTERN(__imp__sub_8261EAB8);
REX_HOOK_RAW(sub_8261EAB8) {
  static const bool check = REXCVAR_GET(bo2zm_test_vp8_check_idct);
  if (!check) {
    __imp__sub_8261EAB8(ctx, base);
    return;
  }
  const int dc = int16_t(ctx.r3.u32 & 0xFFFF);
  const uint32_t pred = ctx.r4.u32, dst = ctx.r5.u32, pitch = ctx.r6.u32, stride = ctx.r7.u32, sp = ctx.r1.u32;
  uint8_t want[16], got[16];
  const int a1 = (dc + 4) >> 3;
  for (int r = 0; r < 4; ++r) {
    for (int c = 0; c < 4; ++c) {
      const int v = int(base[pred + r * pitch + c]) + a1;
      want[4 * r + c] = uint8_t(v < 0 ? 0 : v > 255 ? 255 : v);
    }
  }
  __imp__sub_8261EAB8(ctx, base);
  for (int r = 0; r < 4; ++r) {
    for (int c = 0; c < 4; ++c) {
      got[4 * r + c] = base[dst + r * stride + c];
    }
  }
  const uint64_t n = g_dc_calls.fetch_add(1, std::memory_order_relaxed) + 1;
  if (std::memcmp(got, want, 16) != 0) {
    const uint64_t bad = g_dc_bad.fetch_add(1, std::memory_order_relaxed) + 1;
    if (bad <= 12) {
      std::string text;
      for (int i = 0; i < 16; ++i) {
        text += std::to_string(got[i]) + "/" + std::to_string(want[i]) + (i == 15 ? "" : " ");
      }
      REXLOG_WARN("[webm] DC-only add mismatch {} of {}: dc {} (a1 {}), pred {:08X} pitch {}, dst {:08X} stride {}, "
                  "r1 {:08X} (mod 16: {}): got/want {}",
                  bad, n, dc, a1, pred, pitch, dst, stride, sp, sp & 15, text);
    }
  }
  if (n % 20000 == 0) {
    REXLOG_INFO("[webm] DC-only add checked: {} calls, {} different", n, g_dc_bad.load(std::memory_order_relaxed));
  }
}


REXCVAR_DEFINE_BOOL(bo2zm_test_vp8_check_sixtap, false, "BO2",
                    "Test: every call of the VP8 decoder's VMX128 six-tap predictors (sub_82642288 16x16, sub_826467E0 "
                    "8x8) is compared with libvpx's reference filter, and mismatches are logged ([webm])")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);

namespace {
constexpr int kSixtap[8][6] = {{0, 0, 128, 0, 0, 0},   {0, -6, 123, 12, -1, 0}, {2, -11, 108, 36, -8, 1},
                               {0, -9, 93, 50, -6, 0},  {3, -16, 77, 77, -16, 3}, {0, -6, 50, 93, -9, 0},
                               {1, -8, 36, 108, -11, 2}, {0, -1, 12, 123, -6, 0}};

// libvpx's sixtap_predict: a horizontal pass over rows -2..h+2 (kept as ints, clamped), then a vertical one.
void ReferenceSixtap(const uint8_t* base, uint32_t src, int stride, int xoff, int yoff, int w, int h, uint8_t* out) {
  int temp[(16 + 5) * 16];
  const int* hf = kSixtap[xoff & 7];
  const int* vf = kSixtap[yoff & 7];
  for (int r = 0; r < h + 5; ++r) {
    const uint32_t row = src + uint32_t((r - 2) * stride);
    for (int c = 0; c < w; ++c) {
      int t = 64;
      for (int k = 0; k < 6; ++k) {
        t += int(base[row + uint32_t(c + k - 2)]) * hf[k];
      }
      t >>= 7;
      temp[r * w + c] = t < 0 ? 0 : t > 255 ? 255 : t;
    }
  }
  for (int r = 0; r < h; ++r) {
    for (int c = 0; c < w; ++c) {
      int t = 64;
      for (int k = 0; k < 6; ++k) {
        t += temp[(r + k) * w + c] * vf[k];
      }
      t >>= 7;
      out[r * w + c] = uint8_t(t < 0 ? 0 : t > 255 ? 255 : t);
    }
  }
}

std::atomic<uint64_t> g_six_calls{0}, g_six_bad{0};

void CheckSixtap(const char* name, const PPCContext& in, const uint8_t* base, int w, int h) {
  uint8_t want[256];
  ReferenceSixtap(base, in.r3.u32, in.r4.s32, in.r5.s32, in.r6.s32, w, h, want);
  const uint32_t dst = in.r7.u32, pitch = in.r8.u32;
  int wrong = 0, first = -1;
  for (int r = 0; r < h; ++r) {
    for (int c = 0; c < w; ++c) {
      if (base[dst + r * pitch + c] != want[r * w + c]) {
        ++wrong;
        if (first < 0) first = r * w + c;
      }
    }
  }
  const uint64_t n = g_six_calls.fetch_add(1, std::memory_order_relaxed) + 1;
  if (wrong) {
    const uint64_t bad = g_six_bad.fetch_add(1, std::memory_order_relaxed) + 1;
    if (bad <= 16) {
      std::string rows;
      for (int r = 0; r < std::min(h, 6); ++r) {
        rows += " |";
        for (int c = 0; c < w; ++c) {
          rows += " " + std::to_string(base[dst + r * pitch + c]) + "/" + std::to_string(want[r * w + c]);
        }
      }
      REXLOG_WARN("[webm] six-tap mismatch {} of {} in {}: x {} y {}, src {:08X} (mod 16 {}) stride {}, dst {:08X} "
                  "(mod 16 {}) pitch {}: {} pixels wrong, first at ({}, {}); got/want rows{}",
                  bad, n, name, in.r5.s32, in.r6.s32, in.r3.u32, in.r3.u32 & 15, in.r4.s32, dst, dst & 15, pitch,
                  wrong, first / w, first % w, rows);
    }
  }
  if (n % 20000 == 0) {
    REXLOG_INFO("[webm] six-tap checked: {} calls, {} different", n, g_six_bad.load(std::memory_order_relaxed));
  }
}
}  // namespace

REX_EXTERN(__imp__sub_82642288);
REX_HOOK_RAW(sub_82642288) {
  static const bool check = REXCVAR_GET(bo2zm_test_vp8_check_sixtap);
  if (!check) {
    __imp__sub_82642288(ctx, base);
    return;
  }
  const PPCContext in = ctx;
  __imp__sub_82642288(ctx, base);
  CheckSixtap("sub_82642288 (16x16)", in, base, 16, 16);
}

REX_EXTERN(__imp__sub_826467E0);
REX_HOOK_RAW(sub_826467E0) {
  static const bool check = REXCVAR_GET(bo2zm_test_vp8_check_sixtap);
  if (!check) {
    __imp__sub_826467E0(ctx, base);
    return;
  }
  const PPCContext in = ctx;
  __imp__sub_826467E0(ctx, base);
  CheckSixtap("sub_826467E0 (8x8)", in, base, 8, 8);
}

REX_EXTERN(__imp__sub_824BFC78);
REX_HOOK_RAW(sub_824BFC78) {
  constexpr uint32_t kMaxThreads = 25404;
  if (REXCVAR_GET(bo2zm_test_vp8_one_thread)) {
    uint8_t* const p = base + ctx.r3.u32 + kMaxThreads;
    uint32_t asked;
    std::memcpy(&asked, p, 4);
    const uint32_t one = __builtin_bswap32(1);
    std::memcpy(p, &one, 4);
    REXLOG_INFO("[webm] VP8 decoder: one thread (the game asked for {})", __builtin_bswap32(asked));
  }
  __imp__sub_824BFC78(ctx, base);
}
