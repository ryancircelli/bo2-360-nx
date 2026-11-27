#pragma once

// Force-included into every Metal plugin source. The platform-neutral macro layer lives in the SDK
// (rex/graphics/xenia_compat.h) so the Linux check can compile the patched graphics core with it;
// only the Metal cvar declarations are added here. Private to this plugin: must not leak into the
// game or the installed SDK.
#include <rex/graphics/xenia_compat.h>
#include "metal_flags.h"
