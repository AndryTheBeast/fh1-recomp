# Runs inside RenderDoc's UI Python (qrenderdoc --python tools/rdc_inputs.py), with
#   RDC_FILE = capture, RDC_OUT = output folder, RDC_EIDS = "eid,eid,..." (draw event ids).
# For each event: lists the pixel shader's textures and the render targets with their min/max
# values (inputs.txt) and saves each as PNG (in_<eid>_<res>.png / out_<eid>_<res>.png). Finds the
# pass where a picture goes wrong (e.g. a black or white target) without opening the UI.
import os

import renderdoc as rd

cap_path = os.environ["RDC_FILE"]
out_dir = os.environ["RDC_OUT"]
eids = [int(e) for e in os.environ["RDC_EIDS"].split(",") if e]
os.makedirs(out_dir, exist_ok=True)
log = open(os.path.join(out_dir, "inputs.txt"), "w")


def run():
    cap = rd.OpenCaptureFile()
    if cap.OpenFile(cap_path, "", None) != rd.ResultCode.Succeeded:
        log.write("OpenFile failed\n")
        return
    result, controller = cap.OpenCapture(rd.ReplayOptions(), None)
    if result != rd.ResultCode.Succeeded:
        log.write("OpenCapture failed: %s\n" % str(result))
        return
    textures = {t.resourceId: t for t in controller.GetTextures()}

    def describe(rid):
        t = textures.get(rid)
        if t is None:
            return "%d (not a texture)" % int(rid)
        sub = rd.Subresource(0, 0, 0)
        mn, mx = controller.GetMinMax(rid, sub, rd.CompType.Typeless)
        return "%d %dx%d s%d %s min=(%.4g %.4g %.4g %.4g) max=(%.4g %.4g %.4g %.4g)" % (
            int(rid), t.width, t.height, t.msSamp, t.format.Name(),
            mn.floatValue[0], mn.floatValue[1], mn.floatValue[2], mn.floatValue[3],
            mx.floatValue[0], mx.floatValue[1], mx.floatValue[2], mx.floatValue[3])

    def save(rid, name):
        if rid not in textures:
            return
        s = rd.TextureSave()
        s.resourceId = rid
        s.mip = 0
        s.slice.sliceIndex = 0
        s.alpha = rd.AlphaMapping.Discard
        s.destType = rd.FileType.PNG
        controller.SaveTexture(s, os.path.join(out_dir, name))

    for eid in eids:
        controller.SetFrameEvent(eid, True)
        pipe = controller.GetPipelineState()
        log.write("== eid %d\n" % eid)
        try:
            used = pipe.GetReadOnlyResources(rd.ShaderStage.Pixel, True)
        except TypeError:
            used = pipe.GetReadOnlyResources(rd.ShaderStage.Pixel)
        seen = set()
        for u in used:
            rid = u.descriptor.resource
            if rid == rd.ResourceId.Null() or rid in seen:
                continue
            seen.add(rid)
            log.write("  in  %s\n" % describe(rid))
            save(rid, "in_%d_%d.png" % (eid, int(rid)))
        for o in pipe.GetOutputTargets():
            rid = o.resource
            if rid == rd.ResourceId.Null():
                continue
            log.write("  out %s\n" % describe(rid))
            save(rid, "out_%d_%d.png" % (eid, int(rid)))
        log.flush()
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
