// sharpdroid host layer -- guest threads.
//
// the host layer began single-threaded, and not incidentally: FEXCore's thread state, the
// GDT, the call-return shadow stack, the signal mask and the setjmp escape hatch were all file
// scope singletons in main.cpp. this is where all of that becomes per-thread, because .NET's
// CoreCLR cannot finish initialising without `pthread_create` and glibc implements that with
// `clone`.
//
// the model is one guest thread to one host thread, always. that is not a simplification we are
// making -- it is what keeps everything else in the host layer honest. `gettid`, `sched_*`,
// `futex` and `/proc/self/task` can all keep forwarding to bionic and give truthful answers,
// because the thing the guest calls a thread really is a thread of this process.
//
// what FEXCore does and does not do for us here is worth stating plainly. it will create an
// `InternalThreadState` and run it on whatever host thread calls `ExecuteThread`; it will not
// create the host thread, populate the GDT, allocate the call-return stack, or know anything
// about `clone` semantics. all of that is ours, exactly as it is FEX's own LinuxEmulation's.

#pragma once

#include "guest_signals.h"

#include <FEXCore/Core/CoreState.h>

#include <atomic>
#include <csetjmp>
#include <cstdint>
#include <pthread.h>

namespace FEXCore::Context {
class Context;
}
namespace FEXCore {
struct SignalDelegatorConfig;
}

namespace HostLayer {

// how execution left the guest.
enum class Escape {
  Returned,    // ExecuteThread came back on its own (hlt, with EnableExitOnHLT)
  Fault,       // a guest instruction faulted and there was no guest handler for it
  Exited,      // this guest thread asked to exit
  ExitedGroup, // the guest asked for the whole process to exit
  Restart,     // guest state was rewritten (signal delivered, or sigreturn); dispatch again
};

struct FaultReport {
  bool Caught;
  uint64_t HostPC;
  uint64_t GuestRIP;
  void* FaultAddress;
  int Signal;
  ///< siginfo's si_code. worth carrying because two of the host layer's own mechanisms are told
  ///< apart by it -- SEGV_ACCERR on a page we protected ourselves is a signal to deliver or a guest
  ///< rewriting its own code, not a crash.
  int SiCode;
  bool InJitCode;
  uint64_t GPR[16];
};

// everything one guest thread owns. allocated by the host layer and pointed at from FEXCore's
// `InternalThreadState::FrontendPtr`, which exists for precisely this.
struct GuestThread {
  FEXCore::Core::InternalThreadState* Thread {};

  // --- storage FEXCore expects the owner of the thread to provide -----------------------------
  //
  // the decoder does not tolerate an empty GDT: Decoder::DecodeInstructionsAtEntry reads the CS
  // descriptor to decide 64-bit mode and dereferences the result unchecked. and the JIT reaches
  // the call-return shadow stack through a pinned register with no null check -- the JIT's
  // ExitFunction op stores to `[REG_CALLRET_SP, #-0x10]` pre-index -- so a zero there turns the
  // thread's first `call` into a store to 0xFFFFFFFFFFFFFFF0.
  //
  // both are per-thread, and both are inherited by a cloned thread rather than defaulted: the GDT
  // is copied from the parent, the call-return stack is fresh because its contents describe host
  // call frames that only ever existed on the parent's host stack.
  FEXCore::Core::CPUState::gdt_segment GDT[32] {};
  void* CallRetAlloc {};
  size_t CallRetAllocSize {};
  uint64_t CallRetDefault {};

  // --- identity ------------------------------------------------------------------------------
  int32_t TID {};      ///< the host tid, which is also the guest tid: one guest thread, one host thread
  bool Initial {};     ///< the thread the program started on, which nothing cloned and nobody joins
  pthread_t Host {};   ///< only valid when !Initial

  ///< CLONE_CHILD_CLEARTID / set_tid_address: zeroed and futex-woken when this thread exits, which
  ///< is the entire mechanism behind pthread_join.
  int32_t* ClearChildTID {};

  // --- guest signal state that is per-thread rather than per-process --------------------------
  //
  // the split matters and is easy to get backwards. `sigaction` installs handlers for the whole
  // process; the blocked mask and the alternate stack belong to one thread. a host layer that
  // shared the mask would have thread B's `pthread_sigmask` silently deafen thread A.
  uint64_t BlockedMask {};
  GuestABI::AltStack AltStack {};

