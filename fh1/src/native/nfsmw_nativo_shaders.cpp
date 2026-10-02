// nfsmw - native renderer, part C5a (see nfsmw_nativo_shaders.h).
//
// 2005 container format: see Leer(). All fields are big-endian.

#include "nfsmw_nativo_shaders.h"

#include "nfsmw_shader_library.h"

#include <rex/cvar.h>
#include <rex/logging.h>

#include <algorithm>
#include <array>
#include <exception>
#include <span>
#include <filesystem>
#include <cstring>
#include <map>
#include <string>
#include <tuple>
#include <unordered_map>

/*
 * xxHash reads with memcpy (XXH_FORCE_MEMORY_ACCESS 0). With the method it picks for GCC (1) it reads through
 * 64- and 32-bit pointers without may_alias, and GCC may move that read ahead of the store of the data being
 * hashed (strict aliasing). With that, the texture key read claves[4] before writing it and the same texture was
 * created several times (see docs/toolchain.md). Same fingerprint values; on AArch64, the same LDR.
 */
#if defined(XXH_IMPLEM_13a8737387)
#error "xxhash.h ya se ha incluido con su implementacion antes de este punto: XXH_FORCE_MEMORY_ACCESS 0 llegaria tarde"
#endif
#undef XXH_FORCE_MEMORY_ACCESS
#define XXH_FORCE_MEMORY_ACCESS 0
#define XXH_INLINE_ALL
#include <xxhash.h>

REXCVAR_DEFINE_STRING(nfsc_dump_ring_shaders, "", "NFSC",
                      "Tools: folder where every shader the library does not know is written (p_/v_<hash>.mc: type byte, "
                      "big-endian word count, big-endian microcode words), to build containers from the microcode alone")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);

namespace nfsmw::nativo {
namespace {

// NFSC: writes the microcode of a shader the library does not know (once per hash).
void VolcarMicrocodigoDesconocido(bool vertices, uint64_t huella, std::span<const uint32_t> microcodigo) {
  const std::string dir = REXCVAR_GET(nfsc_dump_ring_shaders);
  if (dir.empty()) return;
  static std::unordered_map<uint64_t, bool> hechos;
  if (!hechos.emplace(huella, true).second) return;
  std::error_code ec;
  std::filesystem::create_directories(dir, ec);
  char nombre[64];
  std::snprintf(nombre, sizeof(nombre), "%c_%016llX.mc", vertices ? 'v' : 'p', static_cast<unsigned long long>(huella));
  std::vector<uint8_t> datos;
  datos.push_back(vertices ? 'v' : 'p');
  const uint32_t n = uint32_t(microcodigo.size());
  for (int sh = 24; sh >= 0; sh -= 8) datos.push_back(uint8_t(n >> sh));
  for (uint32_t w : microcodigo) {
    for (int sh = 24; sh >= 0; sh -= 8) datos.push_back(uint8_t(w >> sh));
  }
  if (FILE* f = std::fopen((std::filesystem::path(dir) / nombre).string().c_str(), "wb")) {
    std::fwrite(datos.data(), 1, datos.size(), f);
    std::fclose(f);
  }
}

// Bits of a vertex fetch instruction that D3D does not touch
// (VertexFetchInstruction in XenosRecomp shader_code.h): opcode, source and
// destination registers, destination swizzle and predicate. The rest (fetch
// constant, format, stride, offset and numeric modes) comes from the declaration.
constexpr uint32_t kFetchConserva[3] = {0x0007FFFF, 0x80000FFF, 0x80000000};

constexpr uint32_t kMaxAvisos = 120;

constexpr const char* kUsos[] = {"posicion", "peso",     "indices",    "normal",
                                 "tam_punto", "texcoord", "tangente",   "binormal",
                                 "teselado",  "posicion_t", "color",    "niebla",
                                 "profundidad", "muestra", "uso14",     "uso15"};

struct Contenedor {
  const std::vector<uint8_t>& o;

