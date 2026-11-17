// SPDX-License-Identifier: GPL-3.0-or-later
#include "bo2zm_native_module.h"

#include <array>
#include <memory>

#include <rex/driver_module.h>
#include <rex/driver_registration.h>
#include <rex/gpu/graphics_system.h>
#include <rex/module.h>

#include "bo2zm_native_system.h"

namespace bo2zm::native {
namespace {
std::unique_ptr<rex::IDriver> CreateDriver() { return CreateGraphicsSystem(); }
constexpr std::array<rex::InterfaceType, 1> kRequired = {rex::gpu::IGraphicsSystem::kInterface};
constexpr std::array<rex::DriverRegistration, 1> kDrivers = {rex::DriverRegistration{
    rex::DriverType::kGraphics, "bo2native", rex::GraphicsFamily::kXenos, &CreateDriver}};

class NativeModule final : public rex::IModule, public rex::IDriverModule {
 public:
  REX_MODULE_SERVES(::rex::IDriverModule)
  std::string_view GetName() const override { return "bo2zm-native"; }
  std::span<const rex::InterfaceType> GetRequiredInterfaces() const override { return kRequired; }
  [[nodiscard]] rex::Result<void> Connect(rex::IInterfaceFactory&) override { return {}; }
  void Disconnect() override {}
  [[nodiscard]] rex::Result<void> Init() override { return {}; }
  void Shutdown() override {}
  rex::IInterface* QueryInterface(std::string_view name, rex::u32 version) override {
    return rex::FindModuleInterface<rex::IDriverModule>(*this, name, version);
  }
  std::span<const rex::DriverRegistration> GetDrivers() const override { return kDrivers; }
};
}  // namespace

rex::IModule* DriverModule() {
  static NativeModule module;
  return &module;
}
}  // namespace bo2zm::native