  ///< signals raised on this thread by another one and not yet taken, one bit per signal at
  ///< (n - 1). atomic because the raising thread writes it and this thread reads it, sometimes
  ///< from inside its own host signal handler. it is the entire payload of an asynchronous raise:
  ///< the host signal that goes with it carries no number of its own.
  std::atomic<uint64_t> PendingSignals {};

  ///< what TakeGeneratedFault picked up, held until DeliverToGuest builds the frame from it.
  struct {
    bool Pending;
    uint8_t TrapNo;
    uint8_t SiCode;
    uint16_t ErrCode;
  } Generated {};

  // --- how this thread leaves guest code -----------------------------------------------------
  //
  // spelled EscapeHatch rather than Escape because a member named Escape would hide the enclosing
  // namespace's enum inside this class, and `Escape::Returned` below would then name the buffer.
  sigjmp_buf EscapeHatch;
  Escape Reason {Escape::Returned};
  int ExitStatus {};
  FaultReport Fault {};

  ///< the host alternate signal stack. per-thread by definition: a fault arrives while JIT'd code
  ///< is running on the guest stack, and every thread needs somewhere else to land.
  void* SignalStack {};
  size_t SignalStackSize {};

  // the two halves of the start-up handshake with the cloning thread. they live here, in storage
  // that outlives both, rather than on the parent's stack -- the parent returns from clone straight
  // back into JIT'd code, so by the time the child re-checks its wake-up condition the parent's
  // frame has already been overwritten by guest execution.
  bool StartPublished {};
  bool StartReleased {};

  // --- pausing -------------------------------------------------------------------------------
  //
  // read by the thread that pauses, written by this one, so all three are atomic.

