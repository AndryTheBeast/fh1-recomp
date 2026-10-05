# Prompt for the next session (written 2026-10-05, at the end of the fourth session of that day, after the user's drive)

Paste the block below as the first message of a new Claude session opened in `C:\Users\andre\Desktop\FH1-recomp`.

```
Read fh1-recomp/CLAUDE.md, then fh1-recomp/docs/native-renderer-status.md, and work on the native renderer
(run_fh1.bat --fh1_renderer=native).

Where we are: I drove the last build and everything fixed last time is confirmed (first-person view, no more
brightness jumps, picture as bright as the emulated one, crowd edges, the paint shop's blurred background). Glows
of tail lights through walls show on both renderers: leave them.

Fix these, in this order, one at a time:
1. FIRST: the design creator (paint shop > Design creator > Paint car) is broken on the native renderer: the
   wheels are solid magenta boxes, the car body is flat cyan with a black lower half and no reflections, the
   paint booth has no shading, and the "Leaving paint shop" dialog is an empty black box. My pictures are in
   build_logs\reference\user-20261005-design-creator-*.webp. Compare with the emulated GPU in the same screen.
2. The thumbnail bug is back: when I save the car with a new modification its thumbnail is wrong. Ask me whether
   it also happens on the emulated GPU and what it looks like before you start.
3. Night colors at the festival: the native picture is warm grey where the emulated one is blue (the status
   document has the numbers).
4. Races, garage and car photos on the native renderer (I drive there).
After each one: show me the comparison, commit and push, then go on to the next.

For a screen that only the native renderer gets wrong, capture both renderers with RenderDoc and compare the
numbers pass by pass before guessing (tools\view_capture.ps1, auto_test.ps1 -RenderDoc -Triggers,
tools\rdc_tex_stats.py, tools\rdc_draw_textures.py): the status document's fourth session section has the method.
The unattended test can enter the paint shop by itself (X at the festival); never save a design or buy anything
with it. For a spot it does not reach I start run_native_capture.bat or run_emulated_capture.bat, go there, stop
and type "now", and you run tools\capture_now.ps1. Tell me exactly what to run and when.

If a busy place drops below 30 fps, measure it (the [fps] lines and "real GPU per Swap" in the log, with and
without --fh1_native_ssaa=false) and tell me the numbers before changing anything.

Rules:
- Compare every change against the emulated GPU at the same second or the same spot (tools\auto_test.ps1, with
  and without --fh1_renderer=native; tools\fh1_pic_stats.py gives the numbers and a crop) and show me.
- At the festival the lights and the car's reflections change within seconds around 92-95 s: compare at 58 s,
  at 100 s or later, or at night (300-340 s), and take several shots. The game remembers my camera view and the
  unattended tests change it: check that two shots show the same view before comparing them.
- I test driving by hand. Ask me for a short drive when you need one, and tell me when the build is ready: I must
  not start the game while a build is running, and you cannot build while my game is open (ask me to close it).
- If you start a test with a temporary option, or open a test window yourself, say so before I look at the
  window, so I do not take its picture for the normal build's or start driving it.
- If you change anything in shaders\XenosRecomp (its shader_common.h is the one in use), rebuild the shader
  library (with the extra vertex shaders of build_logs\shaders\synth*) and copy it next to fh1.exe; the library
  and fh1.exe must match. Such a change also makes the app build take about 14 minutes.
- Don't edit sources while a build is running, and never wait with an open-ended loop.
- Commit and push to main when something works, and keep the status document, ROADMAP.md and CLAUDE.md up to date.
- Explain things to me in plain words, and end with a summary of what you did and what is still open, plus a
  prompt for the session after.
```
