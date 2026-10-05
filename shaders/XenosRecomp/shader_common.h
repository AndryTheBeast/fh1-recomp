#ifndef SHADER_COMMON_H_INCLUDED
#define SHADER_COMMON_H_INCLUDED

#define SPEC_CONSTANT_R11G11B10_NORMAL  (1 << 0)
#define SPEC_CONSTANT_ALPHA_TEST        (1 << 1)
// NFSMW: constants arrive through a dynamic UBO (a constant bank on Maxwell) instead of being read
// through a 64-bit pointer. High bit so as not to clash with the UNLEASHED_RECOMP ones.
#define SPEC_CONSTANT_CONSTANTS_UBO    (1 << 8)
// The tfetch offset is scaled with 1/size taken from the shared constants instead of querying the
// texture for its size. A size query is another texture unit operation, and the library has 160 of
// them for 435 samples.
#define SPEC_CONSTANT_INV_SIZE_TEX    (1 << 9)
// Cheap PCF. The shaders that sample the shadow map use a 3x3 pattern at half-texel offsets: nine
// samples per pixel. In p_000101 (the smoke and wheel spray, which are full-screen rectangles) there
// are eleven, and that single draw is 21 % of the scene. With this bit the eight outer offsets are
// set to zero: the nine samples become identical, DXC merges them into one and the shadow goes from
// 3x3 filtered to a single texel. Edge smoothness is lost.
#define SPEC_CONSTANT_PCF_CHEAP        (1 << 15)
/*
 * The alpha test function, specialized (bits 16-18).
 *
 * g_AlphaFunction used to come from the shared constants, that is, at run time, so alphaTestValue
 * was a 7-case switch: ~8 branches in every pixel shader that does alpha testing. In p_000131 (the
 * distant world, 45 % of the scene's fragments) that was 12 branches for 1 sample and 14 operations.
 * And it was not needed at all: the app already knows the function when it creates the pipeline (it
 * comes from RB_COLORCONTROL, which is already in the key), so with three more bits the compiler
 * keeps the one comparison that applies and drops the other six. The output is bit-identical.
 */
/*
 * The radial blur of the final composition.
 *
 * p_000139 is the race VisualTreatment: one full-screen quad, exactly 1280x720, no overdraw, with
 * twelve samples. It takes 2.8-3.2 ms real, 72-80 % of all post-processing.
 *
 * Of those twelve, seven are offset DIFFUSEMAP taps (plus one HEIGHTMAP tap that only feeds the
 * factor): the radial speed blur. They are blended with `r5*factor + r3`, where r3 is the center tap,
 * so with factor 0 the output is exactly the center tap and the eight samples are dead: DXC and the
 * driver remove them when specializing.
 *
 * The edge blur when accelerating and with NOS is lost. The image is sharper.
 */
#define SPEC_CONSTANT_WITHOUT_BLUR    (1 << 19)
/*
 * The world shadow map as the minimum of two textures (fh1_native_shadow_minimum).
 *
 * The game resolves the same 1600x1600 map twice: without cars (sampled by the car body) and with the
 * cars drawn on top (sampled by the world). With this option the renderer draws the cars onto a
 * render target cleared to 1.0, without copying the world underneath, and the shaders that sample the
 * map with cars take the minimum of the two textures: with the game's LESS/LEQUAL depth test,
 * min(world, cars) is exactly what drawing the cars on top produces. The second texture arrives in the
 * 3D index word of the same register (the shadow map is 2D: that word is unused). With the bit off the
 * code is that of tfetch2DShadow.
 */
#define SPEC_CONSTANT_SHADOW_MINIMUM     (1 << 23)
/*
 * The app knows the library has tfetch2DShadowMin because this constant appears in the SPIR-V of the
 * shaders that use it (OpConstant) and nowhere else. The specialization constant never reaches this
 * value (bits 24-30).
 */
#define FH1_MARK_SHADOW_MINIMUM       0x5E3B1A84u
#define SPEC_CONSTANT_ALPHA_FUNC_SHIFT  16
#define SPEC_CONSTANT_ALPHA_FUNC_MASK   (7 << 16)

#ifdef UNLEASHED_RECOMP
    #define SPEC_CONSTANT_BICUBIC_GI_FILTER (1 << 2)
    #define SPEC_CONSTANT_ALPHA_TO_COVERAGE (1 << 3)
    #define SPEC_CONSTANT_REVERSE_Z         (1 << 4)
#endif

#if !defined(__cplusplus) || defined(__INTELLISENSE__)

#define FLT_MIN asfloat(0xff7fffff)
#define FLT_MAX asfloat(0x7f7fffff)

#ifdef __spirv__

struct PushConstants
{
    uint64_t VertexShaderConstants;
    uint64_t PixelShaderConstants;
    uint64_t SharedConstants;
};

[[vk::push_constant]] ConstantBuffer<PushConstants> g_PushConstants;

