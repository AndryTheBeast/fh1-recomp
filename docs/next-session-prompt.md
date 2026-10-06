# Prompt for the next session

Paste this to start the session after 2026-10-06 (twelfth session: several shaders made at once and the
pipeline list at 1067 are on main, not in a release yet).

---

Read fh1-recomp/CLAUDE.md, then ROADMAP.md (Stage 3: the items marked "After the first pre-release" and the
ones under them) and fh1-recomp/docs/native-renderer-status.md ("Twelfth session"). The files of v0.1.0-pre1
(https://github.com/AndryTheBeast/fh1-recomp/releases/tag/v0.1.0-pre1) are still those of 2026-10-06's eleventh
session. On main since then, tested but not published: shaders the library lacks are made up to four at once,
the pipeline list's records that waited for one are compiled the moment it is taken in, and the shipped list
has 1067 pipelines (887 before).

Do it in this order:

1. Ask me whether I want those three things given to players now (replace the files of the pre-release as on
   2026-10-06, or v0.1.0-pre2: Program.Version in the installer, a new release text,
   installer\build_installer.ps1, installer\make_package.ps1, the complete test, then gh release create).
   Nothing is published without my yes at that moment.
2. Look at the GitHub issues (gh issue list) and tell me what players reported, in plain words. Do not answer
   or close an issue without asking me. (None on 2026-10-06.)
3. Ask me whether I have driven more. If yes: python tools\fh1_pipelines.py info fh1\data\fh1_pipelines.nfpl
   fh1\out\win-release\cache\fh1_native_pipelines.bin, then merge, build, commit.
4. The black window: rarely (2 of 319 starts) the window stays black for the whole run while the game runs;
   the log has "Presenter: paint mode -> none" in the first seconds (sdk/src/ui/presenter.cpp,
   PaintFromUIThread). Read who is meant to bring painting back, and make it come back. Changing the SDK gives
   the long build.
5. The other faults, one at a time, each checked against the emulated GPU at the same spot before any guess
   (RenderDoc on both renderers, pass by pass):
   * the car's dashboard: dark with dim dials on the Volkswagen, lit on the Subaru, dark at dusk on the Mustang
     (my pictures: build_logs\reference\user-dashboard-*-20261006.webp; ask me whether the Volkswagen's lights
     up with the emulated launcher; a lead, not checked: the log line "texture format not supported yet: an
     empty one is used (cause 422)");
   * a car's thumbnail that is sometimes a grey card (timing: start at fh1_native_read_one_off_wait_texels;
     user-thumbnail-grey-20261006.webp);
   * the pink triangles on the tyre icons of the upgrade menu (user-upgrade-tyres-pink-20261006.webp: compare
     with the emulated picture first, they may be the game's own);
   * the ground missing at the Montano Plains outpost and the dark patch east of it (ask me first whether it is
     still there: the developer's build had no shader tools until 2026-10-06, and that gave flat ground);
   * upside-down scenery patches in a mountain race (same question first);
   * then night colors, garage / car damage, the Carbon clean-up.

Left for later, only if I ask: the game window's title still shows the SDK's build name; the installer's unpack
step flashes the game window for a second; the "Preparing shaders" screen uses small text and does not show on
the emulated launchers; the emulated Vulkan monitor's title line is the backend's own long name; the folders
%LOCALAPPDATA%\AMD\VkCache.test-* that the first-run tests leave behind (driver caches of test runs: mine to
delete); the festival's crowd after the bone change (I have not looked: it must still be animated and whole).

Rules:

* Explain things to me in plain words: I am not a programmer. Say exactly what to run and when.
* The repository is public: nothing committed may contain my Windows user folder or my e-mail address.
* StevensND's installer is the model for the app: credit him by name; his installer and nfsmw-nx are public
  (reference/nfsmw-README.md has the links). GoatHonks' repositories are private: credit him by name, never write
  a link to them.
* The installer is written for the C# 5 compiler that is part of Windows: no newer language features.
* Tests never use my saves or my installed copy: a new empty folder on the Desktop, and a game started for a
  test gets --user_data_root=<another empty folder>. The unattended route is the new-game one (CLAUDE.md,
  twelfth session, has the timetable). An installer test overwrites my desktop shortcut: copy it first and put
  it back. Delete the test folders when done.
* A test about first runs must set the graphics driver's cache aside (tools\fresh_pc_test.ps1 shows how) and
  put it back. Run it on an installed test copy, and never make the game wait for a shader or a pipeline: a
  frame of about 3 seconds stops the game for good.
* Never give tools\auto_test.ps1 an option the script already passes, and never an empty -ExtraArgs. A plain
  run is the native renderer: an emulated reference shot needs -ExtraArgs "--fh1_renderer=xenos".
* Before test windows open, say how many, and for each one whose saves it uses. Look at a shot of a test run
  before telling me it is fine: a black window is a fault, not a slow start.
* Tell me when a build is ready. I must not start the game while a build is running, and you cannot build
  while my game is open: check it with a command that prints "running" or "closed", and ask me to close it.
  Don't edit sources while a build is running, and never wait with an open-ended loop. Changing
  fh1\CMakeLists.txt or the SDK gives the long build (about 14 minutes).
* After any change to how fh1.exe draws, check the picture against the emulated GPU at the same moment
  (tools\fh1_pic_stats.py); two runs drift apart by seconds, so take shots every few seconds and look at them.
* Write patch scripts to a file first, and keep a document's line endings when a script rewrites it.
* Nothing is published (no release, no tag, no upload) without my yes at that moment.
* Commit and push to main when something works, and keep the status document, ROADMAP.md and CLAUDE.md up to
  date.
* End with a summary of what you did and what is still open, plus a prompt for the session after.
