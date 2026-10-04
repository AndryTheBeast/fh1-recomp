// Regression test for the format and the local corpus. It contains no game data.
#include "nfsmw_container_2005.h"
#include "XenosRecomp/shader_code.h"
#include <filesystem>
#include <fstream>
#include <iostream>
#include <set>

static std::vector<uint8_t> Read(const std::filesystem::path& p) {
  std::ifstream f(p, std::ios::binary);
  return {std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>()};
}
static void Require(bool input_value, const char* message) {
  if (!input_value) throw std::runtime_error(message);
}
static void Reject(const std::vector<uint8_t>& data) {
  try { nfsmw::Flow f; nfsmw::Convert2005(data, f); }
  catch (const std::runtime_error&) { return; }
  throw std::runtime_error("an invalid input was accepted");
}
static void Set(std::vector<uint8_t>& d, size_t o, uint32_t v) {
  d[o] = v >> 24; d[o+1] = v >> 16; d[o+2] = v >> 8; d[o+3] = v;
}

int main(int argc, char** argv) try {
  if (argc != 3) throw std::runtime_error("use: nfsmw_test_container <disco> <dumps anteriores>");
  std::vector<std::vector<uint8_t>> corpus;
  std::set<uint32_t> operations;
  size_t pixels = 0, vertices = 0, discard = 0;
  for (const auto& e : std::filesystem::directory_iterator(argv[1])) {
    if (e.path().extension() != ".bin") continue;
    auto data = Read(e.path());
    const nfsmw::Reader l{data};
    nfsmw::Flow flow;
    auto output = nfsmw::Convert2005(data, flow);
    const nfsmw::Reader normal{output};
    Require(std::equal(data.begin()+l.u32(4), data.end(), output.begin()+normal.u32(4)), "microcode alterado al normalize");
    uint32_t exports = 0;
    bool kills = false;
    for (uint32_t i = 0; i < flow.bytes; i += 12) {
      const uint32_t o = l.u32(4) + i;
      const uint64_t cf[] = {uint64_t(l.u32(o)) | uint64_t(l.u32(o+4) & 65535) << 32,
                             uint64_t(l.u32(o+4) >> 16) | uint64_t(l.u32(o+8)) << 16};
      for (uint64_t c : cf) {
        const uint32_t op = (c >> 44) & 15;
        operations.insert(op);
        if (!((op >= 1 && op <= 6) || op == 13 || op == 14)) continue;
        uint32_t sequence = (c >> 16) & 4095;
        for (uint32_t j = 0; j < ((c >> 12) & 7); ++j, sequence >>= 2) {
          if (sequence & 1) continue;
          const uint32_t a = l.u32(4) + ((c & 4095) + j)*12;
          const uint32_t words[] = {l.u32(a), l.u32(a+4), l.u32(a+8)};
          AluInstruction alu;
          static_assert(sizeof(alu) == sizeof(words));
          std::memcpy(&alu, words, sizeof(alu));
          kills |= (alu.vectorOpcode >= AluVectorOpcode::KillEq && alu.vectorOpcode <= AluVectorOpcode::KillNe);
          kills |= (alu.scalarOpcode >= AluScalarOpcode::KillsEq && alu.scalarOpcode <= AluScalarOpcode::KillsOne);
          if (alu.exportData) {
            if (alu.vectorDest < 4) exports |= 1u << alu.vectorDest;
            else if (alu.vectorDest == 61) exports |= 16;
          }
        }
      }
    }
    if (!(l.u32(0) & 1)) {
      ++pixels;
      const uint32_t mask = l.u32(l.u32(20) + 28);
      Require((mask & 15) == (exports & 15), "color outputs different from the microcode");
      Require(!(exports & 16), "the corpus now contains depth: review its format");
      Require(bool(mask & 32) == kills, "bit 5 does not match the discard instructions");
      discard += kills;
    } else ++vertices;
    // Truncated headers, overflowing limits and nonexistent CF.
    for (size_t n = 0; n < 24; ++n) Reject({data.begin(), data.begin()+n});
    auto corrupt = data;
    Set(corrupt, 20, 0xFFFFFFF0); Reject(corrupt);
    corrupt = data; Set(corrupt, 16, 0xFFFFFFF0); Reject(corrupt);
    corrupt = data; Set(corrupt, 4, 0xFFFFFFF0); Reject(corrupt);
    corrupt = data; std::fill(corrupt.begin()+l.u32(4), corrupt.end(), 0); Reject(corrupt);
    corpus.push_back(std::move(data));
  }
  size_t rejected = 0, matches = 0;
  for (const auto& e : std::filesystem::directory_iterator(argv[2])) {
    if (e.path().extension() != ".bin") continue;
    const auto data = Read(e.path());
    Reject(data); ++rejected;
    const nfsmw::Reader l{data};
    const uint32_t ct = l.u32(16), sh = l.u32(20);
    bool existe = false;
    for (const auto& original : corpus) {
      const nfsmw::Reader r{original};
      if (l.u32(0) != r.u32(0) || l.u32(4) != r.u32(4) || l.u32(8) != r.u32(8) || ct != r.u32(16) || sh != r.u32(20)) continue;
      // CTAB is immutable; the live object also contains working fields.
      existe |= std::equal(data.begin()+ct, data.begin()+sh, original.begin()+ct);
    }
    if (!existe) std::cout << "No equivalent CTAB on disk: " << e.path().filename().string() << '\n';
    matches += existe;
  }
  Require(!corpus.empty() && rejected != 0, "corpus empty");
  std::cout << corpus.size() << " containers: " << vertices << " vertices, " << pixels << " pixels; "
            << discard << " with discard; " << rejected << " wrong dumps rejected; "
            << matches << " CTAB recuperadas.\nCF presentes:";
  for (auto op : operations) std::cout << ' ' << op;
  std::cout << '\n';
  return 0;
} catch (const std::exception& e) {
  std::cerr << "error: " << e.what() << '\n';
  return 1;
}
