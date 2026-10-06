Read fh1-recomp/CLAUDE.md, then ROADMAP.md ("Later - Nintendo Switch", "Other projects to borrow from") and
fh1-recomp/docs/native-renderer-status.md (its first two sections). This session is about one thing: **a very
early build of Forza Horizon for the Nintendo Switch, only to see how fast it runs.** It does not have to be
playable or pretty. I want numbers: frames per second in the logo videos, the menus, the opening and the first
drive, and what the time goes to.

What is known before starting:

* Nothing has been built for the Switch from our code yet. The Switch parts inherited from nfsmw-nx are still
  in the repository, untouched (`sdk` Horizon layer, `tools\switch`, `mesa`, `shaders`, `docs\nfsmw-nx`).
  devkitPro (the Switch compiler and library) is not installed on this PC.
* The route to copy is GoatHonks' Switch port of Carbon, the same kind of port as ours: `..\repos\nfsc-nx-main`
  (its `docs\building.md`, `cmake\switch-devkitA64.cmake`, `tools\build_nro.ps1`, `tools\package_sd.ps1`,
  `tools\make_toml.py`, `mesa`, `docs\platform-notes.md`, `docs\console-run-log.md`,
  `docs\performance-history.md`) and `..\repos\nfsc-recomp-main` (his `docs\carbon\switch-from-most-wanted.md`
  and ROADMAP). StevensND's own notes are in `docs\nfsmw-nx` here (building, platform notes, measuring,
  performance history, Mesa). Read these before writing anything.
* Use their fixes, do not reinvent them. From StevensND and GoatHonks, the ones made for the Switch's speed:
  no busy waiting on the render thread, the vertex cache kept across frames (`nfsc_native_vertex_cache.h`), the
  car's reflection cube drawn a few faces per frame (`nfsc_native_cube_faces.h`), the FFmpeg video decoder, the
  native audio filter, direct calls / LTO / function ordering, their swapchain and thread-priority settings,
  the Mesa / NVK patch. Take first what the build needs to start at all, then what their run logs say gave the
  most. Port by hand and say for each one whether it was taken, changed or left out, and why
  (`fh1\src\native\README.md` keeps that list).
* nfsuc-sw (`..\repos\nfsuc-sw-main`, Carbon from the original Xbox on the Switch) is a different kind of port:
  only its lessons about the console itself apply (its CLAUDE.md, "Findings"). No license file: ideas only.
* The Switch can make no shader and no pipeline while the game runs. Everything must be made on the PC first:
  the shader library with the shaders that are not on the disc (the 42 `synth` ones and whatever
  `shaders_extra` holds from my PC runs), in the form its graphics driver takes, and the pipeline list
  (`fh1\data\fh1_pipelines.nfpl`, 1067) built at start, never during play.
* What is off on the Switch: the 60 fps option (my decision: PC only), the smooth edges at double size
  (`fh1_native_ssaa`), the emulated GPU. A frame near 3 seconds stops the game for good.
* Expect it to be very slow. On my Legion Go a driving frame costs about 20 ms of graphics chip time and the
  Switch's chip is 20 to 50 times weaker. GoatHonks' Carbon, a lighter game, runs at 18-25 fps there after a
  lot of tuning. A slideshow is an acceptable result for this session: the numbers are the goal.

My answers of 2026-10-06 (settled, do not ask again):

1. **The console**: my Switch runs HOS 22.5.0 with Atmosphere 1.11.2 (custom firmware, homebrew works) and I
   have the SD card. Not said yet: which model it is (the first one, the revised one, Lite or OLED) and how
   much is free on the card (about 10 GB are needed): ask me these two once, at the start. Check that the
   devkitPro / libnx and Mesa versions you take work on that firmware.
2. **Downloads**: yes, you can download and install anything (devkitPro with devkitA64 and libnx, MSYS2, Rust,
   the Vulkan SDK if needed). I am on a phone hotspot, so still say the size before a large one.
