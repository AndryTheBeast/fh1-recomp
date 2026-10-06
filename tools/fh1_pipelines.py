#!/usr/bin/env python3
"""
The native renderer's pipeline lists (fh1/src/native/fh1_native_draws.cpp, "prewarm").

A list holds one record per pipeline the game has asked for: drawing state, formats and the fingerprints of
its two shaders. No game data and no driver cache. Two kinds of file hold one:

  cache\\fh1_native_pipelines.bin   this PC's file ("NFPC": the list, then the driver's cache)
  fh1_pipelines.nfpl               the list alone ("NFPL"): what ships with the port, next to fh1.exe

    python tools/fh1_pipelines.py info FILE [FILE ...]
        how many pipelines each file lists, and how many of them the files before it did not have

    python tools/fh1_pipelines.py merge OUT.nfpl FILE [FILE ...]
        writes the union of the lists (first seen first; never any driver cache) to OUT.nfpl

The shipped list lives in fh1/data/fh1_pipelines.nfpl; the build copies it next to fh1.exe.
"""
import struct
import sys

MAGIC_FILE = 0x4350464E  # "NFPC"
MAGIC_LIST = 0x4C50464E  # "NFPL"
VERSION_LIST = 1
HEADER_FILE = 24  # magic, version, list bytes (u64), cache bytes (u64)
HEADER_LIST = 16  # magic, version, record size, record count
KEY_BYTES = 80    # sizeof(KeyPipeline): what the renderer hashes to tell two pipelines apart
RECORD_BYTES = 424  # sizeof(RegisterPipeline)


def read_list(path):
    """The records of a .bin or .nfpl file, as a list of bytes objects."""
    with open(path, "rb") as f:
        data = f.read()
    if len(data) >= HEADER_FILE and struct.unpack_from("<I", data, 0)[0] == MAGIC_FILE:
        _, _, bytes_list, bytes_cache = struct.unpack_from("<IIQQ", data, 0)
        if HEADER_FILE + bytes_list + bytes_cache != len(data):
            raise SystemExit(f"{path}: damaged pipelines file")
        data = data[HEADER_FILE:HEADER_FILE + bytes_list]
    if len(data) < HEADER_LIST:
        return []
    magic, version, size, count = struct.unpack_from("<IIII", data, 0)
    if magic != MAGIC_LIST or version != VERSION_LIST:
        raise SystemExit(f"{path}: not a pipeline list (or another version)")
    if size != RECORD_BYTES:
        raise SystemExit(f"{path}: records of {size} bytes, this tool knows {RECORD_BYTES}: "
                         "RegisterPipeline changed, update the tool and record the list again")
    if len(data) != HEADER_LIST + size * count:
        raise SystemExit(f"{path}: damaged list")
    return [data[HEADER_LIST + i * size:HEADER_LIST + (i + 1) * size] for i in range(count)]


def identity(r):
    """What tells two pipelines apart whatever library recorded them: the key without its two shader numbers
    (they depend on the library: the developer's and an installed copy's differ) plus the two shaders'
    fingerprints, which follow the key in the record. The game matches the same way (LoadListPipelines)."""
    return r[8:KEY_BYTES] + r[KEY_BYTES:KEY_BYTES + 16]


def union(paths, report):
    seen = {}
    for path in paths:
        records = read_list(path)
        new = 0
        for r in records:
            if identity(r) not in seen:
                seen[identity(r)] = r
                new += 1
        if report:
            print(f"{path}: {len(records)} pipelines, {new} new")
    return list(seen.values())


def main(argv):
    if len(argv) >= 3 and argv[1] == "info":
        records = union(argv[2:], True)
        print(f"total: {len(records)} different pipelines ({HEADER_LIST + len(records) * RECORD_BYTES} bytes as a list)")
        return 0
    if len(argv) >= 4 and argv[1] == "merge":
        records = union(argv[3:], True)
        with open(argv[2], "wb") as f:
            f.write(struct.pack("<IIII", MAGIC_LIST, VERSION_LIST, RECORD_BYTES, len(records)))
            for r in records:
                f.write(r)
        print(f"{argv[2]}: {len(records)} pipelines written")
        return 0
    print(__doc__)
    return 1


if __name__ == "__main__":
    sys.exit(main(sys.argv))
