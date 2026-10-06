// fh1 - native renderer: shaders made on the user's PC while the game runs (see fh1_extra_shaders.h).

#include "fh1_extra_shaders.h"

#include <rex/cvar.h>
#include <rex/filesystem.h>
#include <rex/logging.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <deque>
#include <filesystem>
#include <fstream>
#include <map>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_set>

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#endif

REXCVAR_DEFINE_BOOL(fh1_native_extra_shaders, true, "FH1",
                    "Native renderer: a shader the library does not know is translated and compiled on this PC by "
                    "a helper thread (tools folder next to fh1.exe) and saved in shaders_extra");
REXCVAR_DEFINE_STRING(fh1_native_shader_tools, "", "FH1",
                      "Native renderer: folder of the shader tools (fh1_hlsl.exe, shader_common.h, dxc.exe); empty "
                      "= the folder 'tools' next to fh1.exe");
REXCVAR_DEFINE_STRING(fh1_native_extra_shaders_dir, "", "FH1",
                      "Native renderer: folder of the shaders made on this PC; empty = 'shaders_extra' next to "
                      "fh1.exe");
REXCVAR_DEFINE_INT32(fh1_native_extra_shaders_wait_ms, 0, "FH1",
                     "Native renderer: the game waits for a shader being made on this PC (the object shows at "
                     "once, after a short pause) while it has waited less than this many ms in the last 3 s; "
                     "beyond that the shader is finished in the background and what it draws appears a moment "
                     "late. 0 = never wait (the default: on a PC whose driver has no cache yet, the waits and the "
                     "pipelines compiled in the same seconds made a frame of more than 3.2 s, which stops the game "
                     "for good; 2026-10-06, twice, with 20000 and with 1000)");

