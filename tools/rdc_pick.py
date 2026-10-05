# Runs inside RenderDoc's UI Python (qrenderdoc --python tools/rdc_pick.py), with
#   RDC_FILE = capture, RDC_OUT = output text file, RDC_RES = resource id of a texture (a depth buffer or a
#   color target, from rdc_dump.py's textures.txt), RDC_EIDS = "eid,eid,..." and RDC_POINTS = "x:y,x:y,..."
#   (pixels of that texture; fractions of its size when below 1, e.g. "0.5:0.3").
# For each event: the value of the texture at each point as it is after that event (depth and stencil for a
# depth buffer, four floats for a color target) and the smallest and largest value of the whole texture.
# Answers "what is in the depth buffer here before this draw" for either renderer.
import os

import renderdoc as rd

cap_path = os.environ["RDC_FILE"]
out = open(os.environ["RDC_OUT"], "w")
res_wanted = int(os.environ["RDC_RES"])
eids = [int(e) for e in os.environ["RDC_EIDS"].split(",") if e]
points = [tuple(float(v) for v in p.split(":")) for p in os.environ["RDC_POINTS"].split(",") if p]


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
    out.write("texture %d %dx%d samples=%d %s\n" % (res_wanted, tex.width, tex.height, tex.msSamp, tex.format.Name()))
    sub = rd.Subresource(0, 0, 0)
    for eid in eids:
        controller.SetFrameEvent(eid, True)
        try:
            lo, hi = controller.GetMinMax(tex.resourceId, sub, rd.CompType.Typeless)
            out.write("eid %d: min %s max %s\n" % (eid, [round(v, 6) for v in lo.floatValue[:4]],
                                                    [round(v, 6) for v in hi.floatValue[:4]]))
        except Exception as e:
            out.write("eid %d: min/max failed: %s\n" % (eid, e))
        for (px, py) in points:
            x = int(px * tex.width) if px < 1 else int(px)
            y = int(py * tex.height) if py < 1 else int(py)
            v = controller.PickPixel(tex.resourceId, x, y, sub, rd.CompType.Typeless)
            out.write("    (%d, %d): %s  stencil/int %s\n" % (x, y, [round(f, 6) for f in v.floatValue[:4]],
                                                              list(v.uintValue[:4])))
    controller.Shutdown()
    cap.Shutdown()


try:
    run()
except Exception as e:
    out.write("failed: %s\n" % e)
out.write("done\n")
out.close()
os._exit(0)
