Read fh1-recomp/CLAUDE.md, then fh1-recomp/docs/native-renderer-status.md ("Thirteenth session") and ROADMAP.md
("Maybe later": the 60 fps item).

Where we are: `--fh1_fps60` exists and is off by default (2026-10-06). It makes the game draw and simulate
60 times a second by running the screen refresh the game counts at 120 per second; the game's speed stays
right (the log's `[fps60]` line says 1.000 game seconds per real second). On my Legion Go it gives 50-58 fps
in the opening cutscene and 32-40 while driving, because the graphics chip needs about 20 ms per frame and
60 needs 16.7. Without the smooth edges it reaches 40-50, and I said that picture looks bad: smooth edges
stay on.

Start by asking me what I saw when I drove with it (I start it with `run_fh1.bat --fh1_fps60=true`):
speedometer and race timer against a stopwatch, the festival's crowd and people, traffic, particles, buying a
car (the camera of that scene), the HUD (flashing?), night, far scenery, whether the road loads in time on a
long fast drive. Then, depending on my answer:

1. If something runs fast or looks wrong with it on: find it (pinyon-shift's hook file and its
   CHANGELOG list what they had to fix at high frame rates) and fix it behind the option.
2. If I say it is good: making the frame faster so the Legion Go gets nearer 60. The status document has the
   numbers: the scene's copies take 12-16 ms of the 20 with the double-size scene. Tell me the plan before
   changing code, and measure before and after at the same spot.
3. Only if I ask: the option for players (a setting or a launcher in the installed copy) and a release.

Waiting, not for this session unless I ask (ROADMAP.md has them): the sharp rectangle of ground under the car
in the opening cutscene (my picture, `build_logs\reference\user-rectangle-under-car-20261006.webp`: check it
against the emulated picture and with the option off first), the rare black window at the start, the dark
dashboard of the Volkswagen, the grey thumbnail, the pink triangles on the upgrade menu's tyre icons, night
colors, garage / car damage, the Carbon clean-up. If a GitHub issue has come in (gh issue list), tell me what
it says first; do not answer or close one without asking me.

Rules:

* Explain things to me in plain words: I am not a programmer. Say exactly what to run and when.
* The repository is public: nothing committed may contain my Windows user folder or my e-mail address.
* GoatHonks' repositories are private: credit him by name, never write a link to them. pinyon-shift is
  public (BSD 3-Clause): credit it by name and link when something is taken from it.
* Tests never use my saves or my installed copy: a game started for a test gets --user_data_root=<an empty
  folder> and the new-game route (`build_logs\fps60-autoplay.txt`). Delete the test folders when done.
* Never make the game wait for a shader or a pipeline, and never let a frame come near 3 seconds: it stops
  the game for good.
* Never give tools\auto_test.ps1 an option the script already passes, and never an empty -ExtraArgs. A plain
  run is the native renderer: an emulated reference shot needs -ExtraArgs "--fh1_renderer=xenos".
* Before test windows open, say how many, and for each one whose saves it uses. Look at a shot of a test run
  before telling me it is fine: a black window is a fault, not a slow start.
* Tell me when a build is ready. I must not start the game while a build is running, and you cannot build
  while my game is open: check it with a command that prints "running" or "closed", and ask me to close it.
  Don't edit sources while a build is running, and never wait with an open-ended loop. Changing
  fh1\CMakeLists.txt, fh1\overrides.toml or the SDK gives the long build (about 14 minutes).
* One run each of "on" and "off" is not a result for something that comes and goes: repeat before blaming.
* Write patch scripts to a file first, and keep a document's line endings when a script rewrites it.
* Nothing is published (no release, no tag, no upload) without my yes at that moment.
* Commit and push to main when something works, and keep the status document, ROADMAP.md and CLAUDE.md up
  to date.
* End with a summary of what you did and what is still open, plus a prompt for the session after.
