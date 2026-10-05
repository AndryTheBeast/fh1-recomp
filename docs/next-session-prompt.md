# Prompt for the next session (written 2026-10-06, at the end of the seventh session)

Paste the block below as the first message of a new Claude session opened in `C:\Users\andre\Desktop\FH1-recomp`.

```
Read fh1-recomp/CLAUDE.md, then fh1-recomp/ROADMAP.md (Stage 3, "Offline shader library for the PC") and
fh1-recomp/docs/native-renderer-status.md.

Where we are: the photos of a saved car are right on the native renderer (I checked three paint jobs on two
cars). The other native renderer fixes (night colors, races / garage / car damage, Carbon clean-up, the F3 fps
viewer) are paused. The goal now is the first pre-release of the port.

Do these, in this order:
1. The offline shader library for the PC: no shader or pipeline building while I play. Tell me the plan in
   plain words before you build anything (what is built ahead of time, when, how long it takes, how big it is,
   what happens after a driver update), then build it and show me the numbers before and after (stutter in the
   log's [hitch] and [fps] lines on a first run and on a second run).
2. The first pre-release (ROADMAP.md, "Source-only pre-release"): tell me what it needs (install guide, known
   issues, which renderer is the default, a tag) and what you think is missing before we publish. No game data
   in git, ever.
3. Small thing left from last session, only if it is quick: the first 25 seconds at the festival have a few
   late frames since the photo fix (28.4-29.6 fps, then 30). The status document's seventh session section has
   the numbers and the setting.

Rules:
- Compare every change against the emulated GPU at the same second or the same spot (tools\auto_test.ps1, with
  and without --fh1_renderer=native; tools\fh1_pic_stats.py gives the numbers and a crop) and show me. My save
  now starts at the festival at night with the purple Corrado: take new reference shots of the emulated GPU
  before comparing, the daylight ones of 2026-10-05 no longer match.
- The unattended test reaches the paint shop (X at the festival, 60 s) and the design creator (A at 74 s);
  never save a design, buy anything or confirm a dialog with it. Check the booth after any change to what is
  written to the game's memory: the car's lower body turns black when it goes wrong.
- I test driving and saving by hand. Tell me exactly what to run and when, and tell me when the build is ready:
  I must not start the game while a build is running, and you cannot build while my game is open.
- If you start a test with a temporary option, or open a test window yourself, say so before I look at it.
- If you change anything in shaders\XenosRecomp, rebuild the shader library (with the extra vertex shaders of
  build_logs\shaders\synth*) and copy it next to fh1.exe; the library and fh1.exe must match.
- Don't edit sources while a build is running, and never wait with an open-ended loop.
- Write patch scripts to a file first (a bash heredoc breaks backslashes and quotes), and keep a document's
  line endings when a script rewrites it.
- Commit and push to main when something works, and keep the status document, ROADMAP.md and CLAUDE.md up to
  date.
- Explain things to me in plain words, and end with a summary of what you did and what is still open, plus a
  prompt for the session after.
```
