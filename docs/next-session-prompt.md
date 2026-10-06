# Prompt for the next session

Paste this to start the session after 2026-10-06 (pre-release 1 is published).

---

Read fh1-recomp/CLAUDE.md, then ROADMAP.md (Stage 3: the items marked "After the first pre-release") and
fh1-recomp/docs/native-renderer-status.md ("Pre-release 1 is published"). The first pre-release is out
(v0.1.0-pre1, https://github.com/AndryTheBeast/fh1-recomp/releases/tag/v0.1.0-pre1). This session we fix the
native renderer's faults and answer what players reported.

Do it in this order:
1. Look at the GitHub issues of the repository (gh issue list) and tell me what players reported, in plain
   words. Do not answer or close an issue without asking me.
2. Ask me whether I have driven more. If yes: merge my cache file into the shipped pipeline list
   (python tools\fh1_pipelines.py info fh1\data\fh1_pipelines.nfpl <my cache file>; then merge, build, commit).
   My installed copy is in %USERPROFILE%\Downloads\FH1 (its list: cache\fh1_native_pipelines.bin).
3. The faults, one at a time, each checked against the emulated GPU at the same spot before any guess (RenderDoc
   on both renderers, pass by pass):
   - the ground missing at the Montano Plains outpost and the dark patch east of it;
   - upside-down scenery patches in a mountain race;
   - the car's dashboard that never lights up (ask me first whether it lights up with the emulated launcher; a
     lead, not checked: the log line "texture format not supported yet: an empty one is used (cause 422)");
   - a car's thumbnail that is sometimes wrong (timing: start at fh1_native_read_one_off_wait_texels);
   - then night colors, garage / car damage, the Carbon clean-up.
4. Objects that show late in a first run: the shaders that are not on the disc are made one at a time, about a
   second each (fh1_extra_shaders.cpp, one worker thread). Make several at once and measure it on an installed
   test copy with the driver's cache set aside.
5. When a fix is worth giving to players: tell me, and only with my yes make the next pre-release (v0.1.0-pre2:
   Program.Version in the installer, a new release text, installer\build_installer.ps1,
   installer\make_package.ps1, the complete test, then gh release create).

Left for later, only if I ask: the game window's title still shows the SDK's build name; the installer's unpack
step flashes the game window for a second; the "Preparing shaders" screen uses small text and does not show on
the emulated launchers; the emulated Vulkan monitor's title line is the backend's own long name; about 206 draws
rejected in every run of the new-game route (cause 317).

Rules:
- Explain things to me in plain words: I am not a programmer. Say exactly what to run and when.
- The repository is public: nothing committed may contain my Windows user folder or my e-mail address.
- StevensND's installer is the model for the app: credit him by name; his installer and nfsmw-nx are public
  (reference/nfsmw-README.md has the links). GoatHonks' repositories are private: credit him by name, never
  write a link to them.
- The installer is written for the C# 5 compiler that is part of Windows: no newer language features.
- Tests never use my saves or my installed copy: a new empty folder on the Desktop, and a game started for a
  test gets --user_data_root=<another empty folder>. My save is no longer in Documents (the game starts a new
  game), so the unattended route is the new-game one (Start / A every few seconds; the first drive at about
  140 s). An installer test overwrites my desktop shortcut: copy it first and put it back. Delete the test
  folders when done.
- A test about first runs must set the graphics driver's cache aside (tools\fresh_pc_test.ps1 shows how) and
  put it back: with the cache there, everything is "already compiled" and the test proves nothing.
- Never give tools\auto_test.ps1 an option the script already passes (an option given twice makes the game drop
  all of them), and never an empty -ExtraArgs. A plain run is the native renderer: an emulated reference shot
  needs -ExtraArgs "--fh1_renderer=xenos".
- Before test windows open, say how many, and for each one whose saves it uses.
- Tell me when a build is ready. I must not start the game while a build is running, and you cannot build while
  my game is open: check it with a command that prints "running" or "closed", and ask me to close it. Don't edit
  sources while a build is running, and never wait with an open-ended loop. Changing fh1\CMakeLists.txt gives
  the long build (about 14 minutes).
- After any change to how fh1.exe draws, check the picture against the emulated GPU at the same moment
  (tools\fh1_pic_stats.py); two runs drift apart by seconds, so take shots every few seconds and look at them.
- Write patch scripts to a file first, and keep a document's line endings when a script rewrites it.
- Nothing is published (no release, no tag, no upload) without my yes at that moment.
- Commit and push to main when something works, and keep the status document, ROADMAP.md and CLAUDE.md up to
  date.
- End with a summary of what you did and what is still open, plus a prompt for the session after.
