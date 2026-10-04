# -*- coding: utf-8 -*-
# Creates the build tree for another edition of the game (app_<edition>) from app/.
#
# Each different default.xex is a different program and needs its own NRO. Our code (app/src, the codegen
# toml files and the linker function order) is written against the Spanish PAL executable. This step
# copies what the build needs and translates every 82xxxxxx address with the table from
# tools/editions/match_functions.py. app/ is not touched.
#
# app_<edition> sits next to app/: the relative paths (../assets, ../tools, the SDK) resolve the same and
# the build objects keep the same names. That is why PGO finds its .gcda files under the same name.
#
# The partition of functions across files (generated/default/codegen.partition.json) is seeded with the
# translated Spanish one: the codegen honors it, and every file comes out with the same functions in the
# same order.
#
# Usage: py tools/editions/create_tree.py <edition> <table.tsv> <xex relative to app/> [OLD=NEW ...]
#   OLD=NEW: XXH3 fingerprints of shaders that change in that edition (kFingerprintBrightPass, kFingerprintSky...).
import glob
import json
import os
import re
import shutil
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
APP = os.path.join(ROOT, 'app')
PAT = re.compile(r'(?<![0-9A-Fa-f])82[0-9A-Fa-f]{6}(?![0-9A-Fa-f])')
# headers with SPIR-V or other binary data written in hexadecimal: not addresses
BINARIES = ('_spirv.h', '_ps.h', '_vs.h')
SAFE = ('exact', 'same address', 'constant', 'references')  # 'references': from data_by_references.py


def load_table(path):
    safe, all = {}, {}
    for line in open(path, encoding='utf-8').read().splitlines()[1:]:
        pal, section, other, state = line.split('\t')
        if other == '-':
            continue
        all[int(pal, 16)] = int(other, 16)
        if state.startswith(SAFE):
            safe[int(pal, 16)] = int(other, 16)
    return safe, all


# Names split in two to be pasted with ## (NFSC_SCENERY_JOIN_(__imp__sub_824F, D7C0)): that way
# tools/direct_calls.py does not count them as hooked. PAT does not see them; without translating them,
# the NRO of another edition does not link or, worse, calls another function that in that edition sits
# exactly at the Spanish address.
GAME = re.compile(r'(sub_)(82[0-9A-Fa-f]{0,6})(\s*,\s*)([0-9A-Fa-f]{1,6})(?![0-9A-Fa-f])')


def translate_text(text, map, missing):
    def change(m):
        old = m.group()
        d = int(old, 16)
        if d not in map:
            missing.add(old.upper())
            return old
        new_value = '%08X' % map[d]
        return new_value.lower() if any(c in 'abcdef' for c in old) else new_value

    def change_game(m):
        high, low = m.group(2), m.group(4)
        if len(high) + len(low) != 8:
            return m.group()
        d = int(high + low, 16)
        if d not in map:
            missing.add((high + low).upper())
            return m.group()
        new_value = '%08X' % map[d]
        return m.group(1) + new_value[:len(high)] + m.group(3) + new_value[len(high):]
    return GAME.sub(change_game, PAT.sub(change, text))


