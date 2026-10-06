Read fh1-recomp/CLAUDE.md, then fh1-recomp/docs/native-renderer-status.md ("Fourteenth session") and
fh1-nx/docs/running.md and building.md. This session continues the very early Switch build: **make it run on my
console and measure it.** It does not have to be playable or pretty. I want numbers: frames per second in the
logo videos, the menus, the opening and the first drive, and what the time goes to.

Where it stands (2026-10-06, night):

* The program builds: `fh1-nx\out\sw\fh1-nx.nro` (106 MB), and `fh1-nx\out\sd\switch\fh1-nx\` is the folder
  for the SD card (7.1 GB). It has not run on the console yet, unless I say so below.
* My console: Switch OLED, system 22.5.0, Atmosphere 1.11.2, more than 10 GB free. Settled, do not ask again.
* The tools are installed (MSYS2, devkitPro in `C:\msys64\opt\devkitpro`, Rust, the driver in `..\mesa-switch`).
  One command each: `fh1-nx\tools\build_nro.ps1`, `fh1-nx\tools\package_sd.ps1 -NoGame`.
* Not in the build yet: LTO, direct calls, function ordering, the vertex cache across frames, the cube faces in
  rotation, a render-queue wait, FFmpeg video, native audio functions (`fh1\src\native\README.md` has the list
  and the reasons).

Then, in this order:

1. If I bring a `logs` folder from the console (I will say where it is): read `logs\fh1_*.log`,
   `logs\rex\rex_stderr.log`, `rex_crash.log`, `watchdog.log`. A black screen or a crash is a result: find the
   cause, fix it, build, package with `-NoGame`, and tell me exactly which files to copy. Keep each round small:
   every round costs me a trip to the SD card.
2. When it shows the game: measure. The `[fps]` lines, the renderer's per-frame lines, the threads' load
   (`rex_profile.log`), at the logo videos, the menu, the opening and the first drive. Then tell me plainly:
   how many frames per second, what limits it (graphics chip, the command thread, the game's own threads,
   audio), and the first three things to do about it, with what each could gain according to StevensND's and
   GoatHonks' numbers.
3. Write it down: the status document, the Switch part of ROADMAP.md, CLAUDE.md, fh1-nx's documents.

Waiting, not for this session unless I ask: my drive with the 60 fps option on the PC and its faults, making
the PC frame faster, the sharp rectangle of ground under the car, the rare black window at the start, the
dark dashboard of the Volkswagen, the grey thumbnail, the pink triangles on the upgrade menu's tyre icons,
night colors, garage / car damage, the Carbon clean-up. If a GitHub issue has come in (gh issue list), tell me
what it says first; do not answer or close one without asking me.

Rules:

* Explain things to me in plain words: I am not a programmer. Say exactly what to run and when.
* The repository fh1-recomp is public: nothing committed may contain my Windows user folder or my e-mail
  address, and nothing of the game (no game file, no generated code, no shader library, no NRO).
* GoatHonks' repositories are private: credit him by name, never write a link to them. StevensND's nfsmw-nx,
  pinyon-shift and nfsuc-sw are public: credit by name and link. Keep `fh1\src\native\README.md`,
  THIRD_PARTY_NOTICES.md and the README's credits right for everything taken.
* The PC version must keep working: after a change in shared code, build the PC version and run one unattended
  test before going on, then build the NRO too (Switch-only SDK files are never compiled on the PC). Tests on
  the PC never use my saves or my installed copy (`--user_data_root=<an empty folder>`, the new-game route in
  `build_logs\fps60-autoplay.txt`); delete the test folders when done.
* The Switch can make no shader while the game runs, and the pipeline list is built before the game starts.
  Never make the game wait for a shader or a pipeline, and never let a frame come near 3 seconds.
* No 60 fps and no double-size smooth edges on the Switch. Measure at the console's normal clocks.
* Never give tools\auto_test.ps1 an option the script already passes, and never an empty -ExtraArgs.
* Before test windows open on the PC, say how many, and for each one whose saves it uses. Look at a shot of a
  test run before telling me it is fine.
* Tell me when a build is ready. I must not start the game while a build is running, and you cannot build
  while my game is open: check it with a command that prints "running" or "closed". Don't edit sources while a
  build is running (check the process list: a log can look finished while the last file compiles), and never
  wait with an open-ended loop.
* Downloads and installs are allowed; say the size before a large one.
* Write patch scripts to a file first, and keep a document's line endings when a script rewrites it.
* Nothing is published (no release, no tag, no upload) without my yes at that moment. A Switch build is not
  published at all until I say so; fh1-nx stays private.
* Commit and push when something works (shared code to fh1-recomp, Switch-only files to fh1-nx), and keep the
  status document, ROADMAP.md, CLAUDE.md and fh1-nx's README up to date.
* End with a summary of what you did and what is still open, plus a prompt for the session after.
