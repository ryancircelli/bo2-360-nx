// Multi-instance environment: REX_INSTANCE (0..15) and REX_INSTANCE_COUNT name this process's slot when several
// copies of one game run on one machine. BO2_INSTANCE / BO2_INSTANCE_COUNT are the old names and stay accepted;
// the REX_ name wins when both are set.
#pragma once

#include <cstdlib>

namespace rex {

inline const char* InstanceEnv() {
  if (const char* value = std::getenv("REX_INSTANCE")) return value;
  return std::getenv("BO2_INSTANCE");
}

inline const char* InstanceCountEnv() {
  if (const char* value = std::getenv("REX_INSTANCE_COUNT")) return value;
  return std::getenv("BO2_INSTANCE_COUNT");
}

}  // namespace rex
