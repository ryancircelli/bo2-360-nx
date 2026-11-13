# The home screen's credits line

`credits_720.png` (handheld) and `credits_1080.png` (docked) are the line "Ported by: AlvaroEPena" with GitHub's
mark, drawn bottom right on the Zombies home menu by `games/bo2/src/bo2zm_credits.cpp` and embedded by CMake
(`bo2zm_credits_png.h.in`). Regenerate them after changing the text or sizes:

```bash
python -I tools/make_credits.py --preview credits_preview.png
```

Nothing here comes from the game:

| Part | Source | License / terms |
|---|---|---|
| GitHub's mark | The `mark-github` path of [Octicons](https://github.com/primer/octicons), in white, otherwise unaltered | Octicons: MIT. The mark is GitHub's trademark, used as [GitHub's logo guidelines](https://github.com/logos) allow, to point to the author's GitHub profile |
| The text, in Roboto Medium | `Roboto-Medium.ttf` as Dear ImGui carries it (`sdk/thirdparty/imgui/misc/fonts`) | Apache License 2.0 (Google); the images hold rendered glyphs, not the font |
