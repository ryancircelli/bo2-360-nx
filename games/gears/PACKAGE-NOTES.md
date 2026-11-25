# Gears of War package requirements

Disc build: title `4D5307D5`, media `76E9DF5B`, version/base `0.0.0.1`.
The tested executable is `default.xex`, SHA-256
`df1041da72d2b947e3bb2f701a19fde9e84488dc848fea2decd6d43434efe2d1`.
No additional XEX/DLL modules or title-update patches were found on this disc.

A future `packages/gears` manifest needs:

- Identification by title ID and executable hash; reject other executable
  versions until generated and tested separately.
- One entrypoint, `default.xex`, with output `generated/default` and project
  prefix `gears`. The supplied overrides and gaps are analysis-derived entries,
  rather than gameplay hooks. Include all executable sections, including Bink
  and the `.embsec_` sections, when checking coverage.
- The generic manifest registration used by `src/main.cpp`, adapted to
  metal360's package API and pinned SDK version. No native gameplay patch has
  been necessary to reach the title screen.
- Metal as the default GPU plugin, with the shared renderer/runtime dependencies.
  No precompiled Vulkan shader pack is required for the tested Metal path.
- Read-only extracted disc data, separate writable profile/cache storage,
  fullscreen off by default, and the existing app settings. Never include game
  files or decrypted images in the package repository.
- Reproducible code generation with unresolved-tail repair, gap analysis down to
  four bytes, strict register-read checking, setjmp checking, direct calls, and
  guest barriers. Generated output is ignored and must be regenerated from the
  user's own executable. Keep inputs within the generator's canonical game root.
- The gate-kit menu route and future gameplay route. Test with a script pad,
  muted audio, hidden/minimized windows and physical HID access disabled.

Bring-up currently verifies a clean build and the PRESS START title screen.
Main-menu navigation, gameplay, audio, save persistence, title updates, other
disc editions, and metal360's newer package API remain unverified. This note
describes requirements; it does not claim a working metal360 package.
