# Runs inside RenderDoc's UI Python (qrenderdoc --python tools/rdc_constants.py), with
#   RDC_FILE = capture, RDC_OUT = output text file,
#   RDC_COUNTS = "n,n,..." index counts of the draws wanted (triangles * 3) and / or RDC_EIDS = "eid,..." event
#   ids, RDC_MAX = draws listed per count
#   (default 2), RDC_STAGE = "vs" for the vertex shader's buffers instead of the pixel shader's.
# For each such draw: its render targets, the pixel shader's textures (resource, size, format) and every constant
# buffer of the pixel shader as floats. The emulated GPU packs the float constants a shader uses in rising order
# of their register number (c0..c255), so the n-th float4 of the buffer named xe_float_constants is the n-th
# register the shader uses. Used to compare a draw's constants with the native renderer's
# (--fh1_native_diag_constants_ps).
import os
import struct

import renderdoc as rd

cap_path = os.environ["RDC_FILE"]
out_path = os.environ["RDC_OUT"]
counts = [int(c) for c in os.environ.get("RDC_COUNTS", "").split(",") if c]
eids = [int(c) for c in os.environ.get("RDC_EIDS", "").split(",") if c]
per_count = int(os.environ.get("RDC_MAX", "2"))
out = open(out_path, "w")


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
    textures = {t.resourceId: t for t in controller.GetTextures()}
    found = []

    def walk(actions):
        for a in actions:
            # depth-only passes (no color output) are left out
            if a.eventId in eids:
                found.append(a)
            elif (a.flags & rd.ActionFlags.Drawcall) and a.numIndices in counts and any(
                    o != rd.ResourceId.Null() for o in a.outputs):
                found.append(a)
            walk(a.children)

    walk(controller.GetRootActions())
    seen = {}
    for a in found:
        seen[a.numIndices] = seen.get(a.numIndices, 0) + 1
        if seen[a.numIndices] > per_count:
            continue
        controller.SetFrameEvent(a.eventId, True)
        pipe = controller.GetPipelineState()
        outs = [int(o) for o in a.outputs if o != rd.ResourceId.Null()]
        out.write("draw eid %d indices %d outputs %s\n" % (a.eventId, a.numIndices, outs))
        stage = rd.ShaderStage.Vertex if os.environ.get("RDC_STAGE", "") == "vs" else rd.ShaderStage.Pixel
        try:
            for used in pipe.GetReadOnlyResources(stage):
                rid = used.descriptor.resource
                t = textures.get(rid)
                if t:
                    out.write("  tex bind %d res %d %dx%d layers %d mips %d %s\n" % (
                        used.access.index, int(rid), t.width, t.height, t.arraysize, t.mips, t.format.Name()))
        except Exception as e:
            out.write("  textures: %s\n" % e)
        refl = pipe.GetShaderReflection(stage)
        if refl is None:
            continue
        for i, block in enumerate(refl.constantBlocks):
            try:
                used = pipe.GetConstantBlock(stage, i, 0)
                d = used.descriptor
                rid, offset, size = d.resource, d.byteOffset, d.byteSize
            except Exception as e:
                out.write("  cbuffer %s: %s\n" % (block.name, e))
                continue
            if rid == rd.ResourceId.Null():
                continue
            size = min(size if size else block.byteSize, 4096)
            data = controller.GetBufferData(rid, offset, size)
            n = len(data) // 4
            values = struct.unpack("<%df" % n, data[:n * 4])
            raw = struct.unpack("<%dI" % n, data[:n * 4])
            out.write("  cbuffer %d %s (%d bytes)\n" % (i, block.name, len(data)))
            for k in range(0, n, 4):
                if any(raw[k:k + 4]):
                    out.write("    [%d] %s  | %s\n" % (
                        k // 4, " ".join("%.5g" % v for v in values[k:k + 4]),
                        " ".join("%08X" % v for v in raw[k:k + 4])))
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
