// fh1 - "where does the first NaN come from" trap (car-collision investigation, ROADMAP).
//
// Cars and loose objects pass through the player's car: the hit is noticed (the camera flicks)
// but there is no push, no sound and no damage, as if the impact strength were invalid. With
// --fh1_nan_trap=N, N seconds after the physics code first runs, the SSE "invalid operation"
// exception is unmasked on every thread that reaches the physics hook below. Each instruction
// that then produces a NaN from non-NaN inputs (0/0, inf-inf, 0*inf, sqrt(-1)) traps; the
// handler logs the translated function and source line once per site, masks the exception in
// the trapped context so the instruction finishes normally, and lets the game go on.
// The guest's next flush-mode switch writes ctx.fpscr.csr back, re-arming the trap.
//
// Off by default; only for investigation (it changes timing, never results).

#if defined(_WIN32)

#include <atomic>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <set>

#include <windows.h>
// dbghelp.h needs windows.h first.
#include <dbghelp.h>

#include <rex/cvar.h>
#include <rex/hook.h>
#include <rex/logging.h>

REXCVAR_DEFINE_INT32(fh1_nan_trap, 0, "FH1",
                     "Debug: N > 0 = N s after the physics first runs, log every code site that "
                     "creates a NaN on the physics threads (SSE invalid-operation trap)");
REXCVAR_DEFINE_INT32(fh1_nan_trap_max, 40, "FH1", "Debug: stop logging after this many sites");

namespace {

constexpr uint32_t kInvalidMask = 0x80;  // MXCSR.IM
std::atomic<uint64_t> g_first_tick{0};
std::atomic<int> g_sites{0};
std::mutex g_mutex;
std::set<DWORD64> g_seen;
bool g_sym_ready = false;

std::string Describe(HANDLE process, DWORD64 pc) {
  alignas(SYMBOL_INFO) char buf[sizeof(SYMBOL_INFO) + 256];
  auto* sym = reinterpret_cast<SYMBOL_INFO*>(buf);
  sym->SizeOfStruct = sizeof(SYMBOL_INFO);
  sym->MaxNameLen = 255;
  DWORD64 disp = 0;
  char out[512];
  const char* name = SymFromAddr(process, pc, &disp, sym) ? sym->Name : "?";
  IMAGEHLP_LINE64 line = {};
  line.SizeOfStruct = sizeof(line);
  DWORD line_disp = 0;
  if (SymGetLineFromAddr64(process, pc, &line_disp, &line)) {
    const char* file = std::strrchr(line.FileName, '\\');
    std::snprintf(out, sizeof(out), "%s+0x%llX (%s:%lu)", name, (unsigned long long)disp,
                  file ? file + 1 : line.FileName, line.LineNumber);
  } else {
    std::snprintf(out, sizeof(out), "%s+0x%llX", name, (unsigned long long)disp);
  }
  return out;
}

LONG CALLBACK NanTrapHandler(EXCEPTION_POINTERS* ep) {
  DWORD code = ep->ExceptionRecord->ExceptionCode;
  if (code != STATUS_FLOAT_INVALID_OPERATION && code != STATUS_FLOAT_MULTIPLE_TRAPS) {
    return EXCEPTION_CONTINUE_SEARCH;
  }
  CONTEXT* c = ep->ContextRecord;
  DWORD64 pc = c->Rip;
  {
    std::lock_guard<std::mutex> lock(g_mutex);
    if (g_sites.load() < REXCVAR_GET(fh1_nan_trap_max) && g_seen.insert(pc).second) {
      HANDLE process = GetCurrentProcess();
      if (!g_sym_ready) {
        SymSetOptions(SYMOPT_UNDNAME | SYMOPT_DEFERRED_LOADS | SYMOPT_LOAD_LINES);
        SymInitialize(process, nullptr, TRUE);
        g_sym_ready = true;
      }
      // The site plus two callers (frame-pointer walk is enough for a hint).
      std::string where = Describe(process, pc);
      STACKFRAME64 frame = {};
      frame.AddrPC.Offset = c->Rip;
      frame.AddrPC.Mode = AddrModeFlat;
      frame.AddrFrame.Offset = c->Rbp;
      frame.AddrFrame.Mode = AddrModeFlat;
      frame.AddrStack.Offset = c->Rsp;
      frame.AddrStack.Mode = AddrModeFlat;
      CONTEXT walk = *c;
      std::string callers;
      for (int i = 0; i < 3; ++i) {
        if (!StackWalk64(IMAGE_FILE_MACHINE_AMD64, process, GetCurrentThread(), &frame, &walk,
                         nullptr, SymFunctionTableAccess64, SymGetModuleBase64, nullptr)) {
          break;
        }
        if (i > 0) callers += " <- " + Describe(process, frame.AddrPC.Offset);
      }
      int n = g_sites.fetch_add(1) + 1;
      REXLOG_WARN("[nan] #{} thread {} at {}{}", n, GetCurrentThreadId(), where, callers);
    }
  }
  // Finish the instruction masked (NaN result as usual) and clear the sticky flags.
  c->MxCsr = (c->MxCsr | kInvalidMask) & ~0x3Fu;
  c->FltSave.MxCsr = c->MxCsr;
  return EXCEPTION_CONTINUE_EXECUTION;
}

void Arm(PPCContext& ctx) {
  int32_t delay = REXCVAR_GET(fh1_nan_trap);
  if (delay <= 0) return;
  uint64_t now = GetTickCount64();
  uint64_t first = 0;
  if (g_first_tick.compare_exchange_strong(first, now)) {
    AddVectoredExceptionHandler(1, NanTrapHandler);
    REXLOG_INFO("[nan] physics running; trap arms in {} s", delay);
    return;
  }
  if (now - first < uint64_t(delay) * 1000) return;
  thread_local bool armed = false;
  if (armed) return;
  armed = true;
  ctx.fpscr.csr &= ~kInvalidMask;
  ctx.fpscr.setcsr(ctx.fpscr.csr);
  REXLOG_INFO("[nan] trap armed on thread {}", GetCurrentThreadId());
}

}  // namespace

// sub_82D3DB00: the physics surface-table lookup (also wrapped in fh1_trace_load.cpp, which
// calls the original); this runs on the physics threads every frame while driving.
void Fh1NanTrapArm(PPCContext& ctx) { Arm(ctx); }

#else

#include <rex/hook.h>
void Fh1NanTrapArm(PPCContext&) {}

#endif
