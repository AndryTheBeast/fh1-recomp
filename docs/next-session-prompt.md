# Prompt for the next session (written 2026-10-05, after the user's night drive)

Paste the block below as the first message of a new Claude session opened in `C:\Users\andre\Desktop\FH1-recomp`.

```
Read fh1-recomp/CLAUDE.md, then fh1-recomp/docs/native-renderer-status.md, and work on the native renderer
(run_fh1.bat --fh1_renderer=native).

Order (all three were seen by me driving at night on 2026-10-05; my screenshots are in
build_logs\reference\user-night-20261005-*):
1. The blue outline on the car at night is still there after both fixes: a thin blue line exactly on the car's
   silhouette (roof edge, rear window frame, bumper rim), and the rear window and bumper look blue. Follow "Blue
   outline at night" in the status document.
2. The headlights are on (the lamps glow) but they do not light the road in front of the car.
3. The map screen: the circle selector (the cursor you move over the map) does not appear.
Then the rest of the open list in the status document, in its order: the picture slightly darker than the
emulated one, stair-stepped edges (4x MSAA), measuring the lights' visibility for real.

For 1 and 2 you need the night road, which the unattended test does not reach (it stops at the festival at
evening). Ask me to drive to a spot and tell you the second, or use the dump-on-demand option, and take the same
spot on the emulated GPU with a RenderDoc capture: compare the pictures pass by pass, the way the green car was
found. Tell me exactly what to run and when.

Rules:
- Compare every change against the emulated GPU at the same second or the same spot (tools\auto_test.ps1, with
  and without --fh1_renderer=native) and show me the numbers or a crop.
- I test driving by hand. Ask me for a short drive when you need one, and tell me when the build is ready: I must
  not start the game while a build is running.
- If you start a test with a temporary option, say so before I look at the window, so I do not take its picture
  for the normal build's.
- If you change anything in shaders\XenosRecomp or shader_common.h, rebuild the shader library and copy it next to
  fh1.exe; the library and fh1.exe must match.
- Don't edit sources while a build is running, and never wait with an open-ended loop.
- Commit and push to main when something works, and keep the status document, ROADMAP.md and CLAUDE.md up to date.
- Explain things to me in plain words, and end with a summary of what you did and what is still open, plus a
  prompt for the session after.
```
