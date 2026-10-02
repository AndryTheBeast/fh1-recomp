// fh1 - guest memory scanner for finding game variables (see fh1_memscan.cpp).
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

namespace fh1::memscan {

// Records the addresses of all floats with |value| in [lo, hi]; returns how many.
size_t Start(float lo, float hi, const std::string& dir);
// Saves the current values of the recorded floats as memscan-<name>.bin in dir.
void Sample(const std::string& name, const std::string& dir);
// Keeps the candidates passing OP against their previous values: inc, dec, same, changed,
// gt V, lt V, abs_gt V, abs_lt V. Returns how many are left.
size_t Filter(const std::string& op, float v);
// Logs up to n candidates with their values.
void List(size_t n);
// Logs pointer chains from the executable's data to the candidates.
void Pointers(uint32_t max_offset, int depth);
// Picks the candidate whose current value (> min_value) has the most near-equal copies among
// the candidates (a game value is usually stored in several places) and remembers it as a named
// variable. Returns false if none qualifies.
bool Pick(const std::string& name, float min_value);
// Names a known address as a variable.
void Define(const std::string& name, uint32_t guest_address);
// Current value of a picked variable; false if it was never picked.
bool Variable(const std::string& name, float& out);
// Read one big-endian float / word of guest memory.
bool Read(uint32_t guest_address, float& out);
bool Read32(uint32_t guest_address, uint32_t& out);

}  // namespace fh1::memscan