// NFSMW: the same blocks of the upload buffer, also as dynamic UBOs in set 4. With
// -fvk-use-dx-layout each float4 takes 16 contiguous bytes, so any 4-byte word of the shared block is
// a component: v[B / 16][(B % 16) / 4], and asuint reads it without changing a bit.
struct Fh1BlockVs { float4 v[256]; };
// FH1: pixel shaders use all 256 constants (NFS: 224).
struct Fh1BlockPs { float4 v[256]; };
struct Fh1BlockShared { float4 v[64]; };  // FH1: 256 words (g_PosScale 252; booleans 244-251; nfsc-recomp: loop constants at 122-153; FH1 154-163, exponent scales 164-179, ranked fetches 180-243)
[[vk::binding(0, 4)]] ConstantBuffer<Fh1BlockVs> g_UboVertex;
[[vk::binding(1, 4)]] ConstantBuffer<Fh1BlockPs> g_UboPixel;
[[vk::binding(2, 4)]] ConstantBuffer<Fh1BlockShared> g_UboShared;
#define NFSMW_UBO ((g_SpecConstants & SPEC_CONSTANT_CONSTANTS_UBO) != 0)
#define FH1_SHARED_UINT(B)  asuint(g_UboShared.v[(B) / 16][((B) % 16) / 4])
#define FH1_SHARED_FLOAT(B) g_UboShared.v[(B) / 16][((B) % 16) / 4]

#define g_Booleans                 (NFSMW_UBO ? FH1_SHARED_UINT(256) : vk::RawBufferLoad<uint>(g_PushConstants.SharedConstants + 256))
// FH1: all 256 boolean constants (words 244-251 = registers BOOL_000_031..BOOL_224_255; pixel shader b<n> is
// bit 128 + n). g_Booleans only holds b0-b15 of each stage, and FH1's shaders go past that (the scenery's
// bEnableDeferredLightContribution is b100: the headlights did not light the road).
#define g_BooleanWord(W)           (NFSMW_UBO ? FH1_SHARED_UINT(976 + (W) * 4) : vk::RawBufferLoad<uint>(g_PushConstants.SharedConstants + 976 + (W) * 4))
#define FH1_BOOL(N)                ((g_BooleanWord((N) >> 5) >> ((N) & 31)) & 1u)
#define g_SwappedTexcoords         (NFSMW_UBO ? FH1_SHARED_UINT(260) : vk::RawBufferLoad<uint>(g_PushConstants.SharedConstants + 260))
#define g_HalfPixelOffset          (NFSMW_UBO ? float2(FH1_SHARED_FLOAT(264), FH1_SHARED_FLOAT(268)) : vk::RawBufferLoad<float2>(g_PushConstants.SharedConstants + 264))
#define g_AlphaThreshold           (NFSMW_UBO ? FH1_SHARED_FLOAT(272) : vk::RawBufferLoad<float>(g_PushConstants.SharedConstants + 272))
// FH1: guest pixels per host pixel (1, or 0.5 in a 4x pass drawn at twice the size): the pixel position register
// counts guest pixels.
#define g_PosScale                 (NFSMW_UBO ? FH1_SHARED_FLOAT(1008) : vk::RawBufferLoad<float>(g_PushConstants.SharedConstants + 1008))
// FH1: the slot of the shared block that holds the texture of vertex sampler register N (0-7), four bits each.
// The slots are numbered by the pixel shader's sampler registers; the renderer gives a vertex sampler whose number
// the pixel shader also uses a free one (the bloom's bright pass reads the adapted luminance in its vertex shader
// at register 1, and its pixel shader has a texture at 1 too).
#define g_VsSlots                  (NFSMW_UBO ? FH1_SHARED_UINT(1012) : vk::RawBufferLoad<uint>(g_PushConstants.SharedConstants + 1012))
#define FH1_VS_SLOT(N)             ((g_VsSlots >> ((N) * 4)) & 15u)
// NFSMW: alpha test function (RB_COLORCONTROL.alpha_func): 0 never, 1 <, 2 ==, 3 <=,
// 4 >, 5 !=, 6 >=, 7 always.
#define g_AlphaFunction            (NFSMW_UBO ? FH1_SHARED_UINT(276) : vk::RawBufferLoad<uint>(g_PushConstants.SharedConstants + 276))
// NFSMW: position to host clip space, like ndc_scale/ndc_offset in the
// emulation (graphics/util/draw.cpp). (1, 1) and (0, 0) for normal draws;
// with Xenos clipping disabled it converts from pixels to NDC.
#define g_NdcScale                 (NFSMW_UBO ? float2(FH1_SHARED_FLOAT(280), FH1_SHARED_FLOAT(284)) : vk::RawBufferLoad<float2>(g_PushConstants.SharedConstants + 280))
#define g_NdcOffset                (NFSMW_UBO ? float2(FH1_SHARED_FLOAT(288), FH1_SHARED_FLOAT(292)) : vk::RawBufferLoad<float2>(g_PushConstants.SharedConstants + 288))
// NFSMW: where each component of the vertex input at that location comes from
// (D3D patches the fetch swizzle according to the declaration). 0xFFF = as is.
#define g_InputRemap(LOC)          (NFSMW_UBO ? FH1_SHARED_UINT(296 + (LOC) * 4) : vk::RawBufferLoad<uint>(g_PushConstants.SharedConstants + 296 + (LOC) * 4))

[[vk::constant_id(0)]] const uint g_SpecConstants = 0;

