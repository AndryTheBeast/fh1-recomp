"""Finds which EDRAM ownership-transfer kinds FH1 needs for a correct picture: runs the game with
--gpu_skip_all_transfers and a --gpu_keep_transfers subset, and judges the 80 s festival shot with
tools/img_diff.py's dome-warmth measure (fog on far scenery = a needed transfer was skipped).
    python tools/bisect_transfers.py KINDS_LOG [extra game args]
KINDS_LOG: a run log made with --gpu_log_transfer_kinds=true (normal mode)."""
import glob, os, re, subprocess, sys
import numpy as np
from PIL import Image

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
LOGS = os.path.join(os.path.dirname(REPO), 'build_logs')
AP = '33+0.2=start;33.8+0.2=start;34.6+0.2=start;35.5+0.2=a;36.8+0.2=a;38.1+0.2=a'
EXTRA = ' '.join(sys.argv[2:]) or '--gpu_backend=vulkan'
run_no = 0


def warmth(path):
    a = np.asarray(Image.open(path).convert('RGB'), dtype=np.float32)
    h, w, _ = a.shape
    dome = a[int(h * 0.355):int(h * 0.395), int(w * 0.72):int(w * 0.82)]
    return float((dome[..., 0] - dome[..., 2]).mean())


def good(keep):
    # A shot that is missing or exactly neutral is not the game (window not there): try again.
    for _ in range(3):
        w = run(keep)
        if w is not None and abs(w) > 0.5:
            return w > 20
    sys.exit('no usable screenshot')


def run(keep):
    global run_no
    run_no += 1
    name = f'bisect{run_no}'
    args = f'{EXTRA} --gpu_skip_all_transfers=true --gpu_keep_transfers={",".join(keep) or "none"}'
    subprocess.run(['powershell', '-ExecutionPolicy', 'Bypass', '-File',
                    os.path.join(REPO, 'tools', 'auto_test.ps1'), '-Name', name, '-Seconds', '84',
                    '-Shots', '80', '-Autoplay', AP, '-ExtraArgs', args],
                   stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    shots = sorted(glob.glob(os.path.join(LOGS, f'test-{name}-*-80s.png')))
    if not shots:
        print(f'  run {run_no}: no screenshot', flush=True)
        return None
    w = warmth(shots[-1])
    print(f'  run {run_no}: keep {len(keep)} kinds -> warmth {w:.1f} '
          f'({os.path.basename(shots[-1])})', flush=True)
    return w


kinds = []
for line in open(sys.argv[1], encoding='utf-8', errors='replace'):
    m = re.search(r'new transfer kind (\S+)', line)
    if m and m.group(1) not in kinds:
        kinds.append(m.group(1))
print(f'{len(kinds)} kinds', flush=True)
if not good(kinds):
    sys.exit('keeping every kind is not good - check the measure')

# Shrink: drop halves that are not needed (works for several needed kinds too).
needed = list(kinds)
chunk = len(needed) // 2
while chunk >= 1:
    i = 0
    while i < len(needed):
        trial = needed[:i] + needed[i + chunk:]
        if good(trial):
            print(f'  not needed: {needed[i:i + chunk]}', flush=True)
            needed = trial
        else:
            i += chunk
    chunk //= 2
print('NEEDED:', ','.join(needed), flush=True)
