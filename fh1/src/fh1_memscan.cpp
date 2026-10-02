// fh1 - guest memory scanner for finding game variables (car speed, menu state...) for the test
// autoplay, driven by autoplay script commands (format in fh1_autoplay.h):
//   memscan_start LO HI       record every aligned big-endian float with |value| in [LO, HI]
//                             (writes memscan-addrs.bin in the autoplay folder)
//   memscan_sample NAME       save the candidates' current values (memscan-NAME.bin)
//   memscan_filter OP [V]     keep candidates by comparing with the previous filter/start:
//                             inc, dec, absinc, absdec, same, changed, gt V, lt V, abs_gt V,
//                             abs_lt V, delta_lt V, delta_gt V (|change| below/above V)
//   memscan_list N            log up to N candidates with their values
//   memscan_ptrs MAXOFF DEPTH log pointer chains from the executable's data (static) to the
//                             candidates: [static] -> +off -> ... -> candidate (each step reads a
//                             pointer, adds an offset of at most MAXOFF)
// tools/memscan_match.py finds, from samples, the addresses that follow known quantities.

#include "fh1_memscan.h"

#if defined(_WIN32)
#include <windows.h>
#endif

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

#include <fmt/format.h>

#include <rex/logging.h>
#include <rex/memory.h>
#include <rex/system/kernel_state.h>

