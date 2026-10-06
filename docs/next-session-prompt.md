# Prompt for the next session (written 2026-10-06, at the end of the eighth session)

Paste the block below as the first message of a new Claude session opened in `C:\Users\andre\Desktop\FH1-recomp`.

```
Read fh1-recomp/CLAUDE.md, then the section "Offline pipeline list" at the top of
fh1-recomp/docs/native-renderer-status.md and ROADMAP.md (Stage 3, "Offline shader library for the PC", and
"Maybe later", the first pre-release). This session continues the offline pipeline list for the PC on the native
renderer (run_fh1.bat --fh1_renderer=native), then the first pre-release. The other native renderer fixes (night
colors, garage / car damage, Carbon clean-up, the F3 fps viewer, and the three faults I found on 2026-10-06:
missing ground at the Montano Plains outpost and east of it, upside-down scenery patches in a mountain race) are
for after the pre-release: do not start them.

Done last time (check it in the code before you trust it): a list of pipelines ships with the port
(fh1/data/fh1_pipelines.nfpl, 796 of them) and is built during the logo videos (22.5 s on a fresh PC); a pipeline
that no list knows is compiled by helper threads and its object appears a moment late
(fh1_native_pipelines_background), except full-screen passes and things drawn once, which still stop the frame.

Do it in this order:
1. First merge what I have played since: tell me how many new pipelines my cache file has
   (python tools\fh1_pipelines.py info ...), merge them into the shipped list, build, commit.
2. Tell me in plain words how you will do the progress display ("Preparing shaders 412 / 1500") and how the game
   is held before the title screen when the list is not finished, and wait for my yes. The game must never be
   made to wait in the middle: one frame of about 3.2 s freezes it for good.
3. Build it, then show me the numbers of a fresh PC run (tools\fresh_pc_test.ps1: it sets our cache file and the
   AMD driver's cache aside and puts both back; I agreed to that).
4. Tell me exactly what to play to complete the list (every area by day and by night, one race of each kind, the
   garage, the paint shop, the upgrade shop, buying a car, the map, first-person view, a crash), and after each
   of my sessions tell me how many new pipelines it met, until a session meets none.
5. Then the first pre-release. I want a simple Windows GUI app like StevensND's installer page for nfsmw-nx: it
   downloads the pre-built fh1.exe (my decision, do not ask again), lets me choose my ISO and a folder, extracts
   the files and builds the shader library there. Tell me the plan first: how the extra vertex shaders of
   build_logs\shaders\synth* can be made from the disc alone, what to do about Smart App Control, the install
   guide, the known issues, which renderer is the default, the tag. Do not publish anything without my yes.

Rules:
- Explain things to me in plain words: I am not a programmer. Say exactly what to run and when.
- The unattended test (tools\auto_test.ps1) only boots into my save. My save may start anywhere (on 2026-10-06 it
  moved from the festival at night to a highway by day): look at a shot before comparing pictures, and ask me to
  park at the festival if you need the paint shop (X at 60 s, A at 74 s). It must never save a design, buy
  anything or confirm a dialog. Everything else I do by hand: ask me.
- Tell me when a build is ready. I must not start the game while a build is running, and you cannot build while
  my game is open: check it with a command that prints "running" or "closed", and ask me to close it. Don't edit
  sources while a build is running, and never wait with an open-ended loop.
- If you open a test window, or start a test with a temporary option, say so before I look at it.
- After any change, check that the picture did not change against the emulated GPU at the same second
  (tools\fh1_pic_stats.py), with new emulated shots at my save's current spot.
- If you change the size of a pipeline record, the shipped list must be recorded again (the renderer asserts the
  sizes; tools\fh1_pipelines.py knows them).
- Write patch scripts to a file first, and keep a document's line endings when a script rewrites it.
- GoatHonks' repositories are private: credit him by name, never write a link to them.
- Commit and push to main when something works, and keep the status document, ROADMAP.md and CLAUDE.md up to
  date.
- End with a summary of what you did and what is still open, plus a prompt for the session after.
```
