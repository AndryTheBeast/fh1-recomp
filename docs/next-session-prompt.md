# Prompt for the next session (written 2026-10-05, after the second session of that day)

Paste the block below as the first message of a new Claude session opened in `C:\Users\andre\Desktop\FH1-recomp`.

```
Read fh1-recomp/CLAUDE.md, then fh1-recomp/docs/native-renderer-status.md, and work on the native renderer
(run_fh1.bat --fh1_renderer=native).

Done last time and seen by me: the blue outline on the car, the headlights lighting the road, the map screen's
circle selector. Now the open list of the status document, in this order:
1. Ask me for a normal drive on the native renderer first (day and night, a race if I have time) and for what
   still looks wrong: nobody has looked at motion blur, frame drops and flashing since the fixes.
2. Chrome and paint are sharper and whiter than the emulated ones (parked at the festival at night:
   build_logs\test-parkN-*-340s.png against test-parkX-*-340s.png). The reflection cube map has one level here
   and nine on the console; the game renders the smaller ones itself (1C9F9000, 1CA59000, 1CA71000 ...).
3. The picture slightly darker than the emulated one (may be the same cause as 2).
4. Stair-stepped edges (the scene is drawn with one sample where the console has 4x MSAA).
5. Measuring the lights' visibility for real (occlusion queries), then the small items of the list.
Also watch for the loading screen freezing for good (it happened once, on the first run after a new shader
library): if it happens again, find what the renderer's thread is waiting for.

For a spot the unattended test does not reach (the road, night, the map) use the tools of last time: I start
run_native_capture.bat or run_emulated_capture.bat (or run_native_skip.bat), drive there, stop and type "now",
and you run tools\capture_now.ps1 (or tools\skip_cycle.ps1). Night falls about 5 minutes after launch. Tell me
exactly what to run and when.

Rules:
- Compare every change against the emulated GPU at the same second or the same spot (tools\auto_test.ps1, with
  and without --fh1_renderer=native) and show me the numbers or a crop.
- I test driving by hand. Ask me for a short drive when you need one, and tell me when the build is ready: I must
  not start the game while a build is running, and you cannot build while my game is open (ask me to close it).
- If you start a test with a temporary option, or open a test window yourself, say so before I look at the
  window, so I do not take its picture for the normal build's or start driving it.
- If you change anything in shaders\XenosRecomp (its shader_common.h is the one in use), rebuild the shader
  library and copy it next to fh1.exe; the library and fh1.exe must match.
- Don't edit sources while a build is running, and never wait with an open-ended loop.
- Commit and push to main when something works, and keep the status document, ROADMAP.md and CLAUDE.md up to date.
- Explain things to me in plain words, and end with a summary of what you did and what is still open, plus a
  prompt for the session after.
```