#define g_SpecConstants() g_SpecConstants

#else

#define DEFINE_SHARED_CONSTANTS() \
    uint g_Booleans : packoffset(c16.x); \
    uint g_SwappedTexcoords : packoffset(c16.y); \
    float2 g_HalfPixelOffset : packoffset(c16.z); \
    float g_AlphaThreshold : packoffset(c17.x); \
    uint g_AlphaFunction : packoffset(c17.y); \
    float2 g_NdcScale : packoffset(c17.z); \
    float2 g_NdcOffset : packoffset(c18.x);

uint g_SpecConstants();

#define g_InputRemap(LOC) 0xFFF
#define FH1_BOOL(N) ((g_Booleans >> ((N) & 31)) & 1u)

#endif

// FH1: vertex data the declaration does not list (the cars' extra streams), read straight from guest
// memory (shared constants: g_GuestBase at 616, g_FetchAddress at 624, after nfsc-recomp's loop
// constants at 488-615). g_GuestBase 0 = guest memory not available: the fetch returns 0. The renderer fills g_GuestBase (device address of guest physical memory) and, for fetch
// constants 24-31, g_FetchAddress(c) = the stream's guest byte address | its endian in bits 0-1
// (0 none, 1 8in16, 2 8in32, 3 16in32). Component layouts as in the SDK's SPIR-V translator
// (spirv_translator_fetch.cpp); fraction formats are normalized (signed: max(v / (2^(w-1) - 1), -1)).
#ifdef __spirv__
#define g_GuestBase       (NFSMW_UBO ? (uint64_t(FH1_SHARED_UINT(616)) | (uint64_t(FH1_SHARED_UINT(620)) << 32)) : vk::RawBufferLoad<uint64_t>(g_PushConstants.SharedConstants + 616))
#define g_FetchAddress(C) (NFSMW_UBO ? FH1_SHARED_UINT(624 + ((C) - 24) * 4) : vk::RawBufferLoad<uint>(g_PushConstants.SharedConstants + 624 + ((C) - 24) * 4))

uint fh1Swap(uint w, uint endian)
{
    if (endian == 1u || endian == 2u)
        w = ((w & 0x00FF00FFu) << 8) | ((w >> 8) & 0x00FF00FFu);
    if (endian == 2u || endian == 3u)
        w = (w << 16) | (w >> 16);
    return w;
}

// FH1: declared fetches indexed by a computed register (fh1FetchRanked). Per declared fetch instruction (rank of its
// address, 0-31): word 180 + rank = where its stream is (as g_FetchAddress), word 212 + rank = stride in words (bits
// 0-7), offset in words (8-23), format (24-29), signed (30), integer (31).
#define g_FetchRankAddress(R) (NFSMW_UBO ? FH1_SHARED_UINT(720 + (R) * 4) : vk::RawBufferLoad<uint>(g_PushConstants.SharedConstants + 720 + (R) * 4))
#define g_FetchRankParam(R)   (NFSMW_UBO ? FH1_SHARED_UINT(848 + (R) * 4) : vk::RawBufferLoad<uint>(g_PushConstants.SharedConstants + 848 + (R) * 4))

uint fh1FetchWordAt(uint address, uint index, uint stride, int offset, uint i);

uint fh1FetchWord(uint c, uint index, uint stride, int offset, uint i)
{
    return fh1FetchWordAt(g_FetchAddress(c), index, stride, offset, i);
}

uint fh1FetchWordAt(uint address, uint index, uint stride, int offset, uint i)
{
    if (g_GuestBase == 0 || address < 4u)
        return 0u;
    uint byteAddress = (address & ~3u) + (index * stride + uint(offset) + i) * 4u;
    return fh1Swap(vk::RawBufferLoad<uint>(g_GuestBase + byteAddress), address & 3u);
}

float fh1Unpack(uint v, uint shift, uint width, bool isSigned, bool normalized)
{
    uint bits = (v >> shift) & (width >= 32u ? 0xFFFFFFFFu : ((1u << width) - 1u));
    if (isSigned)
    {
        int s = int(bits << (32u - width)) >> (32u - width);
        return normalized ? max(float(s) / float((1u << (width - 1u)) - 1u), -1.0) : float(s);
    }
    return normalized ? float(bits) / float((1u << width) - 1u) : float(bits);
}

float4 fh1FetchAt(uint c, float indexValue, bool rounded, uint stride, int offset, uint format, bool isSigned,
                  bool normalized);

float4 fh1Fetch(uint c, float indexValue, bool rounded, uint stride, int offset, uint format, bool isSigned,
                bool normalized)
{
    return fh1FetchAt(g_FetchAddress(c), indexValue, rounded, stride, offset, format, isSigned, normalized);
}

float4 fh1FetchRanked(uint rankFull, uint rankOwn, float indexValue, bool rounded)
{
    uint full = g_FetchRankParam(rankFull);
    uint own = g_FetchRankParam(rankOwn);
    return fh1FetchAt(g_FetchRankAddress(rankFull), indexValue, rounded, full & 0xFFu, int((own >> 8) & 0xFFFFu),
                      (own >> 24) & 0x3Fu, ((own >> 30) & 1u) != 0u, ((own >> 31) & 1u) == 0u);
}

