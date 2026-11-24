# gputest NaN scan tools

Scripts for checking `rexgpu-replay --label metal` output directories (per-draw readbacks in
`draws.jsonl` + `draws/*.npy`) for non-finite pixels, and for telling real shader NaN from
readback artefacts. Run them with numpy, e.g. `uv run --no-project --with numpy python -I nanscan.py OUT`.

- `nanscan.py OUT [--json F]`: per draw, the pixels that became non-finite in that draw, grouped by shader pair.
- `classify.py OUT NANJSON`: splits those pixels into readback poison (all four channels 0xFFFFFFFF,
  the value `rexgpu-replay` fills the readback destination with) and other values, and prints the
  EDRAM tile span of the readback.
- `poisonbytes.py NPY`: maps the poisoned pixels of one readback back to byte ranges in the tiled
  destination (they fall on whole 4 KB pages).
- `pagecount.py OUT`: for runs with `REX_GPU_TRACE_POISON_GPU=1` (the replayer also fills the GPU copy of
  the destination with 0xEE): counts pixels left 0xEE (the readback resolve never wrote them) and
  0xFF (guest-RAM poison uploaded to the GPU copy).
