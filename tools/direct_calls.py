# nfsmw - direct calls to game functions that have no hook.
#
# With GCC every recompiled function sub_X is a weak alias of __imp__sub_X (DEFINE_REX_FUNC, nfsmw_pch.h),
# so that one of our REX_HOOK_RAW(sub_X) replaces it at link time. The codegen always calls sub_X, and the
# compiler cannot inline a weak function into another (a hook could replace it): not within the same file
# and not with LTO. This step changes `sub_X(ctx, base);` into `__imp__sub_X(ctx, base);` when sub_X has no
# hook, which is exactly the same function, but can now be inlined.
#
# Any 82xxxxxx address that appears in the app or SDK sources or in the toml files counts as hooked (the
# D3D trace builds its hooks with sub_##addr). That is slightly more than needed and safe; it is also
# checked against the sub_ symbols the compiled objects really define (--should_check-objects).
#
# It does not touch nfsmw_init.cpp or nfsmw_register.cpp: the dispatch table for indirect calls still
# points to sub_X and sees the hooks. It has to be run again after every codegen.
#
# Usage: py tools/direct_calls.py [--undo] [--gen <generated folder>] [--hooked <list>]
#   --gen and --hooked: for the tree of another edition (tools/editions/create_tree.py), with the
#   list of hooked addresses of the Spanish edition already translated.
import os
import re
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
GEN = os.path.join(ROOT, 'app', 'generated', 'default')
SOURCES = [os.path.join(ROOT, 'app', 'src'), os.path.join(ROOT, 'sdk', 'src'),
           os.path.join(ROOT, 'sdk', 'include'), os.path.join(ROOT, 'app', 'overrides.toml'),
           os.path.join(ROOT, 'app', 'gaps.toml'), os.path.join(ROOT, 'app', 'nfsmw_manifest.toml'),
           os.path.join(ROOT, 'app', 'CMakeLists.txt')]


def hooked():
    names = set()
    for root_value in SOURCES:
        if os.path.isfile(root_value):
            paths = [root_value]
        else:
            paths = [os.path.join(d, f) for d, _, fs in os.walk(root_value) for f in fs
                     if not f.endswith(('.a', '.obj', '.o', '.png', '.jpg', '.bin'))]
        for path in paths:
            try:
                text = open(path, encoding='utf-8', errors='ignore').read()
            except OSError:
                continue
            for m in re.findall(r'(?<![0-9A-Fa-f])(82[0-9A-Fa-f]{6})(?![0-9A-Fa-f])', text):
                names.add('sub_' + m.upper())
    return names


def main():
    undo = '--undo' in sys.argv
    gen = sys.argv[sys.argv.index('--gen') + 1] if '--gen' in sys.argv else GEN
    if '--hooked' in sys.argv:
        with_hook = set(open(sys.argv[sys.argv.index('--hooked') + 1], encoding='utf-8').read().split())
    else:
        with_hook = hooked()
    call = re.compile(r'(?<![\w])sub_([0-9A-F]{8})\(ctx, base\);')
    direct = re.compile(r'__imp__sub_([0-9A-F]{8})\(ctx, base\);')
    changed = 0
    files = 0
    for f in sorted(os.listdir(gen)):
        if not (f.startswith('nfsmw_recomp.') and f.endswith('.cpp')):
            continue
        path = os.path.join(gen, f)
        text = open(path, encoding='utf-8').read()
        if undo:
            new_value, n = direct.subn(lambda m: 'sub_%s(ctx, base);' % m.group(1), text)
        else:
            def change(m):
                return m.group(0) if 'sub_' + m.group(1) in with_hook else '__imp__sub_%s(ctx, base);' % m.group(1)
            new_value = call.sub(change, text)
            n = len(call.findall(text)) - len(call.findall(new_value))
        if new_value != text:
            open(path, 'w', encoding='utf-8', newline='\n').write(new_value)
            files += 1
        changed += n
    print('%s: %d calls in %d files; %d hooked addresses left alone' %
          ('deshecho' if undo else 'direct', changed, files, len(with_hook)))


if __name__ == '__main__':
    main()