// c is the stream's place (as g_FetchAddress gives it), not a fetch constant number.
float4 fh1FetchAt(uint c, float indexValue, bool rounded, uint stride, int offset, uint format, bool isSigned,
                  bool normalized)
{
    uint index = uint(rounded ? round(indexValue) : floor(indexValue));
    uint w0 = fh1FetchWordAt(c, index, stride, offset, 0);
    switch (format)
    {
    case 6:   // 8_8_8_8
        return float4(fh1Unpack(w0, 0, 8, isSigned, normalized), fh1Unpack(w0, 8, 8, isSigned, normalized),
                      fh1Unpack(w0, 16, 8, isSigned, normalized), fh1Unpack(w0, 24, 8, isSigned, normalized));
    case 7:   // 2_10_10_10
        return float4(fh1Unpack(w0, 0, 10, isSigned, normalized), fh1Unpack(w0, 10, 10, isSigned, normalized),
                      fh1Unpack(w0, 20, 10, isSigned, normalized), fh1Unpack(w0, 30, 2, isSigned, normalized));
    case 16:  // 10_11_11
        return float4(fh1Unpack(w0, 0, 11, isSigned, normalized), fh1Unpack(w0, 11, 11, isSigned, normalized),
                      fh1Unpack(w0, 22, 10, isSigned, normalized), 1.0);
    case 17:  // 11_11_10
        return float4(fh1Unpack(w0, 0, 10, isSigned, normalized), fh1Unpack(w0, 10, 11, isSigned, normalized),
                      fh1Unpack(w0, 21, 11, isSigned, normalized), 1.0);
    case 25:  // 16_16
        return float4(fh1Unpack(w0, 0, 16, isSigned, normalized), fh1Unpack(w0, 16, 16, isSigned, normalized), 0.0, 1.0);
    case 26:  // 16_16_16_16
    {
        uint w1 = fh1FetchWordAt(c, index, stride, offset, 1);
        return float4(fh1Unpack(w0, 0, 16, isSigned, normalized), fh1Unpack(w0, 16, 16, isSigned, normalized),
                      fh1Unpack(w1, 0, 16, isSigned, normalized), fh1Unpack(w1, 16, 16, isSigned, normalized));
    }
    case 31:  // 16_16_FLOAT
        return float4(f16tof32(w0 & 0xFFFFu), f16tof32(w0 >> 16), 0.0, 1.0);
    case 32:  // 16_16_16_16_FLOAT
    {
        uint w1 = fh1FetchWordAt(c, index, stride, offset, 1);
        return float4(f16tof32(w0 & 0xFFFFu), f16tof32(w0 >> 16), f16tof32(w1 & 0xFFFFu), f16tof32(w1 >> 16));
    }
    case 36:  // 32_FLOAT
        return float4(asfloat(w0), 0.0, 0.0, 1.0);
    case 37:  // 32_32_FLOAT
        return float4(asfloat(w0), asfloat(fh1FetchWordAt(c, index, stride, offset, 1)), 0.0, 1.0);
    case 57:  // 32_32_32_FLOAT
        return float4(asfloat(w0), asfloat(fh1FetchWordAt(c, index, stride, offset, 1)),
                      asfloat(fh1FetchWordAt(c, index, stride, offset, 2)), 1.0);
    case 38:  // 32_32_32_32_FLOAT
        return float4(asfloat(w0), asfloat(fh1FetchWordAt(c, index, stride, offset, 1)),
                      asfloat(fh1FetchWordAt(c, index, stride, offset, 2)),
                      asfloat(fh1FetchWordAt(c, index, stride, offset, 3)));
    default:
        return float4(0.0, 0.0, 0.0, 1.0);
    }
}
#endif

