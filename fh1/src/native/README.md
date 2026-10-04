# FH1's native renderer

FH1's own native Vulkan renderer. Run with `--fh1_renderer=native`. The shader library must be next to
`fh1.exe` as `fh1_shaders.nfsp` (built by `tools/build_shader_library.ps1`, game-derived: never in git).

## Where it came from (credits)

It started as the native renderer of **nfsc-recomp** (Need for Speed: Carbon, by GoatHonks), which is itself the
native renderer of **nfsmw-nx** (Need for Speed: Most Wanted, by StevensND). Both are GPL-3.0; their credits and
licenses apply. Copied on 2026-10-02 with the author's permission, re-based on his English-named files on
2026-10-04, and made FH1's own the same day (user decision): files, namespace, settings and macros renamed from
`nfsc_*` to `fh1_*`. Comments that start with `NFSC:` mark what Carbon changed over Most Wanted.

nfsc-recomp stays a source of fixes, ported by hand when they work for FH1. To compare a file with his, replace
`nfsc_`/`NFSC_`/`nfsc::` by `fh1_`/`FH1_`/`fh1::` in his copy first, then diff.

## Fixes taken from nfsc-recomp (keep this list current)

| His repo as of | What |
| --- | --- |
| 2026-10-04 13:42 (local copy in `..\repos\nfsc-recomp-main`) | everything: this is the base |

## FH1 changes on top of that base (keep this list current)

- `ring_progress.cpp`, `compat/rex/frame_stats.h`, `compat/rex/ring_progress.h`: nfsc-recomp's SDK additions,
  kept here so our SDK stays as it is (gpu_timing removed).
- `fh1_native_draws.cpp`: the shared-constant block is 164 words / 41 float4 (FH1's `g_GuestBase` and
  `g_FetchAddress` at words 154-163, after Carbon's loop constants; zero for now, so `fh1Fetch` returns 0).
  Packed 10_11_11 vertex formats (`--fh1_vertices_10_11_11_mask`).
- `fh1_native_shaders.cpp`: 2008 microcode located through the shader header; tolerant vertex shader pass
  (FH1's Direct3D patches fetch swizzles and nulls exports).
- `fh1_native_targets.cpp`: 2_10_10_10 resolves, wide render target formats, wide resolves
  (`--fh1_msaa_4x_as_1x` experiment, off).
- `fh1_shader_library.cpp`: 256 MB file limit (FH1's library is ~130 MB).
- `fh1_native_hooks.cpp`: Most Wanted's address-based game hooks (shader constructors, fetch patcher) removed;
  FH1 identifies shaders from the command ring. `fh1_native_stubs.cpp` holds the neutral stand-ins for the
  other Most Wanted helpers.
- Kept on purpose: the `REX_PLATFORM_SWITCH` / `__aarch64__` blocks (small, compiled out on Windows, and the
  Switch port will start from this code).
- `fh1_native_targets.cpp`: k_8_8_8_8 and k_8_8_8_8_GAMMA render targets share one image (FH1's videos); a Swap
  of a front buffer nobody resolved into shows the last screen-sized resolved texture (trademark screen).
- `fh1_native_system.cpp`: the piecewise-linear gamma ramp (DC_LUT_PWL_DATA) is read and applied on the output;
  vertex shaders get the vertex index (SDK feature shaderDrawParameters).
- 2026-10-04 night, the 3D scene (details: `docs/native-renderer-status.md`):
  - `fh1_rect_gs_spirv.h` (generator `tools/fh1_make_rect_gs.py`): the rectangle's corner is the vertex opposite the
    longest edge; rectangle lists are never culled.
  - `fh1_depth_pack_spirv.h` (generator `tools/fh1_make_depth_pack.py`) and `TextureResolvedBytes`: a resolved depth
    fetched as a color texture gives the console's bytes (24-bit depth + stencil); depth resolves copy the stencil.
  - 4x MSAA passes of 640 pitch or less (Direct3D's clears) draw into the 1x target of twice the pitch.
  - Vertex fetches with a computed index in quad lists (billboards): each stored vertex repeated four times;
    k_10_11_11 positions on by default; alpha to mask as an alpha test.
- SDK: `rex/ui/vulkan/device.h` and `vulkan_device.cpp` enable VK_KHR_maintenance5 as nfsc-recomp does.
