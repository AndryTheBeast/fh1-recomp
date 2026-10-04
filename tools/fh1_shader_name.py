r"""Which file is shader nNNNN of a native-renderer trace? Prints the container name(s) of library entries.

    python tools/fh1_shader_name.py 1212 1089

The numbers in "[trace] draw VS n1089 PS n1212" are positions in fh1_shaders.nfsp (sorted by fingerprint). The
translated source is build_logs/shaders/hlsl/<name>.hlsl: read only its last ~60 lines, the rest is the common
header (shader_common.h).
"""
import hashlib, os, struct, sys

sh = os.path.join(os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__)))), "build_logs", "shaders")
want = [int(x) for x in sys.argv[1:]]
d = open(os.path.join(sh, "fh1_shaders.nfsp"), "rb").read()
n = struct.unpack_from("<I", d, 12)[0]
pos = 24
entries = []
for i in range(n):
    orig, words = struct.unpack_from("<II", d, pos)
    pos += 16
    entries.append(hashlib.md5(d[pos:pos + orig]).hexdigest())
    pos += orig + words * 4
by = {}
for folder in ["containers", "synth", "synth2", "synth2_new"]:
    p = os.path.join(sh, folder)
    if not os.path.isdir(p):
        continue
    for f in os.listdir(p):
        if f.endswith(".bin"):
            by.setdefault(hashlib.md5(open(os.path.join(p, f), "rb").read()).hexdigest(), []).append(folder + "\\" + f)
for w in want:
    print(w, by.get(entries[w], "?"))