  bool Hay(size_t posicion, size_t bytes) const {
    return posicion <= o.size() && bytes <= o.size() - posicion;
  }
  uint32_t U32(size_t p) const {
    return uint32_t(o[p]) << 24 | uint32_t(o[p + 1]) << 16 | uint32_t(o[p + 2]) << 8 | o[p + 3];
  }
  uint16_t U16(size_t p) const { return uint16_t(uint32_t(o[p]) << 8 | o[p + 1]); }
};

// nfsmw_nativo_sombra_minimo. The zero-terminated string starting at `posicion` is `nombre`.
bool NombreEs(const Contenedor& c, size_t posicion, const char* nombre) {
  const size_t n = std::strlen(nombre);
  return c.Hay(posicion, n + 1) && std::memcmp(c.o.data() + posicion, nombre, n) == 0 && c.o[posicion + n] == 0;
}

// Reads what is needed from the 2005 container, which is not the one in XenosRecomp
// shader.h (that is the 2008 one): 24-byte header with signature, virtual part,
// physical part, definitions (+12), CTAB (+16) and shader header (+20). The
// microcode is the whole physical part. Reference: docs/shaders.md and
// shaders/nfsmw_contenedor.h (Convertir2005). Returns the reason if it cannot be
// read.
// NFSC: Carbon's Direct3D also REORDERS the vertex fetches of a vertex shader. For identification the fetch
// instructions (the library's element instructions) are compared as a sorted set: their three words are put in
// sorted order within the slots the elements occupy. The attribute mapping later finds each fetch by its destination
// register, so it does not depend on the order.
void CanonicalizarFetches(std::vector<uint32_t>& m, std::vector<uint32_t> slots) {
  std::sort(slots.begin(), slots.end());
  std::vector<std::array<uint32_t, 3>> triples;
  triples.reserve(slots.size());
  for (uint32_t i : slots) triples.push_back({m[size_t(i) * 3], m[size_t(i) * 3 + 1], m[size_t(i) * 3 + 2]});
  std::sort(triples.begin(), triples.end());
  for (size_t k = 0; k < slots.size(); ++k) {
    for (size_t j = 0; j < 3; ++j) m[size_t(slots[k]) * 3 + j] = triples[k][j];
  }
}

// FH1 (measured 2026-10-02, from the FH1-only renderer fh1_native_shaders.cpp): its Direct3D also nulls the exports
// of outputs the pixel shader does not read (this instruction), and patches fetches the declaration does not list
// (the cars' extra streams) like the declared ones. Tolerant pass: fetches compared only by opcode and registers.
constexpr uint32_t kFetchConservaTolerante[3] = {0x0007FFFF, 0x80000000, 0x80000000};
constexpr uint32_t kInstruccionNula[3] = {0xC8000000, 0x00000000, 0x02000000};

bool CoincideFh1(const EntradaShader& e, std::span<const uint32_t> subido) {
  const std::vector<uint32_t>& original = e.microcodigo_shader;
  if (subido.size() != original.size()) return false;
  std::vector<uint8_t> fetch(subido.size() / 3 + 1, 0);
  for (const ElementoVertice& elemento : e.elementos) fetch[elemento.instruccion] = 1;
  for (size_t i = 0; i < subido.size(); i += 3) {
    const size_t n = std::min<size_t>(3, subido.size() - i);
    const bool fetch_sin_declarar = n == 3 && !fetch[i / 3] && (original[i] & 0x1F) == 0 && ((original[i] >> 19) & 1) &&
                                    (subido[i] & kFetchConservaTolerante[0]) == (original[i] & kFetchConservaTolerante[0]);
    bool igual = true;
    for (size_t j = 0; j < n; ++j) {
      uint32_t a = subido[i + j], b = original[i + j];
      if (fetch[i / 3] || fetch_sin_declarar) {
        a &= kFetchConservaTolerante[j];
        b &= kFetchConservaTolerante[j];
      }
      igual = igual && a == b;
    }
    if (igual) continue;
    if (e.vertices && n == 3 && subido[i] == kInstruccionNula[0] && subido[i + 1] == kInstruccionNula[1] &&
        subido[i + 2] == kInstruccionNula[2]) {
      continue;
    }
    return false;
  }
  return true;
}

const char* Leer(const nfsmw::native::Shader& shader, EntradaShader& e) {
  const Contenedor c{shader.original};
  if (!c.Hay(0, 36)) return "contenedor demasiado corto";
  // NFSC: Carbon's containers are the 2008 layout (signature 0x102A11xx): the shader header offset is at +24
  // (not +20) and the vertex elements follow the 0x24-byte vertex header directly. The rest is the same.
  const bool es2008 = (c.U32(0) & 0xFFFFFFFEu) == 0x102A1100u;
  const uint32_t virtuales = c.U32(4);
  const uint32_t fisicos = c.U32(8);
  const uint32_t tabla = c.U32(16);
  const uint32_t cabecera = c.U32(es2008 ? 24 : 20);
  if (!fisicos || (fisicos % 4) || !c.Hay(virtuales, fisicos)) {
    return "microcodigo fuera del contenedor";
  }
  if (cabecera >= virtuales || size_t(cabecera) + (shader.vertices ? 40 : 32) > virtuales) {
    return "cabecera del shader fuera de la parte virtual";
  }

  e.vertices = shader.vertices;
  // FH1: in its 2008 containers the physical part also holds other data; the shader header says where the
  // microcode is (+0 offset in the physical part, +4 size in bytes). NFSC's containers start with it.
  size_t inicio_codigo = 0, bytes_codigo = fisicos;
  if (es2008) {
    inicio_codigo = c.U32(cabecera);
    bytes_codigo = c.U32(cabecera + 4);
    if (!bytes_codigo || (bytes_codigo % 4) || inicio_codigo + bytes_codigo > fisicos) {
      return "microcodigo fuera de la parte fisica (cabecera 2008)";
    }
  }
  e.microcodigo.resize(bytes_codigo / 4);
  for (size_t i = 0; i < e.microcodigo.size(); ++i) {
    e.microcodigo[i] = c.U32(size_t(virtuales) + inicio_codigo + i * 4);
  }
  e.microcodigo_shader = e.microcodigo;

  if (e.vertices) {
    // List at +0x28, after skipping the words at +0x18; +0x1C = element count.
    const uint32_t previos = c.U32(cabecera + 24);  // field18: words before the element list (both layouts)
    const uint32_t cantidad = c.U32(cabecera + 28);
    const size_t comienzo = size_t(cabecera) + (es2008 ? 36 : 40) + size_t(previos) * 4;
    if (cantidad > 64 || previos > 1024 || comienzo + size_t(cantidad) * 4 > virtuales) {
      return "elementos de vertices fuera de la parte virtual";
    }
    for (uint32_t i = 0; i < cantidad; ++i) {
      const uint32_t valor = c.U32(comienzo + size_t(i) * 4);
      ElementoVertice elemento;
      elemento.instruccion = uint16_t(valor & 0xFFF);
      elemento.uso = uint8_t((valor >> 12) & 0xF);
      elemento.indice_uso = uint8_t((valor >> 16) & 0xF);
      if ((size_t(elemento.instruccion) + 1) * 3 > e.microcodigo.size()) {
        return "elemento de vertices apunta fuera del microcodigo";
      }
      e.elementos.push_back(elemento);
      for (size_t j = 0; j < 3; ++j) {
        e.microcodigo[size_t(elemento.instruccion) * 3 + j] &= kFetchConserva[j];
      }
    }
  } else {
    if (!c.Hay(cabecera + 24, 8)) return "cabecera de pixel shader corta";
    e.salidas = c.U32(cabecera + 28);
  }

  // Constant table: only the samplers (register and type).
  if (!tabla || !c.Hay(tabla + 4, 28)) return "sin tabla de constantes";
  const size_t base = size_t(tabla) + 4;
  const uint32_t constantes = c.U32(base + 12);
  const uint32_t info = c.U32(base + 16);
  if (constantes > 1024 || !c.Hay(base + info, size_t(constantes) * 20)) {
    return "tabla de constantes fuera del contenedor";
  }
  // Float registers the SPIR-V reads: the highest in the table, or up to the end of the
  // buffer if there is an array with relative indexing (XenosRecomp shader_recompiler.cpp:
  // 1172-1183: tailCount = 256 in VS and 224 in PS).
  uint32_t registros_float = 0;
  for (uint32_t i = 0; i < constantes; ++i) {
    const size_t p = base + info + size_t(i) * 20;
    if (c.U16(p + 4) == 2) {  // RegisterSet::Float4
      const uint32_t indice = c.U16(p + 6);
      const uint32_t cuantos = c.U16(p + 8);
      registros_float = std::max(registros_float, cuantos > 1 ? 256u
                                                              : indice + 1);
    }
  }
  e.constantes_bytes = std::min<uint32_t>(std::max<uint32_t>(registros_float, 1), 256) * 16;
  for (uint32_t i = 0; i < constantes; ++i) {
    const size_t p = base + info + size_t(i) * 20;
    if (c.U16(p + 4) != 3) continue;  // RegisterSet::Sampler
    SamplerShader sampler;
    sampler.registro = c.U16(p + 6);
    const uint32_t tipo = c.U32(p + 12);
    sampler.tipo = c.Hay(base + tipo, 4) ? c.U16(base + tipo + 2) : 0;
    // nfsmw_nativo_sombra_minimo. The name is at that distance from the start of the table, like the type
    // (XenosRecomp shader_recompiler.cpp: constantTableData + constantInfo->name).
    sampler.mapa_sombras = NombreEs(c, base + size_t(c.U32(p)), "SHADOWMAP_SAMPLER");
    e.samplers.push_back(sampler);
  }

  e.huella = XXH3_64bits(e.microcodigo.data(), e.microcodigo.size() * sizeof(uint32_t));
  e.microcodigo_comparar = e.microcodigo;
  for (const ElementoVertice& elemento : e.elementos) {
    e.microcodigo_comparar[size_t(elemento.instruccion) * 3 + 1] &= ~uint32_t(0xFFF);
  }
  {
    std::vector<uint32_t> ranuras;
    for (const ElementoVertice& elemento : e.elementos) ranuras.push_back(elemento.instruccion);
    if (!ranuras.empty()) CanonicalizarFetches(e.microcodigo_comparar, ranuras);
  }
  e.huella_comparar = XXH3_64bits(e.microcodigo_comparar.data(), e.microcodigo_comparar.size() * sizeof(uint32_t));
  return nullptr;
}

struct ClaveCruda {
  uint64_t huella;
  uint32_t palabras;
  bool vertices;
  bool operator==(const ClaveCruda&) const = default;
};

struct HashClaveCruda {
  size_t operator()(const ClaveCruda& c) const {
    return size_t(c.huella ^ (uint64_t(c.palabras) << 1) ^ uint64_t(c.vertices));
  }
};

}  // namespace

struct ShadersNativos::Datos {
  nfsmw::native::BibliotecaShaders biblioteca;
  std::vector<EntradaShader> entradas;
  std::unordered_map<const nfsmw::native::Shader*, uint32_t> por_shader;
  // (vertices, palabras) -> entradas candidatas.
  std::map<std::pair<bool, uint32_t>, std::vector<uint32_t>> candidatos;
  std::unordered_map<ClaveCruda, const EntradaShader*, HashClaveCruda> cache;
  std::vector<uint32_t> temporal;
  EstadisticasShaders estadisticas;
  uint32_t avisos = 0;
  bool cargada = false;
};

ShadersNativos::ShadersNativos() : datos_(std::make_unique<Datos>()) {}
ShadersNativos::~ShadersNativos() = default;

bool ShadersNativos::cargada() const {
  return datos_->cargada;
}

bool FetchCoherentes(const EntradaShader& vs, std::span<const uint32_t> parcheado) {
  if (!vs.vertices || parcheado.size() != vs.microcodigo.size()) {
    return false;
  }
  for (const ElementoVertice& elemento : vs.elementos) {
    const uint32_t registro = (vs.microcodigo[size_t(elemento.instruccion) * 3] >> 12) & 0x3F;
    bool encontrado = false;
    for (const ElementoVertice& otro : vs.elementos) {
      const uint32_t d0 = parcheado[size_t(otro.instruccion) * 3];
      if (((d0 >> 12) & 0x3F) == registro && (d0 & 0x1F) == 0) {
        encontrado = true;
        break;
      }
    }
    if (!encontrado) {
      return false;
    }
  }
  return true;
}

// nfsmw_nativo_sombra_minimo. The library has tfetch2DSombraMin if the pixel shader's SPIR-V contains the
// NFSMW_MARCA_SOMBRA_MINIMO constant from shader_common.h (a 32-bit OpConstant): nothing else uses it.
constexpr uint32_t kMarcaSombraMinimo = 0x5E3B1A84u;
static bool TieneMarcaSombraMinimo(const std::vector<uint32_t>& spirv) {
  if (spirv.size() < 5 || spirv[0] != 0x07230203u) {
    return false;
  }
  for (size_t i = 5; i < spirv.size();) {
    const uint32_t palabras = spirv[i] >> 16;
    if (palabras == 0 || i + palabras > spirv.size()) {
      return false;
    }
    if ((spirv[i] & 0xFFFF) == 43 && palabras == 4 && spirv[i + 3] == kMarcaSombraMinimo) {
      return true;
    }
    i += palabras;
  }
  return false;
}

// OpKill, OpTerminateInvocation and OpDemoteToHelperInvocation in the module. If the SPIR-V cannot be walked
// it returns 2, the conservative answer (the fragment stage is kept).
static uint32_t ContarKills(const std::vector<uint32_t>& spirv) {
  if (spirv.size() < 5 || spirv[0] != 0x07230203u) {
    return 2;
  }
  uint32_t kills = 0;
  for (size_t i = 5; i < spirv.size();) {
    const uint32_t palabras = spirv[i] >> 16;
    const uint32_t codigo = spirv[i] & 0xFFFF;
    if (palabras == 0 || i + palabras > spirv.size()) {
      return 2;
    }
    if (codigo == 252 || codigo == 4416 || codigo == 5380) {
      ++kills;
    }
    i += palabras;
  }
  return kills;
}

bool ShadersNativos::Cargar(const std::filesystem::path& archivo) {
  Datos& d = *datos_;
  try {
    d.biblioteca.Cargar(archivo);
  } catch (const std::exception& e) {
    REXLOG_WARN("[nativo] C5a: biblioteca de shaders no disponible ({}): {}", archivo.string(),
                e.what());
    return false;
  }
  const auto& shaders = d.biblioteca.shaders();
  uint32_t con_kill = 0, sin_kill = 0;
  uint32_t con_mapa_sombras = 0, con_minimo = 0;  // nfsmw_nativo_sombra_minimo
  d.entradas.clear();
  d.entradas.reserve(shaders.size());
  uint32_t vertex = 0, pixel = 0;
  for (uint32_t i = 0; i < shaders.size(); ++i) {
    EntradaShader e;
    e.shader = &shaders[i];
    e.numero = i;
    if (const char* motivo = Leer(shaders[i], e)) {
      REXLOG_WARN("[nativo] C5a: contenedor {} de la biblioteca ignorado: {}", i, motivo);
      continue;
    }
    if (!e.vertices) {
      e.kills = ContarKills(shaders[i].spirv);
      e.descarta = e.kills != 1;                // 1 = only the alpha test one
      (e.descarta ? con_kill : sin_kill) += 1;
      // nfsmw_nativo_sombra_minimo. Whether its SPIR-V can take the shadow map minimum.
      e.sombra_minimo = TieneMarcaSombraMinimo(shaders[i].spirv);
      for (const SamplerShader& s : e.samplers) {
        if (s.mapa_sombras) {
          ++con_mapa_sombras;
          con_minimo += e.sombra_minimo ? 1 : 0;
          break;
        }
      }
    }
    (e.vertices ? vertex : pixel) += 1;
    d.entradas.push_back(std::move(e));
  }
  d.candidatos.clear();
  d.por_shader.clear();
  std::map<std::tuple<bool, uint32_t, uint64_t>, std::vector<uint32_t>> grupos;
  for (uint32_t i = 0; i < d.entradas.size(); ++i) {
    const EntradaShader& e = d.entradas[i];
    const uint32_t palabras = uint32_t(e.microcodigo.size());
    d.candidatos[{e.vertices, palabras}].push_back(i);
    d.por_shader[e.shader] = i;
    grupos[{e.vertices, palabras, e.huella}].push_back(i);
  }
  uint32_t repetidos = 0, repetidos_distintos = 0;
  for (const auto& [clave, miembros] : grupos) {
    if (miembros.size() < 2) continue;
    ++repetidos;
    const auto& spirv = d.entradas[miembros[0]].shader->spirv;
    for (uint32_t m : miembros) {
      if (d.entradas[m].shader->spirv != spirv) {
        ++repetidos_distintos;
        break;
      }
    }
  }
  d.cache.clear();
  d.cargada = !d.entradas.empty();
  REXLOG_INFO("[nativo] C5a: biblioteca con {} shaders ({} vertex, {} pixel); {} grupos con el "
              "mismo microcodigo, {} de ellos con SPIR-V distinto",
              d.entradas.size(), vertex, pixel, repetidos, repetidos_distintos);
  REXLOG_INFO("[nativo] C5a: pixel shaders que pueden descartar pixeles {} de {} (el resto solo llevan el "
              "kill de la prueba de alfa: sin color que escribir, su etapa se puede quitar)",
              con_kill, con_kill + sin_kill);
  REXLOG_INFO("[nativo] C5a: sombra por minimo (build 184): {} pixel shaders muestrean el mapa de sombras "
              "(SHADOWMAP_SAMPLER) y {} traen tfetch2DSombraMin{}",
              con_mapa_sombras, con_minimo,
              con_minimo && con_minimo == con_mapa_sombras
                  ? " (biblioteca con el minimo)"
                  : " (sin el minimo: el mapa de sombras se sigue copiando como siempre)");
  return d.cargada;
}

const EntradaShader* ShadersNativos::Identificar(bool vertices,
                                                 std::span<const uint32_t> microcodigo) {
  Datos& d = *datos_;
  ++d.estadisticas.cargas;
  const ClaveCruda clave{XXH3_64bits(microcodigo.data(), microcodigo.size_bytes()),
                         uint32_t(microcodigo.size()), vertices};
  if (auto it = d.cache.find(clave); it != d.cache.end()) {
    return it->second;
  }
  ++d.estadisticas.distintos;

  const EntradaShader* elegido = nullptr;
  uint32_t coincidencias = 0;
  if (auto c = d.candidatos.find({vertices, uint32_t(microcodigo.size())});
      c != d.candidatos.end()) {
    for (uint32_t indice : c->second) {
      const EntradaShader& e = d.entradas[indice];
      d.temporal.assign(microcodigo.begin(), microcodigo.end());
      for (const ElementoVertice& elemento : e.elementos) {
        for (size_t j = 0; j < 3; ++j) {
          d.temporal[size_t(elemento.instruccion) * 3 + j] &= kFetchConserva[j];
        }
      }
      // NFSC: ignore the destination swizzle D3D patches into the fetches (see EntradaShader::huella_comparar).
      for (const ElementoVertice& elemento : e.elementos) {
        d.temporal[size_t(elemento.instruccion) * 3 + 1] &= ~uint32_t(0xFFF);
      }
      if (!e.elementos.empty()) {
        std::vector<uint32_t> ranuras;
        for (const ElementoVertice& elemento : e.elementos) ranuras.push_back(elemento.instruccion);
        CanonicalizarFetches(d.temporal, ranuras);
      }
      if (XXH3_64bits(d.temporal.data(), d.temporal.size() * sizeof(uint32_t)) != e.huella_comparar ||
          d.temporal != e.microcodigo_comparar) {
        continue;
      }
      if (!elegido) {
        elegido = &e;
      }
      ++coincidencias;
    }
  }

  // FH1: tolerant pass (CoincideFh1) when the exact one finds nothing.
  if (!elegido) {
    if (auto c = d.candidatos.find({vertices, uint32_t(microcodigo.size())}); c != d.candidatos.end()) {
      for (uint32_t indice : c->second) {
        const EntradaShader& e = d.entradas[indice];
        if (!CoincideFh1(e, microcodigo)) continue;
        if (!elegido) elegido = &e;
        ++coincidencias;
      }
      if (elegido) ++d.estadisticas.tolerantes;
    }
  }

  // Vertex shaders arrive patched: a mismatch is normal.
  const bool avisar = d.avisos < kMaxAvisos;  // NFSC: also vertex shaders (diagnostic, to see how Carbon patches them)
  if (elegido) {
    ++d.estadisticas.identificados;
    if (coincidencias > 1) {
      ++d.estadisticas.ambiguos;
    }
    if (avisar && vertices) {
      // What D3D has patched in each fetch: it becomes the vertex input.
      ++d.avisos;
      std::string detalle;
      for (const ElementoVertice& elemento : elegido->elementos) {
        const size_t p = size_t(elemento.instruccion) * 3;
        const uint32_t d0 = microcodigo[p], d1 = microcodigo[p + 1], d2 = microcodigo[p + 2];
        const uint32_t fetch = ((d0 >> 20) & 0x1F) * 3 + ((d0 >> 25) & 0x3);
        const int32_t offset = int32_t(d2 << 1) >> 9;
        detalle += fmt::format(" {}{}:f{}/fmt{}/z{}/o{}{}", kUsos[elemento.uso & 0xF],
                               elemento.indice_uso, fetch, (d1 >> 16) & 0x3F, d2 & 0xFF, offset,
                               ((d1 >> 30) & 0x1) ? "/mini" : "");
      }
      REXLOG_INFO("[nativo] C5a: VS n{} ({} palabras, {} coincidencias):{}", elegido->numero,
                  microcodigo.size(), coincidencias, detalle);
    }
  } else {
    ++d.estadisticas.sin_identificar;
    if (avisar) {
      ++d.avisos;
      const auto c = d.candidatos.find({vertices, uint32_t(microcodigo.size())});
      REXLOG_WARN("[nativo] C5a: {} shader sin identificar: {} palabras, huella {:016X} "
                  "({} contenedores de ese tipo y longitud)",
                  vertices ? "vertex" : "pixel", microcodigo.size(), clave.huella,
                  c != d.candidatos.end() ? c->second.size() : 0);
      // Diagnostics: which words change against the containers of the same
      // length (incoming / container original, without the mask).
      for (size_t k = 0; c != d.candidatos.end() && k < c->second.size() && k < 2; ++k) {
        const EntradaShader& e = d.entradas[c->second[k]];
        const auto& o = e.shader->original;
        const size_t virtuales = size_t(o[4]) << 24 | size_t(o[5]) << 16 | size_t(o[6]) << 8 | o[7];
        std::string diferencias;
        uint32_t cuantas = 0;
        for (size_t i = 0; i < microcodigo.size(); ++i) {
          uint32_t entrante = microcodigo[i];
          for (const ElementoVertice& elemento : e.elementos) {
            const size_t p = size_t(elemento.instruccion) * 3;
            if (i >= p && i < p + 3) {
              entrante &= kFetchConserva[i - p];
            }
          }
          if (entrante == e.microcodigo[i]) {
            continue;
          }
          if (++cuantas <= 24) {
            (void)virtuales;
            const uint32_t original = e.microcodigo_shader[i];  // FH1: the microcode may not start the physical part
            diferencias += fmt::format(" {}:{:08X}/{:08X}", i, microcodigo[i], original);
          }
        }
        std::string fetch;
        for (const ElementoVertice& elemento : e.elementos) {
          fetch += fmt::format(" {}", elemento.instruccion);
        }
        REXLOG_WARN("[nativo] C5a:   frente a n{}: {} palabras distintas; fetch en las "
                    "instrucciones{}:{}",
                    e.numero, cuantas, fetch, diferencias);
      }
    }
  }
  if (!elegido) {
    VolcarMicrocodigoDesconocido(vertices, clave.huella, microcodigo);
  }
  d.cache.emplace(clave, elegido);
  return elegido;
}

const EntradaShader* ShadersNativos::IdentificarContenedor(
    std::span<const uint8_t> contenedor) const {
  const Datos& d = *datos_;
  if (!d.cargada) {
    return nullptr;
  }
  const nfsmw::native::Shader* shader = d.biblioteca.Buscar(contenedor);
  if (!shader) {
    return nullptr;
  }
  const auto it = d.por_shader.find(shader);
  return it != d.por_shader.end() ? &d.entradas[it->second] : nullptr;
}

// Entries are in library order (Cargar), with gaps where a container was skipped.
const EntradaShader* ShadersNativos::PorNumero(uint32_t numero) const {
  const Datos& d = *datos_;
  if (!d.cargada) {
    return nullptr;
  }
  const auto it = std::lower_bound(d.entradas.begin(), d.entradas.end(), numero,
                                   [](const EntradaShader& e, uint32_t n) { return e.numero < n; });
  return it != d.entradas.end() && it->numero == numero ? &*it : nullptr;
}

const char* NombreUso(uint8_t uso) {
  return kUsos[uso & 0xF];
}

EstadisticasShaders ShadersNativos::Estadisticas() const {
  return datos_->estadisticas;
}

}  // namespace nfsmw::nativo
