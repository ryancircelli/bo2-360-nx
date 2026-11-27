# Content-install discs

Install add-on discs before packing the app. Use the game's expected title ID;
the installer discovers packages by their STFS headers, not their filenames or
the disc's directory structure. It accepts an ISO or an extracted directory.

```sh
python3 -I tools/mac/install_content_disc.py '/path/to/disc2.iso' \
  --title-id 425307D5 --list
python3 -I tools/mac/install_content_disc.py '/path/to/disc2.iso' \
  --title-id 425307D5 --out /path/to/staging/content \
  --manifest /path/to/install-report.json
```

The output is unpacked content:

```
content/0000000000000000/<title>/00000002/<package>/<files>
content/0000000000000000/<title>/Headers/00000002/<package>.header
```

The header is the SDK's 0x148-byte `XCONTENT_AGGREGATE_DATA` plus the package's
licence mask. Licence entries are preserved, never fabricated. Other content
Licence bits are retained even when their flags are zero (retail PIRS discs
use this combination); flags are not an entry-present boolean.
types (including title updates) are inventoried but not applied. SVOD packages
are explicitly rejected; the supported payload format is STFS (CON/LIVE/PIRS).
Each package is staged completely before publishing its directory. Existing
packages are refused; rebuild into a fresh staging root to replace content.
This command does not touch a user's saves or install an application.

Place the base disc at `staging/game`, then pack with:

```sh
python3 -I tools/mac/pack_app.py --game fallout3 --build /path/to/build \
  --data /path/to/staging --data-mode clone --out '/scratch/Fallout 3.app'
```

The app launcher exposes its content through the existing shared-XUID links
under the selected support profile. Profile-owned save folders stay separate.

Fallout 3 GOTY DVD2 carries five marketplace PIRS packages for `425307D5`,
despite storing them under `FFED2000/FFFFFFFF`: Anchorage, The Pitt, Broken
Steel, Point Lookout and Mothership Zeta. It also carries Title Update #3
(`default.xexp`, `Update.esp`). Applying that update requires placing the
executable patch beside the base executable **before codegen**, regenerating
and rebuilding; placing it beside an existing recompiled game is insufficient.
The system update and installer executable are never installed as add-ons.

Content-install discs do not participate in `XamSwapDisc`. True swap discs
belong at `data/disc2`, `data/disc3`, etc.
