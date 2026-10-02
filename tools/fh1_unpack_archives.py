"""Unpacks files from FH1's zip archives, including entries stored with method 21 (Xbox XMemCompress
LZX, 128 KB window). FH1's archives have no local file headers: an entry's data starts right at the
offset in the central directory. Each method-21 entry is a series of frames: 0xFF, 16-bit
uncompressed size, 16-bit block size (or just a 16-bit block size for a full 32 KB frame), all
big-endian, then the LZX block. Decoded by shaders/fh1_lzx_decode.exe; every file is checked against
the zip CRC. Native renderer step N0 (docs/native-renderer-fh1.md).

    python tools/fh1_unpack_archives.py GAME_ROOT OUT_DIR [SUFFIX ...]

Default suffix: .fxobj (shader files). Files with the same name and CRC are written once.
"""
import concurrent.futures
import os
import struct
import subprocess
import sys
import tempfile
import zipfile
import zlib

repo = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
decoder = os.path.join(repo, 'shaders', 'fh1_lzx_decode.exe')
game_root, out_dir = sys.argv[1], sys.argv[2]
suffixes = tuple(s.lower() for s in sys.argv[3:]) or ('.fxobj',)
os.makedirs(out_dir, exist_ok=True)
work = tempfile.mkdtemp(prefix='fh1_unpack_')


def blocks(data, size):
    """Strips the XMemCompress frame headers; returns the concatenated LZX blocks."""
    out, pos, produced = bytearray(), 0, 0
    while produced < size and pos + 2 <= len(data):
        if data[pos] == 0xFF:
            frame, block = struct.unpack_from('>HH', data, pos + 1)
            pos += 5
        else:
            frame, block = 32768, struct.unpack_from('>H', data, pos)[0]
            pos += 2
        if block == 0:
            break
        out += data[pos:pos + block]
        pos += block
        produced += frame
    return bytes(out)


def unpack(job):
    archive, info, target = job
    with open(archive, 'rb') as f:
        f.seek(info.header_offset)
        data = f.read(info.compress_size)
    if info.compress_type == 0:
        content = data
    elif info.compress_type == 21:
        tmp_in = os.path.join(work, f'{os.getpid()}_{id(job)}.in')
        tmp_out = tmp_in[:-3] + '.out'
        with open(tmp_in, 'wb') as f:
            f.write(blocks(data, info.file_size))
        r = subprocess.run([decoder, tmp_in, tmp_out, str(info.file_size)], capture_output=True, text=True,
                           timeout=60)
        os.remove(tmp_in)
        if r.returncode != 0:
            return target, f'decode failed: {r.stderr.strip()}'
        with open(tmp_out, 'rb') as f:
            content = f.read()
        os.remove(tmp_out)
    else:
        return target, f'method {info.compress_type} not handled'
    if zlib.crc32(content) != info.CRC:
        return target, 'CRC mismatch'
    os.makedirs(os.path.dirname(target), exist_ok=True)
    with open(target, 'wb') as f:
        f.write(content)
    return target, None


jobs, seen = [], set()
for dp, _, files in os.walk(game_root):
    for name in files:
        if not name.lower().endswith('.zip'):
            continue
        archive = os.path.join(dp, name)
        try:
            infos = zipfile.ZipFile(archive).infolist()
        except zipfile.BadZipFile:
            continue
        for info in infos:
            entry = info.filename.replace('\\', '/')
            if not entry.lower().endswith(suffixes):
                continue
            key = (entry.lower(), info.CRC)
            if key in seen:
                continue
            seen.add(key)
            # Same name with different content in two archives: keep both, the CRC in the name.
            stem, ext = os.path.splitext(entry)
            target = os.path.join(out_dir, f'{stem}_{info.CRC:08x}{ext}')
            jobs.append((archive, info, target))

failed = []
with concurrent.futures.ThreadPoolExecutor(max_workers=min(6, os.cpu_count() or 1)) as pool:
    for target, error in pool.map(unpack, jobs):
        if error:
            failed.append(f'{target}: {error}')
os.rmdir(work) if not os.listdir(work) else None
for line in failed[:20]:
    print(line)
print(f'{len(jobs) - len(failed)} of {len(jobs)} files unpacked to {out_dir}')
