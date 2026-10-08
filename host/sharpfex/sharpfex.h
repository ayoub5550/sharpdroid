// libsharpfex -- FEXCore as a guest-only CPU backend for a native arm64 SharpEmu.
//
// the design is docs/native-arm64-backend.md section 2. this is the proof of concept from
// section 4: the smallest C ABI that lets a native arm64 .NET process run x86-64 guest code in its
// own address space, take HLE import calls from it, call back into it, and park a guest thread
// and resume it later. one guest thread is one host thread, and a guest pointer is a host pointer.
//
// what an HLE import looks like from the guest: a 16-byte slot the loader patches to
//
//   48 89 4c 24 f8    mov [rsp-8], rcx       ; syscall clobbers rcx and r11. both are parked in
//   4c 89 5c 24 f0    mov [rsp-16], r11      ; the stub's red zone and put back before the handler
//   0f 05             syscall                ; sees them, so nothing the guest owns is lost
//   c3                ret
//
// the slot is identified by the address of its `syscall`, not by a number in rax, so rax is
// preserved too. a `syscall` anywhere else is treated as a real linux syscall and refused with
// -ENOSYS: a PS5 title has none (libkernel is HLE).

#pragma once
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define SFX_ABI_VERSION 3

// the guest state an HLE body may read or write. gpr[] is in x86 encoding order:
// rax rcx rdx rbx rsp rbp rsi rdi r8 r9 r10 r11 r12 r13 r14 r15.
typedef struct sfx_regs {
  uint64_t gpr[16];
  uint64_t rip;     // in: the guest return address. out: change it to transfer control there
  uint32_t mxcsr;
  uint32_t reserved;
  uint64_t xmm[8][2]; // xmm0-7, low 128 bits
} sfx_regs;

// what an HLE handler asks the shim to do once it has returned. a managed frame never unwinds
// through FEX: the handler returns one of these and the C++ side acts on it.
enum {
  SFX_CONTINUE = 0, // carry on; rip/rsp changes in sfx_regs are honoured (context transfer)
  SFX_BLOCK = 1,    // park the thread: sfx_run returns SFX_RUN_BLOCKED, the next sfx_run resumes it
  SFX_EXIT = 2,     // end the thread: sfx_run returns SFX_RUN_EXITED
};

enum {
  SFX_RUN_RETURNED = 0, // the guest executed hlt
  SFX_RUN_BLOCKED = 1,
  SFX_RUN_EXITED = 2,
  SFX_RUN_FAULT = 3,
};

typedef int32_t (*sfx_hle_fn)(void* user, uint32_t index, sfx_regs* regs);

// the fast path for leaf imports (strlen, memcpy, a counter read): called with FEX's own
// guest-register array (x86 order, as sfx_regs.gpr), nothing copied. a slot function may change
// any gpr but not rip, may not block, and may not call back into the guest; rax is its result.
typedef void (*sfx_slot_fn)(void* user, uint64_t* gpr);

typedef struct sfx_stats {
  uint64_t hle_calls;
  uint64_t callbacks;
  uint64_t blocks;
  uint64_t transfers;
  uint64_t foreign_syscalls;
  uint64_t unaligned_fixups;
  uint64_t jit_restarts;
  uint64_t signals_chained; // faults handed on to the handler that was installed before ours (.NET)
} sfx_stats;

// FEX options as "Name=Value" pairs separated by ';' (the same names `--fex` takes), plus the
// shim's own `sfx.*` options, or NULL:
//   sfx.CallSave=light|full   what sfx_call preserves (below)
//   sfx.Callback=legacy|fast  how sfx_call enters the guest: FEX's HandleCallback, or the shim's own
//                             entry (fills the static registers, pushes a call-return entry that the
//                             guest's `ret` matches, and branches to the block). same contract.
//                             `fast-fallback` makes every return take the fallback exit (a test)
//   sfx.AFP=0                 hide FEAT_AFP from FEX: no FPCR write at any JIT<->host transition
//                             (a measuring knob; scalar SSE costs more without AFP)
// returns 0 on success.
int32_t sfx_init(const char* options);
const char* sfx_describe(void);
void sfx_set_hle_handler(sfx_hle_fn fn, void* user);
// route import slot `index` (< 64) to a slot function instead of the handler; fn = NULL undoes it.
int32_t sfx_set_slot_fn(uint32_t index, sfx_slot_fn fn, void* user);
// built-in slot functions, for measuring the boundary itself: "add" (rax = rdi + rsi), "null",
// "callback" (rax = sfx_call(rdi, rsi, 7) + 1 on the current thread: a host->guest call from C++).
sfx_slot_fn sfx_builtin(const char* name);
// thunk slots: the import as FEX's thunk op, `0f 3f` + a 32-byte name, which FEX compiles into an
// inline host call (no block exit, no syscall, rcx/r11 untouched) followed by a `ret`. index `i`
// routes like import slot `i`: its slot function if set, else the handler, which may not block or
// change rip (the op returns to [rsp] itself; such a call gets rax = -EPERM). writes the
// SFX_THUNK_CODE_SIZE bytes for slot `index` to out and returns their count (0 on a bad argument).
// the bytes go anywhere in declared guest code; an import slot can `jmp` to them.
#define SFX_THUNK_CODE_SIZE 34
int32_t sfx_thunk_code(uint32_t index, uint8_t* out, uint32_t size);
// the import slot table: `count` slots of `stride` bytes at `base`.
int32_t sfx_set_import_slots(uint64_t base, uint32_t count, uint32_t stride);
// a guest address holding `0f 3e` (CALLBACKRET), where a host->guest call returns to.
int32_t sfx_set_callback_return(uint64_t address);
// declare guest code. pages stay RW on the host: FEX reads them and never executes them.
int32_t sfx_mark_exec(uint64_t base, uint64_t length);
void sfx_invalidate(uint64_t base, uint64_t length);

// a guest thread bound to the calling host thread.
void* sfx_thread_new(uint64_t rip, uint64_t rsp, uint64_t fs_base);
void sfx_thread_free(void* thread);
// run from the thread's current rip until hlt, a block, an exit or a fault.
int32_t sfx_run(void* thread);
// call a guest function (SysV, up to 6 integer arguments) and return its rax. callable from the
// host at top level and from inside an HLE handler (re-entrant). gprs, rip, flags, mxcsr and the
// call-return stack around it are preserved -- what an import boundary needs; option
// `sfx.CallSave=full` preserves every vector register too. rsp = 0 uses the thread's current guest
// stack, below the red zone.
uint64_t sfx_call(void* thread, uint64_t rip, uint64_t rsp, const uint64_t* args, int32_t count);
// a benchmark: n calls of sfx_call(thread, rip, rsp, {i, 7}) from C++, the sum of their results.
uint64_t sfx_bench_call(void* thread, uint64_t rip, uint64_t rsp, uint64_t n);
void sfx_get_regs(void* thread, sfx_regs* out);
void sfx_set_regs(void* thread, const sfx_regs* in);
void sfx_get_stats(sfx_stats* out);

#ifdef __cplusplus
}
#endif
