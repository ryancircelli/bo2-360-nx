/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2020 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 *
 * @modified    Tom Clay, 2026 - Adapted for ReXGlue runtime
 */

#include <rex/audio/flags.h>
#include <rex/audio/sdl/sdl_audio_driver.h>
#include <rex/audio/sdl/sdl_audio_system.h>

#include <chrono>
#include <thread>

namespace rex::audio::sdl {

namespace {
// Stand-in when no SDL audio device opens (no output device, driver missing): the guest still gets its client and
// frame pacing, the samples are dropped. Failing the client registration instead crashes titles that don't check it.
class NullAudioDriver final : public AudioDriver {
 public:
  NullAudioDriver(memory::Memory* memory, rex::thread::Semaphore* semaphore)
      : AudioDriver(memory), semaphore_(semaphore) {}
  void SubmitFrame(uint32_t) override {
    std::this_thread::sleep_for(std::chrono::microseconds(5333));  // one 256-sample frame at 48 kHz
    semaphore_->Release(1, nullptr);
  }

 private:
  rex::thread::Semaphore* semaphore_;
};
}  // namespace

std::unique_ptr<AudioSystem> SDLAudioSystem::Create(
    runtime::FunctionDispatcher* function_dispatcher) {
  return std::make_unique<SDLAudioSystem>(function_dispatcher);
}

SDLAudioSystem::SDLAudioSystem(runtime::FunctionDispatcher* function_dispatcher)
    : AudioSystem(function_dispatcher) {}

SDLAudioSystem::~SDLAudioSystem() {}

void SDLAudioSystem::Initialize() {
  AudioSystem::Initialize();
}

X_STATUS SDLAudioSystem::CreateDriver(size_t index,
                                      rex::thread::Semaphore* semaphore, AudioDriver** out_driver) {
  assert_not_null(out_driver);
  auto driver = new SDLAudioDriver(memory_, semaphore);
  if (!driver->Initialize()) {
    driver->Shutdown();
    delete driver;
    REXAPU_WARN("[audio] no SDL audio device: client {} gets a silent driver", index);
    *out_driver = new NullAudioDriver(memory_, semaphore);
    return X_STATUS_SUCCESS;
  }

  *out_driver = driver;
  return X_STATUS_SUCCESS;
}

void SDLAudioSystem::DestroyDriver(AudioDriver* driver) {
  assert_not_null(driver);
  if (auto* sdldriver = dynamic_cast<SDLAudioDriver*>(driver)) {
    sdldriver->Shutdown();
    delete sdldriver;
  } else {
    delete driver;
  }
}

}  // namespace rex::audio::sdl