def main():
    edition, table, xex = sys.argv[1:4]
    rest = sys.argv[4:]
    # --pairs <json>: {"pairs": {PAL: other}} fixes functions that match_functions.py could not place (code
    # changed around them), and {"new_ones": {other: file}} places unpaired functions where they do not
    # displace the others.
    corrections = {}
    if '--pairs' in rest:
        i = rest.index('--pairs')
        corrections = json.load(open(rest[i + 1], encoding='utf-8'))
        del rest[i:i + 2]
    fingerprints = [a.split('=') for a in rest]
    safe, all = load_table(table)
    all.update({int(p, 16): int(o, 16) for p, o in corrections.get('pairs', {}).items()})
    target = os.path.join(ROOT, 'app_' + edition)
    os.makedirs(os.path.join(target, 'generated', 'default'), exist_ok=True)

    missing = set()
    changes = 0
    written = []

    # only what changes is written: that way an existing build recompiles only those files
    def write(rel, text):
        path = os.path.join(target, rel)
        os.makedirs(os.path.dirname(path), exist_ok=True)
        if os.path.exists(path) and open(path, encoding='utf-8', newline='').read() == text:
            return
        open(path, 'w', encoding='utf-8', newline='').write(text)
        written.append(rel)

    # sources: every address must have a safe translation. Files no longer in app/src are deleted.
    for d, _, fs in os.walk(os.path.join(target, 'src')):
        for f in fs:
            rel = os.path.relpath(os.path.join(d, f), target)
            if not os.path.exists(os.path.join(APP, rel)):
                os.remove(os.path.join(d, f))
    for d, _, fs in os.walk(os.path.join(APP, 'src')):
        for f in fs:
            source = os.path.join(d, f)
            rel = os.path.relpath(source, APP)
            if f.endswith(BINARIES) or not f.endswith(('.cpp', '.h', '.hpp', '.inl', '.c')):
                copy = os.path.join(target, rel)
                os.makedirs(os.path.dirname(copy), exist_ok=True)
                if not os.path.exists(copy) or open(copy, 'rb').read() != open(source, 'rb').read():
                    shutil.copyfile(source, copy)
                    written.append(rel)
                continue
            text = open(source, encoding='utf-8', newline='').read()
            new_value = translate_text(text, safe, missing)
            for old, new_entry in fingerprints:
                new_value = re.sub(old, new_entry, new_value, flags=re.IGNORECASE)
            changes += new_value != text
            write(rel, new_value)

    # the rest of app/ that the build uses
    for rel in ('CMakeLists.txt', 'CMakePresets.json', 'nfsmw.toml', 'order_functions.ld', 'overrides.toml',
                os.path.join('generated', 'rexglue.cmake')):
        text = open(os.path.join(APP, rel), encoding='utf-8', newline='').read()
        if rel == 'CMakeLists.txt':
            # PGO profile translated to this edition (tools/editions/pgo/translate_profile.py)
            if text.count('/../pgo/pal_es"') != 1:
                raise SystemExit('CMakeLists.txt: profile folder not found')
            text = text.replace('/../pgo/pal_es"', '/../pgo/%s"' % edition)
        if rel == 'order_functions.ld':
            # it only orders hot functions; a doubtful entry does no harm (the linker ignores a name that
            # does not exist), so the doubtful translation is acceptable too
            write(rel, translate_text(text, {**all, **safe}, set()))
            continue
        write(rel, translate_text(text, safe, missing))

    # gaps: a function declaration at a doubtful place can split another function; those are removed and
    # tools/gaps.py will find them after the codegen
    removed = []
    lines = []
    for line in open(os.path.join(APP, 'gaps.toml'), encoding='utf-8', newline='').read().splitlines(True):
        m = re.match(r'\s*"0x(82[0-9A-Fa-f]{6})"', line)
        if m and int(m.group(1), 16) not in safe:
            # {"gaps": {PAL: other}} in --pairs: the equivalent gap in the other edition, found by hand
            # with tools/gaps.py (same size, right after the equivalent function). It keeps its options.
            other = corrections.get('gaps', {}).get(m.group(1).upper())
            if other:
                lines.append(line.replace(m.group(1), other))
                continue
            removed.append(m.group(1))
            continue
        lines.append(translate_text(line, safe, missing) if not m else
                      translate_text(line, {**all, **safe}, set()))
    write('gaps.toml', ''.join(lines))

    manifest = open(os.path.join(APP, 'nfsmw_manifest.toml'), encoding='utf-8', newline='').read()
    manifest = manifest.replace('file_path = "../assets/game_root/default.xex"', 'file_path = "%s"' % xex)
    # the codegen requires the XEX to be inside game_root
    manifest = manifest.replace('game_root = "../assets/game_root"', 'game_root = "%s"' % os.path.dirname(xex))
    write('nfsmw_manifest.toml', manifest)

    # function partition seeded with the Spanish one. First the safe and the corrected pairs; a doubtful one
    # only if its slot is still free (a misplaced doubtful one can land on top of another function's exact pair).
    split = json.load(open(os.path.join(APP, 'generated', 'default', 'codegen.partition.json')))
    reliable = dict(safe)
    reliable.update({int(p, 16): int(o, 16) for p, o in corrections.get('pairs', {}).items()})
    seeded = {}
    for map in (reliable, all):
        for address, file in split['assignments'].items():
            d = map.get(int(address, 16))
            if d is not None and '%08X' % d not in seeded:
                seeded['%08X' % d] = file
    seeded.update(corrections.get('new_ones', {}))
    split['assignments'] = dict(sorted(seeded.items()))
    write(os.path.join('generated', 'default', 'codegen.partition.json'), json.dumps(split, indent=2) + '\n')

    # weak calls, for tools/direct_calls.py: the functions that the tested build (app/generated/default)
    # calls through the weak path, translated. That way every call comes out the same as in the Spanish
    # edition, call by call. A list taken from the current sources does not work: they mention new addresses
    # since the last run of the tool.
    weak = re.compile(r'(?<![\w])sub_([0-9A-F]{8})\(ctx, base\);')
    hooked = set()
    for path in glob.glob(os.path.join(APP, 'generated', 'default', 'nfsmw_recomp.*.cpp')):
        for x in weak.findall(open(path, encoding='utf-8').read()):
            d = all.get(int(x, 16))
            hooked.add('sub_%08X' % (d if d is not None else int(x, 16)))
    write('hooked.txt', '\n'.join(sorted(hooked)) + '\n')

    print('files rewritten: %s' % (', '.join(written) or 'none'))
    print('%s: %d sources with changes; %d functions seeded; %d gaps removed %s; %d with a hook' % (
        target, changes, len(seeded), len(removed), removed, len(hooked)))
    if missing:
        print('NO SAFE TRANSLATION (review by hand): %s' % ', '.join(sorted(missing)))
        sys.exit(1)


if __name__ == '__main__':
    main()
