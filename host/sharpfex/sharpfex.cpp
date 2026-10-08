// libsharpfex -- see sharpfex.h. a proof of concept: one context, guest threads bound to the host
// threads that create them, HLE imports through a syscall trap, host->guest calls through
// HandleCallback + CALLBACKRET, and a per-thread escape hatch for blocking.
//
// lifted from the host layer rather than linked against it: thread setup (guest_threads.cpp's
// CreateInitial), the JIT scratch-buffer restart and the unaligned-access backpatch
// (GuestFaultHandler). what the host layer does and this does not: the linux syscall surface, the
// ELF loader, guest signals, the VMA tracker's SMC write protection, and both thunks. none of them
// is needed when the process around FEX is a native SharpEmu.

#include "sharpfex.h"

#include "fex_threads.h"
#include "host_features.h"

#include <FEXCore/Config/Config.h>
#include <FEXCore/Core/CodeCache.h>
#include <FEXCore/Core/Context.h>
#include <FEXCore/Core/CoreState.h>
#include <FEXCore/Core/HostFeatures.h>
#include <FEXCore/Core/SignalDelegator.h>
#include <FEXCore/Core/X86Enums.h>
#include <FEXCore/Debug/InternalThreadState.h>
#include <FEXCore/HLE/SyscallHandler.h>
#include <FEXCore/Utils/ArchHelpers/Arm64.h>
#include <FEXCore/Utils/LongJump.h>
#include <FEXCore/Utils/TypeDefines.h>

#include <atomic>
#include <cerrno>
#include <csetjmp>
#include <csignal>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <shared_mutex>
#include <string>
#include <sys/mman.h>
#include <ucontext.h>
#include <unistd.h>
#include <vector>

