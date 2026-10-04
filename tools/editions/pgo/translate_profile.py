# -*- coding: utf-8 -*-
# Translates the PGO profile (the .gcda files) to another edition of the game.
#
# GCC 16 identifies each function in the profile by a hash of its name (crc32 of the assembler name) and
# checks a line checksum (line, file path and name) and another of the control flow graph. Recompiled
# functions are named after their address (__imp__sub_823B5A40), as are our hooks (sub_824455B8) and five
# C++ functions with the address in the name. In another edition they change address and name: without
# translating the profile, PGO would find almost nothing.
#
# Here every record of those functions is moved to the other edition's name: new hash and new line
# checksum. The control flow graph is not touched (if the function really changed, GCC detects it and
# drops its profile with a warning). Records of functions that no longer exist are removed. Everything
# else (headers, local functions) is copied as is: the other edition's build is compiled with the same
# paths as the Spanish one, so their hashes do not change.
#
# Usage: translate_profile.py <PAL profile> <new profile> <table.tsv> <pairs.json> <PAL generated>
#                          <new generated> <app names with an address (gcc-nm)> <root of the PAL sources>
import glob
import json
import os
import re
import shutil
import struct
import sys

import gcda

# app/ of this repository, spelled as CMake passes it to GCC (forward slashes). The profile of local functions
# only matches when the sources are compiled from this same path (see docs/toolchain.md).
ROOT_APP = os.environ.get('NFSMW_APP_DIR') or os.path.abspath(
    os.path.join(os.path.dirname(__file__), '..', '..', '..', 'app')).replace(os.sep, '/')
DEF = re.compile(r'^DEFINE_REX_FUNC\(sub_([0-9A-F]{8})\)', re.M)
DIR = re.compile(r'82[0-9A-Fa-f]{6}')


def lines_of_functions(path):
    text = open(path, encoding='utf-8').read()
    return {m.group(1): text.count('\n', 0, m.start()) + 1 for m in DEF.finditer(text)}


def lineno(line, file, name):
    return gcda.crc32_gcc(gcda.crc32_gcc(line, file), name)


def main():
    source, target, table, pairs, gen_pal, gen_other, names_app, sources = sys.argv[1:9]
    map = {}
    for l in open(table, encoding='utf-8').read().splitlines()[1:]:
        p, s, o, e = l.split('\t')
        if o != '-':
            map[p.upper()] = o.upper()
    map.update({p.upper(): o.upper() for p, o in json.load(open(pairs, encoding='utf-8')).get('pairs', {}).items()})

    if os.path.exists(target):
        shutil.rmtree(target)
    os.makedirs(target)
    counts = {'traducidas': 0, 'removed': 0, 'hooks': 0, 'without_line': 0, 'files': 0}

    # app names with an address inside: the source tool changes the address unless it is glued to another
    # hexadecimal digit (Sum825FDFB0 stays the same)
    def translate_name(name):
        def change(m):
            before = name[m.start() - 1] if m.start() else ''
            if before and before in '0123456789abcdefABCDEF':
                return m.group()
            new_value = map.get(m.group().upper())
            if new_value is None:
                return m.group()
            return new_value.lower() if any(c in 'abcdef' for c in m.group()) else new_value
        return DIR.sub(change, name)

    with_address = [n.strip() for n in open(names_app, encoding='utf-8') if n.strip()]
    por_hash_app = {gcda.ident_public(n): n for n in with_address if translate_name(n) != n}

    for path in sorted(glob.glob(os.path.join(source, '*.gcda'))):
        base = os.path.basename(path)
        cab, regs = gcda.read(path)
        m = re.search(r'#generated#default#(nfsmw_recomp\.\d+\.cpp)\.gcda$', base)
        remove = set()
        if m:
            file = ROOT_APP + '/generated/default/' + m.group(1)
            pal = lines_of_functions(os.path.join(gen_pal, m.group(1)))
            other = lines_of_functions(os.path.join(gen_other, m.group(1)))
            por_hash = {gcda.ident_public('__imp__sub_' + a): a for a in pal}
            for i, ident, lin, cfg, counters in gcda.functions(regs):
                a = por_hash.get(ident)
                if a is None:
                    continue
                b = map.get(a)
                if b is None or b not in other:
                    remove.add(i)
                    remove.update(counters)
                    counts['removed'] += 1
                    continue
                name = '__imp__sub_' + b
                regs[i][2] = struct.pack('<3I', gcda.ident_public(name), lineno(other[b], file, name), cfg)
                counts['traducidas'] += 1
        else:
            # app source: the file is deduced from the .gcda name (CMakeFiles#nfsmw.dir#src#x.cpp.gcda)
            source_2 = None
            n = re.search(r'#nfsmw\.dir#(src#.+)\.gcda$', base)
            if n:
                source_2 = ROOT_APP + '/' + n.group(1).replace('#', '/')
            for i, ident, lin, cfg, counters in gcda.functions(regs):
                old = por_hash_app.get(ident)
                if old is None:
                    continue
                new_value = translate_name(old)
                line = None
                if source_2:
                    line = next((k for k in range(1, 40000) if lineno(k, source_2, old) == lin), None)
                if line is None:
                    counts['without_line'] += 1
                    new_lin = lin
                else:
                    new_lin = lineno(line, source_2, new_value)
                regs[i][2] = struct.pack('<3I', gcda.ident_public(new_value), new_lin, cfg)
                counts['hooks'] += 1
        # two records with the same hash in one file: GCC treats it as a corrupt profile and stops.
        # The first one is kept.
        seen_2 = set()
        for i, ident, lin, cfg, counters in gcda.functions(regs):
            if i in remove:
                continue
            new_ident = struct.unpack('<3I', regs[i][2])[0]
            if new_ident in seen_2:
                remove.add(i)
                remove.update(counters)
                counts['repetidas'] = counts.get('repetidas', 0) + 1
            seen_2.add(new_ident)
        regs = [r for k, r in enumerate(regs) if k not in remove]
        gcda.write(os.path.join(target, base), cab, regs)
        counts['files'] += 1
    print(counts)


if __name__ == '__main__':
    main()
