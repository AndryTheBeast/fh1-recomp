"""Compiles the translated FH1 shaders (HLSL from shaders/fh1_hlsl.exe) to SPIR-V with DXC, with the
options nfsmw-nx used (shaders/nfsmw_regenerar_biblioteca_pcf.sh). Native renderer step N0.

    python tools/fh1_compile_shaders.py HLSL_DIR SPIRV_DIR [DXC]

DXC defaults to the Windows SDK's (Windows Kits 10, x64). Failures go to SPIRV_DIR/dxc_errors.txt.
"""
import concurrent.futures
import glob
import os
import subprocess
import sys

hlsl_dir, spirv_dir = sys.argv[1], sys.argv[2]
dxc = sys.argv[3] if len(sys.argv) > 3 else None
if not dxc:
    kits = r'C:\Program Files (x86)\Windows Kits\10\bin'
    found = sorted(glob.glob(os.path.join(kits, '10.*', 'x64', 'dxc.exe')))
    if not found:
        sys.exit('dxc.exe not found - pass its path')
    dxc = found[-1]
os.makedirs(spirv_dir, exist_ok=True)


def compile_one(path):
    base = os.path.splitext(os.path.basename(path))[0]
    target, extra = ('ps_6_6', []) if base.startswith('p_') else ('vs_6_6', ['-fvk-invert-y'])
    out = os.path.join(spirv_dir, base + '.spv')
    r = subprocess.run([dxc, '-spirv', '-T', target, '-E', 'main', '-HV', '2021',
                        '-fspv-target-env=vulkan1.2', '-fvk-use-dx-layout', *extra, '-Fo', out, path],
                       capture_output=True, text=True)
    return base, r.returncode, (r.stderr or r.stdout)


files = sorted(glob.glob(os.path.join(hlsl_dir, '*.hlsl')))
ok, errors = 0, []
with concurrent.futures.ThreadPoolExecutor(max_workers=min(6, os.cpu_count() or 1)) as pool:
    for base, code, text in pool.map(compile_one, files):
        if code == 0:
            ok += 1
        else:
            errors.append(f'== {base}\n{text}')
with open(os.path.join(spirv_dir, 'dxc_errors.txt'), 'w') as f:
    f.write('\n'.join(errors))
print(f'{ok} of {len(files)} compiled with {dxc}')