// FH1: exponent scale of a texture fetch (<sampler>_ExpScale, written by the renderer). A negative scale marks a
// resolved picture kept in a float image: the console stores it in an unsigned format, which has no negative values
// and no NaN (a float image keeps both, and they came out as coloured specks on silhouettes), so they are cut to 0.
// Since 2026-10-05 the same unsigned format's upper end too. The console resolves the float scene into 10 bits
// with a negative exponent bias and the fetch multiplies it back: the value is cut at 1 in between, so a fetch
// never returns more than 2^(its own exponent adjust) (4 for the scene, 16 for the reflection cube map). A float
// image has no such cut: single pixels reached 32,000 here, the game's FXAA, bloom and reflections spread them
// (blue rims on chrome and glass, orange dots on the car's outline at night) and the exposure read a brighter
// scene than the console's (a darker picture). The renderer packs the cut into the scale: -s = 2^e * (1 + code /
// 64), scale = 2^e, cut = 2^(code - 17); code 0 = no cut (DrawsVulkan: ExpScaleFloatPicture).
void fh1ExpFloat(float s, out float scale, out float cut)
{
    float exponent;
    float mantissa = frexp(-s, exponent);  // [0.5, 1)
    float code = round((mantissa * 2.0 - 1.0) * 64.0);
    scale = ldexp(1.0, exponent - 1.0);
    cut = code > 0.5 ? ldexp(1.0, code - 17.0) : asfloat(0x7f7fffff);
}
// FH1: a texture fetched with the gamma sign. The console decodes it with a piecewise-linear curve, which is
// brighter than sRGB in the dark and middle tones (0.5 gives 0.25, sRGB 0.214): with the host's sRGB formats the
// whole scene came out darker than on the emulated GPU. The renderer marks such a slot by multiplying its
// (power of two) exponent scale by 1.5; red, green and blue are converted after filtering, as the emulated GPU does.
// Since the same day the mark is the whole sign byte of the fetch constant, already swizzled (two bits per fetched
// component: 1 = biased, value * 2 - 1; 3 = gamma), in bits 15-22 of the scale: the brightness picture the exposure
// reads is fetched biased, and read plain the exposure came out lower than on the emulated GPU.
#define FH1_SIGNS_MASK 0x007F8000u
float3 fh1PwlGammaToLinear(float3 g)
{
    g = saturate(g);
    float3 scale = float3(1.0 / 1024.0, 1.0 / 1024.0, 1.0 / 1024.0);
    float3 offset = float3(0.0, 0.0, 0.0);
    scale = select(g >= 64.0 / 255.0, float3(2.0 / 1024.0, 2.0 / 1024.0, 2.0 / 1024.0), scale);
    offset = select(g >= 64.0 / 255.0, float3(-64.0, -64.0, -64.0), offset);
    scale = select(g >= 96.0 / 255.0, float3(4.0 / 1024.0, 4.0 / 1024.0, 4.0 / 1024.0), scale);
    offset = select(g >= 96.0 / 255.0, float3(-256.0, -256.0, -256.0), offset);
    scale = select(g >= 192.0 / 255.0, float3(8.0 / 1024.0, 8.0 / 1024.0, 8.0 / 1024.0), scale);
    offset = select(g >= 192.0 / 255.0, float3(-1024.0, -1024.0, -1024.0), offset);
    float3 l = g * ((255.0 * 1024.0) * scale) + offset;
    l += trunc(l * scale);
    return l * (1.0 / 1023.0);
}
float4 fh1Gamma(float4 v, float s)
{
    uint signs = s > 0.0 ? (asuint(s) & FH1_SIGNS_MASK) >> 15 : 0u;
    if (signs != 0u)
    {
        uint4 mode = uint4(signs, signs >> 2, signs >> 4, signs >> 6) & 3u;
        float4 gamma = float4(fh1PwlGammaToLinear(v.xyz), fh1PwlGammaToLinear(v.www).x);
        v = select(mode == 1u, v * 2.0 - 1.0, select(mode == 3u, gamma, v));
    }
    return v;
}
float4 fh1Exp(float4 v, float s)
{
    if (s >= 0.0)
        return v * asfloat(asuint(s) & ~FH1_SIGNS_MASK);
    float scale, cut;
    fh1ExpFloat(s, scale, cut);
    // "v > 0" is false for NaN as well
    return min(select(v > 0.0, v, 0.0) * scale, cut);
}
float3 fh1Exp(float3 v, float s) { return fh1Exp(float4(v, 0.0), s).xyz; }
float2 fh1Exp(float2 v, float s) { return fh1Exp(float4(v, 0.0, 0.0), s).xy; }
float fh1Exp(float v, float s) { return fh1Exp(float4(v, 0.0, 0.0, 0.0), s).x; }

// FH1: level of detail of a fetch. The translator calls fh1Lod just before a fetch that does not use the computed
// level (and fh1Lod(0, 0) after it): mode 1 = this level (the register of setTexLOD, or 0, plus the instruction's
// bias), mode 2 = the computed level plus a bias. Both are literals at each call, so the compiler keeps one
// branch. Implicit-level sampling is only allowed in pixel shaders; vertex shaders use level 0.
static int g_fh1LodMode = 0;
static float g_fh1Lod = 0.0;
static float g_fh1RegLod = 0.0;
void fh1Lod(int mode, float lod)
{
    g_fh1LodMode = mode;
    g_fh1Lod = lod;
}
#ifdef FH1_VERTEX_SHADER
#define FH1_SAMPLE(TEXTURE, SAMPLER, COORD) (TEXTURE).SampleLevel(SAMPLER, COORD, g_fh1LodMode == 1 ? g_fh1Lod : 0.0)
#else
#define FH1_SAMPLE_BODY(TYPE, COORD_TYPE) \
    float4 fh1Sample(TYPE<float4> t, SamplerState s, COORD_TYPE c) \
    { \
        if (g_fh1LodMode == 1) \
            return t.SampleLevel(s, c, g_fh1Lod); \
        if (g_fh1LodMode == 2) \
            return t.SampleBias(s, c, g_fh1Lod); \
        return t.Sample(s, c); \
    }
FH1_SAMPLE_BODY(Texture2D, float2)
FH1_SAMPLE_BODY(Texture3D, float3)
FH1_SAMPLE_BODY(TextureCube, float3)
#define FH1_SAMPLE(TEXTURE, SAMPLER, COORD) fh1Sample(TEXTURE, SAMPLER, COORD)
#endif

