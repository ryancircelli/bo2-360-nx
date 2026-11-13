// The game's Direct3D functions in native code (D3D renderer M5): see bo2zm_d3d_native.cpp.
#pragma once

#include <cstdint>

#include <rex/ppc/context.h>

namespace bo2zm::d3d_native {

// SetTexture (sub_82164190). False when the game's function must run (switched off, or after a MISMATCH).
bool SetTextureNative(PPCContext& ctx, uint8_t* base);
// SetVertexShader (sub_82169DE0) and SetPixelShader (sub_82169A00), the same way.
bool SetVertexShaderNative(PPCContext& ctx, uint8_t* base);
bool SetPixelShaderNative(PPCContext& ctx, uint8_t* base);
// DrawIndexedVertices (sub_8216B1D0): not checked call by call (see bo2zm_d3d_native.cpp).
bool DrawIndexedVerticesNative(PPCContext& ctx, uint8_t* base);

}  // namespace bo2zm::d3d_native
