// SPDX-License-Identifier: GPL-3.0-or-later
// The native renderer as an in-process graphics driver module (driver name "bo2native").
#pragma once
namespace rex {
class IModule;
}
namespace bo2zm::native {
// The module the app registers (OnRegisterModules); selected with gpu.driver = "bo2native".
rex::IModule* DriverModule();
}  // namespace bo2zm::native
