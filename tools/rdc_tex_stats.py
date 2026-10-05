# Runs inside RenderDoc's UI Python (qrenderdoc --python tools/rdc_tex_stats.py), with
#   RDC_FILE = capture (either renderer), RDC_OUT = output text file, RDC_EIDS = "eid,eid,..." (draw event ids),
#   RDC_BOX = optional "x0:y0:x1:y1" in fractions of each texture (default the whole texture).
# For each draw: every texture its vertex and pixel shaders read and its first color output as it is after the draw,
# with the mean, smallest and largest value of each channel as numbers (float pictures keep their range, which a
# PNG does not). Compressed textures are only listed. Used to compare the post-processing of the two renderers
# step by step: the same pass on both must read and write the same means.
import os
import struct

import renderdoc as rd

cap_path = os.environ["RDC_FILE"]
out = open(os.environ["RDC_OUT"], "w")
eids = [int(e) for e in os.environ["RDC_EIDS"].split(",") if e]
box = [float(v) for v in os.environ.get("RDC_BOX", "0:0:1:1").split(":")]


def resources_of(state, stage):
    found = []
    try:
        used = state.GetReadOnlyResources(stage)
    except Exception as e:
        out.write("  GetReadOnlyResources failed: %s\n" % e)
        return found
    for u in used:
        if hasattr(u, "descriptor"):
            found.append(u.descriptor.resource)
        else:
            for b in u.resources:
                found.append(b.resourceId)
    return found


def stats(controller, t, slice_index=0):
    """Mean / min / max per channel of level 0, or None for formats not decoded here."""
    name = t.format.Name()
    data = controller.GetTextureData(t.resourceId, rd.Subresource(0, slice_index, 0))
    w, h = t.width, t.height
    if t.depth > 1:  # a 3D texture: every slice, one after another
        h = h * t.depth
    x0, y0, x1, y1 = int(box[0] * w), int(box[1] * h), max(int(box[2] * w), 1), max(int(box[3] * h), 1)
    step = max(1, (x1 - x0) // 320)  # every pixel of small pictures, a grid of the large ones
    rows = range(y0, y1, max(1, (y1 - y0) // 180))
    if "R10G10B10A2" in name or "A2B10G10R10" in name or "A2R10G10B10" in name:
        channels, pitch = 4, w * 4

        def texel(row, x):
            v = struct.unpack_from("<I", data, row * pitch + x * 4)[0]
            return (v & 1023) / 1023.0, ((v >> 10) & 1023) / 1023.0, ((v >> 20) & 1023) / 1023.0, (v >> 30) / 3.0
    else:
        channels = t.format.compCount
        width = t.format.compByteWidth
        kind = str(t.format.compType)
        if t.format.type != rd.ResourceFormatType.Regular or width not in (1, 2, 4):
            return None
        if "Float" in kind:
            code, scale = {2: "e", 4: "f"}.get(width), 1.0
        elif "Depth" in kind:
            return None
        else:
            code, scale = {1: "B", 2: "H", 4: "I"}[width], 1.0 / float((1 << (8 * width)) - 1)
        if code is None:
            return None
        pitch = w * channels * width
        fmt = "<%d%s" % (channels, code)

        def texel(row, x):
            return tuple(v * scale for v in struct.unpack_from(fmt, data, row * pitch + x * channels * width))
    total = [0.0] * channels
    lo = [1e30] * channels
    hi = [-1e30] * channels
    n = 0
    bad = 0
    for row in rows:
        if (row + 1) * pitch > len(data):
            break
        for x in range(x0, x1, step):
            v = texel(row, x)
            if any(c != c for c in v):
                bad += 1
                continue
            n += 1
            for k in range(channels):
                total[k] += v[k]
                lo[k] = min(lo[k], v[k])
                hi[k] = max(hi[k], v[k])
    if not n:
        return "no valid texel (%d NaN)" % bad
    return "mean %s  min %s  max %s%s" % (" ".join("%.5f" % (c / n) for c in total),
                                          " ".join("%.4f" % c for c in lo), " ".join("%.4f" % c for c in hi),
                                          "  NaN %d" % bad if bad else "")


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
        for label, stage in (("vs", rd.ShaderStage.Vertex), ("ps", rd.ShaderStage.Pixel)):
            seen = set()
            for rid in resources_of(state, stage):
                t = textures.get(int(rid))
                if t is None or int(rid) in seen:
                    continue
                seen.add(int(rid))
                try:
                    s = stats(controller, t)
                except Exception as e:
                    s = "failed: %s" % e
                out.write("  %s res %d %dx%dx%d levels %d %s (%d bytes): %s\n" % (
                    label, int(rid), t.width, t.height, t.depth, t.mips, t.format.Name(),
                    len(controller.GetTextureData(t.resourceId, rd.Subresource(0, 0, 0))), s or "(not decoded)"))
                # A cube map or an array: the other faces too (face 0 alone hid a cube whose faces differ).
                for face in range(1, min(t.arraysize, 6)):
                    try:
                        s = stats(controller, t, face)
                    except Exception as e:
                        s = "failed: %s" % e
                    out.write("      face %d: %s\n" % (face, s or "(not decoded)"))
        try:
            targets = state.GetOutputTargets()
            for k, o in enumerate(targets):
                rid = o.resource if hasattr(o, "resource") else o.resourceId
                t = textures.get(int(rid))
                if t is None:
                    continue
                s = stats(controller, t) if t.msSamp == 1 else "(multisampled)"
                out.write("  out%d res %d %dx%d %s: %s\n" % (k, int(rid), t.width, t.height, t.format.Name(),
                                                           s or "(not decoded)"))
        except Exception as e:
            out.write("  outputs failed: %s\n" % e)
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
