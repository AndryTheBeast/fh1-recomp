# Roadmap

Following the order in `docs/porting-another-game.md`, which worked for nfsmw-nx.

## Stage 1 — Translate the game on PC
- [x] Build the ReXGlue code generator (`rexglue`)
- [x] Extract `default.xex` (NTSC-U, Title ID 4D5309C9, v0.0.0.10) with `tools/extract_xex.bat`
- [x] `rexglue init` the `fh1/` project and run the first codegen (passes: 16 tail-call targets declared in `fh1/overrides.toml`)
- [ ] Fix what the translation misses: missing functions, jump tables, split functions (`tools/huecos.py`, `fh1/overrides.toml`)
  - [x] `0x8241A370`: checked, one genuine 110 KB float-math function (only its C++ file is big)
  - [x] `bdz` switch at `0x82AD80D0` ended one instruction early (`overrides.toml`)
  - [x] `0x830ED900` ran past `KeBugCheck` into the next function (`overrides.toml`)
  - [x] Code gaps: 796 found; 191 are data (`fh1/huecos_excluir.txt`), 115 extra this-adjusting
        thunks split out, 58 truncated functions extended (`tools/fusionar_continuaciones.py`).
        Codegen is clean: no unresolved branches, no `REX_FATAL` stubs. 321 gaps left, mostly data.

## Stage 2 — Boot on Windows with ReXGlue's own graphics
- [x] Build the Windows runtime (`tools/build_fh1.bat`; needs the VC++ 2015-2022 redistributable to run)
- [x] First run: boots, loads `XMediaFacade_default.xex` at run time and dies calling into it.
      Both run-time modules (XMediaFacade, SpeechFacade) are now `[[modules]]` in the manifest.
- [x] Turn on the `xenos` GPU plugin (the nfsmw-nx SDK defaults to no GPU emulation)
- [x] First frame: trademark screens with audio (2026-09-29)
- [x] Intro video and the title screen ("PRESS START") render with the xenos GPU plugin (2026-09-29)
- [ ] Past Start: profile sign-in, save device, main menu
  - Press Start -> XamShowDeviceSelectorUI (returns dummy device 1) -> ~130 ms later a guest
    null read (0x00000000) on the main thread. Crash reports (fh1/src/fh1_crash_report.cpp,
    `build_logs\run-*.log.crash.txt`) added to find the function.
  - [x] Crash report: sub_8310C340 read through r20 = 0 right after sub_8310C640 called the XDK's
        SwitchToFiber (0x830ED910), which saves/loads full register contexts and cannot work as
        translated code. The fiber family is now hooked to the SDK's host fibers ([rexcrt] in
        fh1/overrides.toml).
  - [x] Fiber fix works: Start -> device selector -> the playable intro drive (2026-09-29), but at
        1-2 frames per second (xenos GPU emulation on a GTX 1050; the native renderer is Stage 4).
  - [x] Leaving the intro drive: the game unloads XMediaFacade and loads it again. The SDK's
        UnloadUserModule looked the module's translated code up by its resolved "\Device\..." path,
        which never matches the manifest's guest path, so the old entry stayed and the reload was
        refused -> XamShowDirtyDiscErrorUI -> the game's fatal handler (sub_82C09F00, error 255,
        which spins forever by design and compiles to a trap). Fixed in sdk/src/system/kernel_state.cpp.
        Verified 2026-09-29: XMediaFacade and SpeechFacade unload and reload cleanly.
  - [ ] Frame rate: `[fps]` log lines every 10 s measure it and break down the GPU thread's time.
        First finding: every run so far used the laptop's Intel HD 630, not the GTX 1050 (D3D12 took
        adapter 0; the Optimus export sat in rexruntime.dll where drivers ignore it). Draws took
        ~90-290 us each (2300-3300 per frame in the drive -> 4 fps). Fixed: high-performance adapter
        first (sdk/src/ui/d3d12/d3d12_provider.cpp) + Optimus/PowerXpress exports in fh1/src/main.cpp.
  - [x] Intel HD 630: the SDK forced the ROV render-target path on Intel (a 2021 driver workaround);
        it compiled pipelines for ~65 s before the first frame and made draws ~15x slower. Host render
        targets (RTV) are now the default everywhere; clears render correctly. Logos 50 -> 60 fps.
  - [x] Tear lines: the D3D12 presenter allowed tearing by default; now off (vblank-synced).
  - [x] Unattended testing: --fh1_autoplay scripted pad + tools/auto_test.ps1 reach the intro drive
  - [x] Missing geometry (car side mirrors): draws with an "invalid" vertex fetch constant were
        dropped; gpu_allow_invalid_fetch_constants now defaults to true
  - [ ] Glitches still seen in the intro cutscene: dark square in the bottom-right corner, hard-edged
        rectangular car shadow. The dark square only appears with host render targets (RTV); the
        ROV path draws that corner correctly (same frames compared). It sits in the bottom strip of
        FH1's 3-strip predicated tiling (rows 0-256, 256-512, 512-720, window offsets 0/-256/-512;
        the ~6 "Resolve region is empty" errors per frame are just the other strips' resolves being
        scissored away). --native_stencil_value_output_d3d12_intel=true is much worse (green
        wheels, bright band at the bottom): Intel's PS stencil reference output is still broken.
        Next: RenderDoc capture of the cutscene frame to find the pass that writes that corner.
  - [ ] Intro drive on the HD 630: ~7 fps, ~90 ms per frame of host-GPU work (needs ~4x less for 30)
  - [ ] forza_tone.wmv / title scene: ~40 ms per frame of host-GPU work on the HD 630 (18 fps)
  - [x] "Audio cut" in the intro video is the video playing slowly (19 fps): its audio runs in real
        time and ends first. Should go away with the frame rate.
- [ ] First drive
- [ ] Log kernel/XAM calls the game needs that ReXGlue lacks (Kinect, Xbox Live, content/DLC paths)

## Stages 3-5 — Nintendo Switch (separate repository)
The Switch port will be a separate repository, started from this one once the game is fully playable on PC.
The plan carried over from nfsmw-nx, kept here for reference:

### Stage 3 — Move to the Switch system
- [ ] Build with devkitA64 + the Horizon layer in `sdk/`
- [ ] Memory map, threads, audio out

### Stage 4 — Native renderer and pre-translated shaders
- [ ] D3D tracing pass to find FH1's render functions (pattern: `reference/nfsmw-app/src/nfsmw_d3d_trace.cpp`)
- [ ] Port the ring-thread renderer from `reference/nfsmw-app/src/nfsmw_nativo_*`
- [ ] Shader library via XenosRecomp

### Stage 5 — Performance on the console
