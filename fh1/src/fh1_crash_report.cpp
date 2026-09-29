// fh1 - crash report for Windows builds.
//
// When the game dies on an access violation (or any exception nothing handles), write the
// call stack with function names to "<log_file>.crash.txt" and to the log. Translated game
// functions are named after their Xbox 360 address (sub_82XXXXXX), so the report points
// straight at the guest code involved. Needs the PDB that fh1/CMakeLists.txt asks the
// linker for (/DEBUG) and line tables in the objects.
//
// It is installed as the LAST vectored handler: the SDK's own handler (first in line) gets
// every exception before it and resolves the ones it can (MMIO, write watches). Only an
// access violation inside the guest memory window that nobody fixed gets reported there;
// anything else is reported by the unhandled-exception filter.

#include "fh1_crash_report.h"

#if defined(_WIN32)

#include <rex/cvar.h>
#include <rex/logging.h>

#include <windows.h>
// dbghelp.h needs windows.h first.
#include <dbghelp.h>

#include <atomic>
#include <cstdio>
#include <cstring>
#include <string>

REXCVAR_DECLARE(std::string, log_file);

namespace fh1 {
namespace {

std::atomic<bool> g_reported{false};
std::string g_report_path;

// The guest address space is mapped at these host addresses (see "Guest memory arena
// mapped" in the log): virtual at 0x1'0000'0000, physical at 0x2'0000'0000.
constexpr uintptr_t kGuestHostBegin = 0x100000000ull;
constexpr uintptr_t kGuestHostEnd = 0x300000000ull;

// The file is written (and flushed) line by line first; the log gets the same lines only at
// the end, because the crashing thread might hold the logger's lock.
std::string g_for_log;

void Emit(FILE* f, const char* line) {
  if (f) {
    std::fputs(line, f);
    std::fputc('\n', f);
    std::fflush(f);
  }
  g_for_log += line;
  g_for_log += '\n';
}

void WriteReport(EXCEPTION_POINTERS* ep, const char* why) {
  if (g_reported.exchange(true)) return;  // one report per run

  FILE* f = g_report_path.empty() ? nullptr : std::fopen(g_report_path.c_str(), "w");
  char line[1024];
  const EXCEPTION_RECORD* rec = ep->ExceptionRecord;
  CONTEXT ctx = *ep->ContextRecord;

  std::snprintf(line, sizeof(line), "fh1 crash report (%s)", why);
  Emit(f, line);
  std::snprintf(line, sizeof(line), "exception 0x%08lX at host 0x%016llX, thread %lu",
                rec->ExceptionCode, (unsigned long long)rec->ExceptionAddress,
                GetCurrentThreadId());
  Emit(f, line);
  if (rec->ExceptionCode == EXCEPTION_ACCESS_VIOLATION && rec->NumberParameters >= 2) {
    uintptr_t host = rec->ExceptionInformation[1];
    const char* op = rec->ExceptionInformation[0] == 1 ? "write" : rec->ExceptionInformation[0] == 8 ? "execute" : "read";
    if (host >= kGuestHostBegin && host < kGuestHostEnd) {
      std::snprintf(line, sizeof(line), "%s of guest 0x%08llX (host 0x%016llX)", op,
                    (unsigned long long)(host & 0xFFFFFFFFull), (unsigned long long)host);
    } else {
      std::snprintf(line, sizeof(line), "%s of host 0x%016llX (outside guest memory)", op,
                    (unsigned long long)host);
    }
    Emit(f, line);
  }
  std::snprintf(line, sizeof(line),
                "rax=%016llX rbx=%016llX rcx=%016llX rdx=%016llX rsi=%016llX rdi=%016llX",
                ctx.Rax, ctx.Rbx, ctx.Rcx, ctx.Rdx, ctx.Rsi, ctx.Rdi);
  Emit(f, line);
  std::snprintf(line, sizeof(line),
                "r8=%016llX r9=%016llX r10=%016llX r11=%016llX r12=%016llX r13=%016llX r14=%016llX r15=%016llX",
                ctx.R8, ctx.R9, ctx.R10, ctx.R11, ctx.R12, ctx.R13, ctx.R14, ctx.R15);
  Emit(f, line);
  Emit(f, "call stack (innermost first):");

  HANDLE process = GetCurrentProcess();
  HANDLE thread = GetCurrentThread();
  SymSetOptions(SYMOPT_UNDNAME | SYMOPT_DEFERRED_LOADS | SYMOPT_LOAD_LINES);
  SymInitialize(process, nullptr, TRUE);

  STACKFRAME64 frame = {};
  frame.AddrPC.Offset = ctx.Rip;
  frame.AddrPC.Mode = AddrModeFlat;
  frame.AddrFrame.Offset = ctx.Rbp;
  frame.AddrFrame.Mode = AddrModeFlat;
  frame.AddrStack.Offset = ctx.Rsp;
  frame.AddrStack.Mode = AddrModeFlat;

  alignas(SYMBOL_INFO) char sym_buf[sizeof(SYMBOL_INFO) + 512];
  for (int i = 0; i < 64; ++i) {
    if (!StackWalk64(IMAGE_FILE_MACHINE_AMD64, process, thread, &frame, &ctx, nullptr,
                     SymFunctionTableAccess64, SymGetModuleBase64, nullptr)) {
      break;
    }
    DWORD64 pc = frame.AddrPC.Offset;
    if (!pc) break;

    char module[MAX_PATH] = "?";
    DWORD64 mod_base = SymGetModuleBase64(process, pc);
    if (mod_base) {
      char path[MAX_PATH];
      if (GetModuleFileNameA(reinterpret_cast<HMODULE>(mod_base), path, MAX_PATH)) {
        const char* base = std::strrchr(path, '\\');
        std::snprintf(module, sizeof(module), "%s", base ? base + 1 : path);
      }
    }

    auto* sym = reinterpret_cast<SYMBOL_INFO*>(sym_buf);
    sym->SizeOfStruct = sizeof(SYMBOL_INFO);
    sym->MaxNameLen = 511;
    DWORD64 disp = 0;
    const char* name = "?";
    if (SymFromAddr(process, pc, &disp, sym)) name = sym->Name;

    IMAGEHLP_LINE64 src = {};
    src.SizeOfStruct = sizeof(src);
    DWORD line_disp = 0;
    if (SymGetLineFromAddr64(process, pc, &line_disp, &src)) {
      const char* file = std::strrchr(src.FileName, '\\');
      std::snprintf(line, sizeof(line), "#%02d %s!%s+0x%llX  (%s:%lu)", i, module, name,
                    (unsigned long long)disp, file ? file + 1 : src.FileName, src.LineNumber);
    } else {
      std::snprintf(line, sizeof(line), "#%02d %s!%s+0x%llX  [0x%016llX]", i, module, name,
                    (unsigned long long)disp, (unsigned long long)pc);
    }
    Emit(f, line);
  }
  SymCleanup(process);

  if (f) std::fclose(f);
  REXLOG_CRITICAL("[crash] report written to {}\n{}", g_report_path, g_for_log);
  rex::FlushLogging();
}

LONG CALLBACK LastChanceGuestFault(EXCEPTION_POINTERS* ep) {
  const EXCEPTION_RECORD* rec = ep->ExceptionRecord;
  if (rec->ExceptionCode == EXCEPTION_ACCESS_VIOLATION && rec->NumberParameters >= 2) {
    uintptr_t host = rec->ExceptionInformation[1];
    if (host >= kGuestHostBegin && host < kGuestHostEnd) {
      WriteReport(ep, "guest access violation nobody handled");
    }
  }
  return EXCEPTION_CONTINUE_SEARCH;
}

LONG WINAPI Unhandled(EXCEPTION_POINTERS* ep) {
  WriteReport(ep, "unhandled exception");
  return EXCEPTION_CONTINUE_SEARCH;
}

}  // namespace

void InstallCrashReport() {
  std::string log = REXCVAR_GET(log_file);
  g_report_path = log.empty() ? std::string("fh1.crash.txt") : log + ".crash.txt";
  AddVectoredExceptionHandler(0, LastChanceGuestFault);  // 0 = after the SDK's handler
  SetUnhandledExceptionFilter(Unhandled);
  REXLOG_INFO("Crash report enabled: {}", g_report_path);
}

}  // namespace fh1

#else

namespace fh1 {
void InstallCrashReport() {}
}  // namespace fh1

#endif