namespace fh1::native {
namespace {

void Put16(std::vector<uint8_t>& d, uint32_t v) {
  d.push_back(uint8_t(v >> 8));
  d.push_back(uint8_t(v));
}
void Put32(std::vector<uint8_t>& d, uint32_t v) {
  for (int shift = 24; shift >= 0; shift -= 8) d.push_back(uint8_t(v >> shift));
}

struct Instruction {
  uint32_t index;
  uint32_t w[3];
};

// The fetch and ALU instructions the control flow runs (pairs of 48-bit control-flow instructions in three
// words, then the instructions their exec blocks name). False when a block points outside the microcode.
bool Walk(std::span<const uint32_t> words, std::vector<Instruction>& fetches, std::vector<Instruction>& alus) {
  const size_t n = words.size();
  size_t instruction_bytes = n * 4;
  struct Exec {
    uint32_t address, count, sequence;
  };
  std::vector<Exec> execs;
  for (size_t pos = 0; pos < instruction_bytes && pos + 12 <= n * 4; pos += 12) {
    const uint32_t w0 = words[pos / 4], w1 = words[pos / 4 + 1], w2 = words[pos / 4 + 2];
    const uint32_t low[2] = {w0, (w1 >> 16) | (w2 << 16)};
    const uint32_t high[2] = {w1 & 0xFFFF, w2 >> 16};
    for (int k = 0; k < 2; ++k) {
      const uint32_t op = (high[k] >> 12) & 0xF;
      if (!((op >= 1 && op <= 6) || op == 13 || op == 14)) continue;
      const Exec e{low[k] & 0xFFF, (low[k] >> 12) & 7, (low[k] >> 16) & 0xFFF};
      execs.push_back(e);
      if (e.address) instruction_bytes = std::min(instruction_bytes, size_t(e.address) * 12);
    }
  }
  for (const Exec& e : execs) {
    for (uint32_t i = 0; i < e.count; ++i) {
      const size_t index = size_t(e.address) + i;
      if ((index + 1) * 3 > n) return false;
      const Instruction in{uint32_t(index), {words[index * 3], words[index * 3 + 1], words[index * 3 + 2]}};
      (((e.sequence >> (2 * i)) & 1) ? fetches : alus).push_back(in);
    }
  }
  return true;
}

// Usages are only labels: each vertex fetch gets a distinct input location from a fixed pool
// (Position0, Normal0, Tangent0, Binormal0, TexCoord0-3, Color0, BlendWeight0, Color1, TexCoord4-7).
constexpr uint32_t kUsagePool[15][2] = {{0, 0}, {3, 0}, {6, 0}, {7, 0}, {5, 0}, {5, 1}, {5, 2}, {5, 3},
                                        {10, 0}, {1, 0}, {10, 1}, {5, 4}, {5, 5}, {5, 6}, {5, 7}};

}  // namespace

// The container Direct3D releases once the GPU has the microcode, rebuilt (tools/fh1_synth_containers.py, byte
// for byte): vertex fetches -> vertex elements, texture fetches -> sampler entries s<N>, exports -> the pixel
// shader's output mask, constants -> one float4 array over every register (the translator reads c<N>).
std::vector<uint8_t> SynthContainer(bool vertices, std::span<const uint32_t> words) {
  std::vector<Instruction> fetches, alus;
  if (words.empty() || !Walk(words, fetches, alus)) return {};
  std::vector<Instruction> vertex_fetches;
  std::map<uint32_t, uint32_t> samplers;  // register -> dimension, the first fetch of a register decides
  for (const Instruction& f : fetches) {
    const uint32_t kind = f.w[0] & 0x1F;
    if (kind == 0) vertex_fetches.push_back(f);
    if (kind == 1) samplers.emplace((f.w[0] >> 20) & 0x1F, (f.w[2] >> 14) & 3);
  }
  std::sort(vertex_fetches.begin(), vertex_fetches.end(), [](const Instruction& a, const Instruction& b) {
    if (a.index != b.index) return a.index < b.index;
    return std::lexicographical_compare(a.w, a.w + 3, b.w, b.w + 3);
  });
  if (vertices && vertex_fetches.size() > std::size(kUsagePool)) return {};
  uint32_t outputs = 0;
  if (!vertices) {
    for (const Instruction& a : alus) {
      if (!((a.w[0] >> 15) & 1)) continue;
      const uint32_t destination[2] = {a.w[0] & 0x3F, (a.w[0] >> 8) & 0x3F};
      const uint32_t mask[2] = {(a.w[0] >> 16) & 0xF, (a.w[0] >> 20) & 0xF};
      for (int k = 0; k < 2; ++k) {
        if (!mask[k]) continue;
        if (destination[k] <= 3) outputs |= 1u << destination[k];
        if (destination[k] == 61) outputs |= 0x10;
      }
    }
  }

  // ---- constant table (offsets from the start of the 28-byte ConstantTable struct)
  struct Constant {
    std::string name;
    uint32_t register_set, index, count;  // 2 = Float4, 3 = Sampler
  };
  std::vector<Constant> constants{{"g_c", 2, 0, 256}};
  for (const auto& [reg, dimension] : samplers) constants.push_back({"s" + std::to_string(reg), 3, reg, 1});
  const uint32_t info_offset = 28;
  const uint32_t types_offset = info_offset + 20 * uint32_t(constants.size());
  const uint32_t strings_base = types_offset + 16 * uint32_t(constants.size());
  std::vector<uint8_t> types, strings, entries;
  // Vector, Float, array of float4
  Put16(types, 1), Put16(types, 3), Put16(types, 1), Put16(types, 4), Put16(types, 1), Put16(types, 0), Put32(types, 0);
  for (const auto& [reg, dimension] : samplers) {  // Object, Sampler1D / 2D / 3D / Cube (D3DXPARAMETER_TYPE 11-14)
    Put16(types, 4), Put16(types, 11 + dimension), Put16(types, 1), Put16(types, 1), Put16(types, 1), Put16(types, 0);
    Put32(types, 0);
  }
  for (size_t i = 0; i < constants.size(); ++i) {
    const Constant& c = constants[i];
    Put32(entries, strings_base + uint32_t(strings.size()));
    Put16(entries, c.register_set), Put16(entries, c.index), Put16(entries, c.count), Put16(entries, 0);
    Put32(entries, types_offset + 16 * uint32_t(i)), Put32(entries, 0);
    strings.insert(strings.end(), c.name.begin(), c.name.end());
    strings.push_back(0);
  }
  std::vector<uint8_t> table;
  Put32(table, strings_base + uint32_t(strings.size())), Put32(table, 0), Put32(table, 0);
  Put32(table, uint32_t(constants.size())), Put32(table, info_offset), Put32(table, 0), Put32(table, 0);
  table.insert(table.end(), entries.begin(), entries.end());
  table.insert(table.end(), types.begin(), types.end());
  table.insert(table.end(), strings.begin(), strings.end());
  while (table.size() % 4) table.push_back(0);

  // ---- shader struct
  const uint32_t code_bytes = uint32_t(words.size()) * 4;
  std::vector<uint8_t> shader;
  if (!vertices) {
    for (uint32_t v : {0u, code_bytes, 0u, 0xFF00u, 0u, 16u << 5, 0u, outputs}) Put32(shader, v);
    for (uint32_t i = 0; i < 16; ++i) Put32(shader, 5u << 4 | i << 8);  // TexCoord i in register i (positional)
  } else {
    for (uint32_t v : {0u, code_bytes, 0u, 0u, 0u, 0u, 0u, uint32_t(vertex_fetches.size()), 0u}) Put32(shader, v);
    for (size_t k = 0; k < vertex_fetches.size(); ++k) {
      Put32(shader, vertex_fetches[k].index | kUsagePool[k][0] << 12 | kUsagePool[k][1] << 16);
    }
  }

  const uint32_t shader_offset = 36;
  const uint32_t table_offset = shader_offset + uint32_t(shader.size());
  uint32_t virtual_bytes = table_offset + 4 + uint32_t(table.size());
  const uint32_t pad = (4 - virtual_bytes % 4) % 4;
  virtual_bytes += pad;
  std::vector<uint8_t> out;
  for (uint32_t v : {0x102A1100u | (vertices ? 1u : 0u), virtual_bytes, code_bytes, 0u, table_offset, 0u, shader_offset,
                     0u, 0u}) {
    Put32(out, v);
  }
  out.insert(out.end(), shader.begin(), shader.end());
  Put32(out, uint32_t(table.size()) + 4);  // ConstantTableContainer: size, then the table
  out.insert(out.end(), table.begin(), table.end());
  out.insert(out.end(), pad, 0);
  for (uint32_t w : words) Put32(out, w);
  return out;
}

namespace extra_shaders {
namespace {

namespace fs = std::filesystem;

fs::path Folder() {
  const std::string chosen = REXCVAR_GET(fh1_native_extra_shaders_dir);
  return chosen.empty() ? rex::filesystem::GetExecutableFolder() / "shaders_extra" : fs::path(chosen);
}

fs::path Tools() {
  const std::string chosen = REXCVAR_GET(fh1_native_shader_tools);
  return chosen.empty() ? rex::filesystem::GetExecutableFolder() / "tools" : fs::path(chosen);
}

bool ReadFile(const fs::path& path, std::vector<uint8_t>& data) {
  std::ifstream f(path, std::ios::binary | std::ios::ate);
  if (!f) return false;
  const auto n = f.tellg();
  if (n <= 0 || n > 8 * 1024 * 1024) return false;
  data.resize(size_t(n));
  f.seekg(0);
  return bool(f.read(reinterpret_cast<char*>(data.data()), std::streamsize(data.size())));
}

bool WriteFile(const fs::path& path, const uint8_t* data, size_t bytes) {
  // Under another name until it is whole: a session never reads half a file of an earlier one.
  fs::path part = path;
  part += ".part";
  {
    std::ofstream f(part, std::ios::binary | std::ios::trunc);
    if (!f.write(reinterpret_cast<const char*>(data), std::streamsize(bytes)) || !f.flush()) return false;
  }
  std::error_code ec;
  fs::remove(path, ec);
  fs::rename(part, path, ec);
  return !ec;
}

// The checks the library makes on its own entries, for a pair of files of this folder.
bool Assemble(std::vector<uint8_t> container, const std::vector<uint8_t>& spirv, Shader& out) {
  if (container.size() < 36 || container.size() > 64 * 1024 || spirv.size() < 20 || spirv.size() % 4) return false;
  const uint32_t signature =
      uint32_t(container[0]) << 24 | uint32_t(container[1]) << 16 | uint32_t(container[2]) << 8 | container[3];
  if ((signature & ~1u) != 0x102A1100u) return false;
  out.vertices = (signature & 1) != 0;
  out.original = std::move(container);
  out.spirv.resize(spirv.size() / 4);
  for (size_t i = 0; i < out.spirv.size(); ++i) {
    out.spirv[i] = uint32_t(spirv[i * 4]) | uint32_t(spirv[i * 4 + 1]) << 8 | uint32_t(spirv[i * 4 + 2]) << 16 |
                   uint32_t(spirv[i * 4 + 3]) << 24;
  }
  if (out.spirv[0] != 0x07230203u) return false;
  // Whole instructions, and the stage of its entry point is the container's.
  bool entry = false;
  for (size_t i = 5; i < out.spirv.size();) {
    const uint32_t words = out.spirv[i] >> 16, op = out.spirv[i] & 0xFFFF;
    if (!words || words > out.spirv.size() - i) return false;
    if (op == 15) {  // OpEntryPoint
      if (words < 3 || out.spirv[i + 1] != (out.vertices ? 0u : 4u)) return false;
      entry = true;
    }
    i += words;
  }
  return entry;
}

#if defined(_WIN32)
// Runs a tool without a window; false when it could not start, ended with an error or ran longer than `seconds`.
bool Run(const fs::path& exe, const std::wstring& arguments, uint32_t seconds) {
  std::wstring command = L"\"" + exe.wstring() + L"\" " + arguments;
  STARTUPINFOW startup{};
  startup.cb = sizeof(startup);
  PROCESS_INFORMATION process{};
  // Below normal: the game's threads come first. Not when the game waits for the shader: then the tool comes first.
  const DWORD priority = REXCVAR_GET(fh1_native_extra_shaders_wait_ms) > 0 ? NORMAL_PRIORITY_CLASS
                                                                           : BELOW_NORMAL_PRIORITY_CLASS;
  if (!CreateProcessW(nullptr, command.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW | priority,
                      nullptr, exe.parent_path().wstring().c_str(), &startup, &process)) {
    return false;
  }
  DWORD code = 1;
  if (WaitForSingleObject(process.hProcess, seconds * 1000) != WAIT_OBJECT_0) {
    TerminateProcess(process.hProcess, 1);
    WaitForSingleObject(process.hProcess, 5000);
  } else {
    GetExitCodeProcess(process.hProcess, &code);
  }
  CloseHandle(process.hThread);
  CloseHandle(process.hProcess);
  return code == 0;
}

std::wstring Quoted(const fs::path& path) {
  return L"\"" + path.wstring() + L"\"";
}

struct Job {
  bool vertices;
  uint64_t fingerprint;
  std::vector<uint32_t> microcode;
};

struct Worker {
  std::mutex mutex;
  std::condition_variable wake;
  std::deque<Job> jobs;
  std::deque<Shader> finished;
  std::unordered_set<uint64_t> asked;
  std::unordered_set<uint64_t> done;  // made or failed: nothing more comes for these
  std::condition_variable done_wake;
  std::atomic<uint32_t> finished_count{0};
  std::atomic<int> tools{-1};  // -1 not looked yet, 0 missing, 1 there
  bool started = false;

