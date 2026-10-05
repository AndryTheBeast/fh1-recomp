# Runs inside RenderDoc's UI Python (qrenderdoc --python tools/rdc_dump.py), with
#   RDC_FILE = capture to open, RDC_OUT = output folder, RDC_SAVE = optional "eid:resid,..." list.
# Writes actions.txt (every action: event id, name, outputs), textures.txt, and PNGs of every
# render target of at least 640 pixels width as it is after the last action that writes it
# (plus any "eid:resid" pairs asked for in RDC_SAVE). Used for unattended graphics debugging.
import os

import renderdoc as rd

cap_path = os.environ["RDC_FILE"]
out_dir = os.environ["RDC_OUT"]
save_list = os.environ.get("RDC_SAVE", "")
os.makedirs(out_dir, exist_ok=True)
log = open(os.path.join(out_dir, "dump_log.txt"), "w")


def run():
    cap = rd.OpenCaptureFile()
    result = cap.OpenFile(cap_path, "", None)
    if result != rd.ResultCode.Succeeded:
        log.write("OpenFile failed: %s\n" % str(result))
        return
    result, controller = cap.OpenCapture(rd.ReplayOptions(), None)
    if result != rd.ResultCode.Succeeded:
        log.write("OpenCapture failed: %s\n" % str(result))
        return

    sf = controller.GetStructuredFile()
    names = {r.resourceId: r.name for r in controller.GetResources()}
    textures = {t.resourceId: t for t in controller.GetTextures()}

    with open(os.path.join(out_dir, "textures.txt"), "w") as f:
        for rid, t in textures.items():
            f.write("%s %s %dx%d samples=%d fmt=%s\n" % (
                str(int(rid)), names.get(rid, ""), t.width, t.height, t.msSamp, t.format.Name()))

    last_write = {}
    lines = []

    def walk(actions, depth):
        for a in actions:
            outs = [o for o in a.outputs if o != rd.ResourceId.Null()]
            if a.depthOut != rd.ResourceId.Null():
                outs.append(a.depthOut)
            for o in outs:
                last_write[o] = a.eventId
            lines.append("%s%d %s -> %s" % (
                "  " * depth, a.eventId, a.GetName(sf),
                ",".join(str(int(o)) for o in outs)))
            walk(a.children, depth + 1)

    walk(controller.GetRootActions(), 0)
    with open(os.path.join(out_dir, "actions.txt"), "w") as f:
        f.write("\n".join(lines))

    def save(eid, rid, name):
        controller.SetFrameEvent(eid, True)
        ts = rd.TextureSave()
        ts.resourceId = rid
        ts.destType = rd.FileType.PNG
        ts.mip = 0
        ts.slice.sliceIndex = 0
        ts.sample.sampleIndex = 0
        # RDC_ALPHA=1 keeps the alpha channel in the PNG (masks that carry data there)
        ts.alpha = rd.AlphaMapping.Preserve if os.environ.get("RDC_ALPHA") else rd.AlphaMapping.Discard
        path = os.path.join(out_dir, name)
        ok = controller.SaveTexture(ts, path)
        log.write("save eid %d res %d -> %s (%s)\n" % (eid, int(rid), path, str(ok)))

    for rid, eid in last_write.items():
        t = textures.get(rid)
        if t and t.width >= 640:
            save(eid, rid, "last_res%d_eid%d.png" % (int(rid), eid))

    for item in [s for s in save_list.split(",") if s]:
        eid_s, rid_s = item.split(":")
        rid = next((r for r in textures if int(r) == int(rid_s)), None)
        if rid is not None:
            save(int(eid_s), rid, "req_res%s_eid%s.png" % (rid_s, eid_s))

    controller.Shutdown()
    cap.Shutdown()


try:
    run()
except Exception as e:
    import traceback
    log.write(traceback.format_exc())
log.write("done\n")
log.close()
os._exit(0)
