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
        **AMD Z1 Extreme (Legion Go, 2026-09-30): both gone on RTV** (clean corner, soft car shadow):
        an Intel-only problem. ROV there: 6-10 fps, GPU-bound on draws.
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
  - [x] FIXED 2026-10-01 (vmsum3fp128/4fp128 overflow -> QNaN, see below; verified by the user:
        cars, signs, placards collide, bonus boards are detected again; run-fixcheck-20261001-
        021426: GJK 4.6 iterations avg, type-2 pairs with contacts, EPA results valid, 30 fps).
        The floating was a low-fps effect (gone at 30 fps).
        Was: The car drives above the road, and cars clip into each other. Both are collision/ground
        queries; the save-loading crash was an uninitialized index coming out of the same area
        (sub_82A7D730 fills the vector whose w word is the surface index). Check first whether
        those queries return hits at all (a translation bug in the vector math would fit all three).
        2026-10-01, Legion Go race at 30 fps (run-20261001-000832): the car no longer floats
        (was a low-fps effect), but cars pass COMPLETELY through each other, always, all cars
        (AI + traffic) and some objects; ground and world geometry collide fine.
        Checked, all fine: upstream's PPC instruction suite (sdk/tests/ppc, 1458 cases, run with
        tools/run_ppc_tests.ps1) passes; vmaddcfp128/vnmsubfp128/vcsxwfp128/fsel/fcmpu/dcbz/mftb
        read correctly; no physics file is missing; the collision tunables have sane values
        (--fh1_watch_tunables=ollision,host: PostResetNonCollideTime 3, CollisionsOffTime 2,
        CollisionBias* 0.5/1, CollisionSphereRadius 0.5). Tools: --fh1_dump_image writes the
        loaded image for offline analysis; tunables are registered by sub_82C1A110 (float),
        sub_82C096E0 (bool), sub_82C09468 (r4 name, r5 variable). The game has a CollisionMode
        (Default/AlwaysOn/AlwaysOff/Ghosts, used in sub_826063E0 / sub_8260A4D8): logged
        AlwaysOn (--fh1_watch_collision_mode). User: no sound, no damage, only a camera flick.
        ROOT CAUSE (2026-10-01): vmsum3fp128/vmsum4fp128 used the host dot product (dp_ps),
        which gives +inf on float32 overflow; the console (and Xenia, OPCODE_DOT_PRODUCT_3/4)
        sums in float64 and returns QNaN when a finite sum overflows float32. Chain, found with
        --fh1_trace_epa (fh1/src/fh1_trace_epa.cpp): narrow phase sub_82D42638 dispatches pairs
        through the 6x6 table at 832AF138 by shape type; type 1x1 (sub_82D29BE8) made contacts,
        every pair with a type-2 (convex) shape made none (sub_82D1AAE8, sub_82D44B90). Their GJK
        sub_82D43290 stopped every test in iteration 0: the empty simplex holds v = -FLT_MAX, the
        progress test sub_82D420F0 computes |v|^2 with vmsum3fp128 and stops when
        eps*FLT_MAX >= FLT_MAX - |v|^2 - true for +inf, false for NaN. So GJK always answered
        "apart" (identical witness points, NaN normals, unwritten surface index = the old
        save-loading crash's 0xBE index). Fixed in sdk/include/rex/ppc/intrinsics.h
        (simde_mm_vmsum3fp/4fp) + builders; PPC tests added (overflow -> 7FC00000).
        Tools left for later: --fh1_nan_trap=N (logs code sites that create NaNs; very noisy, the
        rsqrt Newton step and (x-a)/(b-a) idioms make NaNs on purpose), --fh1_watch_tunables=*.
        Legion Go (2026-10-01): the user did not see the car float in the first 30 fps run. New
        lead: both may be large-physics-step artifacts of the 7-9 fps runs (suspension settling
        wrong, cars tunnelling between checks). Re-check in a race at 30 fps before digging.
        Audit so far (sdk/src/codegen/builders/vector.cpp): the most used vector/FP builders
        (vcmp*, vrlimi, vsldoi, vperm*, vsplt*, vmsum*, vupkd3d128 types 0-5, fma) match the PPC
        semantics. Real but harmless: vupkd3d128/vpkd3d128 type 6 (NORMPACKED64) read the wrong
        64-bit half, write x..w reversed and skip the 3.0+X form - FH1 never uses it (0 sites).
  - [ ] The HUD flashes sometimes (rendering; compare with ROV). Xenia users see HUD flashing
        in FH1 when the frame pacing is off (vsync off / >30 fps, game-compatibility issue 30);
        ours was seen at 7-9 fps. Re-check at 30 fps.
- [ ] Stop-gap speed before the native renderer (user's request, 2026-09-30). Festival, car still,
      Iris Plus: 7.4 fps, ~3,800 draws/frame, ~130 ms host GPU/frame (draws 62, EDRAM 19, resolves 17).
      Legion Go (Z1 Extreme), RTV, festival + free-roam driving: 28-30 fps = the game's 30 fps cap,
      28 ms host GPU/frame (draws 11, EDRAM 7, resolves 4.5, textures 3), GPU thread busy 100%
      (13 ms of CPU time in ~3,300 draws). Not needed on this PC; kept for weaker GPUs / Switch.
  - Races dip to 22-26 fps (user's race, run-20261001-000832): up to 5,600 draws/frame, host GPU
    26-28 ms (not the limit), GPU command thread 100% busy. Profiled 2026-10-01 night with the
    new --fh1_profile=N (fh1/src/fh1_profiler.cpp, in-process sampler) at Red Rock, car parked:
      - game main thread 99% busy, about half of it waiting for the GPU thread: sub_829F04A8
        (spin loop, cctpl/db16cyc, polls the ring read pointer) + sub_823E91F0 (waits for ring
        space). sub_82438EA8 = CRT pow(), 9% - candidate for a host pow() later.
      - GPU thread: ~16% of wall time writing registers one by one from type-0 packets ->
        now WriteRegisterRangeFromRing (bulk constant path): busy 57% -> 48%, same picture.
        ~15% is inside the AMD driver (names like GetSettingsBlobsAll = nearest driver export).
      - --gpu_log_waits: WAIT_REG_MEM time is nearly all on one memory word (1FCA4006 at Red
        Rock), twice per frame = the game's vblank pacing; only Sleep() overshoot (<1 ms) is lost.
    The "failed draws" (6/frame, edram_mode=6) are the empty resolves of the other strips, not
    tessellation - harmless.
    2026-10-01 morning, user's race (run-race-20261001-111628): busy race scenes 17-22 fps with
    the HOST GPU as the limit there (37 ms/frame: draws 16, EDRAM 8, textures 4.4). The log was
    90 MB in 15 min (empty-resolve / failed-copy errors 12x per frame, XMA unknown-register and
    XamXStudioRequest stub spam) - now rate-limited (~9 MB per session).
  - [x] Garage: car lights bloomed into white streaks; Car Select photos of newly bought cars were
        garbage (Thumbnail_N.xdc in the save = tiled 768x288 8888 texture, saved as garbage).
        Both were CPU reads of resolve results (auto exposure, photo save) with readback off.
        fh1_app.h now defaults --readback_resolve=some (no measurable cost); "full" stalls
        (unplayable) and changes nothing more. Photos saved before the fix stay broken.
  - [x] Blank rear plates: normal - the user checked Xbox 360 gameplay footage, no car shows a
        plate there either. Not a bug.
    Soft rectangle under the car: RTV and ROV render the same (test-rectrtv/rectrov), so not a
    host-render-target shortcut.
  - [x] --gpu_force_msaa_1x (clears the MSAA field of RB_SURFACE_INFO): works, all RTs 1x, but only
        7.4 -> 7.7 fps. MSAA is not the cost; the draw count is.
        Legion Go 2026-10-01 (run-msaa1x-20261001-121649): NOT usable - glowing blue garbage on
        the car's rear (plate area, lights). The game lays out its EDRAM render targets for 4x
        sample sizes; forcing 1x makes surfaces overlap. The single-strip experiment depends on
        it too, so both need a real EDRAM-layout-aware approach (or the native renderer).
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
    data (not via zipmanifest.xml, which only has directory offsets). Changing only that byte and
    keeping both CRCs also fails: the entry's content is verified. So: in memory only.
    The string is at 8223C180 (UITilingScenario 8223C1B4; second copies at 832B1CE2/832B1D16).
    Its only user in code is sub_82D80DD8, a one-line "return name" method, which nothing in the
    image or the code references (0 hits for the word 82D80DD8; no lis/addi pair builds it) -
    the settings table is built some other way. Next options: find the D3D BeginTiling-style
    code that emits SET_BIN_SELECT 80000003/C/30 and force one tile together with
    --gpu_force_msaa_1x (1280x720 at 1x fits EDRAM: 720+720 tiles), or scan the heap for the
    parsed settings block after load. --fh1_find_string="text,0xWORD" helps with both.
  - One strip instead of three (2026-09-30), fh1/src/fh1_trace_load.cpp --fh1_single_tile (with
    --gpu_force_msaa_1x): the D3D command-list walker sub_829F5FF0 copies a 248-byte tiling block
    into device+116 with memcpy sub_82A7D730 (return address 829F60DC): +4 strip count (3), +8
    strip rects x1,y1,x2,y2 (0,0,1280,256 / 0,256,1280,512 / 0,512,1280,720); device+60 = strip
    being replayed, +364 = bin select. Setting count 1 + rect 0..720 works mechanically: one pass,
    draws 4,250 -> 2,300/frame, 7.4 -> 8-9 fps; with --gpu_bin_select_or=3C (all strip packets
    run) 13.3 fps. But the picture is wrong: only one band is right, the rest black.
    Per-strip packets (--gpu_trace_pm4_strip_packets / _from_copy_dest traces):
      - pass start and per object: PA_SC_WINDOW_OFFSET 0x2080 = 0 / 7F000000 (-256) / 7E000000
        (-512), PA_SC_WINDOW_SCISSOR_TL/BR = rows 0-256 / 256-512 / 512-720 (op 2D and op 55,
        predicated mask 3 / C / 30 in sequence);
      - each copy-out: RB_COPY_DEST_BASE 0x2319 for strip 1/2/3 (e.g. 1C4E1000 / 1C621000 /
        1C761000, one strip apart), then ONE unpredicated resolve (op 36 = DRAW_INDX_2, 00030088)
        and EVENT_WRITE 6.
    Next: in single-tile mode run only strip 1's packets (select 80000003) but force window
    offset 0 and scissor BR to the full height, so draws cover the frame and the one resolve
    copies all 720 rows to strip 1's base (= the start of the image). Check whether the resolve's
    own rectangle is full-frame or per strip.
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
