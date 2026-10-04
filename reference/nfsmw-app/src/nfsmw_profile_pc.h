// nfsmw - sampling CPU profiler and stack dumper for the PC build (testing only).
//
// The console has its own profiler with stacks (switch_perf.cpp), but using it requires a console build. This
// one does the same on the PC: with nfsmw_profile_pc_since_s > 0, from that second of process lifetime and
// for nfsmw_profile_pc_duration_s, it suspends the busy threads every ~1 ms, records their program counter
// and a few stack addresses, and writes logs/profile_pc.csv. The addresses are symbolized offline with
// llvm-symbolizer. On the Switch it does nothing.
//
// DumpStacks is for hangs: the full stacks of every thread, including blocked ones, in logs/stacks_N.txt.

#pragma once

namespace nfsmw::profile_pc {

#if defined(_WIN32)
// Starts the profiler thread and the timed stack dump thread (nfsmw_profile_pc_stacks_s) if the cvars ask
// for them.
void Start();
// Stops the threads (idempotent).
void Stop();
// Dumps the stacks of every thread in the process except the caller. Used by the watchdog in nfsmw_app.h
// when it sees the game stalled.
void DumpStacks(const char* reason);
#else
inline void Start() {}
inline void Stop() {}
inline void DumpStacks(const char*) {}
#endif

}  // namespace nfsmw::profile_pc