  bool HasTools() {
    int state = tools.load(std::memory_order_acquire);
    if (state < 0) {
      const fs::path folder = Tools();
      std::error_code ec;
      state = fs::is_regular_file(folder / "fh1_hlsl.exe", ec) && fs::is_regular_file(folder / "shader_common.h", ec) &&
                      fs::is_regular_file(folder / "dxc.exe", ec)
                  ? 1
                  : 0;
      tools.store(state, std::memory_order_release);
      REXLOG_INFO("[native] C5c: shaders made on this PC: tools {} in {}", state ? "found" : "NOT found", folder.string());
    }
    return state == 1;
  }

  // Container -> HLSL -> SPIR-V, saved in the folder. One shader takes about a second.
  void Make(const Job& job) {
    char name[32];
    std::snprintf(name, sizeof(name), "%c_%016llX", job.vertices ? 'v' : 'p',
                  static_cast<unsigned long long>(job.fingerprint));
    const std::vector<uint8_t> container = SynthContainer(job.vertices, job.microcode);
    if (container.empty()) {
      REXLOG_WARN("[native] C5c: {}: the microcode could not be rebuilt into a container", name);
      return;
    }
    const fs::path folder = Folder(), tools_folder = Tools();
    const fs::path work = folder / (std::string("work_") + name);
    std::error_code ec;
    fs::remove_all(work, ec);
    fs::create_directories(work / "in", ec);
    const fs::path bin = work / "in" / (std::string(name) + ".bin");
    const fs::path hlsl = work / "out" / (std::string(name) + ".hlsl");
    const fs::path spv = work / (std::string(name) + ".spv");
    bool ok = WriteFile(bin, container.data(), container.size());
    ok = ok && Run(tools_folder / "fh1_hlsl.exe",
                   Quoted(work / "in") + L" " + Quoted(work / "out") + L" " + Quoted(tools_folder / "shader_common.h"), 60);
    ok = ok && fs::is_regular_file(hlsl, ec);
    // The options of the library (tools/fh1_compile_shaders.py): a vertex shader's SV_VertexID is the index of
    // the draw, as on the console.
    ok = ok && Run(tools_folder / "dxc.exe",
                   std::wstring(L"-spirv -T ") +
                       (job.vertices ? L"vs_6_6 -fvk-invert-y -fvk-support-nonzero-base-vertex" : L"ps_6_6") +
                       L" -E main -HV 2021 -fspv-target-env=vulkan1.2 -fvk-use-dx-layout -Fo " + Quoted(spv) + L" " +
                       Quoted(hlsl),
                   300);
    std::vector<uint8_t> code;
    Shader shader;
    ok = ok && ReadFile(spv, code) && Assemble(container, code, shader);
    if (ok) {
      ok = WriteFile(folder / (std::string(name) + ".spv"), code.data(), code.size()) &&
           WriteFile(folder / (std::string(name) + ".bin"), container.data(), container.size());
    }
    fs::remove_all(work, ec);
    if (!ok) {
      REXLOG_WARN("[native] C5c: {} ({} words) could not be translated and compiled: what it draws stays missing", name,
                  job.microcode.size());
      return;
    }
    REXLOG_INFO("[native] C5c: {} ({} words) made on this PC and saved in {}", name, job.microcode.size(),
                folder.string());
    std::lock_guard<std::mutex> lock(mutex);
    finished.push_back(std::move(shader));
    finished_count.fetch_add(1, std::memory_order_release);
  }

