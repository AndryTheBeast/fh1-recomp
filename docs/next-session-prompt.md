# Prompt for the next session (written 2026-10-05)

Paste the block below as the first message of a new Claude session opened in `C:\Users\andre\Desktop\FH1-recomp`.

```
Read fh1-recomp/CLAUDE.md, then fh1-recomp/docs/native-renderer-status.md, and work on the native renderer
(run_fh1.bat --fh1_renderer=native).

Order:
1. Ask me first what I saw on my night drive with the build of 2026-10-05 (the cut of float pictures at the
   console's ceiling): are the blue rims on the chrome and the rear window and the orange dots on the car's
   outline gone? If not, follow "If the specks are still there" in the status document.
2. The map screen: the circle selector (the cursor you move over the map) does not appear. Compare the map screen
   on both renderers (I will tell you how I open it, or ask me for a screenshot), then find its draw with a
   one-frame trace and a RenderDoc capture of the emulated GPU, the way the green car was found.
3. Then the rest of the open list in the status document, in its order: the picture slightly darker than the
   emulated one (start from the raw scene before post-processing), stair-stepped edges (4x MSAA), and measuring
   the lights' visibility for real instead of always answering "visible".

Rules:
- Compare every change against the emulated GPU at the same second (tools\auto_test.ps1, with and without
  --fh1_renderer=native) and show me the numbers or a crop.
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
