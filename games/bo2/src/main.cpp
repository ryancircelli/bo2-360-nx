// bo2zm - ReXGlue Recompiled Project
//
// BO2 registers its hooks in bo2zm_register.cpp.
#include "generated/default_mp/bo2zm_init.h"

#include <rex/game_api.h>

#include "native/bo2zm_native_module.h"

namespace {
// GameApp plus the native renderer's in-process graphics driver module (selected with gpu.driver = "bo2native").
class Bo2App final : public ::rex::GameApp {
 public:
  Bo2App() : ::rex::GameApp("bo2zm", kPPCImageConfig) {}

 protected:
  void OnRegisterModules(std::vector<::rex::IModule*>& modules) override {
    ::rex::GameApp::OnRegisterModules(modules);
    modules.push_back(::bo2zm::native::DriverModule());
  }
};
}  // namespace

REX_TITLE_APP(Bo2App)
