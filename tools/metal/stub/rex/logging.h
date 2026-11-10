// Stand-in for the SDK's logging in tools that build the Metal backend's sources on their own (check_backend.mm):
// prints the format string and drops the arguments.
#pragma once

#include <cstdio>

#define REXLOG_STUB(level, fmt, ...) std::fprintf(stderr, "[%s] %s\n", level, fmt)
#define REXLOG_ERROR(fmt, ...) REXLOG_STUB("error", fmt)
#define REXLOG_WARN(fmt, ...) REXLOG_STUB("warn", fmt)
#define REXLOG_INFO(fmt, ...) REXLOG_STUB("info", fmt)
