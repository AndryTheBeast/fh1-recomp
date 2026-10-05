# Runs inside RenderDoc's UI Python (qrenderdoc --python tools/rdc_texture.py), with
#   RDC_FILE = capture, RDC_OUT = output folder, RDC_RES = "resource,resource,..." (ids from rdc_dump.py's
#   textures.txt), RDC_EID = event at which to look (default: the last one).
# For each texture: the smallest and largest value of every array slice (cube map face) at mip 0, and each slice
# saved as PNG (tex_<res>_s<slice>.png). Answers "what is in this cube map on the emulated GPU" without the UI.
import os

import renderdoc as rd

cap_path = os.environ["RDC_FILE"]
out_dir = os.environ["RDC_OUT"]
wanted = [int(r) for r in os.environ["RDC_RES"].split(",") if r]
eid = int(os.environ.get("RDC_EID") or "0")
# RDC_WHITE = the value saved as 255 in the PNG (dark float pictures: 0.0625 shows a 10-bit picture times 16)
white = float(os.environ.get("RDC_WHITE") or "0")
os.makedirs(out_dir, exist_ok=True)
log = open(os.path.join(out_dir, "textures_stats.txt"), "w")


def run():
    cap = rd.OpenCaptureFile()
    if cap.OpenFile(cap_path, "", None) != rd.ResultCode.Succeeded:
        log.write("OpenFile failed\n")
        return
    result, controller = cap.OpenCapture(rd.ReplayOptions(), None)
    if result != rd.ResultCode.Succeeded:
        log.write("OpenCapture failed: %s\n" % str(result))
        return
    last = [0]

    def walk(actions):
        for a in actions:
            last[0] = max(last[0], a.eventId)
            walk(a.children)

    walk(controller.GetRootActions())
    controller.SetFrameEvent(eid or last[0], True)
    for t in controller.GetTextures():
        if int(t.resourceId) not in wanted:
            continue
        log.write("== %d %dx%d slices %d mips %d %s\n" % (int(t.resourceId), t.width, t.height, t.arraysize, t.mips,
                                                        t.format.Name()))
        for s in range(t.arraysize):
            mn, mx = controller.GetMinMax(t.resourceId, rd.Subresource(0, s, 0), rd.CompType.Typeless)
            log.write("  slice %d min=(%.4g %.4g %.4g %.4g) max=(%.4g %.4g %.4g %.4g)\n" % (
                s, mn.floatValue[0], mn.floatValue[1], mn.floatValue[2], mn.floatValue[3],
                mx.floatValue[0], mx.floatValue[1], mx.floatValue[2], mx.floatValue[3]))
            ts = rd.TextureSave()
            ts.resourceId = t.resourceId
            ts.mip = 0
            ts.slice.sliceIndex = s
            ts.alpha = rd.AlphaMapping.Discard
            ts.destType = rd.FileType.PNG
            if white:
                ts.comp.blackPoint = 0.0
                ts.comp.whitePoint = white
            controller.SaveTexture(ts, os.path.join(out_dir, "tex_%d_s%d.png" % (int(t.resourceId), s)))
    controller.Shutdown()
    cap.Shutdown()


try:
    run()
except Exception as e:
    import traceback
    log.write("ERROR %s\n%s" % (e, traceback.format_exc()))
log.write("done\n")
log.close()
os._exit(0)
