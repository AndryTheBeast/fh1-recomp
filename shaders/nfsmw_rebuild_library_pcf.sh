#!/bin/bash
# nfsmw_rebuild_library_pcf.sh OUTPUT ENTRY
# Regenerates the NFSSPV library from bash with the same options as nfsc_validate.ps1
# (which on Windows PowerShell 5.1 stops at the first DXC warning):
#   1. builds the translator with -DNFSMW_RECOMP and the format regression test;
#   2. translates nfsmw_shaders_combinados into OUTPUT/hlsl;
#   3. DXC, spirv-val, packaging and library regression (nfsc_complete_spirv.sh).
# OUTPUT must be a new folder. ENTRY is the folder with the game's shader containers.
set -u
# Tools: g++ (MinGW on Windows), DXC and spirv-val from the Vulkan SDK, and Python ("py" on Windows).
# SDK is the sdk/ folder of this repository with its thirdparty sources; MESA is the mesa-switch source tree
# (its src/util/xxhash.h is the one the translator was built with).
ROOT=$(cd "$(dirname "$0")" && pwd)
SDK=${SDK:-$ROOT/../sdk}
MESA=${MESA:?set MESA to the mesa-switch source tree}
OUTPUT=${1:?the output folder is missing}
ENTRY=${2:?the input folder is missing}
DXC=${DXC:-dxc}
VAL=${SPIRV_VAL:-spirv-val}

[ -e "$OUTPUT" ] && { echo "The output folder already exists"; exit 1; }
mkdir -p "$OUTPUT/hlsl" "$OUTPUT/spirv"

g++ -std=c++23 -O1 "-I$ROOT" "-I$ROOT/XenosRecomp" "-I$SDK/thirdparty/fmt/include" \
  "-I$MESA/src/util" -include "$ROOT/pch_min.h" \
  -DFMT_HEADER_ONLY -DXXH_INLINE_ALL -DNFSMW_RECOMP \
  "$ROOT/nfsmw_hlsl.cpp" "$ROOT/XenosRecomp/shader_recompiler.cpp" -o "$ROOT/nfsmw_hlsl.exe" \
  > "$OUTPUT/compile.log" 2>&1 || { echo "Failed to compile the translator; see compile.log"; exit 1; }
# Format regression against a reference set of containers, when there is one (REFERENCE).
if [ -n "${REFERENCE:-}" ]; then
  "$ROOT/nfsmw_test_container.exe" "$ENTRY" "$REFERENCE" > "$OUTPUT/format.log" \
    || { echo "Format regression failure; see format.log"; exit 1; }
fi
"$ROOT/nfsmw_hlsl.exe" "$ENTRY" "$OUTPUT/hlsl" "$ROOT/XenosRecomp/shader_common.h" > "$OUTPUT/translation.log" \
  || { echo "There are untranslated shaders; see translation.log"; exit 1; }

