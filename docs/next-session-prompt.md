# Prompt for the next session (written 2026-10-05, at the end of the sixth session of that day)

Paste the block below as the first message of a new Claude session opened in `C:\Users\andre\Desktop\FH1-recomp`.

```
Read fh1-recomp/CLAUDE.md, then fh1-recomp/docs/native-renderer-status.md, and work on the native renderer
(run_fh1.bat --fh1_renderer=native).

Where we are: last session fixed the paint booth of the design creator (the car's sides, gloss, badge, tyres
and rims). I saw it in screenshots and saved a paint job on both renderers; I have not driven that build yet.
Glows of tail lights through walls show on both renderers: leave them.

Fix these, in this order, one at a time:
1. FIRST: the photos of a car after I save a paint job are wrong on the native renderer only. On the emulated
   GPU both are right (build_logs\reference\user-thumbnail-20261005-emulated-correct.webp). On the native
   renderer (user-thumbnail-20261005-native-wrong.webp) the small thumbnail in the car list shows a piece of
   ANOTHER car (the red Corrado), turned on its side, with a strip of red speckles, and the big photo shows the
   red Corrado twice (one turned on its side) with rows of speckles, instead of my orange Mustang. The status
   document's item 0b has what is known and where to start. The unattended test must never save a design:
   tell me what to start and when to save.
2. Night colors at the festival: the native picture is redder than the emulated one (340 s: mean color
   72 55 51 against 66 59 56, one tint over the whole scene). The status document's item C has the numbers and
   the three suspects in the order to check them.
3. Races, garage and car photos on the native renderer, and car damage (I drive there; the parts of a car
   take their damage shape from the data streams that were fixed last session, so I will hit something).
4. Clean up any Most Wanted and Carbon code left that has no use on our native renderer (one piece at a time,
   with a festival comparison after each).
5. Bring Carbon's fps counter and frame time viewer (F3) to our native renderer, for more debugging options.
6. Start on the offline shader library for the PC (ROADMAP.md, Stage 3): no shader or pipeline building while
   I play. Tell me the plan before you build it.
After each one: show me the comparison, commit and push, then go on to the next.

For a screen that only the native renderer gets wrong, capture both renderers with RenderDoc and compare the
numbers pass by pass before guessing (auto_test.ps1 -RenderDoc -Triggers, tools\rdc_pixel_history.py,
tools\rdc_tex_stats.py, tools\rdc_constants.py, tools\rdc_counts.py). When the wrong thing is a value or a
picture the GAME makes on the CPU, dump the game's memory on both renderers and compare (--fh1_dump_memory,
--fh1_trap_writes_to): the status document's sixth session section has the method. The unattended test reaches
the paint shop (X at the festival, 60 s), the design creator (A at 74 s) and the leaving dialog (B at 92 and
98 s); never save a design, buy anything or confirm a dialog with it. For a spot it does not reach I start
run_native_capture.bat or run_emulated_capture.bat, go there, stop and type "now", and you run
tools\capture_now.ps1. Tell me exactly what to run and when.

If a busy place drops below 30 fps, measure it (the [fps] lines and "real GPU per Swap" in the log, with and
without --fh1_native_ssaa=false) and tell me the numbers before changing anything. Trees cast shadows again
(--fh1_shadows_without_vegetation=true is the old behavior): check the frame rate while I drive past forests.

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
- Write patch scripts to a file first (a bash heredoc breaks backslashes and quotes), and keep a document's
  line endings when a script rewrites it.
- Commit and push to main when something works, and keep the status document, ROADMAP.md and CLAUDE.md up to date.
- Explain things to me in plain words, and end with a summary of what you did and what is still open, plus a
  prompt for the session after.
```
