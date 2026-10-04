/**
 * @file    nfsmw_hlsl.cpp
 * @brief   Minimal Xbox 360 shader translator to HLSL, for NFS Most Wanted
 *
 * Why XenosRecomp's main.cpp is not used
 *
 * The original does three things: translate to HLSL, compile to DXIL/SPIR-V with the
 * DXC API, and package the result with smol-v and zstd. Of those three, only the
 * first is needed here:
 *
 *  - smol-v and zstd are submodules that are not checked out, and they only compress
 *    the output.
 *  - The DXC API requires the library (dxcompiler), but the Vulkan SDK ships the
 *    dxc.exe executable, which does exactly the same from the command line.
 *
 * So this translates to HLSL and nothing more. dxc.exe does the step to SPIR-V.
 *
 * The other difference, and the important one
 *
 * XenosRecomp assumes the shader container carries the signature 0x102A1100, which is
 * Sonic Unleashed's (2008). NFS Most Wanted is from 2005 and uses 0x102A0E00: an
 * earlier version of Microsoft's shader compiler. nfsmw_container_2005.h normalizes the
 * 2005 tables and validates the control flow.
 *
 * Input
 *
 * Original containers extracted from ZZDATA0.BIN with nfsmw_find_containers. Old
 * console dumps lack the correct physical part; they are kept for regression and
 * rejected.
 *
 * Usage
 *   nfsmw_hlsl <input folder> <output folder> <shader_common.h>
 */

#include "XenosRecomp/shader_recompiler.h"
#include "nfsmw_container_2005.h"

#include <cstdio>
#include <filesystem>
#include <string>
#include <vector>
#include <cstring>

namespace {

std::vector<uint8_t> ReadAll(const std::filesystem::path& path) {
  std::vector<uint8_t> data;
  FILE* f = std::fopen(path.string().c_str(), "rb");
  if (!f) {
    return data;
  }
  std::fseek(f, 0, SEEK_END);
  const long n = std::ftell(f);
  std::fseek(f, 0, SEEK_SET);
  if (n > 0) {
    data.resize(static_cast<size_t>(n));
    if (std::fread(data.data(), 1, data.size(), f) != data.size()) {
      data.clear();
    }
  }
  std::fclose(f);
  return data;
}

/*
 * The two known signatures: the 2008 one XenosRecomp expects and the 2005 one Most
 * Wanted uses. They are compared without the low byte, which belongs to the tool itself.
 */
bool SignatureValid(uint32_t flags_be) {
  const uint32_t flags = __builtin_bswap32(flags_be);
  const uint32_t v = flags & 0xFFFFFF00u;
  return v == 0x102A1100u || v == 0x102A0E00u;
}


}  // namespace

int main(int argc, char** argv) try {
  if (argc < 4) {
    std::printf("use: nfsmw_hlsl <entry> <output> <shader_common.h>\n");
    return 1;
  }
  const std::filesystem::path entry = argv[1];
  const std::filesystem::path output = argv[2];

  const std::vector<uint8_t> common = ReadAll(argv[3]);
  if (common.empty()) {
    std::printf("no pude read %s\n", argv[3]);
    return 1;
  }
  const std::string_view include(reinterpret_cast<const char*>(common.data()), common.size());

  std::error_code ec;
  if (std::filesystem::exists(output) && !std::filesystem::is_empty(output)) {
    std::fprintf(stderr, "the output must be empty so earlier results are not mixed in\n");
    return 1;
  }
  std::filesystem::create_directories(output, ec);
  if (ec) throw std::runtime_error("could not create the output folder");

  size_t total = 0, ok = 0, skipped = 0;
  for (const auto& e : std::filesystem::directory_iterator(entry)) {
    if (!e.is_regular_file() || e.path().extension() != ".bin") {
      continue;
    }
    const std::vector<uint8_t> data = ReadAll(e.path());
    ++total;
    if (data.size() < 24) {
      std::printf("  %s: file ilegible o header truncada\n", e.path().filename().string().c_str());
      ++skipped;
      continue;
    }

    uint32_t flags_be = 0;
    std::memcpy(&flags_be, data.data(), 4);
    if (!SignatureValid(flags_be)) {
      std::printf("  %-20s signature unknown 0x%08X\n", e.path().filename().string().c_str(),
                  __builtin_bswap32(flags_be));
      ++skipped;
      continue;
    }

    /* The 2005 one is converted to the 2008 form before translating. */
    const bool es2005 = (__builtin_bswap32(flags_be) & 0xFFFFFF00u) == 0x102A0E00u;
    nfsmw::Flow flow;
    std::vector<uint8_t> conv;
    try {
      conv = es2005 ? nfsmw::Convert2005(data, flow) : data;
    } catch (const std::exception& error) {
      std::printf("  %s: %s\n", e.path().filename().string().c_str(), error.what());
      ++skipped;
      continue;
    }
    if (conv.empty()) {
      std::printf("  %-20s could not convert the container\n", e.path().filename().string().c_str());
      ++skipped;
      continue;
    }

    std::printf("  %s: CF %u bytes, %u instructions\n", e.path().filename().string().c_str(), flow.bytes, flow.instructions);
    std::fflush(stdout);
    ShaderRecompiler recompiler;
    try {
      recompiler.recompile(conv.data(), include);
    } catch (const std::exception& error) {
      std::printf("  %s: translation rechazada: %s\n", e.path().filename().string().c_str(), error.what());
      ++skipped;
      continue;
    }
    if (recompiler.out.empty()) {
      std::printf("  %-20s no produced nothing\n", e.path().filename().string().c_str());
      ++skipped;
      continue;
    }

    auto target = output / e.path().filename();
    target.replace_extension(".hlsl");
    FILE* f = std::fopen(target.string().c_str(), "wb");
    if (f) {
      const bool complete = std::fwrite(recompiler.out.data(), 1, recompiler.out.size(), f) == recompiler.out.size();
      const bool closed = std::fclose(f) == 0;
      if (complete && closed) ++ok;
      else ++skipped;
    } else {
      ++skipped;
    }
  }

  std::printf("\n%zu shaders: %zu traducidos, %zu skipped\n", total, ok, skipped);
  return total > 0 && ok == total ? 0 : 2;
} catch (const std::exception& error) {
  std::fprintf(stderr, "error: %s\n", error.what());
  return 1;
}
