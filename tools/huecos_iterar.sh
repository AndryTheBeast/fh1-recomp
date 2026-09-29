#!/bin/bash
# Repeats codegen + tools/huecos_pasada.py --restos + tools/fusionar_continuaciones.py for the
# three FH1 modules until nothing changes (at most 10 passes, ~3 min each). REXGLUE overrides the path.
cd "$(dirname "$0")/.."
mods="default:huecos.toml:fh1/huecos_excluir.txt xmediafacade_default:xmediafacade_huecos.toml:fh1/xmediafacade_huecos_excluir.txt speechfacade_default:speechfacade_huecos.toml:fh1/speechfacade_huecos_excluir.txt"
for i in $(seq 1 10); do
  (cd fh1 && "${REXGLUE:-../sdk/out/linux-amd64/rexglue}" codegen fh1_manifest.toml > codegen.log 2>&1); rc=$?
  nerr=$(grep -vE "not in any code region" fh1/codegen.log | grep -ciE "unresolved|outside function|Jump target|outranked")
  ndata=$(grep -c "not in any code region" fh1/codegen.log)
  echo "== pass $i rc=$rc problems=$nerr data=$ndata"
  grep -A4 "ANALYSIS ERRORS" fh1/codegen.log | head -6
  added=0
  for spec in $mods; do
    IFS=: read g h e <<< "$spec"
    out=$(python3 tools/huecos_pasada.py fh1 --gen generated/$g --huecos $h --excluir $e --restos); echo "  $g: $out"
    n=$(echo "$out" | grep -oE "[0-9]+" | paste -sd+ | bc); added=$((added+n))
    out2=$(python3 tools/fusionar_continuaciones.py fh1 --gen generated/$g --huecos $h | head -1); echo "  $g: $out2"
    m=$(echo "$out2" | grep -oE "[0-9]+" | head -1); added=$((added+m))
  done
  if [ $rc -eq 0 ] && [ $nerr -eq 0 ] && [ $added -eq 0 ]; then echo CLEAN; break; fi
done
python3 tools/comprobar_simbolos.py fh1
grep -h "REX_FATAL" fh1/generated/*/*.cpp | wc -l
echo LOOP_DONE
