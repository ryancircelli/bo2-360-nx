# Order matters: later patches apply on top of earlier ones.
set(REXGPU_METAL_PATCHES
    sdk-metal.patch
    sdk-metal-native-scale.patch
    sdk-metal-rt-shrink.patch
    sdk-metal-hle-census.patch
    sdk-metal-gpucmd.patch
    sdk-metal-ssmask.patch
    sdk-metal-loadwindow.patch
)
