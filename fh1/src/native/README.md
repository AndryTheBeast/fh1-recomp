# Native renderer (copied from nfsc-recomp)

These files are the native Vulkan renderer of **nfsc-recomp** (Need for Speed: Carbon, by GoatHonks), which is
itself the native renderer of **nfsmw-nx** (Need for Speed: Most Wanted, by StevensND). They are GPL-3.0; the
credits and licenses of both projects apply. Copied on 2026-10-02 with the author's permission; re-based on
his English-named files (`nfsc_*`, same names as nfsc-recomp so the two projects diff easily) on 2026-10-04.

Run FH1 with `--nfsc_renderer=native`. The shader library must be next to `fh1.exe` as
`nfsc_shaders.nfsp` (built by `tools/build_shader_library.ps1`, game-derived: never in git).

FH1 changes (keep this list current):
- `ring_progress.cpp`, `compat/rex/frame_stats.h`, `compat/rex/ring_progress.h`: nfsc-recomp's SDK additions,
  kept here so our SDK stays as it is (gpu_timing removed).
- `nfsc_native_draws.cpp`: the shared-constant block is 164 words / 41 float4 (FH1's `g_GuestBase` and
  `g_FetchAddress` at words 154-163, after nfsc's loop constants; zero for now, so `fh1Fetch` returns 0).
- `nfsc_shader_library.cpp`: 256 MB file limit (FH1's library is ~100 MB).
- Built with `NFSC_NATIVE_RENDERER=1`, which keeps Most Wanted's game hooks (`nfsc_native_hooks.cpp`) out.
- SDK: `rex/ui/vulkan/device.h` and `vulkan_device.cpp` enable VK_KHR_maintenance5 as nfsc-recomp does.

The older FH1-only attempt (`fh1_native_*.cpp`, `--fh1_renderer=native`) stays for comparison until this one
is better everywhere.
