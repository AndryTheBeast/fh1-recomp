# Handoff (2026-10-04): where the native renderer stands, and the next task (English names)

## DONE 2026-10-04: English names
The rename below was done on 2026-10-04. What changed for daily use:
- Native renderer: `--nfsc_renderer=native` (was `--nfsmw_renderizador=nativo`); the shader library next to
  fh1.exe is `nfsc_shaders.nfsp`. Files are `fh1/src/native/nfsc_*` with the same names as nfsc-recomp.
- `fh1_msaa_4x_as_1x`, `fh1_vertices_10_11_11_mask`; SDK settings follow nfsc-recomp (`nfsc_io_*`, `audio_dump_*`...).
- Gap files and tools: `fh1/gaps.toml`, `fh1/*_gaps.toml`, `*_gaps_exclude.txt`, `tools/gaps.py`, `gaps_pass.py`
  (`--gaps`, `--exclude`, `--rests`), `gaps_iterate.sh`, `merge_continuations.py`, `check_symbols.py`;
  `shaders/fh1_pack_library.cpp`.
- How it was done: names taken from nfsc-recomp by aligning his English files with the Spanish ones (same code,
  so the two projects stay easy to diff); other names translated word by word; messages taken from his files.
- Left: the log-message texts of the Most Wanted-only files in `reference/nfsmw-app` (names are English, the
  message texts are still partly Spanish); `mesa/mesa-switch-nfsmw.patch` untouched; Switch-only SDK files were
  renamed but cannot be compiled on this PC.

## The task as it was written (user request, 2026-10-04): everything in English
The user wants the Spanish code translated to English: Stevens' (nfsmw-nx) code in this repo and our own.
GoatHonks already did it for nfsc-recomp (his ROADMAP: "Housekeeping: English names, DONE 2026-10-03").
His updated repo is at `%USERPROFILE%\Desktop\FH1-recomp\repos\nfsc-recomp-main\` (one folder up from before;
no inner `nfsc-recomp-main` folder any more). His renamed native renderer is `carbon/src/native/nfsc_native_*.{cpp,h}`.

Suggested way (cheapest and safest):
1. Native renderer (`fh1/src/native`, ~27,000 lines, copied from his Spanish version on 2026-10-02): take his English
   files as the new base and re-apply the FH1 changes on top. The FH1 changes are small and all in git:
   `git log --oneline 4af56f2^..HEAD -- fh1/src/native` and `fh1/src/native/README.md` list them (2008 microcode
   location + tolerant VS pass in the shaders file; wide formats, parked resolved textures, FormatoCopiaFh1 in the
   targets file; shared constants 164 words, k_10_11_11 inputs, exp_adjust, fh1_* cvars and diagnostics in the draws
   file; Most Wanted address reads removed in the system file). Check his git history / diff for anything he fixed
   since (barriers, depth resolves, point lists, HDR) - free improvements.
   The cvar names change (`--nfsc_renderer=native` etc.): update fh1_app.h, CMakeLists, docs, CLAUDE.md,
   run commands, and tell the user the new command.
2. The rest: `shaders/` (nfsmw_hlsl.cpp, fh1_pack_library.cpp, XenosRecomp comments under NFSMW_RECOMP, shader_common.h
   names like NfscBlockShared - careful: renaming those means rebuilding the shader library, ~20 min),
   `tools/` (gaps*.py, merge_continuations.py, check_symbols.py), `fh1/*gaps*.toml` file names,
   SDK parts with Spanish comments/log lines, `reference/nfsmw-app/`, docs. Compare with how his repo named the
   same files and reuse his names so the two projects stay easy to diff.
3. Our own older attempt (`fh1/src/fh1_native_*.cpp`, `--fh1_renderer=native`) is already English and superseded;
   ask the user whether to delete it.
Rules: build after each step (`tools\build_windows.ps1 -SkipFetch`), run the title-screen smoke test and the
festival test below, LOOK at the screenshots, commit in small steps, keep credits (GPL-3.0: StevensND, GoatHonks).

## How to test without the user
- Title screen: `tools\auto_test.ps1 -Name t -Seconds 45 -Shots 43 -ExtraArgs "--nfsc_renderer=native"`
- Festival (boots the user's save, no driving):
  add `-Seconds 100 -Shots 95 -Autoplay "33+0.2=start;33.8+0.2=start;34.6+0.2=start;35.5+0.2=a;36.8+0.2=a;38.1+0.2=a"`
- Shader library: must sit next to fh1.exe as `nfsc_shaders.nfsp` (copy of build_logs\shaders\fh1_shaders.nfsp).
  After changing shaders/XenosRecomp/shader_common.h: patch or regenerate build_logs\shaders\hlsl, then
  `tools\fh1_compile_shaders.py` + `shaders\fh1_pack_library.exe` (delete the old .nfsp first: the packer refuses to
  overwrite).

## State of the native renderer (2026-10-03 night)
Works: title, menus (white background), loading screens (3D), festival driving at 30 fps.
Open problems, in the order I would take them:
1. **Vertex index in r0.x** (found, not fixed). FH1's vegetation / billboard vertex shaders (the ones with
   k_10_11_11 POSITIONS, e.g. containers v_e5c9177a018ca384 and v_f91e79c713f746c7) read `r0.x` before writing it:
   on the Xbox 360 a vertex shader starts with the vertex index there. The translator writes `float4 r0 = 0.0`, so
   every corner collapses and the maths divides by zero: with packed positions enabled the festival turns black
   (`--fh1_vertices_10_11_11_mask=65535` to see it; default 0xFFFE = everything but positions).
   Plan: in shaders/XenosRecomp/shader_recompiler.cpp (NFSMW_RECOMP, vertex shaders) declare
   `in uint iVertexId : SV_VertexID` and start r0 as `float4(float(iVertexId), 0, 0, 0)` (the UNLEASHED_RECOMP
   branch at the register initialisation shows where); compile vertex shaders with DXC
   `-fvk-support-nonzero-base-vertex` so SV_VertexID is the raw index (the renderer draws with a negative
   vertexOffset); that needs the Vulkan feature `shaderDrawParameters`: add
   VkPhysicalDeviceVulkan11Features to sdk/src/ui/vulkan/vulkan_device.cpp (link it next to features_1_2, enable
   it in the vulkan_native_shader_features block) - I had written that edit and reverted it untested.
   Non-indexed draws would still need the index offset (VGT_INDX_OFFSET) added.
2. **Speckled edges**: FH1 draws depth/stencil fills at 640 pitch 4x MSAA (surface 0A020280) that the 1280x720 1x
   passes then use (same EDRAM). `--fh1_msaa_4x_as_1x=true` (shared image, viewport x2) turned the picture
   pink/black: needs a per-image dump (`--nfsc_dump_resolved_at_s=N`) to see which fill lands where.
3. **White title/menus**: the video is drawn correctly (render target format 1, base 0); the frame composite
   (PS n767 at the time) outputs white.
4. 2x MSAA reflection targets and their resolves; strong glare; the 9 worst-1% frame drops since wide resolves.
Debug switches that helped: `--nfsc_native_diag_frame_s=N` (every draw and copy of one frame),
`--nfsc_native_diag_resolved=true` (mosaic), `--nfsc_dump_resolved_at_s=N` (PNG of every image, in
fh1\out\win-release\dump_resolved), `--nfsc_dump_ring_shaders=DIR` + tools/nfsc_synth_containers.py (unknown shaders).
