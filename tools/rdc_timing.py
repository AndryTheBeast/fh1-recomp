# Runs inside RenderDoc's UI Python (qrenderdoc --python tools/rdc_timing.py), with
#   RDC_FILE = capture, RDC_OUT = output text file.
# Measures the GPU duration of every action in the capture (GPUDuration counter) and sums it per
# pass: consecutive actions writing the same set of render targets. Lists the passes in frame order
# with their time, draw count and target sizes, then the 25 most expensive single actions - for
# finding which parts of FH1's frame cost the most host GPU time.
import os

import renderdoc as rd

cap_path = os.environ["RDC_FILE"]
log = open(os.environ["RDC_OUT"], "w")


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
    sf = controller.GetStructuredFile()

    counters = controller.EnumerateCounters()
    if rd.GPUCounter.EventGPUDuration not in counters:
        log.write("no GPU duration counter\n")
        return
    results = controller.FetchCounters([rd.GPUCounter.EventGPUDuration])
    desc = controller.DescribeCounter(rd.GPUCounter.EventGPUDuration)
    log.write("counter %s: %d results, unit %s, type %s, width %d\n" % (
        desc.name, len(results), str(desc.unit), str(desc.resultType), desc.resultByteWidth))
    duration = {}
    for r in results:
        if desc.resultType == rd.CompType.Float:
            v = r.value.d if desc.resultByteWidth == 8 else r.value.f
        else:
            v = float(r.value.u64 if desc.resultByteWidth == 8 else r.value.u32)
        duration[r.eventId] = v
    for r in results[:5]:
        log.write("  sample eid %d: d=%r f=%r u64=%r\n" % (r.eventId, r.value.d, r.value.f, r.value.u64))

    actions = []

    def walk(items):
        for a in items:
            if a.children:
                walk(a.children)
            else:
                actions.append(a)

    walk(controller.GetRootActions())

    def target_desc(ids):
        parts = []
        for rid in ids:
            t = textures.get(rid)
            if t is not None:
                parts.append("%dx%d%s %s" % (t.width, t.height,
                                             (" s%d" % t.msSamp) if t.msSamp > 1 else "",
                                             t.format.Name()))
        return ", ".join(parts) if parts else "-"

    passes = []
    total = 0.0
    for a in actions:
        d = duration.get(a.eventId, 0.0)
        total += d
        outs = tuple(o for o in a.outputs if o != rd.ResourceId.Null())
        if a.depthOut != rd.ResourceId.Null():
            outs = outs + (a.depthOut,)
        is_draw = bool(a.flags & rd.ActionFlags.Drawcall)
        kind = "draw" if is_draw else ("dispatch" if a.flags & rd.ActionFlags.Dispatch else "other")
        key = outs if is_draw else ("(%s)" % kind,)
        if passes and passes[-1]["key"] == key:
            p = passes[-1]
        else:
            p = {"key": key, "first": a.eventId, "time": 0.0, "count": 0,
                 "desc": target_desc(outs) if is_draw else "(%s)" % kind}
            passes.append(p)
        p["time"] += d
        p["count"] += 1

    log.write("total GPU time %.2f ms over %d actions, %d passes\n\n" % (
        total * 1000.0, len(actions), len(passes)))
    for p in passes:
        if p["time"] * 1000.0 < 0.05:
            continue
        log.write("%7.3f ms %5d actions  eid %6d  %s\n" % (p["time"] * 1000.0, p["count"],
                                                         p["first"], p["desc"]))
    log.write("\nmost expensive actions:\n")
    top = sorted(actions, key=lambda a: -duration.get(a.eventId, 0.0))[:25]
    for a in top:
        log.write("%7.3f ms eid %6d %s\n" % (duration.get(a.eventId, 0.0) * 1000.0, a.eventId,
                                             a.GetName(sf)))
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
