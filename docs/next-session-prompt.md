# Prompt for the next session

Paste this to start the session after 2026-10-06 (twelfth session). The user's choice for it: a 60 fps patch.

---

Read fh1-recomp/CLAUDE.md, then fh1-recomp/docs/native-renderer-status.md ("Twelfth session") and ROADMAP.md
("Tried and dropped", "Other projects to borrow from", "Maybe later"). This session is about one thing: **a
60 fps patch for driving** (the game draws the 3D world at 30 frames per second; the logo videos and menus
already run at 60). It is an option that is off by default until I say it is good.

What is known before starting:

* It was tried once on the emulated GPU by turning vsync off, and dropped: it broke distant rendering
  (ROADMAP.md, "Tried and dropped"). Find that attempt in docs/history/roadmap-history.md and in git before
  repeating it.
* The game paces itself on the screen's refresh: its GPU wait (WAIT_REG_MEM) sits almost entirely on one memory
  word, twice per frame (docs/history/roadmap-history.md; --gpu_log_waits shows it). That is the 30 fps cap as
  the port sees it.
* pinyon-shift (https://github.com/arcanite24/pinyon-shift) has an fps unlock for this game: read how it does
  it before writing anything (ROADMAP.md, "Other projects to borrow from").
* 60 fps needs a frame in 16.7 ms. The native renderer's GPU time per frame at the festival was about 21 ms on
  my Legion Go (docs/performance-review.md, status document): the patch can be right and still not reach 60
  there. Measure and tell me plainly.
* A text I was given about this engine (general advice, nothing in it is checked against our game; treat each
  point as a guess to prove or disprove, and tell me which were true):
  1. the menus / videos and the 3D world are paced separately, and the world's loop presents on every second
     screen refresh (a divider or a fixed time step of 33.33 ms to find and change to 16.67 ms);
  2. physics and input already run at 60 per second, and the picture is interpolated to 30; forcing 60 may make
     menu animations, people, particles and traffic run at double speed unless the time step that drives them
     follows the real frame time;
  3. the loading of the world while driving may be budgeted for 33.3 ms per frame, so at 60 the road could load
     too late.

Do it in this order:

1. Tell me the plan in plain words before changing code: where the 30 comes from in our game (the refresh
   counter the game waits on, the present interval it asks Direct3D for, or a time step in its own code), which
   of the three you will try first, and how I will see that it worked.
2. Find it. Tools we have: --gpu_log_waits, --fh1_profile, --fh1_dump_image with the strings / cross-reference
   scripts, hooks on game functions (REX_HOOK_RAW), --fh1_dump_memory and --fh1_trap_writes_to. Change settings
   in memory only, never in the game's files (the game verifies them).
3. Make it an option (for example --fh1_fps60), off by default, on the native renderer first. Check with it on:
   the frame rate in the [fps] lines, the speed of the game against a clock (the car's speedometer and the race
   timer must agree with real seconds; menu animations, people, traffic and particles must not run fast), far
   scenery (what broke last time), that the road still loads in time at full speed, and that no frame comes
   near 3 seconds.
4. Compare the picture with the option off at the same spot (tools\fh1_pic_stats.py), and tell me what to
   drive to judge it myself. I decide whether it stays.
5. Only if I ask afterwards: give players the 60 fps option once I call it good (the files of v0.1.0-pre1 were
   replaced a third time on 2026-10-06 with several shaders made at once and the pipeline list at 1067, so
   nothing else is waiting to be published).

Waiting, not for this session unless I ask (ROADMAP.md has them): the rare black window at the start (SDK
presenter "paint mode -> none"), the dark dashboard of the Volkswagen, the grey thumbnail, the pink triangles
on the upgrade menu's tyre icons, night colors, garage / car damage, the Carbon clean-up. (The festival's crowd,
the Montano Plains ground and the upside-down scenery are fine: I said so on 2026-10-06.) If a GitHub issue has come in (gh issue list), tell me what it says first; do
not answer or close one without asking me.

Rules:

* Explain things to me in plain words: I am not a programmer. Say exactly what to run and when.
* The repository is public: nothing committed may contain my Windows user folder or my e-mail address.
* GoatHonks' repositories are private: credit him by name, never write a link to them. pinyon-shift is public:
  credit it by name and link if anything is taken from it, and check its license first.
* Tests never use my saves or my installed copy: a game started for a test gets --user_data_root=<an empty
  folder> and the new-game route (CLAUDE.md, twelfth session, has the timetable). Delete the test folders when
  done.
* Never make the game wait for a shader or a pipeline, and never let a frame come near 3 seconds: it stops the
  game for good.
* Never give tools\auto_test.ps1 an option the script already passes, and never an empty -ExtraArgs. A plain
  run is the native renderer: an emulated reference shot needs -ExtraArgs "--fh1_renderer=xenos".
* Before test windows open, say how many, and for each one whose saves it uses. Look at a shot of a test run
  before telling me it is fine: a black window is a fault, not a slow start.
* Tell me when a build is ready. I must not start the game while a build is running, and you cannot build
  while my game is open: check it with a command that prints "running" or "closed", and ask me to close it.
  Don't edit sources while a build is running, and never wait with an open-ended loop. Changing
  fh1\CMakeLists.txt or the SDK gives the long build (about 14 minutes).
* One run each of "on" and "off" is not a result for something that comes and goes: repeat before blaming.
* Write patch scripts to a file first, and keep a document's line endings when a script rewrites it.
* Nothing is published (no release, no tag, no upload) without my yes at that moment.
* Commit and push to main when something works, and keep the status document, ROADMAP.md and CLAUDE.md up to
  date.
* End with a summary of what you did and what is still open, plus a prompt for the session after.
