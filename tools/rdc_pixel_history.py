# Runs inside RenderDoc's UI Python (qrenderdoc --python tools/rdc_pixel_history.py), with
#   RDC_FILE = capture (either renderer), RDC_OUT = output text file, RDC_RES = resource id of a color target
#   (from rdc_dump.py's actions.txt / textures.txt), RDC_POINTS = "x:y,x:y,..." in the game's 1280x720 pixels,
#   RDC_STRIP = rows of one strip of the target (default 256: FH1 draws its scene in three strips of 256 rows
#   into the same target; 0 = the target holds the whole picture).
# For each point: every draw that touched that pixel of the target (event id, passed or why not, the color the
# shader wrote and the color in the target afterwards). With strips, a point's pixel is touched by the draws of
# all three strips: the ones that matter are those of strip y // 256 (the list is in frame order).
# Answers "which draw paints this pixel" on either renderer, to feed rdc_tex_stats.py / rdc_draw_textures.py.
import os

import renderdoc as rd

cap_path = os.environ["RDC_FILE"]
out = open(os.environ["RDC_OUT"], "w")
res_wanted = int(os.environ["RDC_RES"])
points = [tuple(float(v) for v in p.split(":")) for p in os.environ["RDC_POINTS"].split(",") if p]
strip = int(os.environ.get("RDC_STRIP", "256"))


def run():
    cap = rd.OpenCaptureFile()
    result = cap.OpenFile(cap_path, "", None)
    if result != rd.ResultCode.Succeeded:
        out.write("OpenFile failed: %s\n" % str(result))
        return
    result, controller = cap.OpenCapture(rd.ReplayOptions(), None)
    if result != rd.ResultCode.Succeeded:
        out.write("OpenCapture failed: %s\n" % str(result))
        return
    tex = None
    for t in controller.GetTextures():
        if int(t.resourceId) == res_wanted:
            tex = t
    if tex is None:
        out.write("texture %d not found\n" % res_wanted)
        return
    scale = max(1, tex.width // 1280)
    out.write("texture %d %dx%d samples=%d %s scale %d\n" % (res_wanted, tex.width, tex.height, tex.msSamp,
                                                           tex.format.Name(), scale))
    def last_event(actions):
        found = 0
        for a in actions:
            found = max(found, a.eventId, last_event(a.children))
        return found

    controller.SetFrameEvent(last_event(controller.GetRootActions()), True)
    for (gx, gy) in points:
        x = int(gx) * scale
        y = (int(gy) % strip if strip else int(gy)) * scale
        out.write("point %d,%d (strip %d) = target pixel %d,%d\n" % (gx, gy, int(gy) // strip if strip else 0, x, y))
        try:
            mods = controller.PixelHistory(tex.resourceId, x, y, rd.Subresource(0, 0, 0), rd.CompType.Typeless)
        except Exception as e:
            out.write("  PixelHistory failed: %s\n" % e)
            continue
        for m in mods:
            why = "passed" if m.Passed() else "failed:" + ",".join(
                n for n, f in (("cull", m.backfaceCulled), ("depth", m.depthTestFailed), ("stencil", m.stencilTestFailed),
                               ("scissor", m.scissorClipped), ("discard", m.shaderDiscarded),
                               ("sample", m.sampleMasked), ("clip", m.depthClipped)) if f)
            out.write("  eid %d prim %d %s  out %s  after %s\n" % (
                m.eventId, m.primitiveID, why, [round(v, 4) for v in m.shaderOut.col.floatValue[:4]],
                [round(v, 4) for v in m.postMod.col.floatValue[:4]]))
    controller.Shutdown()
    cap.Shutdown()


try:
    run()
except Exception as e:
    out.write("failed: %s\n" % e)
out.write("done\n")
out.close()
os._exit(0)