  ///< inside a guest syscall, from the moment the handler is entered to the moment it returns. a
  ///< thread in one is either blocked, and so already stopped, or on its way to the exit where it
  ///< parks -- so a pause leaves it alone rather than interrupting it.
  std::atomic<bool> InSyscall {};
  ///< stopped by a pause, in one of the two places a thread is allowed to stop.
  std::atomic<bool> Parked {};
  ///< poked by the pause rather than by a guest signal. a poke that lands in a blocking host call
  ///< brings it back with EINTR, and this is what says that EINTR is the host layer's and not
  ///< something the guest should see.
  std::atomic<bool> PauseKicked {};
};

namespace Threads {

void Initialize(FEXCore::Context::Context* CTX, GuestSignals* Signals, const FEXCore::SignalDelegatorConfig* Config);

///< the thread the program starts on. RIP and RSP come from the ELF loader.
GuestThread* CreateInitial(uint64_t RIP, uint64_t RSP);

///< the guest thread running on this host thread, or nullptr if this host thread is not one.
GuestThread* Current();

/**
 * @brief Run something for every guest thread that currently exists.
 *
 * the VMA tracker needs this: dropping a translation means dropping it from the shared code
 * buffers *and* from every thread's own lookup cache, and only the host layer knows what "every
 * thread" is.
 *
 * a plain function pointer and a void* rather than a std::function, because one caller is the
 * SIGSEGV handler and an allocation there is not something to rely on. the registry lock this
 * takes is a leaf -- nothing is ever acquired while holding it -- so it is safe to call with the
 * code invalidation mutex already held, which is how the tracker calls it.
 */
void ForEachLive(void (*Fn)(GuestThread&, void*), void* User);

///< dispatch guest code until the thread faults, exits, or stops. this is where the escape hatch
///< is armed, so it must stay on the stack for as long as the thread runs.
void Run(GuestThread& T);

///< release everything the thread owns, including its FEXCore state.
void Destroy(GuestThread& T);

///< install the host's SIGSEGV/SIGBUS/SIGILL/SIGFPE/SIGTRAP handlers. process-wide, so once.
void InstallProcessFaultHandlers();

/**
 * @brief clone(2), for the CLONE_THREAD case.
 *
 * argument order is the x86-64 kernel's, which is *not* the one x86-32 uses -- x86-32 selects
 * CLONE_BACKWARDS and swaps tls with child_tid, and copying that mapping here would put a TLS
 * pointer where a tid pointer belongs.
 *
 * @return the new thread's tid, or a negative errno. anything without CLONE_THREAD gets -ENOSYS:
 * that is fork, and fork of a JIT with a shared code cache is a milestone of its own.
 */
uint64_t Clone(FEXCore::Core::CpuStateFrame* Frame, uint64_t Flags, uint64_t StackPtr, int32_t* ParentTID, int32_t* ChildTID, uint64_t TLS);

///< exit(2) ends this guest thread; exit_group(2) ends the process. neither returns.
[[noreturn]] void ExitCurrent(int Status, bool Group);

/**
 * @brief Re-dispatch this thread from the guest state that was just installed.
 *
 * used by rt_sigreturn and by any syscall that delivers a signal to the guest: both *replace*
 * guest state wholesale, so there is no value to return in RAX and nowhere in the calling JIT
 * block to carry on from. does not return.
 */
[[noreturn]] void RestartCurrent();

// --- asynchronous signals ----------------------------------------------------------------------
//
// two places, and only two, where a guest thread can be redirected into a signal handler: inside a
// translated block, and on the way out of a guest syscall. everywhere else -- our own syscall
// implementations, FEXCore's compiler, bionic -- there are host locks and host frames that
// abandoning them would strand, and delivering means abandoning them.
//
// so raising a signal on another thread is two steps that deliver nothing: record the bit, then
// poke the thread with a host signal so it reaches one of those two places soon. a thread that was
// somewhere else keeps the signal pending until its next syscall; FEX bounds that wait with a fault
// page and we do not.

/**
 * @brief tgkill(2)/tkill(2): raise a guest signal on a guest thread, this one or another.
 *
 * @return 0, or a negative errno. signal 0 is the existence probe `pthread_kill(t, 0)` makes and
 * raises nothing.
 */
uint64_t SignalGuestThread(int32_t TID, int Signal);

/**
 * @brief Deliver a pending signal, if there is one, on the way out of a guest syscall.
 *
 * called for every syscall this thread makes. at this point CPUState describes the guest exactly
 * and the host layer holds no lock, so it is the safe delivery point for a thread that was
 * somewhere unredirectable when the signal arrived -- including one blocked in a host call, which
 * the interrupting signal has just brought back with EINTR.
 *
 * @param Number the syscall the guest asked for, and @param Result what it returned. both are
 * needed because delivery has to finish the syscall by hand: the JIT is not coming back.
 *
 * does not return if it delivers.
 */
void DeliverPendingAtSyscallExit(GuestThread& T, uint64_t Number, uint64_t Result);

///< the same, for rt_sigreturn, which has already put guest state exactly where it wants it. the
///< restored mask is what makes this a delivery point at all: a signal that arrived blocked during
///< a handler becomes deliverable the instant the handler returns.
void DeliverPendingNow(GuestThread& T);

///< set_tid_address(2). returns this thread's tid, as the syscall does.
uint64_t SetTidAddress(int32_t* TidPtr);

///< block until every guest thread other than this one has finished.
void WaitForOthers();

///< the status a guest thread passed to exit_group, if one did.
bool ProcessExitRequested(int* Status);

/**
 * @brief What to print when the process is torn down from a thread that is not the initial one.
 *
 * exit_group can come from any guest thread, and only the initial one returns up through main()
 * to the code that reports how the run went. rather than duplicate that reporting, the driver
 * hands it over once and whichever thread ends up ending the process calls it.
 */
void SetSummaryCallback(void (*Summary)());

uint64_t LiveCount();
uint64_t CreatedCount();
uint64_t UnalignedFixupCount();

///< whether an unaligned access is backpatched to a half-barrier atomic rather than a plain
///< non-atomic sequence. it follows FEXCore's own `HalfBarrierTSOEnabled`, and the answer is only
///< settled once the fault handlers are installed.
bool UnalignedHandlerIsAtomic();

///< how many times a thread's call-return shadow stack pointer was reset after walking off one of
///< its guard pages. FEX treats that fault as routine, and this number is what says whether the
///< host layer handling it mattered on any given run.
uint64_t CallRetResetCount();

///< what the asynchronous signal path did. Deferred is the number worth watching: it counts the
///< times a thread was interrupted somewhere it could not be redirected from, which is the one
///< case where delivery waits for the target's next syscall rather than happening at once.
struct AsyncSignalStats {
  uint64_t Raised;   ///< signals raised on a guest thread by another one
  uint64_t Deferred; ///< interrupts that landed somewhere unredirectable and had to wait
};
AsyncSignalStats AsyncStats();

/**
 * @brief Trace the asynchronous signal path -- raise, defer, deliver, sigreturn.
 *
 * a handful of lines per run rather than a firehose, because these events are rare. it exists to
 * answer one question `--trace` cannot: what the guest's handler did to the frame. CoreCLR's GC
 * suspension works by *rewriting the RIP in the ucontext* it was handed, so a delivery whose
 * sigreturn comes back at a different RIP than it left at is the runtime redirecting itself, and
 * that is invisible from a syscall log.
 */
void SetSignalTrace(bool Enabled);
bool SignalTrace();

/**
 * @brief Where an asynchronous signal is allowed to be delivered.
 *
 * this matters because of how delivery works here: we abandon the host frames and re-enter
 * `ExecuteThread` at the *guest* RIP that the host PC maps back to. that is only sound at a guest
 * instruction boundary. land in the middle of the arm64 sequence implementing one guest
 * instruction and the guest registers are half-updated while RIP still points at the start of it,
 * so resuming re-runs the instruction over its own partial results. it is an expensive mistake to
 * make: nothing errors, and the corruption surfaces far from the interrupt that caused it.
 *
 * - SyscallOnly (the default): syscall exits alone. sound by construction -- the guest chose that
 *   boundary itself -- and it is what gets `Dreaming Sarah` through its boot. what it does not do
 *   is reach a thread that spins in translated code without ever calling anything, which is case 1
 *   of the `asyncsig` regression guest.
 * - SafePoint: FEXCore's interrupt fault page. the JIT stores to it at every block entry and ahead
 *   of every backward branch, so arming it PROT_NONE turns the next of those into a fault we can
 *   deliver from. it covers the spin case and the regression guest passes all three routes on it --
 *   but the game still dies under it, and a back-edge check is *not* the boundary a block entry is:
 *   the host PC there maps back to a guest instruction that has already run. not trusted yet.
 * - Block: the original behaviour, delivering anywhere inside a translated block. kept because it is
 *   what *shows* the bug -- it is not safe and is not a supported mode.
 */
enum class AsyncSite {
  SyscallOnly,
  SafePoint,
  Block,
};
void SetAsyncSite(AsyncSite Site);

///< true if the chosen site needs FEXCore to emit the interrupt-page check into every block. main
///< asks this before InitCore, because it changes how blocks are compiled.
bool AsyncNeedsInterruptCheck();

void PrintFaultReport(const GuestThread& T);

// --- pausing the guest ---------------------------------------------------------------------------
//
// **every guest thread stops where it holds no host lock, and nowhere else.** that is the same two
// places an asynchronous signal is delivered from, for the same reason: inside translated code, where
// the frames below belong to FEX's dispatcher and nothing of ours is held, and on the way out of a
// syscall, where CPUState describes the guest exactly and the host layer has let go of everything.
// a thread stopped anywhere else could be holding bionic's allocator, a driver's queue lock or
// FEXCore's code cache, and the app's own threads -- which keep running -- would then wait on it.
//
// stopping in translated code needs no redirection at all. the poke's handler simply does not
// return until the pause is over, and then the thread carries on from the instruction it was
// interrupted at. that is why this is sound where delivering a signal from the same place is not.

/**
 * @brief Stop the guest, without waiting for it to have stopped.
 *
 * a thread running guest code is poked and parks in the handler. one interrupted somewhere else --
 * FEXCore's compiler, bionic, the middle of a syscall implementation -- is poked again until it is
 * caught somewhere it may stop. one inside a syscall is left where it is: blocked, it is already
 * stopped, and if it wakes it parks on the way out. a host thread of the host layer's does the
 * poking and says once how long it took and where every thread ended up.
 */
void Pause();

///< let every parked thread go. a thread that was inside a syscall when it was poked makes the call
///< again, so the guest never sees the interruption.
void Resume();

bool Paused();

///< how long the guest has spent paused, in total. the guest's monotonic clocks leave it out, the way
///< a linux program's leave out a suspend -- see the clock syscalls in linux_syscalls.cpp. it moves
///< only at Resume, while every guest thread is still parked.
uint64_t PausedNanos();

///< where the threads of the current or last pause stopped, and how many were still running when the
///< thread finishing it last looked. what `--pause-selftest` asserts on.
struct PauseCounts {
  uint32_t InCode;
  uint32_t AtSyscall;
  uint32_t StillRunning;
};
PauseCounts LastPauseCounts();

///< the entry half of the bracket a guest syscall is made inside. see GuestThread::InSyscall.
void EnterSyscall(GuestThread& T);

/**
 * @brief The exit half: park if the guest is paused, and say whether the call has to be made again.
 *
 * @return true when @param Result is an EINTR the pause caused rather than one the guest's own
 * signal explains, and @param Repeatable says the call may be made twice. the caller then issues the
 * same syscall again, which is what the guest would have seen had it never been interrupted. the
 * bracket stays open across that second call.
 */
bool LeaveSyscall(GuestThread& T, uint64_t Result, bool Repeatable);

} // namespace Threads

} // namespace HostLayer
