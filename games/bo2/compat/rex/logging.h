// Compat: the v0.10 logging header on v0.11.
#pragma once
#include <rex/logging/log_category.h>
#include <rex/logging/logger.h>
#include <fmt/format.h>
namespace rex {
inline void FlushLogging() { REX_LOGGER().Flush(); }
}