Texture2D<float4> g_Texture2DDescriptorHeap[] : register(t0, space0);
Texture3D<float4> g_Texture3DDescriptorHeap[] : register(t0, space1);
TextureCube<float4> g_TextureCubeDescriptorHeap[] : register(t0, space2);
SamplerState g_SamplerDescriptorHeap[] : register(s0, space3);

uint2 getTexture2DDimensions(Texture2D<float4> texture)
{
    uint2 dimensions;
    texture.GetDimensions(dimensions.x, dimensions.y);
    return dimensions;
}

float4 tfetch2D(uint resourceDescriptorIndex, uint samplerDescriptorIndex, float2 texCoord, float2 offset, float2 invSize)
{
    Texture2D<float4> texture = g_Texture2DDescriptorHeap[resourceDescriptorIndex];
    // With the bit set, the size comes from a constant and the texture does not have to be queried. It
    // is the same computation with the same number: the renderer writes 1/size of the host image. With a
    // ternary, DXC evaluates both branches and the size query stays: an if with [branch] is needed for
    // the dead branch to disappear when the pipeline is specialized.
    float2 displacement;
    [branch] if (g_SpecConstants() & SPEC_CONSTANT_INV_SIZE_TEX)
        displacement = offset * invSize;
    else
        displacement = offset / getTexture2DDimensions(texture);
    return FH1_SAMPLE(texture, g_SamplerDescriptorHeap[samplerDescriptorIndex], texCoord + displacement);
}

float2 getWeights2D(uint resourceDescriptorIndex, uint samplerDescriptorIndex, float2 texCoord, float2 offset)
{
    Texture2D<float4> texture = g_Texture2DDescriptorHeap[resourceDescriptorIndex];
    return select(isnan(texCoord), 0.0, frac(texCoord * getTexture2DDimensions(texture) + offset - 0.5));
}

// FH1: the translator passes 1/size of the slot's image (<sampler>_InvSize, filled by the renderer);
// when it is not filled (0), the size is queried as above.
float2 getWeights2D(uint resourceDescriptorIndex, uint samplerDescriptorIndex, float2 texCoord, float2 offset,
                    float2 invSize)
{
    if (invSize.x <= 0.0 || invSize.y <= 0.0)
        return getWeights2D(resourceDescriptorIndex, samplerDescriptorIndex, texCoord, offset);
    return select(isnan(texCoord), 0.0, frac(texCoord / invSize + offset - 0.5));
}

float w0(float a)
{
    return (1.0f / 6.0f) * (a * (a * (-a + 3.0f) - 3.0f) + 1.0f);
}

float w1(float a)
{
    return (1.0f / 6.0f) * (a * a * (3.0f * a - 6.0f) + 4.0f);
}

float w2(float a)
{
    return (1.0f / 6.0f) * (a * (a * (-3.0f * a + 3.0f) + 3.0f) + 1.0f);
}

float w3(float a)
{
    return (1.0f / 6.0f) * (a * a * a);
}

float g0(float a)
{
    return w0(a) + w1(a);
}

float g1(float a)
{
    return w2(a) + w3(a);
}

float h0(float a)
{
    return -1.0f + w1(a) / (w0(a) + w1(a)) + 0.5f;
}

float h1(float a)
{
    return 1.0f + w3(a) / (w2(a) + w3(a)) + 0.5f;
}

// Shadow map sampling. The translator emits tfetch2D for everything; the library rewrite step
// changes the calls whose sampler is SHADOWMAP_SAMPLER to this function, since those are the only
// ones meant to be made cheaper. With the bit set, all the 3x3 samples land on the same texel and
// the compiler keeps only one.
float4 tfetch2DShadow(uint resourceDescriptorIndex, uint samplerDescriptorIndex, float2 texCoord, float2 offset, float2 invSize)
{
    [branch] if (g_SpecConstants() & SPEC_CONSTANT_PCF_CHEAP)
        return tfetch2D(resourceDescriptorIndex, samplerDescriptorIndex, texCoord, float2(0.0, 0.0), invSize);
    return tfetch2D(resourceDescriptorIndex, samplerDescriptorIndex, texCoord, offset, invSize);
}

// The shadow map with the minimum of its pair (fh1_native_shadow_minimum). The library step changes
// all shadow map calls to this function and passes it the 3D index of that same register, which is
// where the app puts the pair: the world map (or the texture itself, which gives the same texel, while
// the app is checking). Both are sampled the same way (same point sampler, same coordinates and
// offsets), so the minimum is per texel. The first comparison is the library marker: it is never
// true and the driver removes it when specializing.
float4 tfetch2DShadowMin(uint resourceDescriptorIndex, uint pairDescriptorIndex, uint samplerDescriptorIndex, float2 texCoord, float2 offset, float2 invSize)
{
    [branch] if (g_SpecConstants() == FH1_MARK_SHADOW_MINIMUM)
        return float4(0.0, 0.0, 0.0, 0.0);
    float4 input_value = tfetch2DShadow(resourceDescriptorIndex, samplerDescriptorIndex, texCoord, offset, invSize);
    [branch] if (g_SpecConstants() & SPEC_CONSTANT_SHADOW_MINIMUM)
        input_value = min(input_value, tfetch2DShadow(pairDescriptorIndex, samplerDescriptorIndex, texCoord, offset, invSize));
    return input_value;
}

