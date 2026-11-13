// Stand-in for the SDK logger, so tools/tests/cvar_alias_test.cpp can compile sdk/src/core/cvar.cpp alone.
#pragma once
#define REXLOG_DEBUG(...) ((void)0)
#define REXLOG_INFO(...) ((void)0)
#define REXLOG_WARN(...) ((void)0)
#define REXLOG_ERROR(...) ((void)0)
