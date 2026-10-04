#!/bin/bash
# Runs the code generator on one edition tree and the two steps the build depends on.
#
#   REXGLUE=out/host/rexglue tools/codegen.sh app
#   REXGLUE=out/host/rexglue tools/codegen.sh app_usa app_usa/hooked.txt app_usa/table.tsv
#
# The second and third arguments are for the trees made by tools/editions/create_tree.py: the list of hooked
# functions of that edition and the address table used to create it (see docs/nfsmw-nx/editions.md).
# Set PYTHON if the Python launcher is not "python" (on Windows it is usually "py").
set -e
ROOT=$(cd "$(dirname "$0")/.." && pwd)
APP=$ROOT/${1:?usage: tools/codegen.sh <app folder> [hooked functions] [address table]}
REXGLUE=${REXGLUE:?set REXGLUE to the rexglue executable built from sdk/}
PY=${PYTHON:-python}
cd "$APP"
echo "== codegen ($APP)"
"$REXGLUE" codegen nfsmw_manifest.toml > codegen.log 2>&1 || { echo "codegen failed, see $APP/codegen.log"; exit 1; }
cd "$ROOT"
echo "== direct calls"
if [ -n "${2:-}" ]; then
  "$PY" tools/direct_calls.py --gen "$APP/generated/default" --hooked "$ROOT/$2"
else
  "$PY" tools/direct_calls.py --gen "$APP/generated/default"
fi
echo "== literal copies"
if [ -n "${3:-}" ]; then
  "$PY" tools/literal_copy.py "$APP/generated/default" "$APP/src/copies_literal" --table "$ROOT/$3"
else
  "$PY" tools/literal_copy.py "$APP/generated/default" "$APP/src/copies_literal"
fi
echo "done"
