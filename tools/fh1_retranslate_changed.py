r"""After a change to the shader translator (shaders/XenosRecomp): rebuild only the shaders whose HLSL changes.

    powershell -ExecutionPolicy Bypass -File tools\build_shader_tools.ps1
    python tools/fh1_retranslate_changed.py            (lists what changes)
    python tools/fh1_retranslate_changed.py --apply    (compiles those, replaces them in hlsl/ and spirv/)

Then repack: move build_logs\shaders\fh1_shaders.nfsp away, run
    shaders\fh1_pack_library.exe build_logs\shaders\containers build_logs\shaders\spirv build_logs\shaders\fh1_shaders.nfsp
and copy the result next to fh1.exe. The replaced files are kept in hlsl_before_change / spirv_before_change.
"""
import filecmp, os, shutil, subprocess, sys

top = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
repo = os.path.join(top, "fh1-recomp")
sh = os.path.join(top, "build_logs", "shaders")
new = os.path.join(sh, "hlsl_new")
chg = os.path.join(sh, "hlsl_changed")
for d in (new, chg):
    shutil.rmtree(d, ignore_errors=True)
subprocess.run([sys.executable, os.path.join(repo, "tools", "fh1_translate_shaders.py"),
                os.path.join(sh, "containers"), new], check=True)
os.makedirs(chg)
old = os.path.join(sh, "hlsl")
changed, missing = [], []
for f in sorted(os.listdir(new)):
    if not f.endswith(".hlsl"):
        continue
    o = os.path.join(old, f)
    if not os.path.exists(o):
        missing.append(f)
    elif not filecmp.cmp(o, os.path.join(new, f), shallow=False):
        changed.append(f)
print("translated", len([f for f in os.listdir(new) if f.endswith(".hlsl")]), "old", len(os.listdir(old)),
      "changed", len(changed), "not in old", len(missing))
print("changed:", " ".join(changed[:60]))
for f in changed + missing:
    shutil.copy(os.path.join(new, f), chg)
if "--apply" in sys.argv:
    out = os.path.join(sh, "spirv_changed")
    shutil.rmtree(out, ignore_errors=True)
    subprocess.run([sys.executable, os.path.join(repo, "tools", "fh1_compile_shaders.py"), chg, out,
                    os.path.join(top, "tools_dxc", "bin", "x64", "dxc.exe")], check=True)
    print(open(os.path.join(out, "dxc_errors.txt")).read()[:3000])
    bh, bs = os.path.join(sh, "hlsl_before_change"), os.path.join(sh, "spirv_before_change")
    os.makedirs(bh, exist_ok=True); os.makedirs(bs, exist_ok=True)
    done = 0
    for f in changed + missing:
        base = f[:-5]
        spv = os.path.join(out, base + ".spv")
        if not os.path.exists(spv):
            continue
        if os.path.exists(os.path.join(old, f)):
            shutil.copy(os.path.join(old, f), bh)
        if os.path.exists(os.path.join(sh, "spirv", base + ".spv")):
            shutil.copy(os.path.join(sh, "spirv", base + ".spv"), bs)
        shutil.copy(os.path.join(new, f), old)
        shutil.copy(spv, os.path.join(sh, "spirv"))
        done += 1
    print("replaced", done)