namespace {
namespace X86 = FEXCore::X86State;

struct SfxThread {
  FEXCore::Core::InternalThreadState* Thread {};
  FEXCore::Core::CPUState::gdt_segment GDT[32] {};
  void* CallRetAlloc {};
  size_t CallRetAllocSize {};
  uint64_t CallRetDefault {};
  sigjmp_buf EscapeHatch;
  int32_t Reason {SFX_RUN_RETURNED};
  int32_t CallDepth {}; // sfx_call nesting; blocking is refused inside a host->guest call
  bool InRun {};
  uint64_t PendingHle {}; // counted here and flushed in batches: no shared cache line per import
};

thread_local SfxThread* CurrentThread;

fextl::unique_ptr<FEXCore::Context::Context> CTX;
bool AvxLayout {};
uint64_t CallbackReturnAddress {};
uint64_t SlotBase {};
uint64_t SlotEnd {};
uint32_t SlotStride {16};
sfx_hle_fn Handler {};
void* HandlerUser {};

constexpr uint32_t MaxSlotFns = 64;
struct SlotFn {
  sfx_slot_fn Fn;
  void* User;
};
SlotFn SlotFns[MaxSlotFns] {};
std::string Description;
// what sfx_call saves around a host->guest call. "light" (the default) keeps what an import
// boundary needs: gprs, rip, flags, mxcsr and the call-return stack. at an import boundary the
// caller-saved vector state is dead (SysV) and xmm0-7 come back from the handler's sfx_regs anyway.
// "full" also keeps every vector, avx-high and mmx register: for injecting a call at an arbitrary
// guest instruction, which nothing in this PoC does. sfx_init option `sfx.CallSave=light|full`.
bool CallSaveFull {};

struct ExecRange {
  uint64_t Base;
  uint64_t Size;
};
std::shared_mutex RangesLock;
std::vector<ExecRange> Ranges;

struct {
  std::atomic<uint64_t> HleCalls, Callbacks, Blocks, Transfers, Foreign, Unaligned, Restarts, Chained;
} Stats {};

FEXCore::ArchHelpers::Arm64::UnalignedHandlerType UnalignedHandler {FEXCore::ArchHelpers::Arm64::UnalignedHandlerType::HalfBarrier};
struct sigaction PreviousSEGV {}, PreviousBUS {};

class Delegator final : public FEXCore::SignalDelegator {
public:
  uintptr_t GetThunkCallbackRET() const override {
    return CallbackReturnAddress;
  }
};
Delegator Signals;

uint64_t& Xmm(FEXCore::Core::CPUState& State, int Index, int Half) {
  return AvxLayout ? State.xmm.avx.data[Index][Half] : State.xmm.sse.data[Index][Half];
}

void StateToRegs(FEXCore::Core::CPUState& State, sfx_regs* Regs) {
  std::memcpy(Regs->gpr, State.gregs, sizeof(Regs->gpr));
  Regs->rip = State.rip;
  Regs->mxcsr = State.mxcsr;
  Regs->reserved = 0;
  for (int i = 0; i < 8; ++i) {
    Regs->xmm[i][0] = Xmm(State, i, 0);
    Regs->xmm[i][1] = Xmm(State, i, 1);
  }
}

void RegsToState(const sfx_regs* Regs, FEXCore::Core::CPUState& State) {
  std::memcpy(State.gregs, Regs->gpr, sizeof(Regs->gpr));
  State.rip = Regs->rip;
  State.mxcsr = Regs->mxcsr;
  for (int i = 0; i < 8; ++i) {
    Xmm(State, i, 0) = Regs->xmm[i][0];
    Xmm(State, i, 1) = Regs->xmm[i][1];
  }
}

// the part of CPUState that is guest-visible. L1Pointer/L1Mask belong to the thread's lookup cache,
// which can be resized while a callback runs, so they are never rolled back.
struct LightGuestState {
  uint64_t Rip;
  uint64_t Gregs[16];
  uint64_t CallRetSp;
  uint32_t Mxcsr;
  uint32_t PfRaw, AfRaw;
  uint8_t Flags[48];
};

struct SavedGuestState {
  uint64_t Rip;
  uint64_t Gregs[16];
  uint64_t CallRetSp;
  uint64_t AvxHigh[16][2];
  FEXCore::Core::CPUState::XMMRegs Xmm;
  uint32_t Mxcsr;
  uint32_t PfRaw, AfRaw;
  uint8_t Flags[48];
  uint64_t Mm[8][2];
};

void Save(const FEXCore::Core::CPUState& S, SavedGuestState& Out) {
  Out.Rip = S.rip;
  std::memcpy(Out.Gregs, S.gregs, sizeof(Out.Gregs));
  Out.CallRetSp = S.callret_sp;
  std::memcpy(Out.AvxHigh, S.avx_high, sizeof(Out.AvxHigh));
  std::memcpy(&Out.Xmm, &S.xmm, sizeof(Out.Xmm));
  Out.Mxcsr = S.mxcsr;
  Out.PfRaw = S.pf_raw;
  Out.AfRaw = S.af_raw;
  std::memcpy(Out.Flags, S.flags, sizeof(Out.Flags));
  std::memcpy(Out.Mm, S.mm, sizeof(Out.Mm));
}

void Restore(const SavedGuestState& In, FEXCore::Core::CPUState& S) {
  S.rip = In.Rip;
  std::memcpy(S.gregs, In.Gregs, sizeof(In.Gregs));
  S.callret_sp = In.CallRetSp;
  std::memcpy(S.avx_high, In.AvxHigh, sizeof(In.AvxHigh));
  std::memcpy(&S.xmm, &In.Xmm, sizeof(In.Xmm));
  S.mxcsr = In.Mxcsr;
  S.pf_raw = In.PfRaw;
  S.af_raw = In.AfRaw;
  std::memcpy(S.flags, In.Flags, sizeof(In.Flags));
  std::memcpy(S.mm, In.Mm, sizeof(In.Mm));
}

class HleSyscalls final : public FEXCore::HLE::SyscallHandler {
public:
  void HandleSyscall(FEXCore::Core::CpuStateFrame* Frame) override {
    auto& State = Frame->State;
    // FEX stores the address of the `syscall` itself (SyscallOp: GetRelocatedPC(Op, -InstSize)).
    const uint64_t SyscallRip = State.rip;
    if (SyscallRip < SlotBase || SyscallRip >= SlotEnd || !Handler) {
      Stats.Foreign.fetch_add(1, std::memory_order_relaxed);
      State.gregs[X86::REG_RAX] = static_cast<uint64_t>(-ENOSYS);
      State.rip = SyscallRip + 2;
      return;
    }
    const uint32_t Index = static_cast<uint32_t>((SyscallRip - SlotBase) / SlotStride);
    const uint64_t Rsp = State.gregs[X86::REG_RSP];
    // put back what `syscall` overwrote; the stub parked both in its red zone.
    State.gregs[X86::REG_RCX] = *reinterpret_cast<const uint64_t*>(Rsp - 8);
    State.gregs[X86::REG_R11] = *reinterpret_cast<const uint64_t*>(Rsp - 16);
    // the frame knows its thread, so no TLS lookup on the hot path.
    auto* T = static_cast<SfxThread*>(Frame->Thread->FrontendPtr);
    if (++T->PendingHle == 4096) {
      Stats.HleCalls.fetch_add(T->PendingHle, std::memory_order_relaxed);
      T->PendingHle = 0;
    }

    if (Index < MaxSlotFns && SlotFns[Index].Fn) {
      SlotFns[Index].Fn(SlotFns[Index].User, State.gregs);
      State.rip = SyscallRip + 2;
      return;
    }

    sfx_regs Regs;
    StateToRegs(State, &Regs);
    const uint64_t ReturnAddress = *reinterpret_cast<const uint64_t*>(Rsp);
    Regs.rip = ReturnAddress;

    const int32_t Action = Handler(HandlerUser, Index, &Regs);

    // the handler's frame has returned, so acting on its answer never unwinds managed code.
    const uint64_t NewRip = Regs.rip;
    RegsToState(&Regs, State);
    if (NewRip != ReturnAddress) {
      // context transfer (longjmp, fiber switch, continuation): the handler set rip and rsp.
      Stats.Transfers.fetch_add(1, std::memory_order_relaxed);
      State.rip = NewRip;
    } else {
      // carry on at the stub's `ret`, which FEX links through the call-return stack.
      State.rip = SyscallRip + 2;
    }

    if (Action == SFX_CONTINUE) {
      return;
    }
    if (!T->InRun || T->CallDepth != 0) {
      // nested inside a host->guest call there is no escape hatch that leaves only FEX frames
      // behind; that case is the design's open problem (one hatch per nesting depth).
      State.gregs[X86::REG_RAX] = static_cast<uint64_t>(-EDEADLK);
      return;
    }
    if (Action == SFX_BLOCK) {
      Stats.Blocks.fetch_add(1, std::memory_order_relaxed);
      T->Reason = SFX_RUN_BLOCKED;
    } else {
      T->Reason = SFX_RUN_EXITED;
    }
    Frame->InSyscallInfo = 0;
    siglongjmp(T->EscapeHatch, 1);
  }

