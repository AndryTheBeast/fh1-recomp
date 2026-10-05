# Runs inside RenderDoc's UI Python (qrenderdoc --python tools/rdc_draw_state.py), with
#   RDC_FILE = capture (of the emulated GPU on Direct3D 12), RDC_OUT = output text file,
#   RDC_EIDS = "eid,eid,..." (draw event ids).
# For each draw: the positions its vertex shader outputs (first 8 vertices), the viewport, the depth and stencil
# test, the blend of target 0 and the write mask. Answers "does this pass draw anything, and where" for one pass
# of the emulated GPU, to compare with the native renderer's one-frame trace.
import os
import struct

import renderdoc as rd

cap_path = os.environ["RDC_FILE"]
out_path = os.environ["RDC_OUT"]
eids = [int(e) for e in os.environ["RDC_EIDS"].split(",") if e]
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
    for eid in eids:
        controller.SetFrameEvent(eid, True)
        out.write("draw eid %d\n" % eid)
        try:
            mesh = controller.GetPostVSData(0, 0, rd.MeshDataStage.VSOut)
            count = min(mesh.numIndices, 8)
            out.write("  vertices %d stride %d topology %s\n" % (mesh.numIndices, mesh.vertexByteStride,
                                                                str(mesh.topology)))
            if mesh.vertexResourceId != rd.ResourceId.Null() and count:
                data = controller.GetBufferData(mesh.vertexResourceId, mesh.vertexByteOffset,
                                                mesh.vertexByteStride * count)
                for i in range(count):
                    p = struct.unpack_from("<4f", data, i * mesh.vertexByteStride)
                    out.write("    pos %d: %.5g %.5g %.5g %.5g\n" % ((i,) + p))
        except Exception as e:
            out.write("  post-VS: %s\n" % e)
        try:
            state = controller.GetD3D12PipelineState()
            for v in state.rasterizer.viewports[:1]:
                out.write("  viewport x %.5g y %.5g w %.5g h %.5g depth %.5g..%.5g\n" % (
                    v.x, v.y, v.width, v.height, v.minDepth, v.maxDepth))
            rs = state.rasterizer.state
            out.write("  depth clip %s cull %s\n" % (str(rs.depthClip), str(rs.cullMode)))
            ds = state.outputMerger.depthStencilState
            out.write("  depth test %s write %s func %s | stencil %s front func %s ref %s\n" % (
                str(ds.depthEnable), str(ds.depthWrites), str(ds.depthFunction), str(ds.stencilEnable),
                str(ds.frontFace.function), str(ds.frontFace.reference)))
            b = state.outputMerger.blendState.blends[0]
            out.write("  blend %s color %s*src %s %s*dst | alpha %s %s %s | mask %s\n" % (
                str(b.enabled), str(b.colorBlend.source), str(b.colorBlend.operation),
                str(b.colorBlend.destination), str(b.alphaBlend.source), str(b.alphaBlend.operation),
                str(b.alphaBlend.destination), str(b.writeMask)))
        except Exception as e:
            out.write("  state: %s\n" % e)
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
