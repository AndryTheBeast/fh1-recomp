// nfsmw - PC build: unmasked floating-point exceptions.
//
// What happens
//   The PC build with the Switch SDK (tools/pc) closes after 4 s with 0xC000008F
//   (STATUS_FLOAT_INEXACT_RESULT), with and without the audio rescue. On x86 an MXCSR mask
//   bit at 0 unmasks that exception, and rex::ppc::FPSCR writes its cached copy "csr" to
//   MXCSR: if that copy is 0 on a thread that did not go through InitHost(), the first
//   inexact result kills the process. On Switch (AArch64) the same 0 leaves the traps off,
//   which is why it does not happen there.
//
// What it does
//   A Windows vectored exception handler that, on a floating-point exception, masks MXCSR
//   and the thread's x87 FPU again and lets execution continue. With everything masked the
//   operation yields the default result, which is what the console does with exceptions
//   off. Each new address is logged from a separate thread: the handler takes no locks,
//   because the exception can fire inside the logger itself (it formats floats).
//
// Windows only: the Switch does not need it.

#if defined(_WIN32)

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <rex/logging.h>

#include <array>
#include <atomic>
#include <cstdint>
#include <thread>

namespace {

constexpr DWORD kCodesCommaFloat[] = {
    0xC000008D,  // STATUS_FLOAT_DENORMAL_OPERAND
    0xC000008E,  // STATUS_FLOAT_DIVIDE_BY_ZERO
    0xC000008F,  // STATUS_FLOAT_INEXACT_RESULT
    0xC0000090,  // STATUS_FLOAT_INVALID_OPERATION
    0xC0000091,  // STATUS_FLOAT_OVERFLOW
    0xC0000092,  // STATUS_FLOAT_STACK_CHECK
    0xC0000093,  // STATUS_FLOAT_UNDERFLOW
    0xC00002B4,  // STATUS_FLOAT_MULTIPLE_FAULTS
    0xC00002B5,  // STATUS_FLOAT_MULTIPLE_TRAPS
};

struct Room {
  std::atomic<uint64_t> rip{0};
  std::atomic<uint32_t> code{0};
  std::atomic<uint32_t> mxcsr{0};
  std::atomic<uint32_t> thread_value{0};
  std::atomic<uint64_t> times{0};
  bool noted = false;  // only the reporter thread touches it
};

// Filled in order without allocating: the handler cannot allocate.
std::array<Room, 64> g_sites;
std::atomic<uint64_t> g_total{0};

Room* FindOCreate(uint64_t rip) {
  for (Room& room : g_sites) {
    uint64_t actual = room.rip.load(std::memory_order_acquire);
    if (actual == rip) {
      return &room;
    }
    if (actual == 0) {
      if (room.rip.compare_exchange_strong(actual, rip, std::memory_order_acq_rel)) {
        return &room;
      }
      if (actual == rip) {
        return &room;
      }
    }
  }
  return nullptr;  // table full
}

LONG CALLBACK HandlerCommaFloat(EXCEPTION_POINTERS* info) {
  const DWORD code = info->ExceptionRecord->ExceptionCode;
  bool is_comma_float = false;
  for (DWORD c : kCodesCommaFloat) {
    is_comma_float |= (c == code);
  }
  if (!is_comma_float) {
    return EXCEPTION_CONTINUE_SEARCH;
  }

  CONTEXT* context_id = info->ContextRecord;
  const DWORD mxcsr = context_id->MxCsr;
  // All masks to 1 and the sticky flags to 0, in SSE and in x87.
  context_id->MxCsr = (mxcsr | 0x1F80) & ~DWORD(0x3F);
  context_id->FltSave.MxCsr = context_id->MxCsr;
  context_id->FltSave.ControlWord |= 0x3F;
  context_id->FltSave.StatusWord &= ~WORD(0xFF);

  g_total.fetch_add(1, std::memory_order_relaxed);
  if (Room* room = FindOCreate(context_id->Rip)) {
    if (room->times.load(std::memory_order_acquire) == 0) {
      room->code.store(code, std::memory_order_release);
      room->mxcsr.store(mxcsr, std::memory_order_release);
      room->thread_value.store(GetCurrentThreadId(), std::memory_order_release);
    }
    room->times.fetch_add(1, std::memory_order_relaxed);
  }
  return EXCEPTION_CONTINUE_EXECUTION;
}

void Reporter() {
  uint64_t total_noted = 0;
  for (;;) {
    Sleep(2000);
    for (Room& room : g_sites) {
      const uint64_t rip = room.rip.load(std::memory_order_acquire);
      if (!rip) {
        break;
      }
      if (room.noted || room.times.load(std::memory_order_acquire) == 0) {
        continue;
      }
      room.noted = true;
      HMODULE modulo = nullptr;
      char name[MAX_PATH] = "?";
      if (GetModuleHandleExA(
              GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
              reinterpret_cast<LPCSTR>(rip), &modulo)) {
        GetModuleFileNameA(modulo, name, MAX_PATH);
      }
      const uint64_t base = uint64_t(reinterpret_cast<uintptr_t>(modulo));
      REXLOG_WARN(
          "[pc] excepcion de comma float {:08X} en {}+0x{:X} (MXCSR {:08X}, thread_value {}); se "
          "enmascara y sigue",
          room.code.load(), name, rip - base, room.mxcsr.load(), room.thread_value.load());
    }
    const uint64_t total = g_total.load(std::memory_order_relaxed);
    if (total != total_noted) {
      total_noted = total;
      REXLOG_INFO("[pc] excepciones de comma float enmascaradas until now: {}", total);
    }
  }
}

struct Register {
  Register() {
    AddVectoredExceptionHandler(1, &HandlerCommaFloat);
    std::thread(Reporter).detach();
  }
};
Register g_register;

}  // namespace

#endif  // _WIN32
