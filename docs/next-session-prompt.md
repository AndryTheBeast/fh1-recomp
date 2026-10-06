# Prompt for the next session

Read fh1-recomp/CLAUDE.md, then ROADMAP.md ("Maybe later": the first pre-release and its sub-items),
fh1-recomp/docs/install.md and fh1-recomp/docs/release-notes-v0.1.0-pre1.md. This session is about publishing
the first pre-release of the port (a Windows installer app), or fixing what my checks of it found. The other
native renderer fixes (night colors, garage / car damage, Carbon clean-up, the three faults of 2026-10-06:
missing ground at the Montano Plains outpost and east of it, upside-down scenery patches in a mountain race) are
for after the pre-release: do not start them.

Done already (check it in the code before you trust it): the installer (`installer\`, C# Windows Forms, built
by `installer\build_installer.ps1`) does a whole installation: "Needed on this PC" with a button for the Visual
C++ runtime, the ISO check with the USA-only warning, the folder check, the disc copy, the download of the two
zips (or the folder `package` next to it), the shader library built on the PC, FH1.exe with a desktop shortcut,
two .bat files (emulated Direct3D 12, emulated Vulkan) and a Read me.txt; "Update" on a folder that holds an
installation. **FH1.exe starts the native renderer by default** (my decision of 2026-10-06: I need feedback on
it for the Switch port, and the emulated version will be deprecated at some point; do not propose the emulated
default again). The emulated GPU is `--fh1_renderer=xenos`. `installer\make_package.ps1` makes the release's files in `installer\out\release`. fh1.exe makes
the shaders the disc does not have while the game runs (`shaders_extra`), finds the folder `game` next to
itself, writes its log and crash report into `logs`. The install guide, known issues, release text and issue
form are written.

Ask me first what my checks gave:
- the installer window on my installed copy (C:\Users\andre\Downloads\FH1: the button should read Update);
- the first-person view and the paint booth with FH1.exe of that copy (the native renderer; its shader library
  has none of the run-time shaders: the game makes them);
- the two .bat files (emulated Direct3D 12, emulated Vulkan), the mouse pointer hiding, and F3 on all three;
- whether I read docs\install.md and the release text and want changes.

Then, in this order:
1. If I say I have driven more: merge my cache file into the shipped pipeline list
   (python tools\fh1_pipelines.py info fh1\data\fh1_pipelines.nfpl
   fh1\out\win-release\cache\fh1_native_pipelines.bin; then merge, build, commit), and make the release files
   again (`installer\make_package.ps1`).
2. Fix what my checks found.
3. Small things, if I want them before the release: the game window's title still shows the SDK's build name
   ("fh1 [rexglue-v0.10.0.0-dev...]"); the unpack step of the installer flashes the game window for a second;
   the "Preparing shaders" screen uses the overlay's small text; the emulated Vulkan monitor's title line is the
   backend's own long name.
4. The release: tell me exactly what will be published (tag v0.1.0-pre1, marked pre-release, the four files of
   installer\out\release with their sizes and checksums, the text of docs\release-notes-v0.1.0-pre1.md) and wait
   for my yes at that moment. Do not publish anything (no release, no tag, no upload) without it.
5. After the release: one real test with FH1Installer.exe alone (no `package` folder next to it) into a new
   empty folder, to see the download from GitHub work.

Rules:
- Explain things to me in plain words: I am not a programmer. Say exactly what to run and when.
- StevensND's installer is the model for the app: credit him by name; his installer and nfsmw-nx are public
  (reference/nfsmw-README.md has the links). GoatHonks' repositories are private: credit him by name, never
  write a link to them.
- The installer is written for the C# 5 compiler that is part of Windows: no newer language features.
- Installer tests go into a new empty folder on the Desktop, never into game_root or my installed copy, and a
  game started from a test folder gets `--user_data_root=<another empty folder>` so my saves are never touched.
  Delete the test folders (and a test shortcut on the desktop) when done.
- The unattended test (tools\auto_test.ps1) only boots into my save: look at a shot before comparing pictures.
  It must never save a design, buy anything or confirm a dialog. Never give it an option the script already
  passes (an option given twice makes the game drop all of them), and never an empty -ExtraArgs. A plain run is
  the native renderer now: an emulated reference shot needs -ExtraArgs "--fh1_renderer=xenos".
- Before test windows open, say how many, and for each one whose saves it uses.
- Tell me when a build is ready. I must not start the game while a build is running, and you cannot build while
  my game is open: check it with a command that prints "running" or "closed", and ask me to close it. Don't edit
  sources while a build is running, and never wait with an open-ended loop.
- After any change to fh1.exe, check that the picture did not change against the emulated GPU at the same second
  (tools\fh1_pic_stats.py), with new emulated shots at my save's current spot.
- Nothing given to other people may contain a path with my Windows user name (make_package.ps1 warns).
- Write patch scripts to a file first, and keep a document's line endings when a script rewrites it.
- Commit and push to main when something works, and keep the status document, ROADMAP.md and CLAUDE.md up to
  date.
- End with a summary of what you did and what is still open, plus a prompt for the session after.