namespace fh1::memscan {
namespace {

std::mutex g_mutex;
std::vector<uint32_t> g_addresses;  // guest addresses of the candidates
std::vector<float> g_previous;      // their values at the last start/filter
std::unordered_map<std::string, uint32_t> g_variables;  // picked variables

constexpr uint32_t kStaticStart = 0x82000000, kStaticEnd = 0x84000000;

uint8_t* GuestBase() {
  auto* ks = rex::system::kernel_state();
  return ks && ks->memory() ? ks->memory()->virtual_membase() : nullptr;
}

uint32_t LoadBigEndian32(const uint8_t* p) {
  uint32_t v;
  std::memcpy(&v, p, 4);
  return __builtin_bswap32(v);
}

float LoadBigEndianFloat(const uint8_t* p) {
  uint32_t v = LoadBigEndian32(p);
  float f;
  std::memcpy(&f, &v, 4);
  return f;
}

// Calls fn(guest_address, host_pointer, size) for every committed, readable host region of the
// guest ranges worth scanning: the virtual heaps, the executable (code + data) and one view of
// physical memory (0xA0000000; the other physical views alias the same bytes).
template <typename Fn>
void ForEachRegion(Fn fn) {
#if defined(_WIN32)
  uint8_t* base = GuestBase();
  if (!base) return;
  static const struct {
    uint64_t start, end;
  } kRanges[] = {{0x00000000, 0x40000000}, {0x40000000, 0x80000000}, {0x80000000, 0x90000000},
                 {0xA0000000, 0xC0000000}};
  for (const auto& r : kRanges) {
    uint64_t a = r.start;
    while (a < r.end) {
      MEMORY_BASIC_INFORMATION mbi;
      if (!VirtualQuery(base + a, &mbi, sizeof(mbi))) break;
      uint64_t region_start = uint64_t(static_cast<uint8_t*>(mbi.BaseAddress) - base);
      uint64_t region_end = std::min<uint64_t>(region_start + mbi.RegionSize, r.end);
      if (region_end <= a) break;
      bool readable = mbi.State == MEM_COMMIT &&
                      (mbi.Protect & (PAGE_READONLY | PAGE_READWRITE | PAGE_WRITECOPY |
                                      PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE)) &&
                      !(mbi.Protect & (PAGE_GUARD | PAGE_NOACCESS));
      if (readable) {
        uint64_t s = std::max(a, region_start);
        fn(uint32_t(s), base + s, size_t(region_end - s));
      }
      a = region_end;
    }
  }
#else
  (void)fn;
#endif
}

void ReadCurrent(std::vector<float>& out) {
  uint8_t* base = GuestBase();
  out.resize(g_addresses.size());
  for (size_t i = 0; i < g_addresses.size(); ++i) out[i] = LoadBigEndianFloat(base + g_addresses[i]);
}

}  // namespace

size_t Start(float lo, float hi, const std::string& dir) {
  std::lock_guard<std::mutex> lock(g_mutex);
  g_addresses.clear();
  ForEachRegion([&](uint32_t guest, const uint8_t* host, size_t size) {
    for (size_t i = 0; i + 4 <= size; i += 4) {
      float f = std::fabs(LoadBigEndianFloat(host + i));
      if (f >= lo && f <= hi) g_addresses.push_back(guest + uint32_t(i));
    }
  });
  ReadCurrent(g_previous);
  if (!dir.empty()) {
    std::ofstream out(std::filesystem::path(dir) / "memscan-addrs.bin", std::ios::binary);
    uint32_t n = uint32_t(g_addresses.size());
    out.write(reinterpret_cast<const char*>(&n), 4);
    out.write(reinterpret_cast<const char*>(g_addresses.data()), std::streamsize(n) * 4);
  }
  REXLOG_INFO("[memscan] {} floats in [{}, {}]", g_addresses.size(), lo, hi);
  return g_addresses.size();
}

void Sample(const std::string& name, const std::string& dir) {
  std::lock_guard<std::mutex> lock(g_mutex);
  if (!GuestBase() || g_addresses.empty() || dir.empty()) return;
  std::vector<float> values;
  ReadCurrent(values);
  std::ofstream out(std::filesystem::path(dir) / ("memscan-" + name + ".bin"), std::ios::binary);
  out.write(reinterpret_cast<const char*>(values.data()), std::streamsize(values.size()) * 4);
  REXLOG_INFO("[memscan] sample '{}' ({} values)", name, values.size());
}

size_t Filter(const std::string& op, float v) {
  std::lock_guard<std::mutex> lock(g_mutex);
  if (!GuestBase()) return 0;
  std::vector<float> now;
  ReadCurrent(now);
  std::vector<uint32_t> keep_addr;
  std::vector<float> keep_val;
  for (size_t i = 0; i < g_addresses.size(); ++i) {
    float a = g_previous[i], b = now[i];
    bool keep = false;
    if (!std::isfinite(b)) {
      keep = false;
    } else if (op == "inc") {
      keep = b > a;
    } else if (op == "dec") {
      keep = b < a;
    } else if (op == "delta_lt") {
      keep = std::fabs(b - a) < v;
    } else if (op == "delta_gt") {
      keep = std::fabs(b - a) > v;
    } else if (op == "absinc") {
      keep = std::fabs(b) > std::fabs(a);
    } else if (op == "absdec") {
      keep = std::fabs(b) < std::fabs(a);
    } else if (op == "same") {
      keep = b == a;
    } else if (op == "changed") {
      keep = b != a;
    } else if (op == "gt") {
      keep = b > v;
    } else if (op == "lt") {
      keep = b < v;
    } else if (op == "abs_gt") {
      keep = std::fabs(b) > v;
    } else if (op == "abs_lt") {
      keep = std::fabs(b) < v;
    }
    if (keep) {
      keep_addr.push_back(g_addresses[i]);
      keep_val.push_back(b);
    }
  }
  g_addresses.swap(keep_addr);
  g_previous.swap(keep_val);
  REXLOG_INFO("[memscan] filter {} {}: {} candidates left", op, v, g_addresses.size());
  return g_addresses.size();
}

void List(size_t n) {
  std::lock_guard<std::mutex> lock(g_mutex);
  uint8_t* base = GuestBase();
  if (!base) return;
  std::string s;
  for (size_t i = 0; i < g_addresses.size() && i < n; ++i) {
    s += fmt::format(" {:08X}={:.3f}", g_addresses[i], LoadBigEndianFloat(base + g_addresses[i]));
  }
  REXLOG_INFO("[memscan] {} candidates:{}", g_addresses.size(), s);
}

void Pointers(uint32_t max_offset, int depth) {
  std::lock_guard<std::mutex> lock(g_mutex);
  if (!GuestBase() || g_addresses.empty()) return;
  // Level by level: targets = addresses we want to reach; find every aligned 32-bit word holding
  // a value p with target - max_offset <= p <= target. Words in the executable's data end a chain.
  struct Node {
    uint32_t address;  // where the pointer (or the candidate) lives
    int parent;        // index in the previous level, -1 for candidates
    uint32_t offset;   // target - pointer value
  };
  std::vector<std::vector<Node>> levels(1);
  for (size_t i = 0; i < g_addresses.size() && i < 64; ++i) levels[0].push_back({g_addresses[i], -1, 0});
  int found = 0;
  for (int level = 1; level <= depth && found < 200; ++level) {
    const auto& targets = levels[level - 1];
    std::vector<std::pair<uint32_t, int>> sorted;  // (target address, index)
    for (int i = 0; i < int(targets.size()); ++i) {
      if (targets[i].address >= kStaticStart && targets[i].address < kStaticEnd && level > 1) {
        continue;  // already static
      }
      sorted.push_back({targets[i].address, i});
    }
    std::sort(sorted.begin(), sorted.end());
    std::vector<Node> next;
    ForEachRegion([&](uint32_t guest, const uint8_t* host, size_t size) {
      for (size_t i = 0; i + 4 <= size && next.size() < 200000; i += 4) {
        uint32_t p = LoadBigEndian32(host + i);
        // Real pointers are 4-byte aligned and point into guest memory.
        if ((p & 3) || p < 0x00100000 || p >= 0xC0000000) continue;
        // First target >= p.
        auto it = std::lower_bound(sorted.begin(), sorted.end(), std::make_pair(p, -1));
        for (; it != sorted.end() && it->first - p <= max_offset; ++it) {
          next.push_back({guest + uint32_t(i), it->second, it->first - p});
        }
      }
    });
    for (const Node& n : next) {
      if (n.address < kStaticStart || n.address >= kStaticEnd) continue;
      // Print the chain: static -> ... -> candidate.
      std::string chain = fmt::format("[{:08X}]", n.address);
      Node cur = n;
      int lvl = level;
      while (true) {
        chain += fmt::format(" +{:X}", cur.offset);
        const Node& child = levels[lvl - 1][cur.parent];
        if (lvl - 1 == 0) {
          chain += fmt::format(" -> {:08X}", child.address);
          break;
        }
        chain += " ->";
        cur = child;
        --lvl;
      }
      REXLOG_INFO("[memscan] chain: {}", chain);
      if (++found >= 200) break;
    }
    REXLOG_INFO("[memscan] level {}: {} pointers", level, next.size());
    levels.push_back(std::move(next));
  }
}

bool Pick(const std::string& name, float min_value) {
  std::lock_guard<std::mutex> lock(g_mutex);
  uint8_t* base = GuestBase();
  if (!base || g_addresses.empty()) return false;
  std::vector<float> now;
  ReadCurrent(now);
  int best = -1, best_copies = 0;
  for (size_t i = 0; i < now.size(); ++i) {
    if (!(std::fabs(now[i]) > min_value)) continue;
    int copies = 0;
    for (size_t j = 0; j < now.size(); ++j) {
      if (std::fabs(now[j] - now[i]) <= 0.02f * std::fabs(now[i])) ++copies;
    }
    if (copies > best_copies) {
      best_copies = copies;
      best = int(i);
    }
  }
  if (best < 0) {
    REXLOG_WARN("[memscan] pick {}: no candidate above {}", name, min_value);
    return false;
  }
  g_variables[name] = g_addresses[best];
  REXLOG_INFO("[memscan] picked {} = {:08X} (value {:.3f}, {} copies)", name, g_addresses[best],
              now[best], best_copies);
  return true;
}

void Define(const std::string& name, uint32_t guest_address) {
  std::lock_guard<std::mutex> lock(g_mutex);
  g_variables[name] = guest_address;
}

bool Variable(const std::string& name, float& out) {
  uint32_t address;
  {
    std::lock_guard<std::mutex> lock(g_mutex);
    auto it = g_variables.find(name);
    if (it == g_variables.end()) return false;
    address = it->second;
  }
  return Read(address, out);
}

bool Read(uint32_t guest_address, float& out) {
  uint8_t* base = GuestBase();
  if (!base) return false;
  out = LoadBigEndianFloat(base + guest_address);
  return true;
}

bool Read32(uint32_t guest_address, uint32_t& out) {
  uint8_t* base = GuestBase();
  if (!base) return false;
  out = LoadBigEndian32(base + guest_address);
  return true;
}

}  // namespace fh1::memscan