# The translator emits tfetch2D for every sample; here only the ones that use SHADOWMAP_SAMPLER, the
# shadow map samples, are changed to tfetch2DShadow. That function obeys SPEC_CONSTANT_PCF_CHEAP:
# with the bit set, the nine samples of the 3x3 land on the same texel and the compiler keeps one.
# Without the bit, the generated code is identical to the unmodified one.
changes=$(grep -l "tfetch2D(SHADOWMAP_SAMPLER_" "$OUTPUT"/hlsl/*.hlsl 2>/dev/null | wc -l)
calls=$(grep -ho "tfetch2D(SHADOWMAP_SAMPLER_" "$OUTPUT"/hlsl/*.hlsl 2>/dev/null | wc -l)
sed -i 's/tfetch2D(SHADOWMAP_SAMPLER_/tfetch2DShadow(SHADOWMAP_SAMPLER_/g' "$OUTPUT"/hlsl/*.hlsl
echo "cheap PCF: $calls calls to the shadow map in $changes shaders"
[ "$calls" -gt 0 ] || { echo "No call was rewritten: something is wrong"; exit 1; }

# Shadow by minimum (nfsc_native_shadow_minimum; SPEC_CONSTANT_SHADOW_MINIMUM in shader_common.h).
# Shadow map calls become tfetch2DShadowMin with the 3D index of the same register, which is where the
# app puts the second texture. Without the bit the code is that of tfetch2DShadow. All of them must be
# converted, or no library is produced.
cat > "$OUTPUT/shadow_minimum.py" <<'END_SHADOW_MINIMUM'
import glob, io, os, sys
cod = chr(117) + chr(116) + chr(102) + chr(45) + chr(56)
short = 'tfetch2DShadow(SHADOWMAP_SAMPLER_'
old = 'tfetch2DShadow(SHADOWMAP_SAMPLER_Texture2DDescriptorIndex, SHADOWMAP_SAMPLER_SamplerDescriptorIndex,'
new_value = ('tfetch2DShadowMin(SHADOWMAP_SAMPLER_Texture2DDescriptorIndex, SHADOWMAP_SAMPLER_Texture3DDescriptorIndex, '
         'SHADOWMAP_SAMPLER_SamplerDescriptorIndex,')
define3d = '#define SHADOWMAP_SAMPLER_Texture3DDescriptorIndex'
shaders = calls = 0
for f in sorted(glob.glob(os.path.join(sys.argv[1], '*.hlsl'))):
    s = io.open(f, encoding=cod, newline='').read()
    n = s.count(short)
    if not n:
        continue
    if s.count(old) != n or define3d not in s:
        print('shadow by minimum: %s has %d calls and %d with the expected shape' % (os.path.basename(f), n, s.count(old)))
        sys.exit(1)
    io.open(f, 'w', encoding=cod, newline='').write(s.replace(old, new_value))
    shaders += 1
    calls += n
print('shadow by minimum: %d calls in %d shaders' % (calls, shaders))
sys.exit(0 if calls else 1)
END_SHADOW_MINIMUM
py "$OUTPUT/shadow_minimum.py" "$OUTPUT/hlsl" || { echo "The shadow map calls were not rewritten"; exit 1; }

# The radial blur of the final composition (p_000139) behind a specialization constant. The factor
# lives in r0.x and is used in `r5 * r0.xxx + r3`; with r0.x = 0 the output is the center tap and the
# seven offset taps plus the HEIGHTMAP one are dead.
comp=$OUTPUT/hlsl/p_000139.hlsl
if [ -f "$comp" ]; then
  # Careful: setting the factor to 0 does not work. In floating point x*0 is not folded (NaN/Inf), so
  # the chain of the seven taps survives DCE. The blend itself has to be cut: with the bit set the
  # output is directly the center tap r3, and then r5 is unused and the taps die.
  py -c "
import io,sys
R=sys.argv[1]
s=io.open(R,encoding=chr(117)+chr(116)+chr(102)+chr(45)+chr(56)).read()
v='r5.xyz = r5.xyz * r0.xxx + r3.xyz;'
n=s.count(v)
if n!=1:
    print('the blur blend appears %d times, expected 1' % n); sys.exit(1)
s=s.replace(v,'r5.xyz = (g_SpecConstants() & SPEC_CONSTANT_WITHOUT_BLUR) ? r3.xyz : (r5.xyz * r0.xxx + r3.xyz);',1)
io.open(R,'w',encoding=chr(117)+chr(116)+chr(102)+chr(45)+chr(56)).write(s)
print('composition blur: blend rewritten')
" "$comp" || { echo "The blur blend was not rewritten"; exit 1; }
fi

: > "$OUTPUT/dxc.log"
: > "$OUTPUT/spirv-val.log"
n=0
for f in "$OUTPUT"/hlsl/*.hlsl; do
  base=$(basename "$f" .hlsl)
  if [[ "$base" == p_* ]]; then type=ps_6_6; extra=(); else type=vs_6_6; extra=(-fvk-invert-y); fi
  "$DXC" -spirv -T "$type" -E main -HV 2021 -fspv-target-env=vulkan1.2 -fvk-use-dx-layout \
    -Werror=parameter-usage "${extra[@]}" -Fo "$OUTPUT/spirv/$base.spv" "$OUTPUT/hlsl/$base.hlsl" \
    2>> "$OUTPUT/dxc.log" || { echo "DXC rejection $base"; exit 1; }
  "$VAL" --target-env vulkan1.2 --scalar-block-layout "$OUTPUT/spirv/$base.spv" \
    2>> "$OUTPUT/spirv-val.log" || { echo "SPIR-V invalid: $base"; exit 1; }
  n=$((n + 1))
done
expected=$(ls "$ENTRY"/*.bin | wc -l)
echo "spirv=$n expected=$expected"
[ "$n" -eq "$expected" ] || { echo "The number of outputs does not match the input"; exit 1; }
# The tfetch2DShadowMin marker (NFSC_MARK_SHADOW_MINIMUM, an OpConstant) must be in the SPIR-V of
# every pixel shader that uses it: it is what the app checks to know it can ask for the minimum.
cat > "$OUTPUT/mark_shadow_minimum.py" <<'END_MARK_SHADOW'
import glob, io, os, struct, sys
cod = chr(117) + chr(116) + chr(102) + chr(45) + chr(56)
def has_mark(path):
    b = open(path, 'rb').read()
    w = struct.unpack('<%dI' % (len(b) // 4), b[:len(b) // 4 * 4])
    if len(w) < 5 or w[0] != 0x07230203:
        return False
    i = 5
    while i < len(w):
        n = w[i] >> 16
        if n == 0 or i + n > len(w):
            return False
        if (w[i] & 0xFFFF) == 43 and n == 4 and w[i + 3] == 0x5E3B1A84:
            return True
        i += n
    return False
with_mark = without_mark = 0
for f in sorted(glob.glob(os.path.join(sys.argv[1], 'hlsl', 'p_*.hlsl'))):
    if 'tfetch2DShadowMin(SHADOWMAP_SAMPLER_' not in io.open(f, encoding=cod).read():
        continue
    spv = os.path.join(sys.argv[1], 'spirv', os.path.basename(f)[:-5] + '.spv')
    if os.path.exists(spv) and has_mark(spv):
        with_mark += 1
    else:
        without_mark += 1
        print('without the tfetch2DShadowMin mark: ' + os.path.basename(spv))
print('tfetch2DShadowMin mark: %d pixel shaders with it and %d without it' % (with_mark, without_mark))
sys.exit(0 if with_mark and not without_mark else 1)
END_MARK_SHADOW
py "$OUTPUT/mark_shadow_minimum.py" "$OUTPUT" || { echo "The tfetch2DShadowMin mark is missing in the SPIR-V"; exit 1; }
"$ROOT/nfsmw_pack_library.exe" "$ENTRY" "$OUTPUT/spirv" "$OUTPUT/nfsmw_shaders.nfsp" > "$OUTPUT/packet.log" \
  || { echo "Miss al pack; ver packet.log"; exit 1; }
"$ROOT/nfsmw_test_library.exe" "$OUTPUT/nfsmw_shaders.nfsp" "$ENTRY" > "$OUTPUT/library.log" \
  || { echo "Library regression failure; see library.log"; exit 1; }
ls -la "$OUTPUT/nfsmw_shaders.nfsp"
echo "regeneracion completada"
