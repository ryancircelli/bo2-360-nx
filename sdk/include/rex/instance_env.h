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

// This process's instance (0..15), or -1 when unset or malformed.
inline int InstanceIndex() {
  const char* value = InstanceEnv();
  if (!value) return -1;
  char* end = nullptr;
  const long instance = std::strtol(value, &end, 10);
  return end && *end == '\0' && instance >= 0 && instance < 16 ? int(instance) : -1;
}

}  // namespace rex
