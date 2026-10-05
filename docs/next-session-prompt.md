# Prompt for the next session (written 2026-10-06, at the end of the seventh session)

Paste the block below as the first message of a new Claude session opened in `C:\Users\andre\Desktop\FH1-recomp`.

```
Read fh1-recomp/CLAUDE.md, then fh1-recomp/ROADMAP.md (Stage 3, "Offline shader library for the PC") and the top
of fh1-recomp/docs/native-renderer-status.md. This session is about one thing: the offline shader library for
the PC, on the native renderer (run_fh1.bat --fh1_renderer=native). It is the last thing before the first
pre-release of the port. The other native renderer fixes (night colors, races / garage / car damage, Carbon
clean-up, the F3 fps viewer) are paused: do not start them.

The goal: nothing is compiled while I play. No stutter the first time I see a place, a car, a menu or a time
of day, on a fresh install too, and never again the loading screen that freezes for good.

What exists today (check it in the code before you trust it, fh1/src/native/fh1_native_draws.cpp, "prewarm"):
- The shaders are translated ahead of time into fh1_shaders.nfsp (3,849 of them, next to fh1.exe).
- What is still built while the game runs is the pipeline of each shader pair with its drawing state. The
  renderer remembers the ones it has built in fh1\out\win-release\cache\fh1_native_pipelines.bin (the
  driver's cache plus a list: about 100 MB and 591 pipelines on my PC) and rebuilds that list in the
  background during the logo videos (0.2 s when the driver's cache already has them).
- So today the list only knows what I have already played on this PC. A new install starts with nothing, and a
  pipeline met for the first time is compiled in the middle of a frame (a hitch; one frame of about 3.2 s and
  the game stops sending commands for good).

Do it in this order:
1. First tell me the plan in plain words, before you build anything, and wait for my yes. I want to know:
   - how the list of pipelines gets complete (what I must play and for how long, or whether it can be worked
     out without playing, from the shaders and the game's files);
   - what gets shipped with the port (the list only, never the driver's cache, and no game data in git) and
     how big it is;
   - when everything is built on a new PC (before the title screen, with a progress display?), how long that
     takes on my Legion Go, and what happens after a graphics driver update or a new shader library;
   - what the game does when it still meets a pipeline that is not in the list;
   - how much of this the Switch can reuse later (it cannot compile while playing at all).
2. Measure today's behavior first, so we have numbers to compare: a run with the cache file moved away (a fresh
   install) and a normal run. From the log: how many pipelines the game built while running, the [hitch] lines,
   the [fps] lines, the longest frame. Move the cache file away, never delete it, and put it back after.
3. Build it, one step at a time, and after each step show me the same numbers again.
4. When it works: tell me exactly what to play so I can check it by hand on a fresh cache (the festival, a
   drive by day and by night, a race, the garage, the paint shop).

Then, only if there is time left, tell me what the first pre-release needs (ROADMAP.md, "Source-only
pre-release": install guide, known issues, which renderer is the default, a tag) and what you think is missing.
Do not publish anything without my yes.

Rules:
- Explain things to me in plain words: I am not a programmer. Say exactly what to run and when.
- The unattended test (tools\auto_test.ps1) reaches the festival, the paint shop (X at 60 s) and the design
  creator (A at 74 s). It must never save a design, buy anything or confirm a dialog. Everything else (driving,
  races, the garage, saving) I do by hand: ask me.
- Tell me when a build is ready. I must not start the game while a build is running, and you cannot build while
  my game is open (ask me to close it). Don't edit sources while a build is running, and never wait with an
  open-ended loop.
- If you start a test with a temporary option, or open a test window yourself, say so before I look at it, so
  I do not take it for the normal game or start driving it.
- After any change, check that the picture did not change: the festival and the paint booth against the
  emulated GPU at the same second (tools\fh1_pic_stats.py gives the numbers). My save now starts at the festival
  at night with the purple Corrado: take new shots of the emulated GPU first, the daylight ones of 2026-10-05
  no longer match. In the booth the car's lower body turns black when something goes wrong with what is written
  to the game's memory.
- If you change anything in shaders\XenosRecomp, rebuild the shader library (with the extra vertex shaders of
  build_logs\shaders\synth*) and copy it next to fh1.exe; the library and fh1.exe must match.
- Write patch scripts to a file first (a bash heredoc breaks backslashes and quotes), and keep a document's
  line endings when a script rewrites it.
- GoatHonks' repositories are private: credit him by name, never write a link to them.
- Commit and push to main when something works, and keep the status document, ROADMAP.md and CLAUDE.md up to
  date.
- End with a summary of what you did and what is still open, plus a prompt for the session after.

Small thing left from last session, only if it is quick: the first 25 seconds at the festival have a few late
frames since the photo fix (28.4-29.6 fps, then 30). The status document's seventh session section has the
numbers and the setting.
```
