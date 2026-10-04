#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
NFSMW Recomp - phase 1: extract the Xbox 360 ISO and dump the XEX info.

No dependencies: only the Python 3.8+ standard library.

Typical usage:
    python tools/phase1_extract.py "D:\\dumps\\NFSMW.iso" --list
    python tools/phase1_extract.py "D:\\dumps\\NFSMW.iso" -o assets/game_root
    python tools/phase1_extract.py assets/game_root/default.xex --info

What it does:
  1. Detects the offset of the game partition (XGD1 / XGD2 / XGD3 / raw).
  2. Walks the XDVDFS tree and lists or extracts the files.
  3. Parses the XEX2 header of default.xex and gets the title id, base address,
     entry point and the compression/encryption type.

It does not decrypt or decompress the PE: ReXGlue takes care of that internally
during codegen. Only the files and the header data are needed here.
"""

import argparse
import os
import struct
import sys

SECTOR = 2048
XDVDFS_MAGIC = b"MICROSOFT*XBOX*MEDIA"

# Known offsets where the game partition starts, by disc type.
KNOWN_BASES = [
    (0x00000000, "particion raw_value / image ya recortada"),
    (0x0FD90000, "XGD2 (most 360 games)"),
    (0x02080000, "XGD3 (titulos late)"),
    (0x18300000, "XGD1 (Xbox original)"),
]

ATTR_DIRECTORY = 0x10


# ---------------------------------------------------------------------------
# XDVDFS
# ---------------------------------------------------------------------------

def _magic_at(fh, offset):
    """True if the volume descriptor is at base=offset."""
    try:
        fh.seek(offset + 32 * SECTOR)
    except OSError:
        return False
    head = fh.read(len(XDVDFS_MAGIC))
    return head == XDVDFS_MAGIC


def detect_base(fh, limit_scan=1 << 30):
    """Returns (base, description) of the game partition."""
    for base, desc in KNOWN_BASES:
        if _magic_at(fh, base):
            return base, desc

    # None of the known ones: brute-force scan in large chunks.
    fh.seek(0, os.SEEK_END)
    tam = fh.tell()
    cap = min(tam, limit_scan)
    CHUNK = 16 << 20
    overlaps = len(XDVDFS_MAGIC)
    pos = 0
    while pos < cap:
        fh.seek(pos)
        buf = fh.read(CHUNK + overlaps)
        if not buf:
            break
        idx = buf.find(XDVDFS_MAGIC)
        while idx != -1:
            abs_off = pos + idx
            if abs_off % SECTOR == 0 and abs_off >= 32 * SECTOR:
                base = abs_off - 32 * SECTOR
                if _magic_at(fh, base):
                    return base, "found by scan (non-standard offset)"
            idx = buf.find(XDVDFS_MAGIC, idx + 1)
        pos += CHUNK

    raise SystemExit(
        "No XDVDFS file system was found in the image.\n"
        "Check that it is an Xbox 360 ISO and not a compressed CCI/GOD/ZAR."
    )


def read_descriptor(fh, base):
    """Returns (sector_root, tam_root) by reading the volume descriptor."""
    fh.seek(base + 32 * SECTOR)
    vd = fh.read(SECTOR)
    if len(vd) < SECTOR or vd[:20] != XDVDFS_MAGIC:
        raise SystemExit("Invalid volume descriptor.")
    if vd[0x7EC:0x7EC + 20] != XDVDFS_MAGIC:
        print("  warning: the closing magic at 0x7EC is missing (truncated image?)",
              file=sys.stderr)
    sector_root, tam_root = struct.unpack_from("<II", vd, 0x14)
    return sector_root, tam_root


def _entries(table, offset, seen_2):
    """Walks the binary tree of a directory. Yields one dict per entry."""
    stack = [offset]
    while stack:
        off = stack.pop()
        if off in seen_2:
            continue
        # An offset of 0 is only valid for the root of the tree.
        if off != 0 and off == 0:
            continue
        if off + 14 > len(table):
            continue
        seen_2.add(off)

        left, der, sector, tam, attrs, is_long = struct.unpack_from(
            "<HHIIBB", table, off)

        # 0xFFFF and 0 mean "no child".
        for child in (left, der):
            if child not in (0, 0xFFFF):
                stack.append(child * 4)

        end_name = off + 14 + is_long
        if is_long == 0 or end_name > len(table):
            continue
        name = table[off + 14:end_name].decode("latin-1")

        yield {
            "name": name,
            "sector": sector,
            "tam": tam,
            "dir": bool(attrs & ATTR_DIRECTORY),
        }


def walk(fh, base, sector, tam, prefix=""):
    """Walks the directory tree recursively. Yields (path, entry)."""
    if tam == 0 or tam > (256 << 20):
        return
    fh.seek(base + sector * SECTOR)
    table = fh.read(tam)
    if len(table) < tam:
        print("  warning: truncated directory table in %s" % (prefix or "/"),
              file=sys.stderr)

    seen_2 = set()
    children = list(_entries(table, 0, seen_2))
    children.sort(key=lambda e: e["name"].lower())

    for e in children:
        path = prefix + "/" + e["name"] if prefix else e["name"]
        yield path, e
        if e["dir"]:
            for sub in walk(fh, base, e["sector"], e["tam"], path):
                yield sub


def extract(fh, base, entry, target):
    os.makedirs(os.path.dirname(target) or ".", exist_ok=True)
    fh.seek(base + entry["sector"] * SECTOR)
    remaining = entry["tam"]
    with open(target, "wb") as out:
        while remaining > 0:
            chunk = fh.read(min(1 << 20, remaining))
            if not chunk:
                raise SystemExit(
                    "Unexpected end of file reading %s. Incomplete image?"
                    % entry["name"])
            out.write(chunk)
            remaining -= len(chunk)


# ---------------------------------------------------------------------------
# XEX2
# ---------------------------------------------------------------------------

KEYS_XEX = {
    0x000002FF: "Resource info",
    0x000003FF: "File format info",
    0x000005FF: "Delta patch descriptor",
    0x000080FF: "Bounding path",
    0x00008105: "Device ID",
    0x00010001: "Original base address",
    0x00010100: "Entry point",
    0x00010201: "Image base address",
    0x000103FF: "Import libraries",
    0x00018002: "Checksum / timestamp",
    0x00018102: "Enabled for callcap",
    0x00018200: "Enabled for fastcap",
    0x000183FF: "Original PE name",
    0x000200FF: "Static libraries",
    0x00020104: "TLS info",
    0x00020200: "Default stack size",
    0x00020301: "Default filesystem cache size",
    0x00020401: "Default heap size",
    0x00028002: "Page heap size and flags",
    0x00030000: "System flags",
    0x00040006: "Execution info",
    0x00040201: "Title workspace size",
    0x00040310: "Game ratings",
    0x00040404: "LAN key",
    0x000405FF: "Xbox 360 logo",
    0x000406FF: "Multidisc media IDs",
    0x000407FF: "Alternate title IDs",
    0x00040801: "Additional title memory",
    0x00E10402: "Exports by name",
}

COMPRESSION = {0: "none", 1: "basic", 2: "normal (LZX)", 3: "delta"}
ENCRYPTED = {0: "none", 1: "normal (AES-128)"}


def _u32(buf, off):
    return struct.unpack_from(">I", buf, off)[0]


def info_xex(path):
    with open(path, "rb") as fh:
        cab = fh.read(0x1000)
        if cab[:4] != b"XEX2":
            raise SystemExit(
                "%s does not begin with the magic 'XEX2'. It is not an "
                "Xbox 360 executable (or it is encrypted in another format)." % path)

        flags_module = _u32(cab, 0x04)
        off_pe = _u32(cab, 0x08)
        off_security = _u32(cab, 0x10)
        n_opt = _u32(cab, 0x14)

        print("== Header XEX2 ==")
        print("  file               : %s (%s bytes)"
              % (path, f"{os.path.getsize(path):,}"))
        print("  module flags          : 0x%08X" % flags_module)
        print("  offset data PE       : 0x%08X" % off_pe)
        print("  offset security info  : 0x%08X" % off_security)
        print("  cabeceras optional  : %d" % n_opt)

        # The optional headers can extend past the 0x1000 bytes read.
        fh.seek(0x18)
        raw_opt = fh.read(n_opt * 8)
        optional = {}
        for i in range(n_opt):
            key, input_value = struct.unpack_from(">II", raw_opt, i * 8)
            optional[key] = input_value

        entry = optional.get(0x00010100)
        base_img = optional.get(0x00010201)

        print()
        print("== Data key ==")

        # Execution info -> title id, version, disco
        title_id = None
        if 0x00040006 in optional:
            fh.seek(optional[0x00040006])
            ei = fh.read(24)
            if len(ei) == 24:
                media_id, version, base_version, title_id = struct.unpack_from(
                    ">IIII", ei, 0)
                platform, table_exe, disco_n, disco_tot = struct.unpack_from(
                    ">BBBB", ei, 0x10)
                print("  Title ID              : %08X" % title_id)
                print("  Media ID              : %08X" % media_id)
                print("  Version               : %d.%d.%d.%d"
                      % ((version >> 28) & 0xF, (version >> 16) & 0xFFF,
                         (version >> 8) & 0xFF, version & 0xFF))
                print("  Disc                  : %d of %d" % (disco_n, disco_tot))
        else:
            print("  Title ID              : (no execution info)")

        if base_img is not None:
            print("  Image base address    : 0x%08X" % base_img)
        if entry is not None:
            print("  Entry point           : 0x%08X" % entry)

        # Security info -> load address, image size
        fh.seek(off_security)
        si = fh.read(0x184)
        if len(si) >= 0x114:
            tam_image = _u32(si, 0x004)
            load_addr = _u32(si, 0x110)
            print("  Load address          : 0x%08X" % load_addr)
            print("  Image size         : %s bytes" % f"{tam_image:,}")

        # File format info -> compression / encrypted
        if 0x000003FF in optional:
            fh.seek(optional[0x000003FF])
            ffi = fh.read(8)
            if len(ffi) == 8:
                _tam, cif, comp = struct.unpack(">IHH", ffi)
                print("  Encrypted               : %s (%d)"
                      % (ENCRYPTED.get(cif, "unknown"), cif))
                print("  Compression            : %s (%d)"
                      % (COMPRESSION.get(comp, "unknown"), comp))

        # Other useful fields stored inline
        if 0x00010001 in optional:
            print("  Original base address : 0x%08X" % optional[0x00010001])
        if 0x00020200 in optional:
            print("  Default stack size    : %s bytes" % f"{optional[0x00020200]:,}")
        if 0x00030000 in optional:
            print("  System flags          : 0x%08X" % optional[0x00030000])

        # Import libraries: which kernel modules the game uses.
        # Each module brings imports that the runtime has to implement or stub.
        if 0x000103FF in optional:
            try:
                fh.seek(optional[0x000103FF])
                cab_imp = fh.read(12)
                if len(cab_imp) == 12:
                    _total, st_tam, st_num = struct.unpack(">III", cab_imp)
                    if 0 < st_tam <= (1 << 16):
                        table = fh.read(st_tam)
                        names = [n.decode("latin-1") for n in table.split(b"\x00")
                                   if n and all(32 <= c < 127 for c in n)]
                        if names:
                            print()
                            print("== Modules importados (%d) ==" % st_num)
                            for n in names:
                                print("  " + n)
            except (OSError, struct.error):
                pass

        print()
        print("== Cabeceras optional presentes ==")
        for key in sorted(optional):
            name = KEYS_XEX.get(key, "")
            print("  0x%08X  %-24s input_value/offset 0x%08X"
                  % (key, name, optional[key]))

        print()
        if title_id == 0x454107D9:
            print("  >> The Title ID matches Need for Speed: Most Wanted (2005). Correct.")
        elif title_id is not None:
            print("  >> WARNING: the Title ID expected for NFSMW 2005 is 454107D9.")
            print("     This XEX is 0x%08X. Check that you dumped the right game." % title_id)

        print()
        print("Copy this output to docs/xex_info.txt: you will need the base")
        print("address every time you declare an address in the TOML.")


# ---------------------------------------------------------------------------
# CLI
# ---------------------------------------------------------------------------

def find_iso():
    """Looks for a single .iso in the project folder (or in the current one)."""
    root_value = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
    candidates = []
    for folder in (root_value, os.getcwd()):
        try:
            for n in os.listdir(folder):
                if n.lower().endswith(".iso"):
                    path = os.path.join(folder, n)
                    if path not in candidates:
                        candidates.append(path)
        except OSError:
            pass

    if not candidates:
        raise SystemExit(
            "No file was given and there is no .iso in:\n"
            "  %s\n"
            "Pass the path as an argument:\n"
            "  python tools/phase1_extract.py \"D:/path/to/game.iso\" --list" % root_value)

    if len(candidates) > 1:
        msg = "ThereIs several .iso; indica which quieres:\n"
        for c in candidates:
            msg += "  %s\n" % c
        raise SystemExit(msg)

    return candidates[0]


def main():
    p = argparse.ArgumentParser(
        description="Extracts an Xbox 360 ISO and dumps the XEX info.")
    p.add_argument("entry", nargs="?", default=None,
                   help="path to the .iso, or to a .xex with --info. If omitted, looks for a single .iso in the project folder.")
    p.add_argument("-o", "--output", default="assets/game_root",
                   help="target folder of the extraction (default: assets/game_root)")
    p.add_argument("--list", action="store_true",
                   help="only list the content, extract nothing")
    p.add_argument("--solo-xex", action="store_true",
                   help="extract only default.xex (and the .xexp if there is one)")
    p.add_argument("--info", action="store_true",
                   help="the input is a .xex: dump its header and exit")
    args = p.parse_args()

    if args.entry is None:
        args.entry = find_iso()
        print("ISO found automaticamente: %s\n" % args.entry)

    if not os.path.exists(args.entry):
        raise SystemExit("The file does not exist: %s" % args.entry)

    if args.info or args.entry.lower().endswith(".xex"):
        info_xex(args.entry)
        return

    with open(args.entry, "rb") as fh:
        base, desc = detect_base(fh)
        print("Game partition at offset 0x%08X  (%s)" % (base, desc))

        sector_root, tam_root = read_descriptor(fh, base)
        print("Directorio root_value: sector %d, %s bytes\n" % (sector_root, f"{tam_root:,}"))

        entries = list(walk(fh, base, sector_root, tam_root))
        if not entries:
            raise SystemExit("The file system is empty. Corrupt image?")

        n_arch = sum(1 for _, e in entries if not e["dir"])
        total = sum(e["tam"] for _, e in entries if not e["dir"])
        print("%d files, %d directories, %s bytes in total\n"
              % (n_arch, len(entries) - n_arch, f"{total:,}"))

        if args.list:
            for path, e in entries:
                if e["dir"]:
                    print("  [dir]  %s" % path)
                else:
                    print("  %12s  %s" % (f"{e['tam']:,}", path))
            return

        targets = entries
        if args.solo_xex:
            targets = [(r, e) for r, e in entries
                         if not e["dir"] and
                         (r.lower().endswith(".xex") or r.lower().endswith(".xexp"))]
            if not targets:
                raise SystemExit("There is no .xex in the image.")

        done = 0
        for path, e in targets:
            if e["dir"]:
                continue
            target = os.path.join(args.output, path.replace("/", os.sep))
            extract(fh, base, e, target)
            done += 1
            if done % 50 == 0:
                print("  ... %d archivos" % done)
        print("\n%d files extracted to %s" % (done, args.output))

    xex = os.path.join(args.output, "default.xex")
    if os.path.exists(xex):
        print()
        info_xex(xex)
    else:
        print("\nWarning: no default.xex showed up in the root. Check the listing "
              "with --list to see where the executable is.")


if __name__ == "__main__":
    main()
