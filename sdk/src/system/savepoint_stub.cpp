/**
 * @file        system/savepoint_stub.cpp
 * @brief       Savepoints need Mach thread suspend; every other platform gets no-ops
 */

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
void ServiceSavepointRequest() {}
SavepointStats CaptureInProcessSavepoint() { return Unsupported(); }
SavepointStats RewindToInProcessSavepoint() { return Unsupported(); }

}  // namespace rex::system
