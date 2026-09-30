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
        1-2 frames per second (xenos GPU emulation on an Intel iGPU; the native renderer is Stage 4).
  - [x] Leaving the intro drive: the game unloads XMediaFacade and loads it again. The SDK's
        UnloadUserModule looked the module's translated code up by its resolved "\Device\..." path,
        which never matches the manifest's guest path, so the old entry stayed and the reload was
        refused -> XamShowDirtyDiscErrorUI -> the game's fatal handler (sub_82C09F00, error 255,
        which spins forever by design and compiles to a trap). Fixed in sdk/src/system/kernel_state.cpp.
        Verified 2026-09-29: XMediaFacade and SpeechFacade unload and reload cleanly.
  - [ ] Frame rate: `[fps]` log lines every 10 s measure it and break down the GPU thread's time.
        On a laptop with switchable graphics D3D12 took adapter 0, the integrated GPU (the Optimus
        export sat in rexruntime.dll where drivers ignore it). Draws took ~90-290 us each (2300-3300
        per frame in the drive -> 4 fps). Fixed for multi-GPU machines: high-performance adapter
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
        Ruled out on the Iris Plus (2026-09-30, square still there with each):
        --direct_host_resolve=false, --mrt_edram_used_range_clamp_to_min=false,
        --execute_unclipped_draw_vs_on_cpu=true (+ _with_scissor=true),
        --depth_transfer_not_equal_test=false, --native_2x_msaa=false.
        The square (~x 1183-1280, y 630-720 of 1280x720, blurred edges, blue-grey) is at a fixed
        screen spot: it shows in sky-only frames and at the drive start too, sometimes with a thin
        dark bar along the bottom edge. Looks like a scratch region the game draws into and later
        covers (on ROV) but that the host-RT path keeps. Needs a frame capture (RenderDoc).
        ROV on the Iris Plus (user watched, 2026-09-30): "fixed almost every glitch" but the drive
        runs at 1-2 fps (0.2 at worst) - usable only as the correct reference picture.
  - [x] Crash at the start of forza_tone.wmv (3 runs in a row on the Surface, ~80 s in):
        "Thunk address space exhausted for module at 823E0000", then XexGetProcedureAddress
        ordinal 6 in XMediaFacade returns 0 and the game calls address 0. UserModule lookups got a
        new thunk on every call (only KernelModule cached them), so the game's repeated lookups
        filled the main module's 64 KB pool. FunctionDispatcher::AllocateThunk now reuses the
        thunk per (pool, host function); 3/3 runs pass the video afterwards.
  - [ ] Intro drive on the HD 630: ~7 fps. Host-GPU time per frame by kind of work (GPU
        timestamps, `[fps] host GPU per frame` log line, 2026-09-30): 137 ms = 54 draws (39%),
        30 render-target/EDRAM transfers (22%), 26 resolves (19%), 19 texture loads (14%),
        4 memory uploads, 2.5 primitive processing, 2 present. Emulating the Xbox's EDRAM and
        render-to-texture costs ~75 ms (55%); the draws alone would still be ~18 fps. 30 fps on the
        HD 630 needs the native renderer (no EDRAM emulation, no per-strip tiling, native shaders)
        plus cheaper draws.
  - [ ] forza_tone.wmv / title scene: ~40 ms per frame of host-GPU work on the HD 630 (18 fps)
  - [x] "Audio cut" in the intro video is the video playing slowly (19 fps): its audio runs in real
        time and ends first. Should go away with the frame rate.
- [x] First drive (the user played it through to the festival and saved, 2026-09-30)
- [ ] Crash entering an event (user, 2026-09-30): the game creates cache:\ghost_stream_0..3 and
      cache:\replay_stream; `cache:` is linked by the game to \Device\cache1, which did not exist,
      so the creates failed and it read a null stream. Runtime::SetupVfs now mounts
      <cache_root>\xbox_utility\Cache0/Cache1 at \Device\Cache0/1 (emptied at every boot, as the
      title formats it). Not verified in an event yet (autoplay cannot reach one: needs the user).
- [x] Crash loading a save (4 of 6 runs, "continue" boot, 2026-09-30). sub_82D3DB00 (from
      sub_82D44B90 / sub_82D1AAE8) indexes a 12-byte table at [r3+84] with r4&0xFF; every good call
      uses index 0, every crash used 190 = 0xBE: the index comes from a stack word the game never
      writes, and XThread::AllocateStack filled new stacks with Xenia's 0xBE debug pattern. Stacks
      are now zeroed like the console's (fiber stacks too): 10 of 11 runs clean. The 11th read a
      stale 192 on a long-lived stack, so fh1/src/fh1_trace_load.cpp adds a guard (WORKAROUND):
      an entry whose first word/list do not point just past the table falls back to entry 0,
      logged as "not a real entry". 6/6 clean with it (guard never fired). The real uninitialized
      read is upstream, probably in sub_82A7D730 (fills the vector at caller sp+384 whose w word
      is the index). --fh1_trace_load logs every table entry used.
- [x] First event (user, 2026-09-30, run-20260930-131706): whole race, no crash; the game opened
      cache:\ghost_stream_0..4, replay_stream and main_side_stream on the new utility mount. 8-13
      fps during the race (7.5-9 in the busy middle); felt like ~4 to the user.
- [ ] Seen in that race (user):
  - [ ] The car drives above the road, and cars clip into each other. Both are collision/ground
        queries; the save-loading crash was an uninitialized index coming out of the same area
        (sub_82A7D730 fills the vector whose w word is the surface index). Check first whether
        those queries return hits at all (a translation bug in the vector math would fit all three).
  - [ ] The HUD flashes sometimes (rendering; compare with ROV).
- [ ] Stop-gap speed before the native renderer (user's request, 2026-09-30). Festival, car still,
      Iris Plus: 7.4 fps, ~3,800 draws/frame, ~130 ms host GPU/frame (draws 62, EDRAM 19, resolves 17).
  - [x] --gpu_force_msaa_1x (clears the MSAA field of RB_SURFACE_INFO): works, all RTs 1x, but only
        7.4 -> 7.7 fps. MSAA is not the cost; the draw count is.
  - Predicated tiling (see the `[fps] tiling` line): the frame is replayed once per strip (bin
    select 3 / C / 30, plus 80000003 for the first pass). Per object the game emits EVENT_WRITE 0x19
    (reset extent), its draws, then EVENT_WRITE_EXT 0x1A to a 16-byte slot; its CPU code reads the
    slots back later and turns them into per-object bin masks (e.g. 28 = strips 2+3). The SDK
    writes a full-screen extent for every query, so most objects get mask FFFFFFFF and are drawn in
    all 3 strips (~4,250 predicated draws, ~3,590 run). Proven with --gpu_screen_extent_top_only
    (objects then vanish from the lower strips). Real extents (Y range of the draws between 0x19 and
    0x1A) would let predication skip them; the SDK's only tool is running the VS on the CPU
    (DrawExtentEstimator), too slow per vertex - needs sampling or a GPU-side approach.
  - media\renderscenarios.zip Global.xml has <TilingScenario value="1"/> (UI scenes use 0). Editing
    the file (byte + zip CRC) made the game call XamShowDirtyDiscErrorUI at boot - it verifies its
    data (not via zipmanifest.xml, which only has directory offsets). Next: override the value in
    memory where the game parses it (find the "TilingScenario" string's users), not on disk.
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