3. **Where the Switch files live**: a new repository, **fh1-nx** (https://github.com/AndryTheBeast/fh1-nx,
   private for now; local folder `FH1-recomp\fh1-nx`). It is laid out like GoatHonks' nfsc-nx: it holds only
   what the Switch needs and the PC does not, and takes fh1-recomp in as the git submodule `recomp\`. Today
   it has a README, the license, ignore rules and the submodule. Shared fixes (SDK, app code, renderer) go into
   fh1-recomp first, are checked on the PC, then reach the Switch by moving the submodule. Switch-only files
   (toolchain file, NRO build, Mesa patch, SD package tools, Switch documents) go into fh1-nx; the Switch
   pieces fh1-recomp inherited from nfsmw-nx (`tools\switch`, `mesa`, `docs\nfsmw-nx`) stay where they are
   until you tell me the plan for them. It stays private until I say otherwise.

Then do it in this order:

1. Tell me the plan in plain words before changing code: the steps, what I have to do by hand at each one
   (installing, copying to the SD card, starting the game, bringing back the log), and what could stop us.
2. Make it compile and link for the Switch (an NRO). Say exactly what was changed for it.
3. Make the SD card package: the NRO, the game's files, the shader library, the pipeline list, the settings
   file. Tell me the exact folder layout and how to start it (GoatHonks' and nfsuc-sw's notes say applet mode has
   too little memory: title takeover).
4. First run on the console: I start it and bring you the log. A black screen or a crash is a result too: read
   the log, fix, repeat. Keep each round small, because every round costs me a trip to the SD card.
5. When it shows the game: measure. The `[fps]` lines, the per-frame lines of the native renderer, the threads'
   load, at the logo videos, the menu, the opening and the first drive. Then tell me plainly: how many frames
   per second, what limits it (graphics chip, the command thread, the game's own threads, audio), and what
   the first three things to do about it would be, with what each could gain according to StevensND's and
   GoatHonks' numbers.
6. Write it down: a section in the status document, the Switch part of ROADMAP.md, CLAUDE.md, and a
   document for the Switch build's steps (so the next build is one command).

Waiting, not for this session unless I ask: my drive with the 60 fps option on the PC and its faults, making
the PC frame faster, the sharp rectangle of ground under the car, the rare black window at the start, the
dark dashboard of the Volkswagen, the grey thumbnail, the pink triangles on the upgrade menu's tyre icons,
night colors, garage / car damage, the Carbon clean-up. If a GitHub issue has come in (gh issue list), tell me
what it says first; do not answer or close one without asking me.

Rules:

* Explain things to me in plain words: I am not a programmer. Say exactly what to run and when.
* The repository is public: nothing committed may contain my Windows user folder or my e-mail address, and
  nothing of the game (no game file, no generated code, no shader library, no NRO built from the game).
* GoatHonks' repositories are private: credit him by name, never write a link to them. StevensND's nfsmw-nx,
  pinyon-shift and nfsuc-sw are public: credit by name and link. Keep `fh1\src\native\README.md`,
  THIRD_PARTY_NOTICES.md and the README's credits right for everything taken.
* The PC version must keep working: after a change in shared code, build the PC version and run one unattended
  test before going on. Tests on the PC never use my saves or my installed copy (`--user_data_root=<an empty
  folder>`, the new-game route in `build_logs\fps60-autoplay.txt`); delete the test folders when done.
* Never make the game wait for a shader or a pipeline, and never let a frame come near 3 seconds.
* Never give tools\auto_test.ps1 an option the script already passes, and never an empty -ExtraArgs.
* Before test windows open on the PC, say how many, and for each one whose saves it uses. Look at a shot of a
  test run before telling me it is fine.
* Tell me when a build is ready. I must not start the game while a build is running, and you cannot build
  while my game is open: check it with a command that prints "running" or "closed". Don't edit sources while a
  build is running, and never wait with an open-ended loop.
* Downloads and installs are allowed (my yes of 2026-10-06); say the size before a large one.
* Write patch scripts to a file first, and keep a document's line endings when a script rewrites it.
* Nothing is published (no release, no tag, no upload) without my yes at that moment. Releases are named
  "Alpha - <version>" with tag `alpha-<version>`. A Switch build is not published at all until I say so.
* Commit and push when something works (shared code to fh1-recomp, Switch-only files to fh1-nx), and keep
  the status document, ROADMAP.md, CLAUDE.md and fh1-nx's README up to date.
* End with a summary of what you did and what is still open, plus a prompt for the session after.
