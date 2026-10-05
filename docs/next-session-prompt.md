# Prompt for the next session (written 2026-10-05, at the end of the second session of that day)

Paste the block below as the first message of a new Claude session opened in `C:\Users\andre\Desktop\FH1-recomp`.

```
Read fh1-recomp/CLAUDE.md, then fh1-recomp/docs/native-renderer-status.md, and work on the native renderer
(run_fh1.bat --fh1_renderer=native).

Where we are: the blue outline, the headlights on the road and the map selector are fixed, and I drove the
fixed build (day, night, a race): everything looks normal. The rectangular patch under the car shows on both
renderers, so leave it.

Fix these four, in this order, one at a time:
1. Chrome and paint are sharper and whiter than on the emulated GPU, where they are soft (parked at the
   festival at night: build_logs\test-parkN-*-340s.png against test-parkX-*-340s.png). The reflection cube map
   has one level here and nine on the console; the game renders the smaller ones itself (1C9F9000, 1CA59000,
   1CA71000 ...).
2. The picture is slightly darker than the emulated one (festival at dusk, median brightness 27 against 39).
   Check first whether fixing 1 already changed it.
3. Stair-stepped edges: the scene is drawn with one sample where the console has 4x MSAA.
4. Glows of lamps and tail lights show through walls and other cars (it also happens on the emulated GPU,
   which does not measure): make the native renderer measure the occlusion queries for real.
After each one: show me the comparison, commit and push, then go on to the next.

Also watch for the loading screen freezing for good (it happened once, on the first run after a new shader
library): if it happens again, find what the renderer's thread is waiting for.

1 and 2 can be tested without me (the car parked at the festival reaches night in about 5 minutes). For a spot
the unattended test does not reach (the road, a wall for 4) use the tools of last time: I start
run_native_capture.bat or run_emulated_capture.bat (or run_native_skip.bat), drive there, stop and type "now",
and you run tools\capture_now.ps1 (or tools\skip_cycle.ps1). Tell me exactly what to run and when.

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
