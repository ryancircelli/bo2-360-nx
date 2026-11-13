// PC test attenuation never changes a Switch build's normal output gain.
#pragma once
namespace bo2zm::audio {
inline constexpr int OutputVolumePercent(int requested) {
#if defined(__SWITCH__)
  (void)requested;
  return 100;
#else
  return requested < 0 ? 0 : requested > 100 ? 100 : requested;
#endif
}
}  // namespace bo2zm::audio
