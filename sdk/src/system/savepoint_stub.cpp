/**
 * @file        system/savepoint_stub.cpp
 * @brief       Savepoints need Mach thread suspend; every other platform gets no-ops
 */

#include <rex/system/interfaces/graphics.h>
#include <rex/system/savepoint.h>

namespace rex::system {

namespace {
SavepointStats Unsupported() {
  SavepointStats s;
  s.error = "savepoints are only supported on macOS";
  return s;
}
}  // namespace

SavepointStats SaveGuestMemory(const std::string&) { return Unsupported(); }
void RequestSavepoint(const std::string&) {}
void NoteSavepointSwap() {}
void SetGraphicsSavepointHooks(IGraphicsSystem*, void (*)(IGraphicsSystem*), void (*)(IGraphicsSystem*)) {}
void ClearGraphicsSavepointHooks(IGraphicsSystem*) {}
void ServiceSavepointRequest() {}
SavepointStats CaptureInProcessSavepoint() { return Unsupported(); }
SavepointStats RewindToInProcessSavepoint() { return Unsupported(); }
std::string ControlInProcessSavepoint(const std::string&, int) { return "error savepoints are only supported on macOS"; }

}  // namespace rex::system