float4 tfetch2DBicubic(uint resourceDescriptorIndex, uint samplerDescriptorIndex, float2 texCoord, float2 offset)
{
    Texture2D<float4> texture = g_Texture2DDescriptorHeap[resourceDescriptorIndex];
    SamplerState samplerState = g_SamplerDescriptorHeap[samplerDescriptorIndex];
    uint2 dimensions = getTexture2DDimensions(texture);
    
    float x = texCoord.x * dimensions.x + offset.x;
    float y = texCoord.y * dimensions.y + offset.y;

    x -= 0.5f;
    y -= 0.5f;
    float px = floor(x);
    float py = floor(y);
    float fx = x - px;
    float fy = y - py;

    float g0x = g0(fx);
    float g1x = g1(fx);
    float h0x = h0(fx);
    float h1x = h1(fx);
    float h0y = h0(fy);
    float h1y = h1(fy);

    float4 r =
        g0(fy) * (g0x * FH1_SAMPLE(texture, samplerState, float2(px + h0x, py + h0y) / float2(dimensions)) +
            g1x * FH1_SAMPLE(texture, samplerState, float2(px + h1x, py + h0y) / float2(dimensions))) +
        g1(fy) * (g0x * FH1_SAMPLE(texture, samplerState, float2(px + h0x, py + h1y) / float2(dimensions)) +
            g1x * FH1_SAMPLE(texture, samplerState, float2(px + h1x, py + h1y) / float2(dimensions)));

    return r;
}

// FH1: 1D textures, stored by the renderer as one-row 2D images.
float4 tfetch1D(uint resourceDescriptorIndex, uint samplerDescriptorIndex, float texCoord)
{
    return FH1_SAMPLE(g_Texture2DDescriptorHeap[resourceDescriptorIndex], g_SamplerDescriptorHeap[samplerDescriptorIndex],
                      float2(texCoord, 0.5));
}

float4 tfetch3D(uint resourceDescriptorIndex, uint samplerDescriptorIndex, float3 texCoord)
{
    return FH1_SAMPLE(g_Texture3DDescriptorHeap[resourceDescriptorIndex], g_SamplerDescriptorHeap[samplerDescriptorIndex], texCoord);
}

struct CubeMapData
{
    float3 cubeMapDirections[2];
    uint cubeMapIndex;
};

float4 tfetchCube(uint resourceDescriptorIndex, uint samplerDescriptorIndex, float3 texCoord, inout CubeMapData cubeMapData)
{
    return FH1_SAMPLE(g_TextureCubeDescriptorHeap[resourceDescriptorIndex], g_SamplerDescriptorHeap[samplerDescriptorIndex], cubeMapData.cubeMapDirections[texCoord.z]);
}

float4 tfetchR11G11B10(uint4 value)
{
    if (g_SpecConstants() & SPEC_CONSTANT_R11G11B10_NORMAL)
    {
        return float4(
            (value.x & 0x00000400 ? -1.0 : 0.0) + ((value.x & 0x3FF) / 1024.0),
            (value.x & 0x00200000 ? -1.0 : 0.0) + (((value.x >> 11) & 0x3FF) / 1024.0),
            (value.x & 0x80000000 ? -1.0 : 0.0) + (((value.x >> 22) & 0x1FF) / 512.0),
            0.0);
    }
    else
    {
        return asfloat(value);
    }
}

float4 tfetchTexcoord(uint swappedTexcoords, float4 value, uint semanticIndex)
{
    return (swappedTexcoords & (1ull << semanticIndex)) != 0 ? value.yxwz : value;
}

// NFSMW: 3 bits per component: 0-3 = data component, 4 = 0, 5 = 1, 7 = unchanged.
// FH1: k_10_11_11 vertex data (positions, texcoords) arrives as four bytes (R8G8B8A8_USCALED) and is unpacked
// here when the renderer sets bit 12 of the remap code (bit 13 signed, bit 14 integer, bit 15 signed "no zero"
// fraction mode). Bits 16-21: the fetch's exp_adjust (signed), a power-of-two scale the Xbox 360 applies to every
// vertex format (SDK spirv_translator_fetch.cpp), to the format's components only (bits 22-23: their count - 1).
float fh1UnpackPacked(uint bits, uint shift, uint width, bool isSigned, bool integer, bool noZero)
{
    uint v = (bits >> shift) & ((1u << width) - 1u);
    if (isSigned)
    {
        int s = int(v << (32u - width)) >> (32u - width);
        if (integer)
            return float(s);
        float scale = float((1u << (width - 1u)) - 1u) + (noZero ? 0.5 : 0.0);
        return noZero ? (float(s) + 0.5) / scale : max(float(s) / scale, -1.0);
    }
    return integer ? float(v) : float(v) / float((1u << width) - 1u);
}

