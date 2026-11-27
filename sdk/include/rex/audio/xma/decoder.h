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

#pragma once

#include <algorithm>
#include <vector>

#include <atomic>
#include <mutex>
#include <queue>

#include <rex/audio/xma/context.h>
#include <rex/audio/xma/register_file.h>
#include <rex/bit.h>
#include <rex/kernel.h>
#include <rex/system/xthread.h>

namespace rex::runtime {
class FunctionDispatcher;
}  // namespace rex::runtime

namespace rex::audio {

struct XMA_CONTEXT_DATA;

class XmaDecoder {
 public:
  explicit XmaDecoder(runtime::FunctionDispatcher* function_dispatcher);
  ~XmaDecoder();

  memory::Memory* memory() const { return memory_; }
  runtime::FunctionDispatcher* function_dispatcher() const { return function_dispatcher_; }

  X_STATUS Setup(system::KernelState* kernel_state);
  void Shutdown();

  uint32_t context_array_ptr() const { return register_file_[XmaRegister::ContextArrayAddress]; }

  uint32_t AllocateContext();
  void ReleaseContext(uint32_t guest_ptr);
  bool BlockOnContext(uint32_t guest_ptr, bool poll);

  uint32_t ReadRegister(uint32_t addr);
  void WriteRegister(uint32_t addr, uint32_t value);

  // Savepoint (same-process save/load): register file, context allocation bitmap and the
  // per-context host decode state. Only while every other thread is parked.
  struct SavepointState {
    std::vector<uint32_t> registers;
    std::vector<uint64_t> bitmap;
    std::vector<XmaContext::SavepointState> contexts;
  };
  void CaptureSavepoint(SavepointState& s) const {
    s.registers.assign(register_file_.values, register_file_.values + XmaRegisterFile::kRegisterCount);
    s.bitmap = context_bitmap_.data();
    s.contexts.resize(kContextCount);
    for (uint32_t i = 0; i < kContextCount; ++i) contexts_[i].CaptureSavepoint(s.contexts[i]);
  }
  // Returns the number of contexts whose allocation differed from the savepoint.
  uint32_t RewindSavepoint(const SavepointState& s) {
    std::copy(s.registers.begin(), s.registers.end(), register_file_.values);
    auto& bits = context_bitmap_.data();
    std::copy(s.bitmap.begin(), s.bitmap.end(), bits.begin());
    uint32_t changed = 0;
    for (uint32_t i = 0; i < kContextCount; ++i) {
      if (contexts_[i].is_allocated() != s.contexts[i].allocated) ++changed;
      contexts_[i].RewindSavepoint(s.contexts[i]);
    }
    return changed;
  }

  bool is_paused() const { return paused_; }
  void Pause();
  void Resume();

 protected:
  int GetContextId(uint32_t guest_ptr);

 private:
  void WorkerThreadMain();

  static uint32_t MMIOReadRegisterThunk(void* ppc_context, XmaDecoder* as, uint32_t addr) {
    return as->ReadRegister(addr);
  }
  static void MMIOWriteRegisterThunk(void* ppc_context, XmaDecoder* as, uint32_t addr,
                                     uint32_t value) {
    as->WriteRegister(addr, value);
  }

 protected:
  memory::Memory* memory_ = nullptr;
  runtime::FunctionDispatcher* function_dispatcher_ = nullptr;

  std::atomic<bool> worker_running_ = {false};
  std::atomic<bool> worker_loop_running_ = {false};   // WorkerThreadMain is inside its loop: only then can it park for Pause()
  system::object_ref<system::XHostThread> worker_thread_;
  std::unique_ptr<rex::thread::Event> work_event_ = nullptr;

  std::atomic<bool> paused_ = false;
  rex::thread::Fence pause_fence_;   // Signaled when worker paused.
  rex::thread::Fence resume_fence_;  // Signaled when resume requested.

  XmaRegisterFile register_file_;

  static const uint32_t kContextCount = 320;
  XmaContext contexts_[kContextCount];
  bit::BitMap context_bitmap_;

  uint32_t context_data_first_ptr_ = 0;
  uint32_t context_data_last_ptr_ = 0;
};

}  // namespace rex::audio
