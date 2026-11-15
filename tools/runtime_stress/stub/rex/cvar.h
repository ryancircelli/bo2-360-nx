// Stand-in for the SDK's rex/cvar.h in the standalone stress build (the clock and threading code define and read
// a few cvars).
#pragma once

#include <string>

#define REXCVAR_DECLARE(type, name) extern type rexstress_cvar_##name
#define REXCVAR_DEFINE_BOOL(name, default_value, category, description) \
  bool rexstress_cvar_##name = default_value
#define REXCVAR_DEFINE_STRING(name, default_value, category, description) \
  std::string rexstress_cvar_##name = default_value
#define REXCVAR_GET(name) rexstress_cvar_##name
