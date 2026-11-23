#pragma once
/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2013 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 *
 * @modified    Tom Clay, 2026 - Adapted for ReXGlue runtime
 */

#include <string>

#include <rex/ppc/function.h>
#include <rex/system/export_resolver.h>
#include <rex/system/kernel_module.h>
#include <rex/system/kernel_state.h>

namespace rex {
namespace kernel {
namespace xam {

bool xeXamIsUIActive();

/// The system Guide opened or closed (the app's own Guide overlay): counts as
/// a shown system UI for XamIsUIActive and broadcasts XN_SYS_UI (true on open,
/// false on close) to the game's notification listeners, as the console does,
/// so games pause themselves where the original would. Repeats are ignored.
void xeXamSetGuideOpen(system::KernelState* kernel_state, bool open);

class XamModule : public system::KernelModule {
 public:
  XamModule(Runtime* emulator, system::KernelState* kernel_state);
  virtual ~XamModule();

  static void RegisterExportTable(rex::runtime::ExportResolver* export_resolver);

  struct LoaderData {
    bool launch_data_present = false;
    std::vector<uint8_t> launch_data;
    uint32_t launch_flags = 0;
    std::string launch_path;  // Full path to next xex
  };

  const LoaderData& loader_data() const { return loader_data_; }
  LoaderData& loader_data() { return loader_data_; }

 private:
  LoaderData loader_data_;
};

}  // namespace xam
}  // namespace kernel
}  // namespace rex
