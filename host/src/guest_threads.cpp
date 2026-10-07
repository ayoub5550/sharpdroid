#include "guest_threads.h"
#include "vma_tracker.h"

#include <FEXCore/Config/Config.h>
#include <FEXCore/Core/CodeCache.h>
#include <FEXCore/Core/Context.h>
#include <FEXCore/Core/CoreState.h>
#include <FEXCore/Core/SignalDelegator.h>
#include <FEXCore/Debug/InternalThreadState.h>
#include <FEXCore/Utils/ArchHelpers/Arm64.h>
#include <FEXCore/Utils/LongJump.h>
#include <FEXCore/Utils/TypeDefines.h>

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <condition_variable>
#include <csignal>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <dlfcn.h>
#include <limits.h>
#include <linux/futex.h>
#include <linux/sched.h>
#include <mutex>
#include <sched.h>
#include <signal.h>
#include <sys/mman.h>
#include <sys/syscall.h>
#include <ucontext.h>
#include <unistd.h>
#include <vector>

namespace HostLayer::Threads {

namespace {

FEXCore::Context::Context* CTX {};
GuestSignals* Signals {};
const FEXCore::SignalDelegatorConfig* Config {};
void (*SummaryCallback)() {};

// the guest thread running on *this* host thread. one guest thread never migrates between host
// threads and no host thread ever runs two, so this is the whole of the mapping. the host fault
// handler has nothing else to go on -- it is handed a ucontext and a signal number, and must decide
// which thread's code buffers and which thread's signal mask apply.
//
// FEXCore's InternalThreadState::FrontendPtr is set to the same value, and exists for exactly this
// purpose. it is the better answer wherever a CpuStateFrame is in hand, since it cannot be stale.
thread_local GuestThread* CurrentGuestThread {};

std::atomic<uint64_t> UnalignedFixups {};

// how an unaligned access is backpatched, settled once in InstallProcessFaultHandlers.
//
// **the two are not equally safe and the ladder is what chooses.** a half-barrier atomic keeps the
// ordering x86 promises across the misaligned access; a non-atomic one is a plain load or store,
// which is faster and can tear under another thread. the default is the safe one, so a run that
// asks for nothing is backpatched exactly as it always was.
FEXCore::ArchHelpers::Arm64::UnalignedHandlerType UnalignedHandler {
  FEXCore::ArchHelpers::Arm64::UnalignedHandlerType::HalfBarrier};
std::atomic<uint64_t> ThreadsCreated {};

///< how often a guest thread's call-return shadow stack was reset after walking off a guard page.
///< reported because the whole question about this mechanism is whether it happens at all: a run
///< that reports zero says the gap it closes is theoretical, and a run that reports thousands says
///< that without it the host layer would be handing FEX's own bookkeeping faults to the guest as
///< crashes.
std::atomic<uint64_t> CallRetResets {};

// one lock covering thread bookkeeping and the two handshakes that use it: a parent waiting for a
// new child to publish its tid, and a thread waiting for every other thread to finish.
std::mutex ThreadLock;
std::condition_variable ThreadChanged;
uint64_t LiveThreads {};

// every thread whose FEXCore state is currently alive, and a lock of its own.
//
// deliberately *not* ThreadLock. this list is walked from the SIGSEGV handler, where the VMA
// tracker has to drop each thread's cached translations, and ThreadLock is held across condition
// variable waits during the clone handshake. keeping the registry a leaf -- held for the duration
// of one insert, erase or walk, with nothing acquired inside it -- is what makes that walk safe.
//
// a thread joins when its InternalThreadState exists and leaves before that state is destroyed,
// which is narrower than the LiveThreads count either side of it.
std::mutex RegistryLock;
std::vector<GuestThread*> Registry;

void RegisterThread(GuestThread* T) {
  std::lock_guard Lock {RegistryLock};
  Registry.push_back(T);
}

void UnregisterThread(GuestThread* T) {
  std::lock_guard Lock {RegistryLock};
  Registry.erase(std::remove(Registry.begin(), Registry.end(), T), Registry.end());
}

bool ExitGroupRequested {};
int ExitGroupStatus {};

// the host signal that makes another guest thread look at its pending mask. it carries no meaning
// of its own -- *which* guest signal was raised is in the target's PendingSignals -- so all it has to
// be is a signal nothing else in this process wants. SIGRTMAX is the far end of the range bionic
// reserves its own from, and is a function call rather than a constant, so this cannot be constexpr.
const int HostInterruptSignal = SIGRTMAX;

std::atomic<uint64_t> AsyncRaises {};
std::atomic<uint64_t> AsyncDeferred {};

bool SignalTraceEnabled {};
AsyncSite AsyncDeliverySite {AsyncSite::SyscallOnly};

// --- the interrupt fault page ---------------------------------------------------------------------
//
// FEXCore reserves a page inside every InternalThreadState and, when the interrupt check is
// enabled, the JIT emits `str xzr, [STATE, #InterruptFaultPage]` at each block entry and ahead of
// each backward branch. those are precisely the places a guest instruction is about to *begin*, so
// taking the page away turns "somewhere in this thread" into "the next guest instruction boundary
// this thread reaches", which is the only place we can resume from.
//
// FEX uses the same page for something else -- deferring signals out of its own critical sections --
// and reads it back RW from its handler. we never enter those sections from here, but we honour
// the same rule about DeferredSignalRefCount, because FEXCore's own code does take them.

// bionic hands out *tagged* heap pointers -- scudo puts a four-bit allocation tag in the top byte
// and the hardware ignores it (TBI). the kernel does not: `si_addr` comes back untagged, so the
// address in a fault and the address of the object that faulted do not compare equal. everything
// that matches one against the other has to strip the tag first, and this was a full debugging
// round on its own.
uint64_t Untag(const void* Pointer) {
  return reinterpret_cast<uint64_t>(Pointer) & 0x00FF'FFFF'FFFF'FFFFULL;
}

void ArmInterruptPage(GuestThread& T) {
  if (!T.Thread) {
    return;
  }
  if (::mprotect(&T.Thread->InterruptFaultPage, sizeof(T.Thread->InterruptFaultPage), PROT_NONE) != 0 && SignalTraceEnabled) {
    std::printf("[sig] arm tid=%d FAILED errno=%d\n", T.TID, errno);
    std::fflush(stdout);
  }
}

void DisarmInterruptPage(GuestThread& T) {
  if (!T.Thread) {
    return;
  }
  ::mprotect(&T.Thread->InterruptFaultPage, sizeof(T.Thread->InterruptFaultPage), PROT_READ | PROT_WRITE);
}

constexpr size_t HostStackSize = 8 * 1024 * 1024;
constexpr size_t SignalStackSize = 256 * 1024;

// FEX keeps a shadow stack of return addresses beside the guest's own stack, so a guest `ret` can
// branch straight back to already-compiled host code instead of going through the block lookup.
//
// mirrors ThreadManager::CreateThread: guard pages either side, and a default position a quarter of
// the way in rather than at the top, so a thread that returns more often than it calls underflows
// into slack instead of into a guard page.
bool SetupCallRetStack(GuestThread& T) {
  constexpr size_t StackSize = FEXCore::Core::InternalThreadState::CALLRET_STACK_SIZE;
  const size_t AllocSize = StackSize + 2 * FEXCore::Utils::FEX_PAGE_SIZE;

  void* Alloc = ::mmap(nullptr, AllocSize, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
  if (Alloc == MAP_FAILED) {
    return false;
  }
  auto* Base = reinterpret_cast<uint8_t*>(Alloc) + FEXCore::Utils::FEX_PAGE_SIZE;
  if (::mprotect(Base, StackSize, PROT_READ | PROT_WRITE) != 0) {
    ::munmap(Alloc, AllocSize);
    return false;
  }

  T.CallRetAlloc = Alloc;
  T.CallRetAllocSize = AllocSize;
  T.CallRetDefault = reinterpret_cast<uint64_t>(Base) + StackSize / 4;
  T.Thread->CallRetStackBase = Base;
  T.Thread->CurrentFrame->State.callret_sp = T.CallRetDefault;
  return true;
}

// an alternate stack for host faults. per-thread and not shared, for the same reason the kernel
// makes sigaltstack per-thread: two threads faulting at once on one stack would corrupt each
// other's signal frames. `sigaltstack` itself is per-thread state, so this has to be re-done on
// every host thread rather than once at startup.
bool SetupSignalStack(GuestThread& T) {
  void* Stack = ::mmap(nullptr, SignalStackSize, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
  if (Stack == MAP_FAILED) {
    return false;
  }
  T.SignalStack = Stack;
  T.SignalStackSize = SignalStackSize;

  stack_t AltStack {};
  AltStack.ss_sp = Stack;
  AltStack.ss_size = SignalStackSize;
  return ::sigaltstack(&AltStack, nullptr) == 0;
}

// the default 64-bit segment setup, for a thread with no parent to inherit one from.
void SetupGuest64BitSegments(GuestThread& T) {
  auto& State = T.Thread->CurrentFrame->State;
  // index 6 is what the linux kernel uses for user CS; <<3 because the low 3 bits of a segment
  // selector are the RPL and table-indicator bits, not part of the index.
  State.cs_idx = FEXCore::Core::CPUState::DEFAULT_USER_CS << 3;
  auto* CS = FEXCore::Core::CPUState::GetSegmentFromIndex(State, State.cs_idx);
  FEXCore::Core::CPUState::SetGDTBase(CS, 0);
  FEXCore::Core::CPUState::SetGDTLimit(CS, 0xF'FFFFU);
  CS->L = 1; // long mode -- this is the bit the decoder actually reads
  CS->D = 0; // reserved when L is set
  State.cs_cached = FEXCore::Core::CPUState::CalculateGDTBase(*CS);
}

// point a thread's CPUState at its own GDT. this has to happen after any CPUState copy, because
// CreateThread's inherited state carries the *parent's* segment_arrays pointers --
// ContextImpl::CreateThread memcpy's the whole CPUState -- and a child left pointing at its
// parent's GDT would keep working right up until the parent exited and freed it.
void AttachSegmentArrays(GuestThread& T) {
  auto& State = T.Thread->CurrentFrame->State;
  State.segment_arrays[FEXCore::Core::CPUState::SEGMENT_ARRAY_INDEX_GDT] = &T.GDT[0];
  // FEX mirrors LDT onto the GDT by default; not strictly correct, but it matches upstream and
  // avoids a second null array to fall through.
  State.segment_arrays[FEXCore::Core::CPUState::SEGMENT_ARRAY_INDEX_LDT] = &T.GDT[0];
}

void ReleaseThreadResources(GuestThread& T) {
  if (T.SignalStack) {
    stack_t Disable {};
    Disable.ss_flags = SS_DISABLE;
    ::sigaltstack(&Disable, nullptr);
    ::munmap(T.SignalStack, T.SignalStackSize);
    T.SignalStack = nullptr;
  }
  if (T.CallRetAlloc) {
    ::munmap(T.CallRetAlloc, T.CallRetAllocSize);
    T.CallRetAlloc = nullptr;
  }
  if (T.Thread) {
    CTX->DestroyThread(T.Thread);
    T.Thread = nullptr;
  }
}

// CLONE_CHILD_CLEARTID, and the only reason pthread_join ever returns: the kernel zeroes this word
// when the thread dies and wakes anything waiting on it as a futex. we are the kernel here, so if
// we skip this every join in the guest hangs forever.
void ClearChildTIDAndWake(GuestThread& T) {
  if (!T.ClearChildTID) {
    return;
  }
  __atomic_store_n(T.ClearChildTID, 0, __ATOMIC_SEQ_CST);
  ::syscall(SYS_futex, T.ClearChildTID, FUTEX_WAKE, INT_MAX, 0, 0, 0);
  T.ClearChildTID = nullptr;
}

[[noreturn]] void EndProcess(int Status) {
  if (SummaryCallback) {
    SummaryCallback();
  }
  std::fflush(stdout);
  ::_exit(Status & 0xFF);
}

// --- pausing --------------------------------------------------------------------------------------
//
// one word every parked thread waits on, as a futex: non-zero while the guest is paused. a futex
// rather than a condition variable because one of the two places a thread parks is a signal handler,
// where the only safe way to wait is the raw syscall.

std::atomic<uint32_t> PauseWord {};
// moves on every Pause and every Resume, so the thread finishing one pause can tell that it has been
// overtaken by a resume, or by a resume and another pause.
std::atomic<uint64_t> PauseGeneration {};
std::atomic<uint64_t> PausedAtNanos {};
std::atomic<uint64_t> PausedTotalNanos {};
// where the threads of the current pause stopped, said when it ends. **the liveness counter for the
// first of the two places**: a game whose threads are all blocked when it is paused never exercises
// the poke landing in translated code, and without this a pause that worked only at syscall exits
// would look exactly like one that works everywhere.
std::atomic<uint32_t> ParksInCode {};
std::atomic<uint32_t> ParksAtSyscall {};
// how many threads the finishing thread found still running guest code on its last walk.
std::atomic<uint32_t> PauseStragglers {};

uint64_t MonotonicNanos() {
  timespec Now {};
  ::clock_gettime(CLOCK_MONOTONIC, &Now);
  return static_cast<uint64_t>(Now.tv_sec) * 1000000000ull + static_cast<uint64_t>(Now.tv_nsec);
}

// waits out the pause on this thread. safe in a signal handler: nothing here allocates, locks or
// prints.
void Park(GuestThread& T, bool InCode) {
  (InCode ? ParksInCode : ParksAtSyscall).fetch_add(1, std::memory_order_relaxed);
  T.Parked.store(true, std::memory_order_release);
  while (PauseWord.load(std::memory_order_acquire) != 0) {
    ::syscall(SYS_futex, reinterpret_cast<uint32_t*>(&PauseWord), FUTEX_WAIT_PRIVATE, 1, nullptr, nullptr, 0);
  }
  T.Parked.store(false, std::memory_order_release);
}

// the other place, which is a syscall exit or the top of a thread's dispatch loop -- both of them
// outside translated code with nothing of the host layer's held.
//
// the check is a plain load, since every syscall exit makes it: a pause it misses finds the thread
// running on its next walk and pokes it.
void ParkIfPaused(GuestThread& T) {
  if (PauseWord.load(std::memory_order_relaxed) != 0) [[unlikely]] {
    Park(T, false);
  }
}

// the host thread that finishes a pause: it pokes every thread that is still running guest code until
// none is, then says where everything ended up. it holds the registry lock only for one walk at a
// time, and that lock is a leaf, so poking under it is what SignalGuestThread already does.
void* FinishPause(void* Arg) {
  const auto Generation = static_cast<uint64_t>(reinterpret_cast<uintptr_t>(Arg));
  const uint64_t Started = MonotonicNanos();
  // five seconds is generous against a pause that takes a millisecond or two. a thread still running
  // past it is worth naming rather than poking forever.
  constexpr uint64_t GiveUpNanos = 5'000'000'000ull;
  for (unsigned Round = 1;; ++Round) {
    if (PauseGeneration.load(std::memory_order_acquire) != Generation) {
      return nullptr;
    }
    unsigned Live = 0, Parked = 0, Blocked = 0, Running = 0;
    char Stragglers[160] = "";
    size_t Used = 0;
    {
      std::lock_guard Lock {RegistryLock};
      for (auto* T : Registry) {
        // a thread registered before its host thread has published a tid. it parks at the top of
        // its own dispatch loop, before it runs a single guest instruction.
        if (!T->TID) {
          continue;
        }
        ++Live;
        if (T->Parked.load(std::memory_order_acquire)) {
          ++Parked;
          continue;
        }
        if (T->InSyscall.load(std::memory_order_relaxed)) {
          ++Blocked;
          continue;
        }
        ++Running;
        if (Used < sizeof(Stragglers) - 16) {
          Used += std::snprintf(Stragglers + Used, sizeof(Stragglers) - Used, " %d", T->TID);
        }
        T->PauseKicked.store(true, std::memory_order_seq_cst);
        ::syscall(SYS_tgkill, ::getpid(), T->TID, HostInterruptSignal);
      }
    }
    PauseStragglers.store(Running, std::memory_order_relaxed);
    const uint64_t Elapsed = MonotonicNanos() - Started;
    if (!Running || Elapsed > GiveUpNanos) {
      if (PauseGeneration.load(std::memory_order_acquire) != Generation) {
        return nullptr;
      }
      if (Running) {
        std::printf("[host-layer] paused, but %u of %u guest threads are still running after %.1f s:%s\n", Running,
                    Live, static_cast<double>(Elapsed) / 1e9, Stragglers);
      } else {
        std::printf("[host-layer] paused: %u guest threads, %u parked and %u inside a syscall, after %.2f ms and %u "
                    "round(s)\n",
                    Live, Parked, Blocked, static_cast<double>(Elapsed) / 1e6, Round);
      }
      std::fflush(stdout);
      return nullptr;
    }
    // quickly at first, since nearly every thread is caught by the first poke, then less often for a
    // thread that keeps being found in host code.
    ::usleep(Round < 10 ? 1000 : 10000);
  }
}

// --- delivering an asynchronous signal -----------------------------------------------------------
//
// every caller below has already established that CPUState describes the guest: either it was
// never left (a syscall boundary, a sigreturn) or it has just been reconstructed out of a host
// signal context. all that is left is to build the frame and go.

[[noreturn]] void EnterGuestHandler(GuestThread& T, int Signal) {
  // the siginfo linux writes for a signal one thread sent another. si_code is negative, which is
  // how a guest tells a raised signal from a faulted one -- and is why TrapNumberForSignal reports
  // no trap for these.
  siginfo_t Info {};
  Info.si_signo = Signal;
  Info.si_code = SI_TKILL;
  Info.si_pid = ::getpid();
  Info.si_uid = ::getuid();

  // one signal has just been taken; the page follows whatever is left. delivering at a syscall
  // boundary is what usually leaves it armed with nothing behind it, and a page left armed costs a
  // fault at the next block entry rather than being wrong -- but there is no reason to pay it.
  if (AsyncDeliverySite == AsyncSite::SafePoint) {
    if (Signals->HasDeliverablePending(T)) {
      ArmInterruptPage(T);
    } else {
      DisarmInterruptPage(T);
    }
  }

  Signals->DeliverToGuest(T, Signal, &Info);
  T.Reason = Escape::Restart;
  siglongjmp(T.EscapeHatch, 1);
  __builtin_unreachable();
}

// a signal the guest has no handler for. linux would terminate the process and the shell would
// report 128 + the signal number, so that is what we do -- SIG_IGN and the signals whose default is
// to do nothing were already dropped by TakePending and never reach here.
[[noreturn]] void DieOnUnhandledSignal(GuestThread& T, int Signal) {
  std::printf("[host-layer] thread %d took signal %d with no handler installed\n", T.TID, Signal);
  ExitCurrent(128 + Signal, true);
  __builtin_unreachable();
}

// --- the host interrupt handler ------------------------------------------------------------------
//
// runs on the *target* thread, wherever it happened to be. its whole job is to decide whether that
// is a place the thread can be redirected from, and to leave the signal pending if it is not.

void GuestInterruptHandler(int Sig, siginfo_t*, void* UContext) {
  if (SignalTraceEnabled) {
    VMA::NoteSignal(Sig);
  }
  GuestThread* T = CurrentGuestThread;
  if (!T || !T->Thread) {
    // a host thread that is not running guest code. nothing to deliver to, and nothing this
    // signal could mean here.
    return;
  }

  // a pause is asked about first. it wants the thread stopped, and a guest signal waiting as well
  // is still waiting when the pause is over -- which is where the code below picks it up.
  if (PauseWord.load(std::memory_order_acquire) != 0) {
    // a poke that lands here rather than inside a blocking call has interrupted nothing, so there is
    // no EINTR on its way for the syscall exit to account for.
    if (!T->InSyscall.load(std::memory_order_seq_cst)) {
      T->PauseKicked.store(false, std::memory_order_relaxed);
    }
    // translated code and FEX's dispatcher are the places with nothing of ours held -- the same
    // test the fault handler makes before treating a fault as the guest's, plus the refcount FEXCore
    // raises around its own critical sections. anywhere else, the thread is left to reach a syscall
    // exit or to be found in translated code by the next poke.
    const uint64_t PC = static_cast<ucontext_t*>(UContext)->uc_mcontext.pc;
    if (T->Thread->CurrentFrame->State.DeferredSignalRefCount.Load() == 0 &&
        ((PC >= Config->DispatcherBegin && PC < Config->DispatcherEnd) || CTX->IsAddressInCodeBuffer(T->Thread, PC))) {
      Park(*T, true);
    }
  }

  if (!Signals->HasDeliverablePending(*T)) {
    // already taken at a syscall boundary, or blocked and waiting for the guest's own mask to
    // change. neither is a reason to disturb anything.
    return;
  }

  auto* Context = static_cast<ucontext_t*>(UContext);
  const uint64_t HostPC = Context->uc_mcontext.pc;

  // inside a translated block, guest state is recoverable from the host registers and the frames
  // below belong to FEX's dispatcher, which we are entitled to abandon -- that is exactly the
  // position a guest fault leaves us in, and it is handled the same way.
  //
  // the refcount is asked first because it is a plain load of thread-local memory, where
  // IsAddressInCodeBuffer walks FEXCore's buffer lists -- and a thread inside one of FEXCore's own
  // signal-deferring sections is exactly the thread whose buffer lists may be mid-edit.
  //
  // under SafePoint this is never taken: the interrupt page armed at the raise is what brings the
  // thread to a boundary, and this handler's only remaining job is to have interrupted whatever
  // blocking host call the thread was parked in.
  const bool Redirectable = AsyncDeliverySite == AsyncSite::Block &&
                            T->Thread->CurrentFrame->State.DeferredSignalRefCount.Load() == 0 &&
                            CTX->IsAddressInCodeBuffer(T->Thread, HostPC);
  if (!Redirectable) {
    // the thread is somewhere with host locks and host frames a longjmp would strand -- inside one
    // of our syscalls, inside FEXCore's compiler, in bionic. leave the bit set: a thread in a
    // syscall checks on the way out, and the host call it was parked in has just been interrupted
    // so that it gets there.
    //
    // what that does not bound is a thread that was in FEXCore's own code and then goes back to
    // running already-compiled guest code without ever making a syscall. it keeps the signal
    // pending until it does. FEX closes this with a fault page and we have not.
    AsyncDeferred.fetch_add(1, std::memory_order_relaxed);
    if (SignalTraceEnabled) {
      std::printf("[sig] defer tid=%d hostpc=0x%llx refcount=%llu\n", T->TID, static_cast<unsigned long long>(HostPC),
                  static_cast<unsigned long long>(T->Thread->CurrentFrame->State.DeferredSignalRefCount.Load()));
      std::fflush(stdout);
    }
    return;
  }

  const int Signal = Signals->TakePending(*T);
  if (!Signal) {
    return;
  }
  if (!Signals->HasHandler(Signal)) {
    DieOnUnhandledSignal(*T, Signal);
  }
  Signals->ReconstructGuestState(*T, UContext);
  if (SignalTraceEnabled) {
    std::printf("[sig] deliver tid=%d sig=%d site=block rip=0x%llx rsp=0x%llx\n", T->TID, Signal,
                static_cast<unsigned long long>(T->Thread->CurrentFrame->State.rip),
                static_cast<unsigned long long>(T->Thread->CurrentFrame->State.gregs[FEXCore::X86State::REG_RSP]));
    std::fflush(stdout);
  }
  EnterGuestHandler(*T, Signal);
}

// --- the host fault handler --------------------------------------------------------------------

// the FPSIMD record of an arm64 signal context, found by walking the record chain in
// mcontext_t::__reserved (guest_signals.cpp has the read-only twin and the reasoning). the JIT
// restart below has to *write* d8-d15 back, since they are callee-saved across the long jump.
struct FPSimdRecord {
  uint32_t Magic;
  uint32_t Size;
  uint32_t FPSR;
  uint32_t FPCR;
  __uint128_t VRegs[32];
};

static FPSimdRecord* FindMutableFPSimd(ucontext_t* Context) {
  // the kernel header's FPSIMD_MAGIC, spelled out for the same reason guest_signals.cpp does: it is a
  // macro there, and this must not depend on which asm/ header happened to be pulled in first.
  constexpr uint32_t FPSimdMagic = 0x46508001;
  size_t Offset = 0;
  while (Offset + 8 <= sizeof(Context->uc_mcontext.__reserved)) {
    auto* Record = reinterpret_cast<FPSimdRecord*>(&Context->uc_mcontext.__reserved[Offset]);
    if (Record->Magic == FPSimdMagic) {
      return Record;
    }
    if (Record->Size == 0) {
      break;
    }
    Offset += Record->Size;
  }
  return nullptr;
}

///< how many compiles outgrew their scratch buffer and were restarted with more -- reported at exit.
std::atomic<uint64_t> JITSpaceRestarts {0};

void GuestFaultHandler(int Signal, siginfo_t* Info, void* UContext) {
  if (SignalTraceEnabled) {
    VMA::NoteSignal(Signal);
  }
  auto* Context = static_cast<ucontext_t*>(UContext);
  const uint64_t HostPC = Context->uc_mcontext.pc;

  GuestThread* T = CurrentGuestThread;
  if (!T) {
    // a fault on a host thread that is not running guest code at all. there is no guest to hand it
    // to and no escape hatch to jump through, so restore the default disposition and return: the
    // instruction re-faults and the process dies the way it would have without us.
    struct sigaction Default {};
    Default.sa_handler = SIG_DFL;
    ::sigaction(Signal, &Default, nullptr);
    return;
  }

  // the JIT outgrowing its scratch buffer, which is not a fault either: it is FEXCore asking for a
  // retry. a block is emitted into a temporary buffer sized from its IR (24 bytes per SSA node)
  // with a PROT_NONE page after it; a block that expands further than that -- per-instruction
  // validation under SMCChecks=full, long vector or x87 runs -- writes into the guard page, and the
  // compile is meant to be restarted with twice the room. Arm64JITCore::CompileCode arms
  // `RestartJump` for exactly this, and FEX's own frontend fires it from its SIGSEGV handler
  // (SignalDelegator::HandleFrontendSIGSEGV). without the same here, a large enough block took the
  // whole process down as a "host-side crash" inside the emitter. rewriting the context so that
  // returning from the handler *is* the long jump keeps it out of FEXCore's C++ frames entirely.
  if (Signal == SIGSEGV && Info->si_code == SEGV_ACCERR && T->Thread && T->Thread->JITGuardPage) {
    const uint64_t Fault = Untag(Info->si_addr);
    const uint64_t Guard = T->Thread->JITGuardPage;
    if (Fault >= Guard && Fault < Guard + FEXCore::Utils::FEX_PAGE_SIZE) {
      if (auto* FPSimd = FindMutableFPSimd(Context)) {
        FEXCore::UncheckedLongJump::ManuallyLoadJumpBuf(T->Thread->RestartJump, T->Thread->JITGuardOverflowArgument,
                                                        reinterpret_cast<uint64_t*>(&Context->uc_mcontext.regs[0]),
                                                        FPSimd->VRegs, reinterpret_cast<uint64_t*>(&Context->uc_mcontext.pc));
        JITSpaceRestarts.fetch_add(1, std::memory_order_relaxed);
        return;
      }
    }
  }

  // the interrupt fault page, asked before anything else because it is not a fault at all -- it is
  // this thread arriving at a guest instruction boundary with a signal waiting for it.
  if (AsyncDeliverySite == AsyncSite::SafePoint && Signal == SIGSEGV && Info->si_code == SEGV_ACCERR && T->Thread &&
      Untag(Info->si_addr) == Untag(&T->Thread->InterruptFaultPage)) {
    // two ways to arrive here that are not delivery points, and both are stepped over rather than
    // acted on. the page stays armed, so the thread simply faults again at the next one.
    //
    // the first is FEXCore's own code. `DeferredSignalRefCountGuard`'s destructor stores to this
    // very page on the way out of every signal-deferring section -- that is FEX's mechanism for
    // noticing a signal it queued, and FEX can act on it because it resumes the interrupted host
    // context exactly. we cannot: acting here would siglongjmp out of FEXCore's C++ frames with
    // its locks held. **this is what made the whole game die earlier and differently than before
    // the safe-point work, and it is the one thing about this mechanism that is not obvious.**
    //
    // the second is a nested deferring section, where the refcount says an outer one is still
    // open. FEX steps the four-byte `str` here too.
    //
    // skipping the store itself is free: nothing ever reads this page, its only purpose is to
    // fault, and every arm64 instruction is four bytes.
    if (!CTX->IsAddressInCodeBuffer(T->Thread, HostPC) || T->Thread->CurrentFrame->State.DeferredSignalRefCount.Load() != 0) {
      Context->uc_mcontext.pc = HostPC + 4;
      return;
    }

    DisarmInterruptPage(*T);
    if (!Signals->HasDeliverablePending(*T)) {
      // the raise armed the page and the syscall-exit path got there first, or the guest has since
      // blocked the signal. either way there is nothing to do and the store may now proceed.
      return;
    }

    const int Pending = Signals->TakePending(*T);
    if (!Pending) {
      return;
    }
    if (!Signals->HasHandler(Pending)) {
      DieOnUnhandledSignal(*T, Pending);
    }
    Signals->ReconstructGuestState(*T, UContext);
    if (SignalTraceEnabled) {
      std::printf("[sig] deliver tid=%d sig=%d site=safepoint rip=0x%llx rsp=0x%llx\n", T->TID, Pending,
                  static_cast<unsigned long long>(T->Thread->CurrentFrame->State.rip),
                  static_cast<unsigned long long>(T->Thread->CurrentFrame->State.gregs[FEXCore::X86State::REG_RSP]));
      std::fflush(stdout);
    }
    EnterGuestHandler(*T, Pending);
  }

  // FEX's call-return shadow stack running off one of its own guard pages, which is **not an
  // error**: it is the predictor being asked to pop more than it was given, and FEX's own
  // `SyscallHandler::HandleSegfault` treats it as routine -- *"Reset REG_CALLRET_SP to the default
  // location to allow for underflows/overflows"*. resetting the pointer at signal delivery and at
  // re-entry -- where the *contents* stop describing live frames -- is not enough on its own: neither
  // catches the pointer walking out of the mapping, which is what this case is for.
  //
  // this workload makes that walk likely rather than theoretical. SharpEmu resumes a cooperatively
  // blocked guest thread by entering a trampoline that abandons the host stack and `ret`s into the
  // continuation, so every resume returns from a call whose push happened in a frame that no longer
  // exists -- an unmatched pop, hundreds of times a second, all in the same direction. there is
  // 3 MiB of room above the default and 1 MiB below, and the drift is one way.
  //
  // x25 is `REG_CALLRET_SP` on arm64 -- see `Arm64Emitter.h`, where x17 is the ARM64EC variant and is
  // not this build. the number is written out because that header is internal to FEXCore, which is
  // why FEX's own frontend spells it 25 in exactly this place too.
  if (Signal == SIGSEGV && T->CallRetAlloc) {
    const uint64_t Fault = Untag(Info->si_addr);
    const auto Base = reinterpret_cast<uint64_t>(T->CallRetAlloc);
    if (Fault >= Base && Fault < Base + T->CallRetAllocSize) {
      Context->uc_mcontext.regs[25] = T->CallRetDefault;
      const uint64_t Nth = CallRetResets.fetch_add(1, std::memory_order_relaxed) + 1;
      // the summary line at exit cannot answer "does this ever happen", because every measurement
      // on this project is taken by killing the process after N seconds and the summary is never
      // reached. so say it the first time and then logarithmically, which is a handful of lines in
      // the worst case and one line in the case that matters.
      if (Nth == 1 || (Nth & (Nth - 1)) == 0) {
        std::printf("[host-layer] call-return shadow stack reset #%llu, tid %d, fault 0x%llx %s the stack\n",
                    static_cast<unsigned long long>(Nth), T->TID, static_cast<unsigned long long>(Fault),
                    Fault < reinterpret_cast<uint64_t>(T->Thread->CallRetStackBase) ? "below" : "above");
        std::fflush(stdout);
      }
      return;
    }
  }

  // deciding whether a fault belongs to the guest: a PC outside FEX's generated code is a genuine
  // host crash and must never be handed to a guest handler. the dispatcher range alone is not
  // enough -- it covers only FEX's trampoline, whereas faults from guest instructions happen inside
  // JIT'd blocks, which live elsewhere. both count, and the code buffers are asked about *this*
  // thread: another thread's buffers are not evidence about this PC.
  const bool InJitCode =
    (HostPC >= Config->DispatcherBegin && HostPC < Config->DispatcherEnd) || CTX->IsAddressInCodeBuffer(T->Thread, HostPC);

  // a write to a page the VMA tracker sealed is not a fault either -- it is the guest rewriting
  // code we have already translated, which is the entire mechanism behind SMCChecks=mtrack.
  //
  // this is asked *before* deciding whether the PC is in JIT'd code, and on purpose: the host
  // layer's own syscall handlers write into guest memory too, so the fault can arrive from host
  // code with a perfectly ordinary host PC.
  if (Signal == SIGSEGV) {
    const auto Result = VMA::HandleWriteFault(T->Thread, reinterpret_cast<uint64_t>(Info->si_addr), HostPC);
    if (Result == VMA::WriteFault::Resume) {
      return;
    }
    if (Result == VMA::WriteFault::SingleStep) {
      // the guest is rewriting code inside the block it is running. re-running the faulting
      // instruction would carry straight on into the translation just dropped, so spill guest
      // state and re-enter the dispatcher asking for a one-instruction block instead --
      // ENTRY_FILL_SRA_SINGLE_INST_REG is x1, and any non-zero value in it means "single step".
      Signals->ReconstructGuestState(*T, UContext);
      Context->uc_mcontext.pc = Config->AbsoluteLoopTopAddressFillSRA;
      Context->uc_mcontext.regs[1] = 1;
      return;
    }
  }

  // a SIGBUS from JIT'd code is routine, not a crash. x86 permits unaligned access everywhere,
  // including on the atomic and TSO-ordered operations FEX compiles guest memory accesses into
  // -- and arm64's atomics require natural alignment. so the JIT emits the fast aligned form and
  // relies on being corrected the first time it is wrong: HandleUnalignedAccess decodes the
  // faulting instruction, backpatches the code buffer to a sequence that tolerates misalignment,
  // and reports how far to move the PC to re-run it.
  if (Signal == SIGBUS && Info->si_code == BUS_ADRALN && CTX->IsAddressInCodeBuffer(T->Thread, HostPC)) {
    const auto Fixup = FEXCore::ArchHelpers::Arm64::HandleUnalignedAccess(
      // bionic types mcontext regs as __u64 (unsigned long long) while FEXCore asks for
      // uint64_t (unsigned long on LP64) -- same width, distinct types.
      T->Thread, UnalignedHandler, HostPC,
      reinterpret_cast<uint64_t*>(&Context->uc_mcontext.regs[0]));
    if (Fixup.has_value()) {
      Context->uc_mcontext.pc = HostPC + Fixup.value();
      // **said once, because a run that is cut off never reaches the exit summary.** every
      // measurement here ends by killing the process at a fixed number of seconds, so the total
      // below is unreadable on exactly the runs that matter -- and without it a knob that changes
      // how this backpatches cannot be told apart from one that never fires.
      if (UnalignedFixups.fetch_add(1, std::memory_order_relaxed) == 0) {
        std::printf("[host-layer] unaligned access backpatched, to a %s sequence\n",
                    UnalignedHandler == FEXCore::ArchHelpers::Arm64::UnalignedHandlerType::HalfBarrier ?
                      "half-barrier atomic" :
                      "non-atomic");
      }
      return;
    }
  }

  T->Fault.Caught = true;
  T->Fault.Signal = Signal;
  T->Fault.SiCode = Info->si_code;
  T->Fault.HostPC = HostPC;
  T->Fault.FaultAddress = Info->si_addr;
  T->Fault.InJitCode = InJitCode;

  if (InJitCode) {
    const siginfo_t* DeliverInfo = Info;

    // two quite different things arrive here. a *real* fault -- the guest touched a bad address --
    // leaves guest state scattered across host registers, and has to be gathered up. a fault the
    // JIT *generated*, like an invalid opcode, arrives from a dispatcher trampoline that already
    // spilled guest state and recorded what to raise, so gathering would overwrite good state
    // with whatever the trampoline left in those registers.
    if (const int GeneratedSignal = Signals->TakeGeneratedFault(*T)) {
      Signal = GeneratedSignal;
      DeliverInfo = nullptr;
    } else {
      Signals->ReconstructGuestState(*T, UContext);
    }

    T->Fault.Signal = Signal;
    T->Fault.GuestRIP = T->Thread->CurrentFrame->State.rip;
    for (int i = 0; i < 16; ++i) {
      T->Fault.GPR[i] = T->Thread->CurrentFrame->State.gregs[i];
    }

    // hand it to the guest if the guest asked for it. a signal the guest has blocked, or has no
    // handler for, is fatal here exactly as it would be on linux -- with the difference that
    // linux would dump core and we print the state instead.
    if (Signals->HasHandler(Signal) && !Signals->IsBlocked(*T, Signal)) {
      Signals->DeliverToGuest(*T, Signal, DeliverInfo);
      T->Reason = Escape::Restart;
      siglongjmp(T->EscapeHatch, 1);
    }
  }

  // nothing left to hand it to. escape the signal frame entirely rather than returning and
  // re-faulting forever; the driver prints what state it can and gives up.
  T->Reason = Escape::Fault;
  siglongjmp(T->EscapeHatch, 1);
}

// --- starting a cloned thread --------------------------------------------------------------

// the child's side of a two-step handshake with the cloning thread. the child has to publish its
// tid before clone can return -- glibc reads it back out of the TCB immediately, and
// CLONE_PARENT_SETTID promises the word is already written by then -- and must not start executing
// guest code until the parent has finished writing the tid pointers the flags asked for.
void* HostThreadEntry(void* Arg) {
  GuestThread& T = *static_cast<GuestThread*>(Arg);

  T.TID = static_cast<int32_t>(::gettid());
  CurrentGuestThread = &T;
  T.Thread->FrontendPtr = &T;

  const bool StackOk = SetupSignalStack(T);

  {
    std::unique_lock Lock {ThreadLock};
    T.StartPublished = true;
    ThreadChanged.notify_all();
    ThreadChanged.wait(Lock, [&T] { return T.StartReleased; });
  }

  if (!StackOk) {
    std::printf("[host-layer] thread %d: could not install a signal stack\n", T.TID);
  }

  Run(T);

  const bool Group = T.Reason == Escape::ExitedGroup;
  const int Status = T.ExitStatus;

  if (T.Reason == Escape::Fault) {
    std::printf("[host-layer] thread %d died on an unhandled fault\n", T.TID);
    PrintFaultReport(T);
  }

  // the wake has to come before the teardown: a guest thread being joined is waiting on this word,
  // and anything that can fail belongs after the wake rather than in front of it.
  UnregisterThread(&T);
  ClearChildTIDAndWake(T);
  ReleaseThreadResources(T);

  {
    std::lock_guard Lock {ThreadLock};
    --LiveThreads;
    if (Group) {
      ExitGroupRequested = true;
      ExitGroupStatus = Status;
    }
    ThreadChanged.notify_all();
  }

  const int32_t TID = T.TID;
  CurrentGuestThread = nullptr;
  delete &T;

  if (Group) {
    std::printf("[host-layer] thread %d called exit_group, status %d\n", TID, Status);
    EndProcess(Status);
  }
  return nullptr;
}

} // namespace

// --- setup ------------------------------------------------------------------------------------

void Initialize(FEXCore::Context::Context* NewCTX, GuestSignals* NewSignals, const FEXCore::SignalDelegatorConfig* NewConfig) {
  CTX = NewCTX;
  Signals = NewSignals;
  Config = NewConfig;
}

void SetSummaryCallback(void (*Summary)()) {
  SummaryCallback = Summary;
}

void SetSignalTrace(bool Enabled) {
  SignalTraceEnabled = Enabled;
}

bool SignalTrace() {
  return SignalTraceEnabled;
}

void SetAsyncSite(AsyncSite Site) {
  AsyncDeliverySite = Site;
}

bool AsyncNeedsInterruptCheck() {
  return AsyncDeliverySite == AsyncSite::SafePoint;
}

GuestThread* Current() {
  return CurrentGuestThread;
}

void ForEachLive(void (*Fn)(GuestThread&, void*), void* User) {
  std::lock_guard Lock {RegistryLock};
  for (auto* T : Registry) {
    Fn(*T, User);
  }
}

GuestThread* CreateInitial(uint64_t RIP, uint64_t RSP) {
  auto* T = new GuestThread;
  T->Initial = true;
  T->TID = static_cast<int32_t>(::gettid());

  // FEX-2609 dropped CreateThread's RIP and RSP parameters: a thread starts from the state it is
  // given, or a zeroed one, and the entry point and stack are set in it afterwards.
  T->Thread = CTX->CreateThread();
  if (!T->Thread) {
    delete T;
    return nullptr;
  }
  T->Thread->CurrentFrame->State.rip = RIP;
  T->Thread->CurrentFrame->State.gregs[FEXCore::X86State::REG_RSP] = RSP;
  T->Thread->FrontendPtr = T;
  AttachSegmentArrays(*T);
  SetupGuest64BitSegments(*T);
  if (!SetupCallRetStack(*T) || !SetupSignalStack(*T)) {
    ReleaseThreadResources(*T);
    delete T;
    return nullptr;
  }

  CurrentGuestThread = T;
  {
    std::lock_guard Lock {ThreadLock};
    ++LiveThreads;
  }
  RegisterThread(T);
  ThreadsCreated.fetch_add(1, std::memory_order_relaxed);
  return T;
}

void Destroy(GuestThread& T) {
  // out of the registry before the FEXCore state it points at is destroyed, not after.
  UnregisterThread(&T);
  ClearChildTIDAndWake(T);
  ReleaseThreadResources(T);
  {
    std::lock_guard Lock {ThreadLock};
    --LiveThreads;
    ThreadChanged.notify_all();
  }
  if (CurrentGuestThread == &T) {
    CurrentGuestThread = nullptr;
  }
  delete &T;
}

void InstallProcessFaultHandlers() {
  // which sequence an unaligned access is backpatched to, read once here rather than per fault:
  // the handler runs on a signal stack and a config lookup in it would be paid on every first
  // touch of every misaligned address.
  //
  // **this has to be read after the JIT knobs are applied and before the first fault**, which is
  // what this function is between. it is the same choice FEX's own signal delegator makes from the
  // same option, so a run naming nothing gets FEXCore's default of a half-barrier atomic.
  UnalignedHandler = FEXCore::Config::Get_HALFBARRIERTSOENABLED()() ?
                       FEXCore::ArchHelpers::Arm64::UnalignedHandlerType::HalfBarrier :
                       FEXCore::ArchHelpers::Arm64::UnalignedHandlerType::NonAtomic;
  // announced only when it is the unsafe one. the safe choice is what a run naming nothing gets and
  // saying so every time would be noise; dropping the ordering x86 promised is not something a log
  // should be silent about.
  if (UnalignedHandler == FEXCore::ArchHelpers::Arm64::UnalignedHandlerType::NonAtomic) {
    std::printf("[host-layer] unaligned accesses backpatch to non-atomic sequences: ordering the "
                "guest expects is not preserved across them\n");
  }

  // sigaction is process-wide, so this is done once. the *stack* the handler runs on is not, and
  // is set up per thread in SetupSignalStack.
  struct sigaction Action {};
  Action.sa_sigaction = GuestFaultHandler;
  Action.sa_flags = SA_SIGINFO | SA_ONSTACK;
  ::sigemptyset(&Action.sa_mask);
  ::sigaction(SIGSEGV, &Action, nullptr);
  ::sigaction(SIGBUS, &Action, nullptr);
  ::sigaction(SIGILL, &Action, nullptr);
  ::sigaction(SIGFPE, &Action, nullptr);
  ::sigaction(SIGTRAP, &Action, nullptr);

  // and the one signal the host layer sends itself, to make a guest thread notice a signal raised
  // on it from elsewhere.
  //
  // deliberately without SA_RESTART. a guest thread parked in futex or poll has to come back out
  // of that host call for the syscall-exit check to run at all, and SA_RESTART would have the
  // kernel silently re-enter it instead -- which is precisely the thread that most needs waking.
  struct sigaction Interrupt {};
  Interrupt.sa_sigaction = GuestInterruptHandler;
  Interrupt.sa_flags = SA_SIGINFO | SA_ONSTACK;
  ::sigemptyset(&Interrupt.sa_mask);
  ::sigaction(HostInterruptSignal, &Interrupt, nullptr);
}

// --- asynchronous signals -------------------------------------------------------------------------

uint64_t SignalGuestThread(int32_t TID, int Signal) {
  if (Signal < 0 || Signal > 64) {
    return static_cast<uint64_t>(-EINVAL);
  }

  // the registry lock is held across both halves on purpose. it is what stops the target from
  // finishing, unregistering and being deleted between being found and being written to -- and it
  // is a leaf, taken for the duration of one walk with nothing acquired inside it, so holding it
  // over a tgkill cannot deadlock against the handler that tgkill runs.
  std::lock_guard Lock {RegistryLock};

  GuestThread* Target {};
  for (auto* T : Registry) {
    if (T->TID == TID) {
      Target = T;
      break;
    }
  }
  if (!Target) {
    return static_cast<uint64_t>(-ESRCH);
  }
  // signal 0 raises nothing and only reports whether the thread is there, which is what
  // pthread_kill(t, 0) is for.
  if (Signal == 0) {
    return 0;
  }

  Signals->SetPending(*Target, Signal);
  AsyncRaises.fetch_add(1, std::memory_order_relaxed);
  if (SignalTraceEnabled) {
    std::printf("[sig] raise from=%d to=%d sig=%d\n", static_cast<int>(::gettid()), TID, Signal);
    std::fflush(stdout);
  }

  // take the target's interrupt page away, so the next block entry or backward branch it executes
  // faults and becomes a delivery point. this is what bounds the wait for a thread that is off
  // running already-compiled guest code and will not make a syscall of its own accord.
  if (AsyncDeliverySite == AsyncSite::SafePoint) {
    ArmInterruptPage(*Target);
  }

  // and make the thread look. not for ourselves: this thread is standing in its own syscall
  // handler, so it is already on its way to the exit check that will find the bit.
  if (TID != static_cast<int32_t>(::gettid())) {
    ::syscall(SYS_tgkill, ::getpid(), TID, HostInterruptSignal);
  }
  return 0;
}

void DeliverPendingAtSyscallExit(GuestThread& T, uint64_t Number, uint64_t Result) {
  const int Signal = Signals->TakePending(T);
  if (!Signal) {
    return;
  }
  if (!Signals->HasHandler(Signal)) {
    DieOnUnhandledSignal(T, Signal);
  }

  auto& State = T.Thread->CurrentFrame->State;

  // the syscall has to be finished by hand, because this path never gets back to CompleteSyscall
  // (syscall_args.h). RIP is still *at* the two-byte `syscall` -- FEX hands the handler the state as
  // it was when the instruction began (OpDispatchBuilder::SyscallOp) -- so stepping it is ours, and
  // RAX is ours to write.
  //
  // unless the signal asked for the call to be restarted, in which case the step is simply not
  // taken and the syscall number goes back where the guest put it. that is exactly what linux does
  // for SA_RESTART, and it is what keeps an interrupted futex or poll from ever showing the guest
  // an EINTR it did not expect.
  if (static_cast<int64_t>(Result) == -EINTR && Signals->WantsRestart(Signal)) {
    State.gregs[FEXCore::X86State::REG_RAX] = Number;
  } else {
    State.gregs[FEXCore::X86State::REG_RAX] = Result;
    State.rip += 2;
  }

  if (SignalTraceEnabled) {
    std::printf("[sig] deliver tid=%d sig=%d site=syscall(%llu) rip=0x%llx rsp=0x%llx\n", T.TID, Signal,
                static_cast<unsigned long long>(Number), static_cast<unsigned long long>(State.rip),
                static_cast<unsigned long long>(State.gregs[FEXCore::X86State::REG_RSP]));
    std::fflush(stdout);
  }
  EnterGuestHandler(T, Signal);
}

void DeliverPendingNow(GuestThread& T) {
  const int Signal = Signals->TakePending(T);
  if (!Signal) {
    return;
  }
  if (!Signals->HasHandler(Signal)) {
    DieOnUnhandledSignal(T, Signal);
  }
  EnterGuestHandler(T, Signal);
}

// --- pausing the guest ----------------------------------------------------------------------------

void Pause() {
  if (PauseWord.exchange(1, std::memory_order_acq_rel) != 0) {
    return;
  }
  PausedAtNanos.store(MonotonicNanos(), std::memory_order_relaxed);
  ParksInCode.store(0, std::memory_order_relaxed);
  ParksAtSyscall.store(0, std::memory_order_relaxed);
  PauseStragglers.store(0, std::memory_order_relaxed);
  const uint64_t Generation = PauseGeneration.fetch_add(1, std::memory_order_acq_rel) + 1;
  // the poking happens on a thread of its own so that the caller -- the app's UI thread, leaving the
  // foreground -- is not held for it. detached: it ends by itself, and a resume makes it end early.
  pthread_t Finisher {};
  if (::pthread_create(&Finisher, nullptr, FinishPause, reinterpret_cast<void*>(static_cast<uintptr_t>(Generation))) == 0) {
    ::pthread_detach(Finisher);
  } else {
    std::printf("[host-layer] pause: could not start the thread that stops the guest\n");
  }
}

void Resume() {
  if (PauseWord.load(std::memory_order_acquire) == 0) {
    return;
  }
  // **the clock first, then the threads.** the total is what the guest's monotonic clock leaves
  // out, and moving it while every guest thread is still parked is what makes the step invisible:
  // no thread reads the clock either side of it.
  const uint64_t Held = MonotonicNanos() - PausedAtNanos.load(std::memory_order_relaxed);
  PausedTotalNanos.fetch_add(Held, std::memory_order_release);
  if (PauseWord.exchange(0, std::memory_order_acq_rel) == 0) {
    return;
  }
  PauseGeneration.fetch_add(1, std::memory_order_acq_rel);
  ::syscall(SYS_futex, reinterpret_cast<uint32_t*>(&PauseWord), FUTEX_WAKE_PRIVATE, INT_MAX, nullptr, nullptr, 0);
  // counted while parked, so a thread that never woke from the call it was blocked in is in neither.
  std::printf("[host-layer] resumed after %.1f s paused. %u thread(s) had stopped in translated code and %u at "
              "a syscall exit\n",
              static_cast<double>(Held) / 1e9, ParksInCode.load(std::memory_order_relaxed),
              ParksAtSyscall.load(std::memory_order_relaxed));
  std::fflush(stdout);
}

bool Paused() {
  return PauseWord.load(std::memory_order_acquire) != 0;
}

// a plain load, because the clock syscalls ask on every call. it moves only while every thread that
// could read it is parked, and leaving the park is a futex wake, which orders it.
uint64_t PausedNanos() {
  return PausedTotalNanos.load(std::memory_order_relaxed);
}

PauseCounts LastPauseCounts() {
  return {ParksInCode.load(std::memory_order_relaxed), ParksAtSyscall.load(std::memory_order_relaxed),
          PauseStragglers.load(std::memory_order_relaxed)};
}

// **every guest syscall pays for these two, so they are plain loads and stores.** nothing about the
// pause needs them ordered: the thread finishing a pause walks the threads again every few
// milliseconds, so a stale reading either way costs one poke the exit below accounts for, or one
// walk's delay. ordered versions of the same three accesses cost 15 ns on every syscall, measured.
void EnterSyscall(GuestThread& T) {
  T.InSyscall.store(true, std::memory_order_relaxed);
}

bool LeaveSyscall(GuestThread& T, uint64_t Result, bool Repeatable) {
  // read before it is cleared, so an ordinary syscall exit never pays for an atomic exchange. the
  // poke that set it is a signal, and taking one is a barrier of its own.
  const bool Kicked = T.PauseKicked.load(std::memory_order_relaxed) &&
                      T.PauseKicked.exchange(false, std::memory_order_relaxed);
  ParkIfPaused(T);
  // **an EINTR the guest's own signal does not explain is the pause's.** the only host signal a
  // guest thread is sent that can interrupt a call is the poke, and a poke raising a guest signal
  // leaves that signal pending -- so with nothing pending, this EINTR was caused by a pause that
  // found the thread running and then reached it just inside a blocking call.
  if (Repeatable && Kicked && static_cast<int64_t>(Result) == -EINTR && !Signals->HasDeliverablePending(T)) {
    return true;
  }
  T.InSyscall.store(false, std::memory_order_relaxed);
  return false;
}

// --- the run loop -----------------------------------------------------------------------------
//
// delivering a signal means abandoning the host call frames of whatever JIT'd block was running
// and re-entering `ExecuteThread`, which dispatches from `State.rip`. FEX does not do it this
// way: it rewrites the host signal context to land at the dispatcher's SRA-fill entry point and
// stashes a copy of the host context on the guest stack, so `rt_sigreturn` can resume the
// interrupted host frame exactly -- which in turn requires being inside a host signal handler at
// sigreturn time, which FEX arranges by signalling itself.
//
// re-entering from the top is much less machinery and is correct for what a signal return
// actually needs: resumption at a *guest* instruction boundary, which is all the x86-64 signal
// ABI ever promises. what it gives up is resuming mid-block, so a host-level fault that needed to
// restart a partially executed guest instruction could not be handled this way. nothing needs
// that yet -- the unaligned-access path handles its own case entirely inside the signal frame.
//
// one thing does have to be reset on re-entry: FEX's call-return shadow stack. its contents
// describe host code addresses in call frames that no longer exist. a stale entry is not unsafe --
// `ret` compares the popped guest address against the real one and falls back to the block lookup
// on a mismatch -- but leaving the pointer where it was would let it drift toward a guard page
// across many signals.
void Run(GuestThread& T) {
  for (;;) {
    // the escape hatch belongs to this thread and lives in its GuestThread, so a fault on one
    // thread cannot land in another thread's dispatch loop. a single file-scope
    // sigjmp_buf would put every thread's fault in the same one.
    if (sigsetjmp(T.EscapeHatch, 1) != 0) {
      if (T.Reason != Escape::Restart) {
        return;
      }
      T.Thread->CurrentFrame->State.callret_sp = T.CallRetDefault;
      // rt_sigreturn and a signal delivered from inside a syscall both leave through here rather
      // than through the syscall's exit, so this is where their bracket closes. left open, a pause
      // would take this thread for one blocked in a syscall and never stop it.
      T.InSyscall.store(false, std::memory_order_release);
    }
    T.Reason = Escape::Returned;
    // a thread born during a pause, or re-dispatched into one by a signal, parks before it runs a
    // single guest instruction.
    ParkIfPaused(T);
    CTX->ExecuteThread(T.Thread);

    // ExecuteThread can also come back carrying a generated fault rather than because the guest
    // stopped. that is a consequence of EnableExitOnHLT: with it set, the dispatcher's
    // generated-SIGSEGV trampoline unwinds out of ExecuteThread instead of raising a host signal
    // (the GuestSignal_SIGSEGV handler Dispatcher::EmitDispatcher emits). without checking here,
    // a guest #GP would look like a guest that quietly ran off the end of itself.
    const int GeneratedSignal = Signals->TakeGeneratedFault(T);
    if (!GeneratedSignal) {
      return;
    }

    T.Fault.Caught = true;
    T.Fault.InJitCode = true;
    T.Fault.Signal = GeneratedSignal;
    T.Fault.GuestRIP = T.Thread->CurrentFrame->State.rip;
    T.Fault.FaultAddress = reinterpret_cast<void*>(T.Thread->CurrentFrame->State.rip);
    for (int i = 0; i < 16; ++i) {
      T.Fault.GPR[i] = T.Thread->CurrentFrame->State.gregs[i];
    }

    if (!Signals->HasHandler(GeneratedSignal) || Signals->IsBlocked(T, GeneratedSignal)) {
      T.Reason = Escape::Fault;
      return;
    }
    Signals->DeliverToGuest(T, GeneratedSignal, nullptr);
    T.Thread->CurrentFrame->State.callret_sp = T.CallRetDefault;
  }
}

// --- clone ------------------------------------------------------------------------------------

uint64_t Clone(FEXCore::Core::CpuStateFrame* Frame, uint64_t Flags, uint64_t StackPtr, int32_t* ParentTID, int32_t* ChildTID, uint64_t TLS) {
  // without CLONE_THREAD this is a fork, and a fork of a JIT means duplicating code buffers and a
  // shared code cache into a child that no longer has the threads that own them. glibc only ever
  // asks for it from posix_spawn and fork(), neither of which .NET needs to get off the ground,
  // so it is honestly refused rather than half-implemented.
  if (!(Flags & CLONE_THREAD)) {
    return static_cast<uint64_t>(-ENOSYS);
  }
  // the sharing flags glibc's pthread_create always passes together with CLONE_THREAD. we make
  // every one of them true by construction -- same process, same address space, same fd table --
  // so the check is only here to catch a guest asking for something we would silently not do.
  constexpr uint64_t Required = CLONE_VM | CLONE_FS | CLONE_FILES | CLONE_SIGHAND;
  if ((Flags & Required) != Required) {
    std::printf("[host-layer] clone: CLONE_THREAD without the shared-resource flags (0x%llX)\n", static_cast<unsigned long long>(Flags));
    return static_cast<uint64_t>(-EINVAL);
  }

  // the cloning thread, found through the frame rather than through the thread_local: with a
  // CpuStateFrame in hand this cannot be stale or belong to the wrong thread, and FrontendPtr is
  // reserved by FEXCore for exactly this kind of back-pointer.
  auto* Parent = static_cast<GuestThread*>(Frame->Thread->FrontendPtr);
  if (!Parent) {
    return static_cast<uint64_t>(-EINVAL);
  }

  auto* T = new GuestThread;

  // inherit the parent's whole CPUState, then correct the parts a new thread does not inherit.
  // CreateThread memcpy's the state it is given (ContextImpl::CreateThread), so the corrections
  // can only come after it.
  T->Thread = CTX->CreateThread(&Frame->State);
  if (!T->Thread) {
    delete T;
    return static_cast<uint64_t>(-ENOMEM);
  }
  auto& State = T->Thread->CurrentFrame->State;

  // the GDT comes across from the parent -- a thread inherits its segments -- but into the child's
  // own storage, and the pointers have to be re-aimed there because the memcpy above brought the
  // parent's addresses with it.
  std::memcpy(T->GDT, Parent->GDT, sizeof(T->GDT));
  AttachSegmentArrays(*T);

  // clone returns 0 in the child and the child's tid in the parent. that is the entire mechanism
  // by which the two halves of a `clone` call site tell themselves apart.
  State.gregs[FEXCore::X86State::REG_RAX] = 0;
  State.gregs[FEXCore::X86State::REG_RSP] = StackPtr;

  // RIP still points *at* the `syscall` instruction: FEX hands the syscall handler the state as it
  // was when the instruction began. the parent is stepped past it when its handler returns
  // (CompleteSyscall, syscall_args.h); the child never returns through that handler, so it has to
  // be stepped over the two bytes of `0F 05` here.
  State.rip += 2;

  if (Flags & CLONE_SETTLS) {
    // on x86-64 the thread pointer is the FS base, and FEX keeps that in CPUState rather than in a
    // descriptor -- the same field arch_prctl(ARCH_SET_FS) writes.
    State.fs_cached = TLS;
  }

  if (!SetupCallRetStack(*T)) {
    CTX->DestroyThread(T->Thread);
    delete T;
    return static_cast<uint64_t>(-ENOMEM);
  }

  if (Flags & CLONE_CHILD_CLEARTID) {
    T->ClearChildTID = ChildTID;
  }

  // the parent's signal mask is inherited; its alternate stack is not, which is what linux does --
  // a new thread starts with no altstack of its own.
  T->BlockedMask = Parent->BlockedMask;

  pthread_attr_t Attr;
  ::pthread_attr_init(&Attr);
  // 8 MiB to match FEX's own guest threads. this is the *host* stack, and FEXCore compiles blocks
  // on whichever thread first hits them -- the IR builder and register allocator run here -- so
  // bionic's default is not obviously enough and being wrong about it would look like a random
  // crash deep inside the JIT.
  ::pthread_attr_setstacksize(&Attr, HostStackSize);
  ::pthread_attr_setdetachstate(&Attr, PTHREAD_CREATE_DETACHED);

  {
    std::lock_guard Lock {ThreadLock};
    ++LiveThreads;
  }
  // registered before the host thread exists, because the child's very first compiled block can
  // be invalidated by another thread and a window where it is not in the registry is a window
  // where it keeps a stale translation.
  RegisterThread(T);

  if (::pthread_create(&T->Host, &Attr, HostThreadEntry, T) != 0) {
    ::pthread_attr_destroy(&Attr);
    UnregisterThread(T);
    {
      std::lock_guard Lock {ThreadLock};
      --LiveThreads;
    }
    ReleaseThreadResources(*T);
    delete T;
    return static_cast<uint64_t>(-EAGAIN);
  }
  ::pthread_attr_destroy(&Attr);

  // wait for the child to publish its tid. clone cannot return before this: glibc reads the tid
  // back out of the TCB as soon as it has it, and CLONE_PARENT_SETTID promises the word is already
  // written by then.
  {
    std::unique_lock Lock {ThreadLock};
    ThreadChanged.wait(Lock, [T] { return T->StartPublished; });
  }

  const int32_t TID = T->TID;

  // both tid pointers are guest addresses, which are host addresses, so they are written directly.
  // this happens before the child is released, so a child that immediately reads its own tid out
  // of its TCB sees it there.
  if ((Flags & CLONE_PARENT_SETTID) && ParentTID) {
    *ParentTID = TID;
  }
  if ((Flags & CLONE_CHILD_SETTID) && ChildTID) {
    *ChildTID = TID;
  }

  {
    std::lock_guard Lock {ThreadLock};
    T->StartReleased = true;
    ThreadChanged.notify_all();
  }

  ThreadsCreated.fetch_add(1, std::memory_order_relaxed);
  return static_cast<uint64_t>(TID);
}

// --- leaving ------------------------------------------------------------------------------------

void ExitCurrent(int Status, bool Group) {
  GuestThread* T = CurrentGuestThread;
  if (!T) {
    // nothing to unwind to. should not happen, but exiting is the one thing that must never get
    // stuck.
    EndProcess(Status);
  }

  T->ExitStatus = Status;
  T->Reason = Group ? Escape::ExitedGroup : Escape::Exited;

  // longjmp out of the syscall handler, which the JIT called from inside a translated block. this
  // abandons FEXCore's dispatcher frame without unwinding it -- which is what FEX does too
  // (LongjumpDeallocateAndExit, in LinuxSyscalls' Thread.cpp), because there is no way back into
  // a guest thread that has asked to stop existing.
  siglongjmp(T->EscapeHatch, 1);
  __builtin_unreachable();
}

void RestartCurrent() {
  GuestThread* T = CurrentGuestThread;
  if (!T) {
    std::fprintf(stderr, "[host-layer] restart requested from a host thread with no guest\n");
    ::_exit(1);
  }
  T->Reason = Escape::Restart;
  siglongjmp(T->EscapeHatch, 1);
  __builtin_unreachable();
}

uint64_t SetTidAddress(int32_t* TidPtr) {
  GuestThread* T = CurrentGuestThread;
  if (T) {
    // set_tid_address moves the word CLONE_CHILD_CLEARTID nominated. glibc calls it once during
    // startup on the initial thread, which is how the *first* thread gets a join word at all --
    // nothing cloned it, so nothing could have passed one.
    T->ClearChildTID = TidPtr;
    return static_cast<uint64_t>(T->TID);
  }
  return static_cast<uint64_t>(::gettid());
}

void WaitForOthers() {
  std::unique_lock Lock {ThreadLock};
  if (LiveThreads <= 1) {
    return;
  }
  std::printf("[host-layer] waiting for %llu other guest thread(s)\n", static_cast<unsigned long long>(LiveThreads - 1));
  ThreadChanged.wait(Lock, [] { return LiveThreads <= 1; });
}

bool ProcessExitRequested(int* Status) {
  std::lock_guard Lock {ThreadLock};
  if (ExitGroupRequested && Status) {
    *Status = ExitGroupStatus;
  }
  return ExitGroupRequested;
}

uint64_t LiveCount() {
  std::lock_guard Lock {ThreadLock};
  return LiveThreads;
}

uint64_t CreatedCount() {
  return ThreadsCreated.load(std::memory_order_relaxed);
}

uint64_t UnalignedFixupCount() {
  return UnalignedFixups.load(std::memory_order_relaxed);
}

bool UnalignedHandlerIsAtomic() {
  return UnalignedHandler == FEXCore::ArchHelpers::Arm64::UnalignedHandlerType::HalfBarrier;
}

uint64_t CallRetResetCount() {
  return CallRetResets.load(std::memory_order_relaxed);
}

uint64_t JITSpaceRestartCount() {
  return JITSpaceRestarts.load(std::memory_order_relaxed);
}

AsyncSignalStats AsyncStats() {
  return {
    AsyncRaises.load(std::memory_order_relaxed),
    AsyncDeferred.load(std::memory_order_relaxed),
  };
}

// --- reporting ----------------------------------------------------------------------------------

namespace {

// a fault that is *not* in JIT'd code is a host crash, and "host PC = 0x63..." on its own says
// nothing about where. dladdr names the shared object and the nearest exported symbol; the maps
// line is the fallback, because FEX's own code buffers are anonymous mappings that no symbol
// table describes. resolved here rather than in the handler -- this runs on an ordinary stack,
// after the fact.
void DescribeHostAddress(uint64_t Addr) {
  Dl_info Info {};
  if (::dladdr(reinterpret_cast<void*>(Addr), &Info) && Info.dli_fname) {
    std::printf("[host-layer]   host PC is in %s", Info.dli_fname);
    if (Info.dli_sname) {
      std::printf(" %s+0x%llX", Info.dli_sname, static_cast<unsigned long long>(Addr - reinterpret_cast<uint64_t>(Info.dli_saddr)));
    }
    // and the link-time address, which is what llvm-addr2line wants. our own binary is a PIE with
    // static FEXCore inside it, so "in sharpdroid-host-layer" spans everything from the syscall
    // table to the JIT's compiler and is not on its own an answer.
    std::printf(" (file offset 0x%llX)\n", static_cast<unsigned long long>(Addr - reinterpret_cast<uint64_t>(Info.dli_fbase)));
    return;
  }

  std::FILE* Maps = std::fopen("/proc/self/maps", "re");
  if (!Maps) {
    return;
  }
  char Line[512];
  while (std::fgets(Line, sizeof(Line), Maps)) {
    unsigned long long Begin = 0, End = 0;
    if (std::sscanf(Line, "%llx-%llx", &Begin, &End) == 2 && Addr >= Begin && Addr < End) {
      std::printf("[host-layer]   host PC is in mapping: %s", Line);
      break;
    }
  }
  std::fclose(Maps);
}

// the guest bytes at a faulting RIP. for SIGILL this is the whole answer -- FEXCore's decoder
// rejected an opcode and nothing else in the report says which one -- and for a SIGSEGV it at least
// says what instruction was doing the access.
//
// guest and host share one address space 1:1, so this is a plain read of T.Fault.GuestRIP. it is
// still checked against /proc/self/maps first: the reporter runs after a fatal fault and must not
// itself fault, and a wild RIP is exactly the case where it would.
void PrintGuestBytes(uint64_t RIP) {
  if (!RIP) {
    return;
  }

  std::FILE* Maps = std::fopen("/proc/self/maps", "re");
  if (!Maps) {
    return;
  }
  uint64_t MappingEnd = 0;
  char Line[512];
  while (std::fgets(Line, sizeof(Line), Maps)) {
    unsigned long long Begin = 0, End = 0;
    if (std::sscanf(Line, "%llx-%llx", &Begin, &End) == 2 && RIP >= Begin && RIP < End) {
      MappingEnd = End;
      break;
    }
  }
  std::fclose(Maps);
  if (!MappingEnd) {
    std::printf("[host-layer]   bytes @rip  = <not mapped>\n");
    return;
  }

  // an x86-64 instruction is at most 15 bytes; 16 gives the next opcode too, which is what tells
  // you whether the decoder stopped where you think it did.
  const uint64_t Available = std::min<uint64_t>(16, MappingEnd - RIP);
  const auto* Bytes = reinterpret_cast<const uint8_t*>(RIP);
  std::printf("[host-layer]   bytes @rip  =");
  for (uint64_t i = 0; i < Available; ++i) {
    std::printf(" %02X", Bytes[i]);
  }
  std::printf("\n");
}

} // namespace

void PrintFaultReport(const GuestThread& T) {
  static const char* const GuestRegisterNames[16] {
    "RAX", "RBX", "RCX", "RDX", "RSI", "RDI", "RBP", "RSP", "R8", "R9", "R10", "R11", "R12", "R13", "R14", "R15",
  };

  std::printf("[host-layer] guest fault on thread %d: signal %d code %d at %p\n", T.TID, T.Fault.Signal, T.Fault.SiCode,
              T.Fault.FaultAddress);
  std::printf("[host-layer]   in JIT code = %d, host PC = 0x%llX\n", T.Fault.InJitCode, static_cast<unsigned long long>(T.Fault.HostPC));
  if (!T.Fault.InJitCode) {
    DescribeHostAddress(T.Fault.HostPC);
    // guest state was never gathered, because a host crash has none to gather: the registers
    // below are zeroes from initialisation, not values read out of anywhere.
    std::printf("[host-layer]   (host-side crash -- the guest registers below are not populated)\n");
  }
  std::printf("[host-layer]   guest RIP   = 0x%llX\n", static_cast<unsigned long long>(T.Fault.GuestRIP));
  PrintGuestBytes(T.Fault.GuestRIP);
  const int Count = std::min<int>(Config->SRAGPRCount, 16);
  for (int i = 0; i < Count; ++i) {
    if (i % 2 == 0) {
      std::printf("[host-layer]  ");
    }
    std::printf(" %-3s = 0x%016llX%s", GuestRegisterNames[i], static_cast<unsigned long long>(T.Fault.GPR[i]), (i % 2) ? "\n" : "");
  }
  if (Count % 2) {
    std::printf("\n");
  }
}

} // namespace HostLayer::Threads
