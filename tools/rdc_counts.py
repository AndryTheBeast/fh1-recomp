# Runs inside RenderDoc's UI Python (qrenderdoc --python tools/rdc_counts.py), with
#   RDC_FILE = capture (either renderer), RDC_OUT = output text file,
#   RDC_COUNT = optional "n,n,..." index / vertex counts to keep (default: every draw).
# Lists the draws in frame order: event id, index or vertex count, instances, first color output.
# The native trace gives each draw's count ("count N"): this finds the same draw in a capture of either renderer.
import os

import renderdoc as rd

cap_path = os.environ["RDC_FILE"]
out = open(os.environ["RDC_OUT"], "w")
wanted = [int(c) for c in os.environ.get("RDC_COUNT", "").split(",") if c]


def walk(actions):
    for a in actions:
        if a.flags & rd.ActionFlags.Drawcall and (not wanted or a.numIndices in wanted):
            out.write("eid %d count %d instances %d out %d\n" % (a.eventId, a.numIndices, a.numInstances,
                                                               int(a.outputs[0])))
        walk(a.children)


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
    walk(controller.GetRootActions())
    controller.Shutdown()
    cap.Shutdown()


try:
    run()
except Exception as e:
    out.write("failed: %s\n" % e)
out.write("done\n")
out.close()
os._exit(0)
