// Hot-reloadable (REX_PATCHABLE) functions of the FH1 recompilation. Replacement bodies live in a
// patch library (see sdk/tests/unit/patch/fh1_patch_lib.cpp) loaded with --patch_library/--patch_reload.
#include "generated/default/fh1_pch.h"
#include <rex/system/patch.h>

// sub_82416380: D3D draw function, called per draw.
REX_PATCHABLE(sub_82416380)
