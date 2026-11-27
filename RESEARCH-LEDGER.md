## newgame-me2, 2026-11-26
- Read owner-provided Mass Effect 2 DVD1/DVD2 ISOs read-only via local metal360 XDVDFS reader. Extracted game files remain outside repos; ignored local XEX copies and decrypted images for analysis/codegen only. No game files or keys committed.
- Read local Gears package and shared SDK/tool sources and dependencies; no external port source copied.

## newgame-gears, 2026-11-25
- Read: owner-provided ~/Games/Xbox360/roms/Gears of War.ISO, read-only XDVDFS extraction; extracted default.xex via existing xexdump.
- Taken: derived title/version/section/import metadata and executable hash only. Game files and decrypted images remain ignored and outside committed content. No external game-port source read or copied.
- Dependencies: read-only local shared sdk/thirdparty via symlinks, existing SDK dependency licences apply; no changes to shared sources.


# Halo 3 bring-up outside-repository reads (2026-11-25)

| Source | License / provenance | Use |
|---|---|---|
| ~/Games/Xbox360/roms/Halo 3 [RF][ENG].iso; extracted ~/Games/Xbox360/halo3/disc | Owner-provided game dump; proprietary | Read-only extraction and XEX header audit; no contents committed |
| ~/dev/metal360/src/metal360/formats/xdvdfs.py | 0BSD, header verified | Existing read-only disc extractor; no source copied |
| /tmp/bo2/nfs-new-games/scripts/{extract.py,regen.sh}; gen/nfshp/generated/rexglue.cmake | Prior local stream tooling / generated build metadata | Reused extractor command; examined codegen/build flow only |
| /tmp/bo2/stub-audit/tools/classify.py; stubs.tsv | Prior local audit | Import/stub audit method; no restrictive reference source read |
| ~/dev/bo2-mac-port/{WORKFLOW.md,RESUME-CONVENTION.md,REJECTED-LEDGER.md,reports/*} | Project coordination documents | Workflow and prior bring-up evidence |
| live-test/b-bo2 cached xenios-source archive | BSD-3-Clause, pinned SHA256 af2eccbd79053709a84c097cf01bc28633575b352c09e286dd2723e20e61dbd6 verified | Reused existing dependency download after connection reset |
| ~/dev/bo2-360-nx/out/host-mac/switch_bo2/xexdump | Existing local SDK loader tool | Read-only image/section/import audit; dumps kept outside repositories |