float4 remapInput(float4 value, uint code)
{
    if ((code & 0x1000u) != 0u)
    {
        // Four bytes as exact 0-255 floats (R8G8B8A8_USCALED): a float input would let the GPU flush or
        // canonicalize bit patterns that look like denormals or NaNs.
        uint bits = uint(value.x) | (uint(value.y) << 8) | (uint(value.z) << 16) | (uint(value.w) << 24);
        bool isSigned = (code & 0x2000u) != 0u;
        bool integer = (code & 0x4000u) != 0u;
        bool noZero = (code & 0x8000u) != 0u;
        value = float4(fh1UnpackPacked(bits, 0u, 11u, isSigned, integer, noZero),
                       fh1UnpackPacked(bits, 11u, 11u, isSigned, integer, noZero),
                       fh1UnpackPacked(bits, 22u, 10u, isSigned, integer, noZero), 1.0);
    }
    int expAdjust = int(code << 10) >> 26;  // bits 16-21, signed
    if (expAdjust != 0)
    {
        uint components = ((code >> 22) & 3u) + 1u;
        float scale = exp2(float(expAdjust));
        for (uint i = 0; i < components; i++)
            value[i] *= scale;
    }
    code &= 0xFFFu;
    if (code == 0xFFF)
        return value;

    float4 result = value;
    for (uint i = 0; i < 4; i++)
    {
        uint source = (code >> (i * 3)) & 7;
        if (source < 4)
            result[i] = value[source];
        else if (source == 4)
            result[i] = 0.0;
        else if (source == 5)
            result[i] = 1.0;
    }
    return result;
}

// NFSMW: Xenos alpha test with its comparison function. Positive if the pixel passes.
// FH1: alpha to mask (RB_COLORCONTROL bit 4: the crowd, foliage, distance fades). The console covers 0 to 4 of a
// pixel's four samples: sample k is kept when alpha + offset / 16 reaches its threshold (0.75, 0.25, 0.5, 1.0 for
// samples 0-3, as the emulated GPU does), with a two-bit offset per pixel of a 2x2 block (bits 16-23 of
// g_AlphaFunction, the byte of RB_COLORCONTROL). In a pass drawn at twice the size (g_PosScale 0.5) each host pixel
// is one of those samples; in a one-sample pass the pixel is kept when two samples or more would be.
bool fh1AlphaToMask(float alpha, float2 pos)
{
    uint2 guest = uint2(pos * g_PosScale);
    uint offset = (g_AlphaFunction >> (16u + 2u * ((guest.x & 1u) + 2u * (guest.y & 1u)))) & 3u;
    float covered = alpha * 4.0 + float(offset) * 0.25;  // samples covered, before rounding down
    if (g_PosScale < 1.0)
    {
        uint2 host = uint2(pos);
        uint sample_index = (host.x & 1u) + 2u * (host.y & 1u);
        float needed = sample_index == 0u ? 3.0 : sample_index == 1u ? 1.0 : sample_index == 2u ? 2.0 : 4.0;
        return covered >= needed;
    }
    return covered >= 2.0;
}

float alphaTestValue(float alpha, float2 pos)
{
    bool pass = true;
    switch ((g_SpecConstants() & SPEC_CONSTANT_ALPHA_FUNC_MASK) >> SPEC_CONSTANT_ALPHA_FUNC_SHIFT)
    {
    // FH1: function 7 (always) with the alpha test specialized means alpha to mask.
    case 7: pass = fh1AlphaToMask(alpha, pos); break;
    case 0: pass = false; break;
    case 1: pass = alpha < g_AlphaThreshold; break;
    case 2: pass = alpha == g_AlphaThreshold; break;
    case 3: pass = alpha <= g_AlphaThreshold; break;
    case 4: pass = alpha > g_AlphaThreshold; break;
    case 5: pass = alpha != g_AlphaThreshold; break;
    case 6: pass = alpha >= g_AlphaThreshold; break;
    default: break;
    }
    return pass ? 1.0 : -1.0;
}

float4 cube(float4 value, inout CubeMapData cubeMapData)
{
    // FH1: the two slots are used in turn. A shader that looks a cube up in a loop (the filter that makes the paint
    // booth's lighting cube does it 4,096 times) ran past the two directions kept here after its second lookup and
    // fetched with garbage: that cube came out empty and the car's lower half black.
    uint index = cubeMapData.cubeMapIndex & 1u;
    cubeMapData.cubeMapDirections[index] = value.xyz;
    ++cubeMapData.cubeMapIndex;

    return float4(0.0, 0.0, 0.0, index);
}

float4 dst(float4 src0, float4 src1)
{
    float4 dest;
    dest.x = 1.0;
    dest.y = src0.y * src1.y;
    dest.z = src0.z;
    dest.w = src1.w;
    return dest;
}

float4 max4(float4 src0)
{
    return max(max(src0.x, src0.y), max(src0.z, src0.w));
}

float2 getPixelCoord(uint resourceDescriptorIndex, float2 texCoord)
{
    return getTexture2DDimensions(g_Texture2DDescriptorHeap[resourceDescriptorIndex]) * texCoord;
}

float computeMipLevel(float2 pixelCoord)
{
    float2 dx = ddx(pixelCoord);
    float2 dy = ddy(pixelCoord);
    float deltaMaxSqr = max(dot(dx, dx), dot(dy, dy));
    return max(0.0, 0.5 * log2(deltaMaxSqr));
}

#endif

#endif
