# Prompt for the next session (written 2026-10-06, at the end of the eighth session)

Paste the block below as the first message of a new Claude session opened in `C:\Users\andre\Desktop\FH1-recomp`.

```
Read fh1-recomp/CLAUDE.md, then ROADMAP.md ("Maybe later", the first pre-release as a Windows installer app, and
Stage 3, "Offline shader library for the PC") and the section "Offline pipeline list" at the top of
fh1-recomp/docs/native-renderer-status.md. This session is about the first pre-release of the port. The offline
pipeline list works (I checked it by hand on 2026-10-06), so we jump to the pre-release now. The other native
renderer fixes (night colors, garage / car damage, Carbon clean-up, the F3 fps viewer, and the three faults I
found on 2026-10-06: missing ground at the Montano Plains outpost and east of it, upside-down scenery patches in
a mountain race) are for after the pre-release: do not start them.

Done already (check it in the code before you trust it): a list of pipelines ships with the port
(fh1/data/fh1_pipelines.nfpl, 796 of them) and is built during the logo videos (22.5 s on a fresh PC); a pipeline
that no list knows is compiled by helper threads and its object appears a moment late
(fh1_native_pipelines_background), except full-screen passes and things drawn once, which still stop the frame;
the list is built before the game's code starts, with a "Preparing shaders n / total" screen when it lasts more
than half a second (20.3 s on a fresh PC).

Do it in this order:
1. First merge what I have driven since (I drive more to add pipelines): tell me how many new ones my cache file
   has (python tools\fh1_pipelines.py info fh1\data\fh1_pipelines.nfpl
   fh1\out\win-release\cache\fh1_native_pipelines.bin), merge them into the shipped list, build, commit. Do the
   same again whenever I say I have driven more.
2. The pre-release. I want a simple Windows GUI app like StevensND's installer page for nfsmw-nx: it downloads the
   pre-built fh1.exe (my decision, do not ask again), lets me choose my ISO and a folder, extracts the needed
   files there and builds the shader library there. Tell me the plan in plain words first and wait for my yes:
   - what the window looks like and what each step does, and what language you write it in;
   - how the shader library is built on the user's PC (the translator and DXC must come with the app), how long
     it takes, and how the extra vertex shaders of build_logs\shaders\synth* can be made from the disc alone
     (they were captured from my running game; they are game data and cannot be shipped);
   - what the release download holds (fh1.exe, its DLLs, fh1_pipelines.nfpl; never the shader library, never
     the driver's cache, no game data) and how big it is;
   - what to do about Smart App Control and the SmartScreen warning for an unsigned program;
   - which renderer is the default, the install guide, the known issues (include the three faults above), the
     tag and the version name.
3. Build it one step at a time. Test each step on my PC with my ISO into a new empty folder (never into
   game_root or over my saves), and tell me exactly what to click to check it by hand.
4. Do not publish anything (no release, no tag, no upload) without my yes at that moment.

Left for later, only if I ask: a nicer look for the "Preparing shaders" screen (it uses the overlay's default
small text), and the paint booth check on the build with the background compiler (my save must be parked at
the festival for it).

Rules:
- Explain things to me in plain words: I am not a programmer. Say exactly what to run and when.
- StevensND's installer is the model for the app: credit him by name; his installer and nfsmw-nx are
  public (reference/nfsmw-README.md has the links).
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
