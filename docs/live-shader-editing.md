# Live shader editing (standard vertex/pixel and emulated geometry/tessellation pixel pipelines)

This branch has not been tested in a game. Do not install it until the later-session gates in RESUME.md pass. The owner’s current app/settings have not been changed.

After the game closes, use a scratch fh1.toml with absolute paths:

```toml
metal_shader_live_dump = "/tmp/fh1-shaders/dump"
metal_shader_live_overrides = "/tmp/fh1-shaders/overrides"
metal_shader_constant_overrides = "/tmp/fh1-shaders/constants.txt"
metal_fh1_blur_threshold = true
```

Create the folders before starting. Restart after enabling the shader folders. As standard pipelines are translated, dump contains `ps_HASH_MODIFICATION` and `vs_HASH_MODIFICATION` artifacts plus `.manifest` files identifying the entry point and ABI. Native translations also dump editable `.metal` source. Converter translations dump DXBC, DXIL and `.metallib`, with no editable MSL available. Copy the desired `.metal` or compatible `.metallib` into overrides using exactly the same basename; edit the copy. A `.metal` takes precedence over `.metallib`. Keep the entry point, stage, buffer layouts and binding ABI intact. Polling runs at most once per second; compilation runs on a utility worker. The log reports `[shader-live] ... swap successful, N pipelines rebuilt` or a compile/load/pipeline error. On an error the previous shader stays active. Remove both override files to restore the original.

Native variant dumps (`_pts`, `_ptsc`, `_ptsm`, `_rect`, `_dscpu`, `_spec`) are inventory only in this milestone: they cannot yet be live-swapped. Pixel shaders in emulated geometry/tessellation pipelines are included; their generated vertex/hull/domain stages still need separate rebuild registration. This is not full variant coverage.

Constant file format (one scalar per line; register/component are decimal, component 0..3 = xyzw):

```
# FH1 velocity pass PS c253.w
ps E68FE55ECD482EC5 253 3 0.5
```

Saving this file changes matching packed constants on subsequent frames. The file overrides the built-in FH1 threshold, so use `0.5019683837890625` for the original threshold and `0.5` for the candidate. Invalid files retain the last parsed set; deleting or emptying the file removes its overrides. Guest registers are never modified. The built-in threshold is enabled only in the FH1 settings template and otherwise defaults off. Existing installed configs need the setting added explicitly in a later authorized session.

Old functions and pipeline states stay resident until exit so queued GPU work can finish. Frequent editing can therefore grow memory; restart between long editing sessions. Source compilation succeeds only for the translation ABI identified in its manifest; changing arbitrary resource bindings is not a supported edit.