  FEXCore::HLE::ExecutableRangeInfo QueryGuestExecutableRange(FEXCore::Core::InternalThreadState*, uint64_t Address) override {
    std::shared_lock Lock {RangesLock};
    for (const auto& R : Ranges) {
      if (Address >= R.Base && Address < R.Base + R.Size) {
        return {R.Base, R.Size, false};
      }
    }
    return {0, 0, false};
  }

  std::optional<FEXCore::ExecutableFileSectionInfo> LookupExecutableFileSection(FEXCore::Core::InternalThreadState*, uint64_t) override {
    return std::nullopt;
  }
};
HleSyscalls Syscalls;

// --- faults: ours first, everything else to whoever was installed before (.NET) ---------------------

struct FPSimdRecord {
  uint32_t Magic;
  uint32_t Size;
  uint32_t FPSR;
  uint32_t FPCR;
  __uint128_t VRegs[32];
};

FPSimdRecord* FindMutableFPSimd(ucontext_t* Context) {
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

uint64_t Untag(const void* Pointer) {
  return reinterpret_cast<uint64_t>(Pointer) & ((1ULL << 56) - 1);
}

void Chain(const struct sigaction& Previous, int Signal, siginfo_t* Info, void* UContext) {
  Stats.Chained.fetch_add(1, std::memory_order_relaxed);
  if (Previous.sa_flags & SA_SIGINFO) {
    if (Previous.sa_sigaction) {
      Previous.sa_sigaction(Signal, Info, UContext);
      return;
    }
  } else if (Previous.sa_handler != SIG_DFL && Previous.sa_handler != SIG_IGN) {
    Previous.sa_handler(Signal);
    return;
  }
  // nobody wanted it: die the way the process would have without us.
  struct sigaction Default {};
  Default.sa_handler = SIG_DFL;
  ::sigaction(Signal, &Default, nullptr);
}

void FaultHandler(int Signal, siginfo_t* Info, void* UContext) {
  auto* Context = static_cast<ucontext_t*>(UContext);
  const uint64_t HostPC = Context->uc_mcontext.pc;
  SfxThread* T = CurrentThread;

  if (T && T->Thread) {
    // the JIT outgrowing its scratch buffer: a retry, not a fault (guest_threads.cpp has the story).
    if (Signal == SIGSEGV && Info->si_code == SEGV_ACCERR && T->Thread->JITGuardPage) {
      const uint64_t Fault = Untag(Info->si_addr);
      const uint64_t Guard = T->Thread->JITGuardPage;
      if (Fault >= Guard && Fault < Guard + FEXCore::Utils::FEX_PAGE_SIZE) {
        if (auto* FPSimd = FindMutableFPSimd(Context)) {
          FEXCore::UncheckedLongJump::ManuallyLoadJumpBuf(T->Thread->RestartJump, T->Thread->JITGuardOverflowArgument,
                                                          reinterpret_cast<uint64_t*>(&Context->uc_mcontext.regs[0]), FPSimd->VRegs,
                                                          reinterpret_cast<uint64_t*>(&Context->uc_mcontext.pc));
          Stats.Restarts.fetch_add(1, std::memory_order_relaxed);
          return;
        }
      }
    }
    // x86 allows unaligned atomics, arm64 does not: backpatch the block and re-run.
    if (Signal == SIGBUS && Info->si_code == BUS_ADRALN && CTX->IsAddressInCodeBuffer(T->Thread, HostPC)) {
      const auto Fixup = FEXCore::ArchHelpers::Arm64::HandleUnalignedAccess(T->Thread, UnalignedHandler, HostPC,
                                                                            reinterpret_cast<uint64_t*>(&Context->uc_mcontext.regs[0]));
      if (Fixup.has_value()) {
        Context->uc_mcontext.pc = HostPC + Fixup.value();
        Stats.Unaligned.fetch_add(1, std::memory_order_relaxed);
        return;
      }
    }
  }
  Chain(Signal == SIGBUS ? PreviousBUS : PreviousSEGV, Signal, Info, UContext);
}

void FlushStats(SfxThread& T) {
  if (T.PendingHle) {
    Stats.HleCalls.fetch_add(T.PendingHle, std::memory_order_relaxed);
    T.PendingHle = 0;
  }
}

void BuiltinAdd(void*, uint64_t* Gpr) {
  Gpr[X86::REG_RAX] = Gpr[X86::REG_RDI] + Gpr[X86::REG_RSI];
}

void BuiltinNull(void*, uint64_t* Gpr) {
  Gpr[X86::REG_RAX] = 0;
}

bool SetupCallRetStack(SfxThread& T) {
  constexpr size_t StackSize = FEXCore::Core::InternalThreadState::CALLRET_STACK_SIZE;
  const size_t AllocSize = StackSize + 2 * FEXCore::Utils::FEX_PAGE_SIZE;
  void* Alloc = ::mmap(nullptr, AllocSize, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
  if (Alloc == MAP_FAILED) {
    return false;
  }
  auto* Base = static_cast<uint8_t*>(Alloc) + FEXCore::Utils::FEX_PAGE_SIZE;
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

void SetupSegments(SfxThread& T, uint64_t FsBase) {
  auto& State = T.Thread->CurrentFrame->State;
  State.segment_arrays[FEXCore::Core::CPUState::SEGMENT_ARRAY_INDEX_GDT] = &T.GDT[0];
  State.segment_arrays[FEXCore::Core::CPUState::SEGMENT_ARRAY_INDEX_LDT] = &T.GDT[0];
  State.cs_idx = FEXCore::Core::CPUState::DEFAULT_USER_CS << 3;
  auto* CS = FEXCore::Core::CPUState::GetSegmentFromIndex(State, State.cs_idx);
  FEXCore::Core::CPUState::SetGDTBase(CS, 0);
  FEXCore::Core::CPUState::SetGDTLimit(CS, 0xF'FFFFU);
  CS->L = 1;
  CS->D = 0;
  State.cs_cached = FEXCore::Core::CPUState::CalculateGDTBase(*CS);
  // the PS5 TLS base. fs is FEX state, so the guest's `mov reg, fs:[0]` needs no load-time patch.
  State.fs_cached = FsBase;
  State.gs_cached = FsBase;
}

bool ApplyOption(std::string_view Pair);

} // namespace

// FEXCore's option table by json name, as the host layer resolves `--fex`.
namespace {
std::optional<FEXCore::Config::ConfigOption> OptionByName(std::string_view Name) {
#define OPT_BASE(type, group, enum, json, default) \
  if (Name == #json) {                              \
    return FEXCore::Config::ConfigOption::CONFIG_##enum; \
  }
#include <FEXCore/Config/ConfigValues.inl>
  return std::nullopt;
}

bool ApplyOption(std::string_view Pair) {
  const auto Equals = Pair.find('=');
  if (Equals == std::string_view::npos || Equals == 0) {
    return false;
  }
  if (Pair.substr(0, Equals) == "sfx.CallSave") {
    const auto V = Pair.substr(Equals + 1);
    if (V != "light" && V != "full") {
      return false;
    }
    CallSaveFull = V == "full";
    Description += " ";
    Description += Pair;
    return true;
  }
  const auto Option = OptionByName(Pair.substr(0, Equals));
  if (!Option) {
    return false;
  }
  FEXCore::Config::Set(*Option, std::string(Pair.substr(Equals + 1)));
  Description += " ";
  Description += Pair;
  return true;
}
} // namespace

extern "C" {

int32_t sfx_init(const char* Options) {
  if (CTX) {
    return 0;
  }
  FEXCore::Config::Initialize();
  // the same three non-negotiables the host layer sets (host_layer.cpp explains each).
  FEXCore::Config::Set(FEXCore::Config::CONFIG_DYNAMICL1CACHEDECREASECOUNTHEURISTIC, "0");
  Description = "FEX";
  if (Options) {
    std::string_view Rest(Options);
    while (!Rest.empty()) {
      const auto Semi = Rest.find(';');
      const auto Pair = Rest.substr(0, Semi);
      if (!Pair.empty() && !ApplyOption(Pair)) {
        std::fprintf(stderr, "[sharpfex] bad FEX option '%.*s'\n", static_cast<int>(Pair.size()), Pair.data());
        return 2;
      }
      if (Semi == std::string_view::npos) {
        break;
      }
      Rest.remove_prefix(Semi + 1);
    }
  }
  FEXCore::Config::Set(FEXCore::Config::CONFIG_IS64BIT_MODE, "1");
  // mtrack: FEX emits no per-block guard and reports compiled pages through
  // MarkGuestExecutableRange. this PoC does not write-protect them (the VMA tracker's job), so
  // code patched after it ran needs sfx_invalidate.
  FEXCore::Config::Set(FEXCore::Config::CONFIG_SMCCHECKS, "1");

  const auto Features = HostLayer::HostFeatures::Build(HostLayer::HostFeatures::Mode::Probe);
  AvxLayout = Features.SupportsAVX && Features.SupportsSVE256;
  HostLayer::FEXThreads::Install();

  CTX = FEXCore::Context::Context::CreateNewContext(Features);
  if (!CTX) {
    return 1;
  }
  CTX->SetSyscallHandler(&Syscalls);
  CTX->SetSignalDelegator(&Signals);
  CTX->EnableExitOnHLT();
  if (!CTX->InitCore()) {
    CTX.reset();
    return 1;
  }

  UnalignedHandler = FEXCore::Config::Get_HALFBARRIERTSOENABLED()() ? FEXCore::ArchHelpers::Arm64::UnalignedHandlerType::HalfBarrier :
                                                                     FEXCore::ArchHelpers::Arm64::UnalignedHandlerType::NonAtomic;
  // after the .NET runtime's own handlers, which are kept and chained to: NativeAOT turns a null
  // dereference into a SIGSEGV and must keep receiving every fault that is not FEX's.
  struct sigaction Action {};
  Action.sa_sigaction = FaultHandler;
  Action.sa_flags = SA_SIGINFO | SA_ONSTACK;
  ::sigemptyset(&Action.sa_mask);
  ::sigaction(SIGSEGV, &Action, &PreviousSEGV);
  ::sigaction(SIGBUS, &Action, &PreviousBUS);

  char Buffer[160];
  std::snprintf(Buffer, sizeof(Buffer), " | TSO=%d atomics=%d rcpc=%d tsoimm9=%d avxlayout=%d",
                static_cast<int>(FEXCore::Config::Get_TSOENABLED()()), Features.SupportsAtomics, Features.SupportsRCPC,
                Features.SupportsTSOImm9, AvxLayout);
  Description += Buffer;
  return 0;
}

const char* sfx_describe(void) {
  return Description.c_str();
}

void sfx_set_hle_handler(sfx_hle_fn Fn, void* User) {
  Handler = Fn;
  HandlerUser = User;
}

int32_t sfx_set_slot_fn(uint32_t Index, sfx_slot_fn Fn, void* User) {
  if (Index >= MaxSlotFns) {
    return 1;
  }
  // FEX threads read the table without a lock. set it while no guest code is running.
  SlotFns[Index].User = User;
  SlotFns[Index].Fn = Fn;
  return 0;
}

sfx_slot_fn sfx_builtin(const char* Name) {
  if (!Name) {
    return nullptr;
  }
  const std::string_view N(Name);
  if (N == "add") {
    return BuiltinAdd;
  }
  if (N == "null") {
    return BuiltinNull;
  }
  return nullptr;
}

int32_t sfx_set_import_slots(uint64_t Base, uint32_t Count, uint32_t Stride) {
  if (Stride < 13) {
    return 1;
  }
  SlotBase = Base;
  SlotStride = Stride;
  SlotEnd = Base + static_cast<uint64_t>(Count) * Stride;
  return 0;
}

int32_t sfx_set_callback_return(uint64_t Address) {
  CallbackReturnAddress = Address;
  return 0;
}

int32_t sfx_mark_exec(uint64_t Base, uint64_t Length) {
  std::unique_lock Lock {RangesLock};
  Ranges.push_back({Base, Length});
  return 0;
}

void sfx_invalidate(uint64_t Base, uint64_t Length) {
  if (CTX) {
    std::lock_guard Lock {CTX->GetCodeInvalidationMutex()};
    CTX->InvalidateCodeBuffersCodeRange(Base, Length);
    if (CurrentThread && CurrentThread->Thread) {
      CTX->InvalidateThreadCachedCodeRange(CurrentThread->Thread, Base, Length);
    }
  }
}

void* sfx_thread_new(uint64_t Rip, uint64_t Rsp, uint64_t FsBase) {
  if (!CTX) {
    return nullptr;
  }
  auto* T = new SfxThread;
  T->Thread = CTX->CreateThread();
  if (!T->Thread) {
    delete T;
    return nullptr;
  }
  T->Thread->CurrentFrame->State.rip = Rip;
  T->Thread->CurrentFrame->State.gregs[X86::REG_RSP] = Rsp;
  T->Thread->FrontendPtr = T;
  T->Thread->CurrentFrame->Pointers.ThunkCallbackRet = CallbackReturnAddress;
  SetupSegments(*T, FsBase);
  if (!SetupCallRetStack(*T)) {
    CTX->DestroyThread(T->Thread);
    delete T;
    return nullptr;
  }
  CurrentThread = T;
  return T;
}

void sfx_thread_free(void* Handle) {
  auto* T = static_cast<SfxThread*>(Handle);
  if (!T) {
    return;
  }
  if (CurrentThread == T) {
    CurrentThread = nullptr;
  }
  FlushStats(*T);
  CTX->DestroyThread(T->Thread);
  if (T->CallRetAlloc) {
    ::munmap(T->CallRetAlloc, T->CallRetAllocSize);
  }
  delete T;
}

int32_t sfx_run(void* Handle) {
  auto* T = static_cast<SfxThread*>(Handle);
  CurrentThread = T;
  T->InRun = true;
  if (sigsetjmp(T->EscapeHatch, 1) != 0) {
    // left the JIT from inside a handler. the FEX thread state already describes the guest at its
    // next instruction, so this *is* the continuation: the next sfx_run resumes it.
    T->Thread->CurrentFrame->State.callret_sp = T->CallRetDefault;
    T->InRun = false;
    FlushStats(*T);
    return T->Reason;
  }
  T->Reason = SFX_RUN_RETURNED;
  CTX->ExecuteThread(T->Thread);
  T->InRun = false;
  FlushStats(*T);
  return T->Reason;
}

uint64_t sfx_call(void* Handle, uint64_t Rip, uint64_t Rsp, const uint64_t* Args, int32_t Count) {
  auto* T = static_cast<SfxThread*>(Handle);
  CurrentThread = T;
  auto* Frame = T->Thread->CurrentFrame;
  auto& State = Frame->State;
  SavedGuestState Saved;
  LightGuestState Light;
  if (CallSaveFull) {
    Save(State, Saved);
  } else {
    Light.Rip = State.rip;
    std::memcpy(Light.Gregs, State.gregs, sizeof(Light.Gregs));
    Light.CallRetSp = State.callret_sp;
    Light.Mxcsr = State.mxcsr;
    Light.PfRaw = State.pf_raw;
    Light.AfRaw = State.af_raw;
    std::memcpy(Light.Flags, State.flags, sizeof(Light.Flags));
  }

  static constexpr int ArgRegs[6] = {X86::REG_RDI, X86::REG_RSI, X86::REG_RDX, X86::REG_RCX, X86::REG_R8, X86::REG_R9};
  for (int i = 0; i < Count && i < 6; ++i) {
    State.gregs[ArgRegs[i]] = Args[i];
  }
  if (!Rsp) {
    Rsp = State.gregs[X86::REG_RSP] - 128; // below the red zone of whatever was running
  }
  // HandleCallback pushes the CALLBACKRET address 16 bytes down, so a stack that is 8 mod 16 here
  // gives the callee the alignment a `call` would have.
  State.gregs[X86::REG_RSP] = (Rsp & ~15ULL) - 8;
  Frame->Pointers.ThunkCallbackRet = CallbackReturnAddress;

  ++T->CallDepth;
  Stats.Callbacks.fetch_add(1, std::memory_order_relaxed);
  CTX->HandleCallback(T->Thread, Rip);
  --T->CallDepth;
  if (T->CallDepth == 0) {
    FlushStats(*T);
  }

  const uint64_t Result = State.gregs[X86::REG_RAX];
  if (CallSaveFull) {
    Restore(Saved, State);
  } else {
    State.rip = Light.Rip;
    std::memcpy(State.gregs, Light.Gregs, sizeof(Light.Gregs));
    State.callret_sp = Light.CallRetSp;
    State.mxcsr = Light.Mxcsr;
    State.pf_raw = Light.PfRaw;
    State.af_raw = Light.AfRaw;
    std::memcpy(State.flags, Light.Flags, sizeof(Light.Flags));
  }
  return Result;
}

void sfx_get_regs(void* Handle, sfx_regs* Out) {
  auto* T = static_cast<SfxThread*>(Handle);
  StateToRegs(T->Thread->CurrentFrame->State, Out);
}

void sfx_set_regs(void* Handle, const sfx_regs* In) {
  auto* T = static_cast<SfxThread*>(Handle);
  RegsToState(In, T->Thread->CurrentFrame->State);
}

void sfx_get_stats(sfx_stats* Out) {
  Out->hle_calls = Stats.HleCalls.load();
  Out->callbacks = Stats.Callbacks.load();
  Out->blocks = Stats.Blocks.load();
  Out->transfers = Stats.Transfers.load();
  Out->foreign_syscalls = Stats.Foreign.load();
  Out->unaligned_fixups = Stats.Unaligned.load();
  Out->jit_restarts = Stats.Restarts.load();
  Out->signals_chained = Stats.Chained.load();
}

} // extern "C"