  void Loop() {
    for (;;) {
      Job job;
      {
        std::unique_lock<std::mutex> lock(mutex);
        wake.wait(lock, [this] { return !jobs.empty(); });
        job = std::move(jobs.front());
        jobs.pop_front();
      }
      Make(job);
      {
        std::lock_guard<std::mutex> lock(mutex);
        done.insert(job.fingerprint ^ (job.vertices ? 0x8000000000000000ull : 0));
      }
      done_wake.notify_all();
    }
  }
};

Worker& TheWorker() {
  static Worker* worker = new Worker;  // lives as long as the process: its thread never ends
  return *worker;
}
#endif

}  // namespace

std::vector<Shader> LoadSaved() {
  std::vector<Shader> shaders;
  if (!REXCVAR_GET(fh1_native_extra_shaders)) return shaders;
  std::error_code ec;
  // The shaders of this folder were made by the translator of one version of the port, and the renderer must
  // agree with it on how each fetch is read (2 = the bones of the characters with a skeleton, 2026-10-06): the
  // files of another version are deleted and made again while the game runs.
  static constexpr char kVersion[] = "2";
  {
    const fs::path stamp = Folder() / "version.txt";
    std::vector<uint8_t> text;
    const bool same = ReadFile(stamp, text) && std::string(text.begin(), text.end()) == kVersion;
    if (!same) {
      uint32_t removed = 0;
      std::vector<fs::path> old;
      for (const auto& entry : fs::directory_iterator(Folder(), ec)) {
        const fs::path extension = entry.path().extension();
        if (entry.is_regular_file(ec) && (extension == ".bin" || extension == ".spv")) old.push_back(entry.path());
      }
      for (const fs::path& path : old) removed += fs::remove(path, ec) ? 1 : 0;
      fs::create_directories(Folder(), ec);
      WriteFile(stamp, reinterpret_cast<const uint8_t*>(kVersion), sizeof(kVersion) - 1);
      if (removed) {
        REXLOG_INFO("[native] C5c: {} files of shaders made by another version of the port deleted: they are made "
                    "again",
                    removed);
      }
    }
  }
  std::vector<fs::path> containers;
  for (const auto& entry : fs::directory_iterator(Folder(), ec)) {
    if (entry.is_regular_file(ec) && entry.path().extension() == ".bin") containers.push_back(entry.path());
  }
  std::sort(containers.begin(), containers.end());
  for (const fs::path& path : containers) {
    fs::path spv = path;
    spv.replace_extension(".spv");
    std::vector<uint8_t> container, code;
    Shader shader;
    if (ReadFile(path, container) && ReadFile(spv, code) && Assemble(std::move(container), code, shader)) {
      shaders.push_back(std::move(shader));
    } else {
      REXLOG_WARN("[native] C5c: {} ignored (no SPIR-V next to it, or damaged)", path.filename().string());
    }
  }
  return shaders;
}

void Request(bool vertices, uint64_t fingerprint, std::span<const uint32_t> microcode) {
#if defined(_WIN32)
  if (!REXCVAR_GET(fh1_native_extra_shaders)) return;
  Worker& w = TheWorker();
  if (!w.HasTools()) return;
  {
    std::lock_guard<std::mutex> lock(w.mutex);
    if (!w.asked.insert(fingerprint ^ (vertices ? 0x8000000000000000ull : 0)).second) return;
    w.jobs.push_back(Job{vertices, fingerprint, std::vector<uint32_t>(microcode.begin(), microcode.end())});
    if (!w.started) {
      w.started = true;
      std::thread([&w] { w.Loop(); }).detach();
    }
  }
  w.wake.notify_one();
#else
  (void)vertices, (void)fingerprint, (void)microcode;
#endif
}

bool Wait(bool vertices, uint64_t fingerprint) {
#if defined(_WIN32)
  const int32_t budget = REXCVAR_GET(fh1_native_extra_shaders_wait_ms);
  if (budget <= 0 || !REXCVAR_GET(fh1_native_extra_shaders)) return false;
  Worker& w = TheWorker();
  if (!w.HasTools()) return false;
  // What the ring has waited in the current window of 3 s (only the ring thread calls this). A first drive asks
  // for six shaders within a few seconds: waited for one after the other, they made one frame of 5.3 s and the
  // game stopped for good (2026-10-06).
  using Clock = std::chrono::steady_clock;
  static Clock::time_point window_start{};
  static std::chrono::nanoseconds window_spent{0};
  const Clock::time_point before = Clock::now();
  if (before - window_start > std::chrono::seconds(3)) {
    window_start = before;
    window_spent = std::chrono::nanoseconds(0);
  }
  const std::chrono::nanoseconds left = std::chrono::milliseconds(budget) - window_spent;
  if (left <= std::chrono::nanoseconds(0)) return false;
  const uint64_t key = fingerprint ^ (vertices ? 0x8000000000000000ull : 0);
  std::unique_lock<std::mutex> lock(w.mutex);
  if (!w.asked.count(key)) return false;
  const bool done = w.done_wake.wait_for(lock, left, [&] { return w.done.count(key) != 0; });
  window_spent += Clock::now() - before;
  return done;
#else
  (void)vertices, (void)fingerprint;
  return false;
#endif
}

bool TakeFinished(Shader& out) {
#if defined(_WIN32)
  Worker& w = TheWorker();
  if (w.finished_count.load(std::memory_order_acquire) == 0) return false;
  std::lock_guard<std::mutex> lock(w.mutex);
  if (w.finished.empty()) return false;
  out = std::move(w.finished.front());
  w.finished.pop_front();
  w.finished_count.fetch_sub(1, std::memory_order_release);
  return true;
#else
  (void)out;
  return false;
#endif
}

}  // namespace extra_shaders
}  // namespace fh1::native
