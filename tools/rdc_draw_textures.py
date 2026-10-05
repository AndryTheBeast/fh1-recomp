# Runs inside RenderDoc's UI Python (qrenderdoc --python tools/rdc_draw_textures.py), with
#   RDC_FILE = capture (either renderer: Direct3D 12 or Vulkan), RDC_OUT = output folder,
#   RDC_EIDS = "eid,eid,..." (draw event ids).
# For each draw: the textures its pixel shader reads (resource id, size, format, levels), each saved as a PNG
# (tex_<eid>_<n>_<resource>.png, level 0), and the smallest / largest value of each. textures.txt has the list.
# Answers "does this draw get the same textures on both renderers".
import os

import renderdoc as rd

cap_path = os.environ["RDC_FILE"]
out_dir = os.environ["RDC_OUT"]
eids = [int(e) for e in os.environ["RDC_EIDS"].split(",") if e]
os.makedirs(out_dir, exist_ok=True)
out = open(os.path.join(out_dir, "draw_textures.txt"), "w")


def resources_of(state, stage):
    found = []
    try:
        used = state.GetReadOnlyResources(stage)
    except Exception as e:
        out.write("  GetReadOnlyResources failed: %s\n" % e)
        return found
    for u in used:
        if hasattr(u, "descriptor"):  # RenderDoc 1.33 and later
            found.append((getattr(u.access, "index", -1), u.descriptor.resource))
        else:
            for k, b in enumerate(u.resources):
                found.append((u.bindPoint.bind if hasattr(u, "bindPoint") else k, b.resourceId))
    return found


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
    textures = {int(t.resourceId): t for t in controller.GetTextures()}
    for eid in eids:
        controller.SetFrameEvent(eid, True)
        state = controller.GetPipelineState()
        out.write("draw eid %d\n" % eid)
        seen = set()
        for n, (bind, rid) in enumerate(resources_of(state, rd.ShaderStage.Pixel)):
            t = textures.get(int(rid))
            if t is None or int(rid) in seen:
                continue
            seen.add(int(rid))
            line = "  [%d] bind %s res %d %dx%d depth %d levels %d %s" % (
                n, bind, int(rid), t.width, t.height, t.depth, t.mips, t.format.Name())
            try:
                lo, hi = controller.GetMinMax(t.resourceId, rd.Subresource(0, 0, 0), rd.CompType.Typeless)
                line += "  min %s max %s" % ([round(v, 4) for v in lo.floatValue[:4]],
                                              [round(v, 4) for v in hi.floatValue[:4]])
            except Exception as e:
                line += "  (min/max failed: %s)" % e
            out.write(line + "\n")
            save = rd.TextureSave()
            save.resourceId = t.resourceId
            save.destType = rd.FileType.PNG
            save.mip = 0
            save.slice.sliceIndex = 0
            save.alpha = rd.AlphaMapping.Discard
            controller.SaveTexture(save, os.path.join(out_dir, "tex_%d_%d_%d.png" % (eid, n, int(rid))))
            save.alpha = rd.AlphaMapping.Preserve
            controller.SaveTexture(save, os.path.join(out_dir, "tex_%d_%d_%d_alpha.png" % (eid, n, int(rid))))
    controller.Shutdown()
    cap.Shutdown()


try:
    run()
except Exception:
    import traceback
    out.write(traceback.format_exc())
out.write("done\n")
out.close()
os._exit(0)
