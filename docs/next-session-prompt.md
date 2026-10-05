# Prompt for the next session (written 2026-10-05, at the end of the third session of that day)

Paste the block below as the first message of a new Claude session opened in `C:\Users\andre\Desktop\FH1-recomp`.

```
Read fh1-recomp/CLAUDE.md, then fh1-recomp/docs/native-renderer-status.md, and work on the native renderer
(run_fh1.bat --fh1_renderer=native).

Where we are: reflections on chrome and paint are soft, textures use the console's gamma curve, the scene is
drawn with four samples per pixel (smooth edges) and the loading-screen freeze after a new shader library is
fixed. All of that was tested parked at the festival only. First ask me what I saw when I drove it (edges,
reflections, speed in busy places, anything that flashes), and fix what I report.

Then, in this order, one at a time:
1. Glows of lamps and tail lights through walls and other cars. The measured mode exists
   (--fh1_native_occlusion=1) but the tail lights lose most of their red halo with it. I will drive it behind a
   wall and behind a car and tell you what I see; find out what the game does with the counts (the boxes of the
   tail lights seem to count almost nothing) and make hidden lights lose their glow without shrinking the
   visible ones.
2. The picture is still a little darker than the emulated one, and at night green and blue are lower over the
   whole picture (sky 29 29 35 against 30 40 50). The scene before post-processing matches, so compare the
   post-processing step by step (exposure, bloom, the color grading lookup texture 136FB000) with a RenderDoc
   capture of the emulated GPU taken at night at the festival.
3. Crowd brighter than the emulated one and with hard cut-out edges (alpha to mask: now that the scene has four
   samples per pixel, cover 0 to 4 of them instead of testing at one half).
4. Races, garage, car photos and paint shop on the native renderer.
After each one: show me the comparison, commit and push, then go on to the next.

If a busy place drops below 30 fps with the smooth edges, measure it (the [fps] lines and "real GPU per Swap" in
the log, with and without --fh1_native_ssaa=false) and tell me the numbers before changing anything.

For a spot the unattended test does not reach use the tools of last time: I start run_native_capture.bat or
run_emulated_capture.bat (or run_native_skip.bat), drive there, stop and type "now", and you run
tools\capture_now.ps1 (or tools\skip_cycle.ps1). Tell me exactly what to run and when.

Rules:
- Compare every change against the emulated GPU at the same second or the same spot (tools\auto_test.ps1, with
  and without --fh1_renderer=native; tools\fh1_pic_stats.py gives the numbers and a crop) and show me.
- At the festival the lights and the car's reflections change within seconds around 92-95 s: compare at 58 s,
  at 100 s or later, or at night (300-340 s), and take several shots.
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
