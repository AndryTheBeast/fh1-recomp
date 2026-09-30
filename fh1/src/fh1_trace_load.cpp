// fh1 - the crash when the game loads a save (ROADMAP, "Crash loading a save"): a guard on the
// table index (always on) and a tracer (--fh1_trace_load).
//
// A worker thread crashes in sub_82D3DB00 right after the loading fiber finishes. That function
// takes an object (r3) and an index (r4 & 0xFF), reads a 12-byte entry at [r3+84] + index*12 and
// walks the list at [entry+4] for [entry+9] items. The values it reads at the crash are garbage,
// so this wraps the function (always calling the original) and logs, per (object, index), what
// the entry held on each call and when it changed, plus which thread called and from where. It
// changes nothing in the game. On only with --fh1_trace_load.

#include <atomic>
#include <cstdint>
#include <cstring>
#include <functional>
#include <mutex>
#include <thread>
#include <unordered_map>

#include <rex/cvar.h>
#include <rex/hook.h>
#include <rex/logging.h>

REXCVAR_DEFINE_BOOL(fh1_trace_load, false, "FH1",
                    "Log the table sub_82D3DB00 walks (save-loading crash investigation)");

namespace {

uint32_t Be32(const uint8_t* base, uint32_t address) {
  uint32_t v;
  std::memcpy(&v, base + address, 4);
  return __builtin_bswap32(v);
}

struct Seen {
  uint32_t table = 0, list = 0, count = 0, entry0 = 0, entry8 = 0;
};

std::mutex g_mutex;
std::unordered_map<uint64_t, Seen> g_seen;
std::atomic<uint32_t> g_lines{0};
std::atomic<uint64_t> g_calls{0};
constexpr uint32_t kMaxLines = 3000;

bool Plausible(uint32_t p) {
  // Guest heaps the game uses: 0x30000000-0x4FFFFFFF (virtual), 0x70000000-0x7FFFFFFF (stacks),
  // 0x80000000-0x9FFFFFFF (image/physical), 0xA0000000-0xFFFFFFFF (physical views).
  return p >= 0x30000000;
}

}  // namespace

REX_EXTERN(__imp__sub_82D3DB00);
REX_HOOK_RAW(sub_82D3DB00) {
  // WORKAROUND. The index can come from a stack word the game never wrote (see
  // XThread::AllocateStack: with Xenia's 0xBE stack fill it was always 190). Zeroed stacks fixed
  // the usual case; a long-lived stack can still hold a stale value (seen: 192, 1 run in 5), which
  // the console, whose kernel calls leave different stack contents, never hits. A real entry lies
  // just after its table: both its first word and its list point within 64 KB past [r3+84]. If the
  // requested one does not, use entry 0 - the only index seen in thousands of good calls.
  {
    uint32_t table = Be32(base, ctx.r3.u32 + 84);
    uint32_t index = ctx.r4.u32 & 0xFF;
    if (index && Plausible(table)) {
      uint32_t entry = table + index * 12;
      uint32_t first = Be32(base, entry), list = Be32(base, entry + 4);
      bool ok = first - table < 0x10000 && list - table < 0x10000;
      if (!ok) {
        static std::atomic<uint32_t> fixes{0};
        if (fixes.fetch_add(1) < 50) {
          REXLOG_WARN("[load] sub_82D3DB00: index {} of object {:08X} is not a real entry "
                      "({:08X} {:08X}); using entry 0 (caller {:08X})",
                      index, ctx.r3.u32, first, list, uint32_t(ctx.lr));
        }
        ctx.r4.u64 = ctx.r4.u32 & ~0xFFu;
      }
    }
  }
  if (REXCVAR_GET(fh1_trace_load)) {
    uint32_t object = ctx.r3.u32;
    uint32_t index = ctx.r4.u32 & 0xFF;
    uint32_t lr = uint32_t(ctx.lr);
    uint64_t call = g_calls.fetch_add(1) + 1;
    uint32_t tid = uint32_t(std::hash<std::thread::id>{}(std::this_thread::get_id()) & 0xFFFF);
    Seen now;
    now.table = Be32(base, object + 84);
    uint32_t entry = now.table + index * 12;
    bool entry_ok = Plausible(now.table);
    if (entry_ok) {
      now.entry0 = Be32(base, entry);
      now.list = Be32(base, entry + 4);
      now.entry8 = Be32(base, entry + 8);
      now.count = (now.entry8 >> 16) & 0xFF;  // byte at entry+9
    }
    bool suspicious = !entry_ok || (now.count && !Plausible(now.list));
    bool changed = false, first = false;
    {
      std::lock_guard<std::mutex> lock(g_mutex);
      auto [it, inserted] = g_seen.try_emplace((uint64_t(object) << 8) | index, now);
      first = inserted;
      if (!inserted) {
        const Seen& old = it->second;
        changed = old.table != now.table || old.list != now.list || old.count != now.count ||
                  old.entry0 != now.entry0 || old.entry8 != now.entry8;
        if (changed) {
          if (g_lines.fetch_add(1) < kMaxLines) {
            REXLOG_INFO("[load] #{} t{:04X} lr {:08X} obj {:08X}[{}] CHANGED table {:08X}->{:08X} "
                        "entry {:08X} {:08X} {:08X} -> {:08X} {:08X} {:08X}",
                        call, tid, lr, object, index, old.table, now.table, old.entry0, old.list,
                        old.entry8, now.entry0, now.list, now.entry8);
          }
          it->second = now;
        }
      }
    }
    if ((first || suspicious) && g_lines.fetch_add(1) < kMaxLines) {
      REXLOG_INFO("[load] #{} t{:04X} lr {:08X} obj {:08X}[{}] table {:08X} entry {:08X} list {:08X} "
                  "count {} r5 {:08X}{}",
                  call, tid, lr, object, index, now.table, now.entry0, now.list, now.count,
                  ctx.r5.u32, suspicious ? "  SUSPICIOUS" : "");
    }
  }
  __imp__sub_82D3DB00(ctx, base);
}
