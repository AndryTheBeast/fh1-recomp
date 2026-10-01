# Runs inside RenderDoc's UI Python (qrenderdoc --python tools/rdc_find.py), with
#   RDC_FILE = capture, RDC_OUT = output text file,
#   RDC_MATCH = "WxH:FORMAT" filter on a draw's render targets (e.g. "80x8192:R32", "x:R32_FLOAT";
#               either part may be empty), RDC_MAX = maximum draws listed (default 40).
# Lists every draw writing a matching target with the min/max of its pixel-shader textures and its
# targets after the draw - to compare the same pass between two captures (e.g. D3D12 vs Vulkan).
import os

import renderdoc as rd

cap_path = os.environ["RDC_FILE"]
out_path = os.environ["RDC_OUT"]
size_f, _, fmt_f = os.environ.get("RDC_MATCH", ":").partition(":")
max_draws = int(os.environ.get("RDC_MAX", "40"))
log = open(out_path, "w")


def run():
    cap = rd.OpenCaptureFile()
    if cap.OpenFile(cap_path, "", None) != rd.ResultCode.Succeeded:
        log.write("OpenFile failed\n")
        return
    result, controller = cap.OpenCapture(rd.ReplayOptions(), None)
    if result != rd.ResultCode.Succeeded:
        log.write("OpenCapture failed\n")
        return
    textures = {t.resourceId: t for t in controller.GetTextures()}

    def matches(t):
        if t is None:
            return False
        if size_f and ("%dx%d" % (t.width, t.height)).find(size_f) < 0:
            return False
        return not fmt_f or t.format.Name().find(fmt_f) >= 0

    def describe(rid):
        t = textures.get(rid)
        if t is None:
            return "%d (buffer?)" % int(rid)
        mn, mx = controller.GetMinMax(rid, rd.Subresource(0, 0, 0), rd.CompType.Typeless)
        return "%d %dx%d s%d %s min=(%.4g %.4g %.4g %.4g) max=(%.4g %.4g %.4g %.4g)" % (
            int(rid), t.width, t.height, t.msSamp, t.format.Name(), mn.floatValue[0],
            mn.floatValue[1], mn.floatValue[2], mn.floatValue[3], mx.floatValue[0],
            mx.floatValue[1], mx.floatValue[2], mx.floatValue[3])

    found = []

    def walk(actions):
        for a in actions:
            if a.children:
                walk(a.children)
            if not (a.flags & rd.ActionFlags.Drawcall):
                continue
            outs = [o for o in a.outputs if o != rd.ResourceId.Null()]
            if any(matches(textures.get(o)) for o in outs):
                found.append(a.eventId)

    walk(controller.GetRootActions())
    log.write("%d matching draws\n" % len(found))
    for eid in found[:max_draws]:
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
            if rid != rd.ResourceId.Null() and rid not in seen:
                seen.add(rid)
                log.write("  in  %s\n" % describe(rid))
        for o in pipe.GetOutputTargets():
            if o.resource != rd.ResourceId.Null():
                log.write("  out %s\n" % describe(o.resource))
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
