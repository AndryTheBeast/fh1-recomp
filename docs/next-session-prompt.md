# Prompt for the next session

Read fh1-recomp/CLAUDE.md, then ROADMAP.md ("Maybe later": the first pre-release and its sub-items),
fh1-recomp/docs/install.md and fh1-recomp/docs/release-notes-v0.1.0-pre1.md. **This session we publish the first
pre-release of the port** (tag v0.1.0-pre1, a Windows installer app). The other native renderer fixes (night
colors, garage / car damage, Carbon clean-up, the three faults of 2026-10-06: missing ground at the Montano
Plains outpost and east of it, upside-down scenery patches in a mountain race) are for after the pre-release:
do not start them.

Done already (check it in the code before you trust it):
- The installer (`installer\`, C# Windows Forms, built by `installer\build_installer.ps1`) does a whole
  installation: "Needed on this PC" with a button for the Visual C++ runtime, the ISO check with the USA-only
  warning, the folder check, the disc copy, the download of the two zips (or the folder `package` next to it),
  the shader library built on the PC, FH1.exe with a desktop shortcut, two .bat files (emulated Direct3D 12,
  emulated Vulkan) and a Read me.txt; "Update" on a folder that holds an installation.
- `installer\make_package.ps1` makes the release's four files in `installer\out\release` (FH1Installer.exe,
  fh1-win64.zip, fh1-shader-tools.zip, SHA256SUMS.txt) and warns when a file names my Windows user folder.
- **FH1.exe starts the native renderer by default** (my decision: I need feedback on it for the Switch port, and
  the emulated version will be deprecated at some point; do not propose the emulated default again). The
  emulated GPU is `--fh1_renderer=xenos`. Without its shader library FH1.exe falls back to the emulated GPU.
- fh1.exe makes the shaders the disc does not have while the game runs (`shaders_extra`), finds the folder
  `game` next to itself, and writes its log and crash report into `logs` (logging is on in the pre-releases so
  that players can attach the log to a GitHub issue).
- The icons: FH1.exe and the installer use my picture (`FH1-recomp\icon-source.png`, the yellow car), as
  `fh1\res\fh1_local.ico` and `installer\fh1_installer_local.ico`. My decision, discussed: do not ask again.
  Those two files are ignored by git (the repository holds a drawn icon); the builds on my PC use mine.
- The install guide, known issues, release text, README section and the issue form that asks for the log.

Do it in this order:
1. Ask me what my checks gave, and fix what they found before anything is published:
   - the installer window on my installed copy (C:\Users\andre\Downloads\FH1: the button should read Update;
     afterwards FH1.exe, the two .bat files, Read me.txt, the desktop shortcut, the new icons);
   - FH1.exe of that copy (native renderer): first-person view and the paint booth (its shader library has none
     of the run-time shaders: the game makes them); the two .bat files; the mouse pointer hiding; F3 on all three;
   - whether I read docs\install.md and docs\release-notes-v0.1.0-pre1.md and want changes.
2. If I say I have driven more: merge my cache file into the shipped pipeline list
   (python tools\fh1_pipelines.py info fh1\data\fh1_pipelines.nfpl
   fh1\out\win-release\cache\fh1_native_pipelines.bin; then merge, build, commit).
3. Make the release's files fresh, from a build of the newest commit: check that the game is closed, build,
   `installer\build_installer.ps1`, `installer\make_package.ps1` (no warning about my user name allowed), and
   check that main is pushed and the working tree is clean except fh1\fh1_manifest.toml.
4. One last complete test before publishing: FH1Installer.exe alone (copied to an empty folder, so no `package`
   next to it) with `--source <installer\out\release>` and `--install`, into a new empty folder on the Desktop;
   then start the installed FH1.exe for 40 s with `--user_data_root=<another empty folder>` and read its log.
   Delete the test folders and the test shortcut afterwards.
5. **The release.** Tell me exactly what will be published and wait for my yes at that moment:
   - the tag v0.1.0-pre1 on the newest commit of main, the title "Forza Horizon recomp - pre-release 1",
     marked as a pre-release;
   - the four files with their sizes and SHA-256 checksums;
   - the release text (the part of docs\release-notes-v0.1.0-pre1.md below its line of dashes);
   - that the icon of both programs is my picture.
   Only after my yes: create the release with the GitHub CLI (gh release create ... --prerelease) and upload the
   four files. Do not publish anything (no release, no tag, no upload) without that yes.
6. After the release: download FH1Installer.exe from the release page and run it by itself (no `package` folder,
   no --source) into a new empty folder, to see the real download from GitHub work; compare the downloaded
   files with SHA256SUMS.txt. Tell me what to click to try it myself, including the SmartScreen prompt. Then
   update ROADMAP.md, CLAUDE.md and the status document, and write the prompt for the session after (the native
   renderer's faults, starting with the three of 2026-10-06, and whatever players report).

Left for later, only if I ask: the game window's title still shows the SDK's build name; the installer's
unpack step flashes the game window for a second; the "Preparing shaders" screen uses small text; the emulated
Vulkan monitor's title line is the backend's own long name.

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
  sources while a build is running, and never wait with an open-ended loop. Changing fh1\CMakeLists.txt gives
  the long build (about 14 minutes).
- After any change to fh1.exe, check that the picture did not change against the emulated GPU at the same second
  (tools\fh1_pic_stats.py), with new emulated shots at my save's current spot.
- Nothing given to other people may contain a path with my Windows user name.
- Write patch scripts to a file first, and keep a document's line endings when a script rewrites it.
- Commit and push to main when something works, and keep the status document, ROADMAP.md and CLAUDE.md up to
  date.
- End with a summary of what you did and what is still open, plus a prompt for the session after.
