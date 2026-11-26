# Mass Effect 2 package requirements

Title `454108CE`, version/base `0.0.0.1`, two discs. Disc 1 media
`6EE5A52F`, default.xex SHA-256
`5868cca5cc285a89c8414247accc02df3874f34472178061360797e71fa23dad`.
Disc 2 media `027D8AAF`, default.xex SHA-256
`8fb1237b493a9616413ef505039afcfb53f12312a072b914617e38c62b3d61e4`.

The two decrypted images differ only at offset `0x1506600`: ASCII `1` vs
ASCII `2`. Every executable section is byte-identical. Generate from disc 1;
validate both hashes before allowing the shared generated code on disc 2.
The generic rule requiring separate generation for different executables may
be relaxed here only with this explicit section-by-section evidence.

Each disc also has `Unreap.xex`, a separate executable at the same load base,
title `4D5308C5`. Its decrypted images are identical across discs. Its role
and whether the game launches it are unverified; no module generation or
launch support for it is claimed.

A future `packages/me2` needs the standard manifest registration adapted to
metal360 package API 2, these accepted executable hashes, derived gap and tail
entries, and the four standard codegen postpasses. No game-specific runtime
hooks have been added. Generated C++, XEXs and decrypted images stay outside
committed package contents.

Stage the boot disc at `data/game/`, disc 2 at `data/disc2/`, preserving each
whole filesystem separately. Their inventory has 989 shared paths, 892 paths
unique to disc 1 and 1,189 unique to disc 2. Shared-path sizes are equal;
content equivalence of every shared file has not yet been checked. Do not
merge the disc directories. Include writable profile storage separately.
The scratch launcher points at an external two-disc data root. A distributable
app assembled locally from the owner's discs must carry or refer to both
folders, with the normal `update/` and `content/` siblings.

The SDK disc-swap implementation (original commit b324d385) remounts `game:`
and `d:` and retains old devices for open handles. `XamSwapDisc` is an actual
import of this title. Runtime swap/menu/gameplay proof remains pending.
