#!/usr/bin/env python3
"""Checks every codegen output folder for functions that are registered but never defined.

    python tools/comprobar_simbolos.py fh1

Such a function links as "undefined symbol: sub_XXXXXXXX" (e.g. a gap declared on the
import thunk area, which the codegen registers but does not emit). Exit code 1 if any.
"""
import glob, os, re, sys

bad = 0
for d in sorted(glob.glob(os.path.join(sys.argv[1], 'generated', '*', ''))):
    reg_file = os.path.join(d, 'fh1_register.cpp')
    if not os.path.exists(reg_file):
        continue
    defined = set()
    for f in glob.glob(os.path.join(d, '*.cpp')):
        defined |= set(re.findall(r'DEFINE_REX_FUNC\((sub_[0-9A-F]+)\)', open(f).read()))
    registered = set(re.findall(r'SetFunction\(0x[0-9A-F]+, (sub_[0-9A-F]+)\)', open(reg_file).read()))
    missing = sorted(registered - defined)
    print('%-40s %6d functions, %d registered without a definition %s' % (d, len(defined), len(missing), missing[:10]))
    bad += len(missing)
sys.exit(1 if bad else 0)
