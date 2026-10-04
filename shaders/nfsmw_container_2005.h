#pragma once

// Adapter for the 2005 XDK container. It does not use the shader tag as an
// offset or as a length: the microcode starts at virtualSize.
#include <algorithm>
#include <cstdint>
#include <cstring>
#include <stdexcept>
#include <string>
#include <vector>

namespace nfsmw {
struct Reader {
  const std::vector<uint8_t>& data;
  void range(size_t o, size_t n) const {
    if (o > data.size() || n > data.size() - o)
      throw std::runtime_error("truncated container or offset outside the file");
  }
  uint32_t u32(size_t o) const {
    range(o, 4);
    return uint32_t(data[o]) << 24 | uint32_t(data[o+1]) << 16 |
           uint32_t(data[o+2]) << 8 | data[o+3];
  }
};

struct Flow {
  uint32_t bytes = 0;
  uint32_t instructions = 0;
};

// The CF limit is deduced from the EXEC addresses, as in Xenia. Each interval
// is also checked before the translator accesses its operands.
inline Flow ValidateMicrocode(const Reader& l, uint32_t start, uint32_t length) {
  l.range(start, length);
  if (!length || length % 12) throw std::runtime_error("invalid microcode length");
  const uint32_t blocks = length / 12;
  uint32_t limit = blocks, executed = 0;
  bool ends = false;
  for (uint32_t i = 0; i < limit; ++i) {
    const uint32_t a = l.u32(start + i*12), b = l.u32(start + i*12+4), c = l.u32(start + i*12+8);
    const uint64_t cf[] = {uint64_t(a) | uint64_t(b & 65535) << 32,
                           uint64_t(b >> 16) | uint64_t(c) << 16};
    for (uint64_t word : cf) {
      const uint32_t op = (word >> 44) & 15;
      if ((op >= 1 && op <= 6) || op == 13 || op == 14) {
        const uint32_t address = word & 4095, amount = (word >> 12) & 7;
        if (!address || amount > 6 || address >= blocks || amount > blocks - address || address <= i)
          throw std::runtime_error("EXEC outside the microcode; possibly a wrong dump of contiguous memory");
        limit = std::min(limit, address);
        executed += amount;
        ends |= op == 2 || op == 4 || op == 6 || op == 14;
      }
    }
  }
  if (!executed || !ends)
    throw std::runtime_error("no useful EXEC and end; an empty HLSL is not accepted as a translation");
  return {limit * 12, executed};
}

inline std::vector<uint8_t> Convert2005(const std::vector<uint8_t>& entry, Flow& flow) {
  const Reader l{entry};
  l.range(0, 24);
  const uint32_t signature = l.u32(0), vs = l.u32(4), ps = l.u32(8);
  if ((signature & 0xFFFFFFFEu) != 0x102A0E00u || vs < 24 || uint64_t(vs) + ps != entry.size())
    throw std::runtime_error("invalid 2005 header");
  const bool pixel = !(signature & 1);
  const uint32_t def = l.u32(12), ct = l.u32(16), sh = l.u32(20);
  const auto virtualRange = [&](size_t o, size_t n) {
    if (o > vs || n > vs - o) throw std::runtime_error("table outside the virtual part");
  };
  virtualRange(sh, pixel ? 32 : 40);
  virtualRange(ct, 32);
  // The CTAB table keeps the same format and offsets relative to ct+4.
  const uint32_t baseCt = ct + 4, constants = l.u32(baseCt + 12);
  virtualRange(size_t(baseCt) + l.u32(baseCt + 16), size_t(constants)*20);
  for (uint32_t i = 0; i < constants; ++i) {
    const size_t info = size_t(baseCt) + l.u32(baseCt + 16) + i*20;
    const size_t name = size_t(baseCt) + l.u32(info);
    virtualRange(name, 1);
    if (!std::memchr(entry.data() + name, 0, vs - name)) throw std::runtime_error("name without a terminator");
    virtualRange(size_t(baseCt) + l.u32(info + 12), 16);
  }
  flow = ValidateMicrocode(l, vs, ps);

  // The original table is kept and normalized structures are appended. Its
  // internal offsets are relative and do not change when it is shifted by twelve bytes.
  std::vector<uint8_t> s(vs + 12, 0), physical(entry.begin() + vs, entry.end());
  std::copy(entry.begin()+24, entry.begin()+vs, s.begin()+36);
  const auto set = [&](size_t o, uint32_t v) {
    if (o + 4 > s.size()) s.resize(o + 4);
    s[o] = v >> 24; s[o+1] = v >> 16; s[o+2] = v >> 8; s[o+3] = v;
  };
  const auto append = [&](uint32_t v) { const size_t o = s.size(); set(o, v); };
  set(0, 0x102A1100u | (signature & 1));
  set(16, ct + 12);
  set(24, s.size());
  const uint32_t interpolators = (l.u32(sh + 16) >> 5) & 31;
  append(0);                 // physicalOffset: CF starts at the first byte.
  append(flow.bytes);       // Length checked against the EXEC addresses.
  append(0);
  append(l.u32(sh + 16));
  append(0);
  append(interpolators << 5);
  if (pixel) {
    virtualRange(sh + 32, size_t(interpolators)*4);
    const uint32_t outputs = l.u32(sh + 28);
    if (outputs & ~0x2Fu) throw std::runtime_error("unknown 2005 output mask");
    append(0);
    // In this corpus bit 5 matches KILL, not a depth output.
    // The test checks the outputs against the microcode exports.
    append(outputs & 15);
    for (uint32_t i = 0; i < interpolators; ++i) append(l.u32(sh + 32 + i*4));
  } else {
    const uint32_t previous = l.u32(sh + 24), elements = l.u32(sh + 28);
    const size_t start_offset = size_t(sh) + 40 + size_t(previous)*4;
    virtualRange(start_offset, (size_t(elements) + interpolators)*4);
    append(0); append(elements); append(0);
    for (uint32_t i = 0; i < elements + interpolators; ++i) append(l.u32(start_offset + i*4));
  }

  if (def) {
    virtualRange(def, 24);
    const size_t fin = size_t(def) + 24 + l.u32(def + 16);
    virtualRange(def, fin - def);
    set(20, s.size());
    for (int i = 0; i < 5; ++i) append(0);
    size_t o = def + 24;
    while (o + 4 <= fin && l.u32(o)) {
      const uint32_t word = l.u32(o), reg_entry = word >> 16, amount = word & 65535;
      o += 4;
      if (reg_entry < 0x300 || reg_entry % 16 || !amount || amount % 4 || size_t(amount)*4 > fin - o)
        throw std::runtime_error("2005 immediate definition not supported");
      // The driver copies to device+0x480+register; c0 starts at +0x780.
      append(((reg_entry - 0x300) / 16) << 16 | amount);
      append(physical.size());
      physical.insert(physical.end(), entry.begin()+o, entry.begin()+o+amount*4);
      o += amount*4;
    }
    if (o + 8 > fin || l.u32(o) || l.u32(o+4))
      throw std::runtime_error("masked definitions not implemented yet");
    append(0); append(0); append(0);
  }
  set(4, s.size());
  set(8, physical.size());
  s.insert(s.end(), physical.begin(), physical.end());
  return s;
}
}  // namespace nfsmw
