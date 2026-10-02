"""Translates every FH1 shader container to HLSL with shaders/fh1_hlsl.exe, one process per
container, in parallel. Containers dumped from guest memory can be damaged and crash the
translator; a crash only drops that container. Native renderer step N0.

    python tools/fh1_translate_shaders.py CONTAINERS_DIR HLSL_DIR

Log: HLSL_DIR/../traduccion.log (UTF-8): one line per container that failed, then a summary.
"""
import concurrent.futures
import os
import shutil
import subprocess
import sys
import tempfile

repo = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
translator = os.path.join(repo, 'shaders', 'fh1_hlsl.exe')
common = os.path.join(repo, 'shaders', 'XenosRecomp', 'shader_common.h')
containers_dir, hlsl_dir = sys.argv[1], sys.argv[2]
os.makedirs(hlsl_dir, exist_ok=True)
work = tempfile.mkdtemp(prefix='fh1_hlsl_')


def translate(name):
    base = os.path.splitext(name)[0]
    src_dir = os.path.join(work, base + '_in')
    out_dir = os.path.join(work, base + '_out')
    os.makedirs(src_dir)
    shutil.copy(os.path.join(containers_dir, name), src_dir)
    r = subprocess.run([translator, src_dir, out_dir, common], capture_output=True, text=True, errors='replace')
    produced = os.path.join(out_dir, base + '.hlsl')
    ok = os.path.exists(produced)
    if ok:
        shutil.move(produced, os.path.join(hlsl_dir, base + '.hlsl'))
    reason = ''
    if not ok:
        lines = [l.strip() for l in (r.stdout + r.stderr).splitlines() if 'rechazada' in l or 'error' in l.lower()]
        reason = lines[-1] if lines else f'translator exit code {r.returncode}'
    shutil.rmtree(src_dir, ignore_errors=True)
    shutil.rmtree(out_dir, ignore_errors=True)
    return name, ok, reason


names = sorted(f for f in os.listdir(containers_dir) if f.endswith('.bin'))
failed = []
with concurrent.futures.ThreadPoolExecutor(max_workers=min(6, os.cpu_count() or 1)) as pool:
    for name, ok, reason in pool.map(translate, names):
        if not ok:
            failed.append(f'{name}: {reason}')
shutil.rmtree(work, ignore_errors=True)
summary = f'{len(names)} shaders: {len(names) - len(failed)} translated, {len(failed)} skipped'
with open(os.path.join(os.path.dirname(os.path.abspath(hlsl_dir)), 'traduccion.log'), 'w', encoding='utf-8') as f:
    f.write('\n'.join(failed + [summary]) + '\n')
print(summary)
