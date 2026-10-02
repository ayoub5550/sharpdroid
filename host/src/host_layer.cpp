// sharpdroid host layer.
//
// two modes:
//
//   sharpdroid-host-layer --spike               24 bytes of hand-assembled x86-64. no loader, no
//                                             syscall table. this is the smoke test:
//                                             it proves the JIT still translates, executes and
//                                             faults on this device.
//   sharpdroid-host-layer [--trace] <elf> [..]  load an x86-64 ELF and run it.
//
// the spike came first on purpose. android enforces W^X far more strictly than desktop linux
// and a JIT must write memory then execute it; Dispatcher::Create() allocating buffers was not
// proof they were usable. they are -- so everything from the loader onwards is typing.
//
// this file is the driver and nothing else. the per-thread machinery -- the GDT, the call-return
// stack, the escape hatch, the fault handler and the dispatch loop -- lives in guest_threads.cpp,
// because there is one of each per guest thread rather than one of each per process.
//
// it is not the process entry either. HostLayer::RunMain is called either by entry_exe.cpp's main()
// or by entry_jni.cpp on behalf of the app -- see host_layer.h.
// the argument vector is the interface in both cases, so the app passes the same flags a shell
// would and every measurement stays comparable.

#include "boot_progress.h"
#include "elf_loader.h"
#include "guest_files.h"
#include "guest_log.h"
#include "host_features.h"
#include "host_layer.h"
#include "guest_signals.h"
#include "guest_threads.h"
#include "linux_syscalls.h"
#include "thunk_abi.h"
#include "vma_tracker.h"

#include <FEXCore/Config/Config.h>
// CodeCache.h before SyscallHandler.h, and not for tidiness: SyscallHandler.h names
// ExecutableFileSectionInfo unqualified from inside namespace FEXCore::HLE, resolving it via
// the enclosing namespace to FEXCore::ExecutableFileSectionInfo -- which CodeCache.h declares.
// without this include the header does not compile on its own.
#include <FEXCore/Core/CodeCache.h>
#include <FEXCore/Core/Context.h>
#include <FEXCore/Core/CoreState.h>
#include <FEXCore/Core/HostFeatures.h>
#include <FEXCore/Core/SignalDelegator.h>
#include <FEXCore/Debug/InternalThreadState.h>
#include <FEXCore/HLE/SyscallHandler.h>
#include <FEXCore/Utils/TypeDefines.h>

#include <chrono>
#include <cstdio>
#include <cstring>
#include <iterator>
#include <optional>
#include <string_view>
#include <thread>
#include <vector>
#include <sys/auxv.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

// same guard as elf_loader.cpp: the NDK headers this builds against predate the flag.
#ifndef MAP_FIXED_NOREPLACE
#define MAP_FIXED_NOREPLACE 0x100000
#endif

namespace {

// --- the pause, tested from a shell ------------------------------------------------------------
//
// `--pause-selftest` pauses the guest for half a second, 300 ms after it starts, and fails the run
// unless at least one thread was stopped inside translated code and none was still running when the
// pause was settled. that is the half the guest cannot see; guests/pause.c checks the other half --
// that its monotonic clock left the pause out and its spinning thread stopped -- from inside.
//
// the app pauses the same way, from its own buttons rather than a timer, so this is the pause's whole
// mechanism under test with nothing of the app's in front of it.
bool PauseSelfTest {};

void StartPauseSelfTest() {
  std::thread([] {
    std::this_thread::sleep_for(std::chrono::milliseconds(300));
    std::printf("[host-layer] pause self-test: pausing the guest for 500 ms\n");
    std::fflush(stdout);
    HostLayer::Threads::Pause();
    std::this_thread::sleep_for(std::chrono::milliseconds(500));
    const auto Counts = HostLayer::Threads::LastPauseCounts();
    HostLayer::Threads::Resume();
    if (Counts.InCode == 0 || Counts.StillRunning != 0) {
      std::printf("[host-layer] pause self-test FAILED: %u thread(s) stopped in translated code, %u still running\n",
                  Counts.InCode, Counts.StillRunning);
      std::fflush(stdout);
      ::_exit(5);
    }
  }).detach();
}

// --- FEXCore configuration by name -------------------------------------------------------------

// resolves a FEXCore option's json name -- `TSOEnabled`, `Multiblock` -- to the enumerator
// FEXCore::Config::Set takes, so that a JIT knob can be chosen at launch rather than compiled in.
//
// the table is FEXCore's own, expanded here rather than restated. `ConfigValues.inl` is generated
// from FEX's option definitions at build time and every line in it carries the json spelling as its
// fourth macro argument, so redefining OPT_BASE over the same header produces a mapping that cannot
// drift from the enum it maps to: an option renamed upstream changes both sides at once, and one
// removed stops compiling here rather than resolving to the wrong thing.
//
// the names are the ones FEX documents. the `FEX_TSOENABLED` environment spelling is deliberately
// not accepted, and cannot be: FEX reads those in EnvLoader, which lives in FEX's frontend -- the
// part this project does not build. a variable by that name reaches nothing here, so accepting the
// spelling would promise a route that does not exist.
std::optional<FEXCore::Config::ConfigOption> ConfigOptionByName(std::string_view Name) {
#define OPT_BASE(type, group, enum, json, default)     \
  if (Name == #json) {                                 \
    return FEXCore::Config::ConfigOption::CONFIG_##enum; \
  }
// the header undefines every OPT_ macro on its way out, including the one above, so this is a
// self-contained expansion rather than something the next include has to be kept clear of.
#include <FEXCore/Config/ConfigValues.inl>
  return std::nullopt;
}

// --- host context, shared by both modes ------------------------------------------------------

FEXCore::SignalDelegator* GlobalSignals = nullptr;

HostLayer::GuestSignals GuestSigs;
HostLayer::LinuxSyscallHandler LinuxSyscalls;

// what the run cost, printed once however the process ends. exit_group can come from any guest
// thread and only the initial one unwinds back to RunELF, so this is handed to the thread layer
// as well as called directly.
void PrintRunSummary() {
  if (HostLayer::Threads::CreatedCount() > 1) {
    std::printf("[host-layer] %llu guest thread(s) created, %llu still live\n",
                static_cast<unsigned long long>(HostLayer::Threads::CreatedCount()),
                static_cast<unsigned long long>(HostLayer::Threads::LiveCount()));
  }
  if (GuestSigs.DeliveredCount()) {
    std::printf("[host-layer] %llu signal(s) delivered to guest handlers\n",
                static_cast<unsigned long long>(GuestSigs.DeliveredCount()));
  }
  const auto Async = HostLayer::Threads::AsyncStats();
  if (Async.Raised) {
    // "deferred" means the host interrupt landed somewhere the thread could not be redirected
    // from. under the default site that is every one of them by construction, so the number to
    // read is Raised against how many were delivered above -- a gap between the two is a thread
    // that never reached a boundary.
    std::printf("[host-layer] %llu signal(s) raised on a guest thread, %llu interrupt(s) left for a later boundary\n",
                static_cast<unsigned long long>(Async.Raised), static_cast<unsigned long long>(Async.Deferred));
  }
  if (HostLayer::VulkanThunk::PresentedFrameCount()) {
    std::printf("[host-layer] %llu frame(s) presented\n",
                static_cast<unsigned long long>(HostLayer::VulkanThunk::PresentedFrameCount()));
  }
  if (HostLayer::VulkanThunk::CallCount() || HostLayer::VulkanThunk::UnresolvedCount()) {
    std::printf("[host-layer] %llu vulkan call(s) thunked, %llu unresolved%s%s\n",
                static_cast<unsigned long long>(HostLayer::VulkanThunk::CallCount()),
                static_cast<unsigned long long>(HostLayer::VulkanThunk::UnresolvedCount()),
                HostLayer::VulkanThunk::LastUnresolved() ? ", last: " : "",
                HostLayer::VulkanThunk::LastUnresolved() ? HostLayer::VulkanThunk::LastUnresolved() : "");
  }
  if (HostLayer::AudioThunk::CallCount() || HostLayer::AudioThunk::UnresolvedCount()) {
    std::printf("[host-layer] %llu audio call(s) thunked, %llu unresolved%s%s, %llu refused as callbacks\n",
                static_cast<unsigned long long>(HostLayer::AudioThunk::CallCount()),
                static_cast<unsigned long long>(HostLayer::AudioThunk::UnresolvedCount()),
                HostLayer::AudioThunk::LastUnresolved() ? ", last: " : "",
                HostLayer::AudioThunk::LastUnresolved() ? HostLayer::AudioThunk::LastUnresolved() : "",
                static_cast<unsigned long long>(HostLayer::AudioThunk::RefusedCount()));
    // a stream that opened and never played is the one failure that looks like success, so the
    // frames-read figure goes in the summary rather than only in the periodic report.
    HostLayer::AudioThunk::ReportStreams();
  }
  // unconditional when the bridge is on, unlike the two above: a guest that never polled and a pad
  // nobody touched are indistinguishable without the count, and the zero is the reading that matters.
  HostLayer::PadBridge::Report();
  if (HostLayer::Threads::CallRetResetCount()) {
    std::printf("[host-layer] %llu call-return shadow stack reset(s) after a guard-page fault\n",
                static_cast<unsigned long long>(HostLayer::Threads::CallRetResetCount()));
  }
  if (HostLayer::Threads::UnalignedFixupCount()) {
    // the mode is named beside the count because the two are only meaningful together: the same
    // number of backpatches means something quite different when each one dropped the ordering
    // x86 promised.
    std::printf("[host-layer] %llu unaligned access(es) backpatched, to %s sequences\n",
                static_cast<unsigned long long>(HostLayer::Threads::UnalignedFixupCount()),
                HostLayer::Threads::UnalignedHandlerIsAtomic() ? "half-barrier atomic" : "non-atomic");
  }
  std::printf("[host-layer] smc=%s: %llu guest mapping(s) tracked, %llu code invalidation(s), %llu SMC write fault(s)\n",
              HostLayer::VMA::Mode() == HostLayer::VMA::SMCMode::None    ? "none" :
              HostLayer::VMA::Mode() == HostLayer::VMA::SMCMode::MTrack ? "mtrack" :
                                                                          "full",
              static_cast<unsigned long long>(HostLayer::VMA::EntryCount()),
              static_cast<unsigned long long>(HostLayer::VMA::InvalidationCount()),
              static_cast<unsigned long long>(HostLayer::VMA::SMCFaultCount()));
  {
    // the third column is the one worth reading. a mapping laid over memory the guest had already
    // declared executable leaves whatever FEXCore compiled there describing bytes that are gone,
    // and the total beside it is what stops a zero being silence.
    const auto Mappings = HostLayer::VMA::MappingsRecorded();
    std::printf("[host-layer] %llu mapping(s) recorded, %llu over live memory, %llu over executable memory\n",
                static_cast<unsigned long long>(Mappings.Total), static_cast<unsigned long long>(Mappings.Overlapping),
                static_cast<unsigned long long>(Mappings.OverExecutable));
  }
  if (HostLayer::Threads::SignalTrace()) {
    const auto Signals = HostLayer::VMA::SignalsDuringInvalidation();
    std::printf("[host-layer] %llu signal(s) delivered, %llu of them inside a code invalidation\n",
                static_cast<unsigned long long>(Signals.Total), static_cast<unsigned long long>(Signals.InsideInvalidation));
  }
  if (LinuxSyscalls.UnhandledCount()) {
    std::printf("[host-layer] %llu unhandled syscall(s), last was %llu\n",
                static_cast<unsigned long long>(LinuxSyscalls.UnhandledCount()),
                static_cast<unsigned long long>(LinuxSyscalls.LastUnhandledNumber()));
  }
}

// --- mode 1: the hand-assembled spike ---------------------------------------------------------

constexpr uint64_t SpikeSyscallNumber = 0xAB;
constexpr uint64_t SpikeSyscallArg = 0xABCD;
constexpr uint64_t SpikeWitnessValue = 0x5AFE;

bool SpikeSyscallWasCalled = false;

class SpikeSyscallHandler final : public FEXCore::HLE::SyscallHandler {
public:
  SpikeSyscallHandler() {
    OSABI = FEXCore::HLE::SyscallOSABI::OS_LINUX64;
  }

  uint64_t HandleSyscall(FEXCore::Core::CpuStateFrame*, FEXCore::HLE::SyscallArguments* Args) override {
    std::printf("[host-layer]   guest syscall: number=0x%llX arg0=0x%llX\n",
                static_cast<unsigned long long>(Args->Argument[0]), static_cast<unsigned long long>(Args->Argument[1]));
    SpikeSyscallWasCalled = Args->Argument[0] == SpikeSyscallNumber && Args->Argument[1] == SpikeSyscallArg;
    return 0;
  }

  // the spike goes through the same VMA tracker as everything else rather than claiming the whole
  // address space is executable. it is the cheapest possible test that the tracker is wired up:
  // if RunSpike's Record call below is wrong, the decoder refuses the first instruction and the
  // spike regression fails immediately instead of something subtle happening much later.
  FEXCore::HLE::ExecutableRangeInfo QueryGuestExecutableRange(FEXCore::Core::InternalThreadState*, uint64_t Address) override {
    return HostLayer::VMA::Query(Address);
  }

  void MarkGuestExecutableRange(FEXCore::Core::InternalThreadState*, uint64_t Start, uint64_t Length) override {
    HostLayer::VMA::MarkExecutable(Start, Length);
  }

  void InvalidateGuestCodeRange(FEXCore::Core::InternalThreadState* Thread, uint64_t Start, uint64_t Length) override {
    HostLayer::VMA::Invalidate(Thread, Start, Length);
  }

  std::optional<FEXCore::ExecutableFileSectionInfo> LookupExecutableFileSection(FEXCore::Core::InternalThreadState*, uint64_t) override {
    return std::nullopt;
  }
};

//  off  bytes                      instruction
//    0  48 c7 c0 ab 00 00 00       mov rax, 0xAB
//    7  48 c7 c7 cd ab 00 00       mov rdi, 0xABCD
//   14  0f 05                      syscall
//   16  48 c7 c3 fe 5a 00 00       mov rbx, 0x5AFE     <- after the syscall, on purpose
//   23  31 c0                      xor eax, eax
//   25  48 8b 08                   mov rcx, [rax]      <- guest reads address 0: SIGSEGV
//   28  f4                         hlt                 (never reached)
//
// rbx is set after the syscall so reading it back proves execution resumed past the callback
// rather than stopping at it. the load from a null guest address is the fault we want to catch:
// FEX maps the 64-bit guest address space 1:1 onto the host, so it arrives as a real host
// SIGSEGV raised from inside JIT-generated code.
constexpr unsigned char SpikeGuestCode[] = {
  0x48, 0xC7, 0xC0, 0xAB, 0x00, 0x00, 0x00, //
  0x48, 0xC7, 0xC7, 0xCD, 0xAB, 0x00, 0x00, //
  0x0F, 0x05,                               //
  0x48, 0xC7, 0xC3, 0xFE, 0x5A, 0x00, 0x00, //
  0x31, 0xC0,                               //
  0x48, 0x8B, 0x08,                         //
  0xF4,                                     //
};
constexpr uint64_t SpikeFaultingInstructionOffset = 25;

int RunSpike(FEXCore::Context::Context* CTX) {
  // guest code is mapped RW, not RWX: FEX *reads* these bytes and emits arm64 elsewhere, so
  // the host never executes this page directly.
  void* CodeMem = ::mmap(nullptr, 64 * 1024, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
  void* StackMem = ::mmap(nullptr, 256 * 1024, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
  if (CodeMem == MAP_FAILED || StackMem == MAP_FAILED) {
    std::fprintf(stderr, "[host-layer] guest mmap failed\n");
    return 1;
  }
  std::memcpy(CodeMem, SpikeGuestCode, sizeof(SpikeGuestCode));

  // the guest's view: executable code, and a writable stack. the host mapping stays RW either
  // way -- HostProt drops PROT_EXEC -- but this is what the decoder is told, and without it the
  // spike never decodes its first instruction.
  HostLayer::VMA::Record(reinterpret_cast<uint64_t>(CodeMem), 64 * 1024, PROT_READ | PROT_EXEC);
  HostLayer::VMA::Record(reinterpret_cast<uint64_t>(StackMem), 256 * 1024, PROT_READ | PROT_WRITE);

  const auto EntryRIP = reinterpret_cast<uint64_t>(CodeMem);
  const auto StackTop = (reinterpret_cast<uint64_t>(StackMem) + 256 * 1024 - 16) & ~15ULL;
  std::printf("[host-layer] guest entry=0x%llX stack=0x%llX (%zu bytes of x86-64)\n",
              static_cast<unsigned long long>(EntryRIP), static_cast<unsigned long long>(StackTop), sizeof(SpikeGuestCode));

  auto* T = HostLayer::Threads::CreateInitial(EntryRIP, StackTop);
  if (!T) {
    std::fprintf(stderr, "[host-layer] could not create the initial guest thread\n");
    return 1;
  }

  std::printf("[host-layer] executing guest...\n");
  HostLayer::Threads::Run(*T);
  std::printf("[host-layer] %s\n",
              T->Reason == HostLayer::Escape::Fault ? "escaped guest fault" : "returned from guest without faulting");

  const uint64_t ExpectedFaultRIP = EntryRIP + SpikeFaultingInstructionOffset;
  // recovered out of a host arm64 register through the SRA mapping, not read out of memory: at
  // fault time guest GPRs are live in host registers and CPUState holds whatever was last
  // spilled there.
  const auto SRARBX = T->Fault.GPR[FEXCore::X86State::REG_RBX];

  std::printf("[host-layer] fault caught=%d in_jit_code=%d\n", T->Fault.Caught, T->Fault.InJitCode);
  std::printf("[host-layer]   guest RIP            = 0x%llX (expected 0x%llX)\n",
              static_cast<unsigned long long>(T->Fault.GuestRIP), static_cast<unsigned long long>(ExpectedFaultRIP));
  std::printf("[host-layer]   guest RBX recovered  = 0x%llX (expected 0x%llX)\n", static_cast<unsigned long long>(SRARBX),
              static_cast<unsigned long long>(SpikeWitnessValue));

  const bool Ok = SpikeSyscallWasCalled && T->Fault.Caught && T->Fault.InJitCode && T->Fault.FaultAddress == nullptr &&
                  T->Fault.GuestRIP == ExpectedFaultRIP && SRARBX == SpikeWitnessValue;
  std::printf("[host-layer] %s\n", Ok ? "spike OK: guest executed, faulted, and guest state was recovered."
                                      : "spike FAILED: see above.");

  HostLayer::Threads::Destroy(*T);
  return Ok ? 0 : 1;
}

// --- mode 2: load and run an x86-64 ELF --------------------------------------------------------

// where an ET_DYN (PIE) guest gets biased to, relative to the base chosen below. ET_EXEC guests
// are mapped where their program headers say, which for a linked-at-fixed-address binary is
// normally 0x400000.
//
// the host layer's address budget gives 0..32 GiB to the host, FEXCore and .NET, so a guest
// down here is out of the way of the 32-36 GiB window the PS5 image will want later.
constexpr uint64_t GuestPIEOffset = 0x2000'0000;

// and where its interpreter goes. this has to clear both the program image and the 512 MiB brk
// arena reserved immediately past it, because the arena is claimed *after* the interpreter is
// mapped and cannot move: brk has to stay contiguous with the image. 2 GiB leaves room for a
// program far larger than the 61 MB SharpEmu publish.
constexpr uint64_t GuestInterpOffset = 0x8000'0000;

// the end of what the pair of them plus the interpreter's own budget can touch, and therefore
// what has to be free for a base to be usable.
constexpr uint64_t GuestSpanEnd = 0xA000'0000;

// **an app process is not an empty process, and that is why these are offsets rather than absolute
// addresses.** as a shell binary the bottom 2.5 GiB is untouched and an absolute layout would do.
// inside an APK, ART is there first: the dalvik main heap is a 256 MiB region at 0x14000000, the
// non-moving heap at 0x34000000, two JIT code caches at 0x54000000, and the boot image and its .oat
// files run from about 0x70cc0000 upwards. a guest program pinned at 512 MiB lands inside the first
// of those and an interpreter pinned at 2 GiB inside the last, so the loader's MAP_FIXED_NOREPLACE
// reservation returns EEXIST and the run ends before the guest exists.
//
// so the base is measured rather than declared, which is the same answer the fork reaches for
// SharpEmu's own layout one level up. **zero is tried first and it is not a formality**: it always
// wins on a shell process, so the shell binary puts the guest at one fixed set of addresses that two
// runs can be diffed against each other. the rest are 4 GiB apart and all stop short of the 32 GiB
// the PS5 image wants.
constexpr uint64_t GuestBaseCandidates[] {
  0, 0x2'0000'0000, 0x3'0000'0000, 0x4'0000'0000, 0x5'0000'0000, 0x6'0000'0000, 0x7'0000'0000,
};

// probe by reserving and releasing, rather than by parsing /proc/self/maps. the kernel is the
// only authority that cannot disagree with itself, and MAP_FIXED_NOREPLACE asks it the exact
// question the loader is about to ask for real.
uint64_t ChooseGuestBase() {
  for (const uint64_t Base : GuestBaseCandidates) {
    const uint64_t Start = Base + GuestPIEOffset;
    const uint64_t Size = GuestSpanEnd - GuestPIEOffset;
    void* Probe = ::mmap(reinterpret_cast<void*>(Start), Size, PROT_NONE,
                         MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED_NOREPLACE | MAP_NORESERVE, -1, 0);
    if (Probe == MAP_FAILED) {
      continue;
    }
    const bool Usable = reinterpret_cast<uint64_t>(Probe) == Start;
    ::munmap(Probe, Size);
    if (Usable) {
      if (Base) {
        std::printf("[host-layer] guest base 0x%llX: something already occupies the usual one\n",
                    static_cast<unsigned long long>(Base));
      }
      return Base;
    }
  }
  // nothing to do but try the usual place and let the loader report what it finds.
  std::fprintf(stderr, "[host-layer] no free guest base below 32 GiB; falling back to 0\n");
  return 0;
}

constexpr size_t GuestStackSize = 8 * 1024 * 1024;

int RunELF(FEXCore::Context::Context* CTX, const char* Path, const char* LibDir, const char* TmpDir,
           const std::vector<const char*>& ExtraEnv, int GuestArgc, const char* const* GuestArgv) {
  const uint64_t GuestBase = ChooseGuestBase();
  auto Program = HostLayer::LoadProgram(Path, GuestBase + GuestPIEOffset, GuestBase + GuestInterpOffset, LibDir);
  if (!Program.Ok) {
    std::fprintf(stderr, "[host-layer] load failed: %s (%s)\n", Program.Error, Path);
    return 1;
  }
  const auto& Elf = Program.Exec;
  // the payload's size alongside its path, so a measurement is attributable to a specific build
  // rather than to whatever happens to be lying at that path. the launcher names a build by its
  // identity; this is the cheapest form of the same guarantee, and it is the one the shell binary
  // gets too, where there is no meta.json to read. without it a rebuilt payload at a familiar path
  // produces a plausible number attributed to the wrong artefact, with nothing erroring.
  struct stat PayloadStat {};
  if (::stat(Path, &PayloadStat) == 0) {
    std::printf("[host-layer] loaded %s (%lld bytes)\n", Path, static_cast<long long>(PayloadStat.st_size));
  } else {
    std::printf("[host-layer] loaded %s\n", Path);
  }
  std::printf("[host-layer]   image 0x%llX..0x%llX, entry 0x%llX, bias 0x%llX\n",
              static_cast<unsigned long long>(Elf.MappingBegin), static_cast<unsigned long long>(Elf.MappingEnd),
              static_cast<unsigned long long>(Elf.Entry), static_cast<unsigned long long>(Elf.LoadBias));
  std::printf("[host-layer]   phdr 0x%llX x%llu, brk base 0x%llX\n", static_cast<unsigned long long>(Elf.PhdrAddr),
              static_cast<unsigned long long>(Elf.PhNum), static_cast<unsigned long long>(Elf.BrkBase));
  if (Program.Interp.Ok) {
    std::printf("[host-layer]   interp %s\n", Elf.InterpPath);
    std::printf("[host-layer]   interp 0x%llX..0x%llX, entry 0x%llX (AT_BASE 0x%llX)\n",
                static_cast<unsigned long long>(Program.Interp.MappingBegin),
                static_cast<unsigned long long>(Program.Interp.MappingEnd),
                static_cast<unsigned long long>(Program.Interp.Entry),
                static_cast<unsigned long long>(Program.InterpBase));
  } else {
    std::printf("[host-layer]   statically linked, no interpreter\n");
  }

  // the guest's own library search path. the same directory PT_INTERP was resolved out of, on the
  // grounds that if ld.so came from there so did everything it is about to load.
  char LibPathVar[512] {};
  std::snprintf(LibPathVar, sizeof(LibPathVar), "LD_LIBRARY_PATH=%s", LibDir ? LibDir : "");

  // android has no /tmp, and .NET reaches for a writable directory for far more than bundles. all
  // three names point at the same place: HOME because the single-file host falls back to
  // $HOME/.net, TMPDIR because everything else in the runtime looks there, and the explicit
  // DOTNET_ variable because relying on a fallback to land somewhere writable is how the previous
  // run failed with "Default extraction directory [/]".
  char HomeVar[512] {};
  char TmpVar[512] {};
  char BundleVar[512] {};
  std::snprintf(HomeVar, sizeof(HomeVar), "HOME=%s", TmpDir ? TmpDir : "/");
  std::snprintf(TmpVar, sizeof(TmpVar), "TMPDIR=%s", TmpDir ? TmpDir : "/");
  std::snprintf(BundleVar, sizeof(BundleVar), "DOTNET_BUNDLE_EXTRACT_BASE_DIR=%s", TmpDir ? TmpDir : "/");

  const char* const GuestEnv[] {
    "PATH=/usr/bin:/bin",
    "LANG=C",
    LibPathVar,
    HomeVar,
    TmpVar,
    BundleVar,
    // .NET links globalization against ICU at runtime and FailFast()s if it cannot find it --
    // "Couldn't find a valid ICU package installed on the system", from a static constructor deep
    // under the first DateTimeOffset.ToLocalTime() SharpEmu's logger performs. that is not a host
    // layer problem: libicu is simply not among the x86-64 shared objects staged in guest-libs/,
    // and it would be ~30 MB of them.
    //
    // invariant mode is the right answer for the proof of concept and probably for the app too:
    // it costs culture-aware formatting and collation, which a PS5 emulator uses for log
    // timestamps, and nothing that affects running a game. if the fork ever needs real
    // globalization the fix is to stage libicuuc/libicui18n/libicudata, not to change anything
    // here.
    "DOTNET_SYSTEM_GLOBALIZATION_INVARIANT=1",
    // deliberately NOT setting DOTNET_GCRegionRange. the early throwaway probe under wine needed
    // it -- the regions GC reserved a range covering 0x8_0000_0000 and the PS5 image landed on top
    // of it -- but that was wine's address space, not ours. measured here: setting it to 0x100000000
    // changes nothing either way, and without it the guest maps the full PS5 image at 32 GiB and
    // applies all 120,776 relocations. so it is a wine artefact, not a latent fix to keep around.
  };

  // the fixed set above, then whatever --env added. a flag rather than more entries in that array
  // because the interesting .NET knobs -- W^X, tiered compilation, AVX -- are one-line experiments
  // whose whole value is being cheap to try, and a rebuild per experiment is not cheap.
  std::vector<const char*> Env {std::begin(GuestEnv), std::end(GuestEnv)};
  for (const char* Entry : ExtraEnv) {
    std::printf("[host-layer]   guest env: %s\n", Entry);
    Env.push_back(Entry);
  }
  Env.push_back(nullptr);

  auto Stack = HostLayer::BuildGuestStack(Program, Path, GuestArgc, GuestArgv, Env.data(), GuestStackSize);
  if (!Stack.Ok) {
    std::fprintf(stderr, "[host-layer] stack setup failed: %s\n", Stack.Error);
    return 1;
  }
  std::printf("[host-layer]   stack 0x%llX..0x%llX, RSP 0x%llX\n", static_cast<unsigned long long>(Stack.Base),
              static_cast<unsigned long long>(Stack.Base + Stack.Size), static_cast<unsigned long long>(Stack.RSP));
  {
    // what the guest is about to find at RSP, from the host's side. worth printing every run:
    // if the guest and the host ever disagree about these eight words, the fault is between
    // them -- in the JIT or in guest state setup -- and not in either one's arithmetic.
    const auto* Slots = reinterpret_cast<const uint64_t*>(Stack.RSP);
    for (int i = 0; i < 8; ++i) {
      std::printf("[host-layer]   [rsp+%02d] = 0x%016llX\n", i * 8, static_cast<unsigned long long>(Slots[i]));
    }
  }

  LinuxSyscalls.SetBrkBase(Elf.BrkBase);
  LinuxSyscalls.SetSignals(&GuestSigs);
  // /proc/self has to describe the guest, not the arm64 executable hosting it.
  LinuxSyscalls.ProcFS().SetExe(Path);
  LinuxSyscalls.ProcFS().SetCmdline(GuestArgc, GuestArgv);

  auto* T = HostLayer::Threads::CreateInitial(Program.StartRIP, Stack.RSP);
  if (!T) {
    std::fprintf(stderr, "[host-layer] could not create the initial guest thread\n");
    return 1;
  }

  std::printf("[host-layer] --- guest starts ---\n");
  if (PauseSelfTest) {
    StartPauseSelfTest();
  }
  HostLayer::Threads::Run(*T);
  std::printf("[host-layer] --- guest stops ---\n");

  int Result = 1;
  switch (T->Reason) {
  case HostLayer::Escape::Exited:
    // the initial guest thread called exit(2) rather than exit_group(2). on linux the process
    // outlives it -- every other thread keeps running -- so we wait rather than tearing the address
    // space down under threads that are still using it.
    HostLayer::Threads::WaitForOthers();
    [[fallthrough]];
  case HostLayer::Escape::ExitedGroup: {
    int Status = T->ExitStatus;
    // if some other thread got to exit_group first, its status is the process's status. that is
    // what linux reports and, more practically, it is the one that says why.
    HostLayer::Threads::ProcessExitRequested(&Status);
    // the kernel keeps only the low 8 bits of an exit status, so a guest exiting with a value
    // that has meaning above them -- .NET's apphost exits with 0x80008031-shaped HRESULTs -- must
    // be reported the way linux would report it, or the number on screen is one no real system
    // would ever show. the raw value is worth printing too, since for those callers it is the
    // actual error code.
    if (static_cast<unsigned>(Status) > 0xFF) {
      std::printf("[host-layer] guest exited with status %d (raw 0x%08X)\n", Status & 0xFF, static_cast<unsigned>(Status));
    } else {
      std::printf("[host-layer] guest exited with status %d\n", Status);
    }
    Result = Status & 0xFF;
    break;
  }
  case HostLayer::Escape::Fault:
    HostLayer::Threads::PrintFaultReport(*T);
    std::printf("[host-layer] guest died on an unhandled fault (no signal delivery yet)\n");
    break;
  case HostLayer::Escape::Returned:
  case HostLayer::Escape::Restart:
    // with EnableExitOnHLT set, a hlt unwinds ExecuteThread cleanly. a linux binary has no
    // business executing one, so this means the guest ran off the end of something.
    std::printf("[host-layer] guest stopped without exiting (stray hlt?) at RIP 0x%llX\n",
                static_cast<unsigned long long>(T->Thread->CurrentFrame->State.rip));
    break;
  }

  PrintRunSummary();

  // exit_group means every thread dies now, and that is also the only safe thing to do: the other
  // guest threads are still inside FEXCore, so unwinding out of here to tear down this thread's
  // state and shut the config down would be racing them. the kernel does not politely join threads
  // on exit_group either.
  if (T->Reason == HostLayer::Escape::ExitedGroup || HostLayer::Threads::LiveCount() > 1) {
    std::fflush(stdout);
    ::_exit(Result);
  }

  HostLayer::Threads::Destroy(*T);
  return Result;
}

} // namespace

int HostLayer::RunMain(int argc, char** argv) {
  // t=0, before anything else does any work: every stamp in the log is measured from here, and the
  // startup this line precedes is exactly the part we most want to see the size of.
  HostLayer::GuestLog::Start();
  // the same t=0, and separately kept: the stamps are a formatting choice on the guest's output and
  // may be off, while a boot's position must be measurable either way. nothing here reads a stamp.
  HostLayer::BootProgress::Start();

  // unbuffered: through `adb shell` stdout is a pipe and therefore fully buffered, so anything
  // printed before a crash is lost. that cost a debugging round already. it also stops two guest
  // threads' trace lines from arriving as interleaved half-lines.
  ::setvbuf(stdout, nullptr, _IONBF, 0);

  bool SpikeMode = false;
  bool TurboRequested = false;
  bool Trace = false;
  const char* FileProbeRoot = nullptr;
  const char* SafMount = nullptr;
  auto SMC = HostLayer::VMA::SMCMode::MTrack;
  const char* LibDir = nullptr;
  const char* TmpDir = nullptr;
  std::vector<const char*> ExtraEnv;
  std::vector<const char*> FexOptions;
  auto FeatureMode = HostLayer::HostFeatures::Mode::Probe;
  int ArgIndex = 1;
  for (; ArgIndex < argc; ++ArgIndex) {
    if (std::strcmp(argv[ArgIndex], "--spike") == 0) {
      SpikeMode = true;
    } else if (std::strcmp(argv[ArgIndex], "--trace") == 0) {
      Trace = true;
    } else if (std::strcmp(argv[ArgIndex], "--trace-files") == 0 && ArgIndex + 1 < argc) {
      // separate from --trace for the same reason --trace-signals is: this is a few hundred events
      // in a whole run against millions, and it answers a question of its own.
      FileProbeRoot = argv[++ArgIndex];
    } else if (std::strcmp(argv[ArgIndex], "--saf-mount") == 0 && ArgIndex + 1 < argc) {
      // where the guest's game directory appears, when the game came from a grant on a directory the
      // user picked rather than from a path. the prefix is invented -- nothing at that path exists --
      // so an ordinary run never names one and never reaches any of that machinery. it needs the
      // app: there is no provider to ask on the other side of a shell.
      SafMount = argv[++ArgIndex];
    } else if (std::strcmp(argv[ArgIndex], "--trace-signals") == 0) {
      // separate from --trace, and not implied by it: the asynchronous signal path is a dozen
      // events in a whole run, where --trace is millions of lines. keeping them apart is what
      // makes it usable on the real workload.
      HostLayer::Threads::SetSignalTrace(true);
    } else if (std::strcmp(argv[ArgIndex], "--asyncsig") == 0 && ArgIndex + 1 < argc) {
      const char* Site = argv[++ArgIndex];
      if (std::strcmp(Site, "safepoint") == 0) {
        HostLayer::Threads::SetAsyncSite(HostLayer::Threads::AsyncSite::SafePoint);
      } else if (std::strcmp(Site, "block") == 0) {
        HostLayer::Threads::SetAsyncSite(HostLayer::Threads::AsyncSite::Block);
      } else if (std::strcmp(Site, "syscall") == 0) {
        HostLayer::Threads::SetAsyncSite(HostLayer::Threads::AsyncSite::SyscallOnly);
      } else {
        std::fprintf(stderr, "[host-layer] unknown --asyncsig site '%s' (safepoint|block|syscall)\n", Site);
        return 2;
      }
    } else if (std::strcmp(argv[ArgIndex], "--timestamps") == 0) {
      // off by default, so an unmeasured run produces exactly the log every milestone recorded.
      HostLayer::GuestLog::Enable();
    } else if (std::strcmp(argv[ArgIndex], "--log-tids") == 0) {
      // the stamp widens to name the host thread that wrote the line. implies --timestamps, since
      // the tid rides in the stamp and there is nowhere else to put it. off by default: the wider
      // prefix moves under every scanner that matches a guest line by its text.
      HostLayer::GuestLog::Enable();
      HostLayer::GuestLog::EnableThreadIds();
    } else if (std::strcmp(argv[ArgIndex], "--boot-progress") == 0) {
      // a flag rather than something always on, for the same reason as the one above: a caller with
      // nothing to draw pays nothing and prints nothing extra. only a caller that has a screen in
      // front of a booting guest asks for it.
      HostLayer::BootProgress::Enable();
    } else if (std::strcmp(argv[ArgIndex], "--smc") == 0 && ArgIndex + 1 < argc) {
      // a flag rather than a constant because the three modes are the natural way to bisect an
      // SMC problem: `full` is correct without any tracking at all and is the fallback if the
      // page-protection machinery misbehaves, `none` says whether tracking is what costs.
      const char* Mode = argv[++ArgIndex];
      if (std::strcmp(Mode, "none") == 0) {
        SMC = HostLayer::VMA::SMCMode::None;
      } else if (std::strcmp(Mode, "full") == 0) {
        SMC = HostLayer::VMA::SMCMode::Full;
      } else if (std::strcmp(Mode, "mtrack") == 0) {
        SMC = HostLayer::VMA::SMCMode::MTrack;
      } else {
        std::fprintf(stderr, "[host-layer] unknown --smc mode '%s' (none|mtrack|full)\n", Mode);
        return 2;
      }
    } else if (std::strcmp(argv[ArgIndex], "--vulkan") == 0) {
      // off by default so that every measurement taken before the thunk existed still reproduces
      // exactly: without it the
      // guest's dlopen of libvulkan.so.1 fails the way it always did, and nothing else changes.
      HostLayer::VulkanThunk::SetEnabled(true);
    } else if (std::strcmp(argv[ArgIndex], "--vulkan-lib") == 0 && ArgIndex + 1 < argc) {
      HostLayer::VulkanThunk::SetLibraryPath(argv[++ArgIndex]);
      HostLayer::VulkanThunk::SetEnabled(true);
    } else if (std::strcmp(argv[ArgIndex], "--vulkan-driver") == 0 && ArgIndex + 1 < argc) {
      // a custom driver for the platform loader to load, injected with libadrenotools. this is
      // **not** --vulkan-lib: that names a different loader, and a turnip .so is not a loader --
      // on android WSI lives in the loader, so dlopening the driver directly would mean no
      // swapchain at all. this leaves the loader alone and changes what it finds underneath.
      //
      // it needs --vulkan-hooks as well, and an app process to be in. neither given, and the
      // library open below is exactly the one every measurement so far was taken against.
      HostLayer::VulkanThunk::SetDriver(argv[++ArgIndex]);
      HostLayer::VulkanThunk::SetEnabled(true);
    } else if (std::strcmp(argv[ArgIndex], "--vulkan-hooks") == 0 && ArgIndex + 1 < argc) {
      HostLayer::VulkanThunk::SetHookLibDir(argv[++ArgIndex]);
    } else if (std::strcmp(argv[ArgIndex], "--vulkan-turbo") == 0) {
      // pins the GPU clocks through KGSL for the life of the run. off by default: it is a thermal
      // and battery trade rather than a free win, and every number recorded before it was taken
      // without it. works on any driver, because it is a kernel call rather than a mesa one.
      TurboRequested = true;
    } else if (std::strcmp(argv[ArgIndex], "--vulkan-driver-env") == 0 && ArgIndex + 1 < argc) {
      // the driver's environment, not the guest's. mesa's knobs -- TU_DEBUG and friends -- are
      // read by host arm64 code, so --env can never reach them.
      HostLayer::VulkanThunk::AddDriverEnv(argv[++ArgIndex]);
    } else if (std::strcmp(argv[ArgIndex], "--vulkan-size") == 0 && ArgIndex + 1 < argc) {
      // the presentation size the guest is told the display is. it must match whatever the
      // client thinks its drawable is, or the client recreates its swapchain forever without
      // ever erroring -- a silent hang rather than a failure. it applies only when there is no window:
      // with one, the size comes from the ANativeWindow and this flag is refused. under android
      // WSI the driver answers the question and none of it is consulted.
      unsigned Width = 0, Height = 0;
      if (std::sscanf(argv[++ArgIndex], "%ux%u", &Width, &Height) != 2 || !Width || !Height) {
        std::fprintf(stderr, "[host-layer] --vulkan-size wants WxH, e.g. 1920x1080\n");
        return 2;
      }
      HostLayer::VulkanThunk::SetSurfaceSize(Width, Height);
    } else if (std::strcmp(argv[ArgIndex], "--vulkan-wsi") == 0 && ArgIndex + 1 < argc) {
      // which window system the guest gets. the default decides itself from whether there is a
      // window, which is the honest answer in both configurations we ship; the two explicit values
      // exist so that a graphics regression can be bisected against the invented swapchain rather
      // than guessed at, in the shape --smc and --asyncsig already established.
      const char* Mode = argv[++ArgIndex];
      if (std::strcmp(Mode, "headless") == 0) {
        HostLayer::VulkanThunk::SetWsiMode(HostLayer::VulkanThunk::WsiMode::Headless);
      } else if (std::strcmp(Mode, "android") == 0) {
        HostLayer::VulkanThunk::SetWsiMode(HostLayer::VulkanThunk::WsiMode::Android);
      } else if (std::strcmp(Mode, "auto") == 0) {
        HostLayer::VulkanThunk::SetWsiMode(HostLayer::VulkanThunk::WsiMode::Auto);
      } else {
        std::fprintf(stderr, "[host-layer] unknown --vulkan-wsi mode '%s' (auto|headless|android)\n", Mode);
        return 2;
      }
    } else if (std::strcmp(argv[ArgIndex], "--vulkan-dump") == 0 && ArgIndex + 1 < argc) {
      HostLayer::VulkanThunk::SetDumpPrefix(argv[++ArgIndex]);
    } else if (std::strcmp(argv[ArgIndex], "--trace-vulkan") == 0) {
      HostLayer::VulkanThunk::SetTrace(true);
    } else if (std::strcmp(argv[ArgIndex], "--vulkan-profile") == 0) {
      // where the time goes, per command, dumped every 300 presented frames. --trace-vulkan says
      // which commands are called and cannot find a stall; this sorts by time and usually answers
      // it on the first line.
      HostLayer::VulkanThunk::SetProfile(true);
    } else if (std::strcmp(argv[ArgIndex], "--audio") == 0) {
      // off by default so that every measurement taken before audio existed still reproduces
      // exactly: without it the guest's AAudio calls fail the way they always did and the fork's
      // backend degrades to silent, which is what every earlier number was taken against.
      HostLayer::AudioThunk::SetEnabled(true);
    } else if (std::strcmp(argv[ArgIndex], "--audio-lib") == 0 && ArgIndex + 1 < argc) {
      HostLayer::AudioThunk::SetLibraryPath(argv[++ArgIndex]);
      HostLayer::AudioThunk::SetEnabled(true);
    } else if (std::strcmp(argv[ArgIndex], "--trace-audio") == 0) {
      HostLayer::AudioThunk::SetTrace(true);
    } else if (std::strcmp(argv[ArgIndex], "--audio-watchdog") == 0) {
      HostLayer::AudioThunk::SetWatchdog(true);
    } else if (std::strcmp(argv[ArgIndex], "--pad") == 0) {
      // off by default, in the shape --vulkan and --audio have and for the same reason: without it
      // the guest's poll is refused, the fork reports no pad, and the run is the argument vector
      // every earlier measurement was taken on.
      HostLayer::PadBridge::SetEnabled(true);
    } else if (std::strcmp(argv[ArgIndex], "--trace-pad") == 0) {
      HostLayer::PadBridge::SetTrace(true);
    } else if (std::strcmp(argv[ArgIndex], "--pad-selftest") == 0) {
      // one fabricated rumble when the guest first polls, so the delivery path can be shown to work
      // on a title that never asks for one. it announces itself in the log.
      HostLayer::PadBridge::SetSelfTest(true);
    } else if (std::strcmp(argv[ArgIndex], "--pause-selftest") == 0) {
      PauseSelfTest = true;
    } else if (std::strcmp(argv[ArgIndex], "--libs") == 0 && ArgIndex + 1 < argc) {
      // one flag, two jobs: where PT_INTERP is resolved from, and what the guest is handed as
      // LD_LIBRARY_PATH. they are the same directory in every case that matters, and splitting
      // them would only create a way to get them out of step.
      LibDir = argv[++ArgIndex];
    } else if (std::strcmp(argv[ArgIndex], "--tmp") == 0 && ArgIndex + 1 < argc) {
      TmpDir = argv[++ArgIndex];
    } else if (std::strcmp(argv[ArgIndex], "--env") == 0 && ArgIndex + 1 < argc) {
      // NAME=VALUE, appended to the guest environment. repeatable.
      ExtraEnv.push_back(argv[++ArgIndex]);
    } else if (std::strcmp(argv[ArgIndex], "--host-features") == 0 && ArgIndex + 1 < argc) {
      // how FEXCore is told what this CPU can do. `probe` reads the ID registers, which is what a
      // run wants; `minimal` is the four extensions AT_HWCAP names plus the AVX decode table, and is
      // here so that the probe has something to be measured against inside one build.
      //
      // this is not FEXCore's own `HostFeatures` option, which is a bitmask of individual overrides
      // consumed only in FEX's frontend. what does reach a library host is `CPUFeatureRegisters`, and
      // `--fex CPUFeatureRegisters=isar0=0x...` is how a feature set other than this CPU's is put to
      // the probe.
      if (!HostLayer::HostFeatures::ParseMode(argv[++ArgIndex], FeatureMode)) {
        std::fprintf(stderr, "[host-layer] --host-features wants probe or minimal, got '%s'\n", argv[ArgIndex]);
        return 2;
      }
    } else if (std::strcmp(argv[ArgIndex], "--fex") == 0 && ArgIndex + 1 < argc) {
      // Name=Value against FEXCore's own option table, repeatable. this is how a JIT knob is
      // chosen per run: FEX's usual routes to one are its config file and its FEX_ environment
      // variables, and both are read by FEX's frontend rather than by FEXCore, so neither exists
      // in a process that hosts the core as a library.
      //
      // the names are validated below rather than here, because resolving one needs the config
      // subsystem initialised. what is checked here is only that there is something to validate.
      FexOptions.push_back(argv[++ArgIndex]);
    } else {
      break;
    }
  }

  if (!SpikeMode && ArgIndex >= argc) {
    std::fprintf(stderr, "usage: sharpdroid-host-layer [--smc none|mtrack|full] --spike\n"
                         "       sharpdroid-host-layer [--trace] [--trace-signals] [--trace-files <prefix>] [--timestamps] [--log-tids] "
                         "[--boot-progress] "
                         "[--smc none|mtrack|full] "
                         "[--asyncsig syscall|safepoint|block] [--vulkan] [--vulkan-lib <so>] "
                         "[--vulkan-driver <so>] [--vulkan-hooks <dir>] [--vulkan-driver-env NAME=VALUE]... [--vulkan-turbo] "
                         "[--vulkan-size WxH] [--vulkan-wsi auto|headless|android] [--trace-vulkan] "
                         "[--vulkan-profile] [--vulkan-dump <prefix>] "
                         "[--audio] [--audio-lib <so>] [--trace-audio] [--audio-watchdog] "
                         "[--pad] [--trace-pad] [--pad-selftest] [--pause-selftest] [--libs <dir>] "
                         "[--saf-mount <prefix>] "
                         "[--fex Name=Value]... [--host-features probe|minimal] "
                         "[--tmp <dir>] [--env NAME=VALUE]... <x86-64-elf> [guest args...]\n");
    return 2;
  }

  // applied after parsing rather than inside it, so that the *absence* of the flag is also an
  // action: SetTurbo(false) clears a clock pin leaked by a previous run that was killed rather
  // than exited, which is how every measurement in this project ends. see vulkan_thunk.cpp.
  HostLayer::VulkanThunk::SetTurbo(TurboRequested);

  std::printf("[host-layer] starting\n");
  if (HostLayer::GuestLog::Enabled()) {
    // said once, because the stamps only appear on the guest's own output and their absence from
    // the host layer's lines should read as deliberate rather than broken.
    std::printf("[host-layer] --timestamps: guest stdout/stderr lines carry [+seconds.millis] since process start\n");
  }
  if (HostLayer::GuestLog::ThreadIdsEnabled()) {
    std::printf("[host-layer] --log-tids: and the host thread that wrote each of them, as [+seconds.millis tN]\n");
  }
  if (HostLayer::BootProgress::Enabled()) {
    // said once and at the top, so that the `[boot]` line at the far end of the log is read as the
    // end of something that was asked for rather than as an assertion appearing out of nowhere.
    std::printf("[host-layer] --boot-progress: the guest's log is matched against %d boot checkpoints\n",
                HostLayer::BootProgress::Count() - 1);
  }
  FEXCore::Config::Initialize();

  // **the JIT's L1 block lookup is resized to fit a thread's working set, and a shrink strands
  // every translation cached above the new size.** LookupCache.h's UpdateDynamicL1Stats halves the
  // entry count and madvises only the region above the new one, so nothing below it is cleared;
  // invalidation clears the single index an address folds to under the mask in force now; and the
  // inline lookup returns on a guest-address match with nothing else checked. a mask that widens
  // again therefore reaches an entry left behind at a narrower one -- and a guest thread resumed
  // through the emulator's continuation trampoline runs the previous translation of that
  // trampoline, which bakes all sixteen registers as immediates and so lands the thread on the
  // right instruction holding the previous resume's operands. the audio stops permanently and
  // sometimes the picture stops with it:
  // https://github.com/mircowuffwuff/sharpdroid/issues/1
  //
  // **stopping the shrink is the whole of what the fault needs, and the option is a threshold
  // rather than a switch.** the shrink branch is `AveragePerSecond < ...Heuristic()` over a rate
  // that cannot fall below zero, so at 0 nothing reaches it. with no shrink an entry is only ever
  // written below the current mask, the region above it is zero from the initial mapping, and a
  // mask that widens can only land on an entry nothing has written. the growth heuristic is
  // untouched, so the L1 still sizes itself upward.
  //
  // DynamicL1Cache=0 closes the same fault by pinning the mask, and costs about 150 MB more for
  // the 128x size change it also makes. that size change is not what the fault needs, so it is not
  // what is paid for.
  //
  // **set before the loop below rather than beside the three values after it**, so that a launch
  // naming this option gets its own value: the unmodified arm has to stay reachable while the
  // faults it is measured against are still open.
  FEXCore::Config::Set(FEXCore::Config::CONFIG_DYNAMICL1CACHEDECREASECOUNTHEURISTIC, "0");
  // said whether or not a launch overrides it, so that a log read cold answers which lookup this
  // run used without anybody inferring it from the absence of a line.
  std::printf("[host-layer] DynamicL1CacheDecreaseCountHeuristic=0 by default, which a --fex naming it replaces\n");

  // --fex, applied ahead of the three values below so that those always win. those three are not
  // preferences: 32-bit mode silently halves the guest register file, the SMC mode has to agree
  // with what the VMA tracker is told, and the interrupt fault page is what makes an asynchronous
  // signal deliverable at all. a launch that names one of them gets the host layer's answer.
  //
  // an unknown name is refused rather than ignored. FEX's own loaders skip what they do not
  // recognise, which suits a config file a user edits by hand; here the names arrive from a table
  // in the app, so a typo that merely did nothing would look exactly like a knob that had no
  // effect on this workload -- and telling those two apart is the whole point of setting it.
  for (const char* Option : FexOptions) {
    const char* Equals = std::strchr(Option, '=');
    if (!Equals || Equals == Option) {
      std::fprintf(stderr, "[host-layer] --fex wants Name=Value, got '%s'\n", Option);
      return 2;
    }
    const std::string_view Name(Option, Equals - Option);
    const auto Resolved = ConfigOptionByName(Name);
    if (!Resolved) {
      std::fprintf(stderr, "[host-layer] --fex: no FEXCore option named '%.*s'\n", static_cast<int>(Name.size()), Name.data());
      return 2;
    }
    // the value is passed through as written. Config::Set takes strings and FEXCore converts by
    // the option's own type, so a bool wants "0" or "1" -- the "none"/"mtrack"/"full" spellings and
    // friends are handled by FEX's argument parser, which this does not use, exactly as the
    // SMCCHECKS line below already notes.
    FEXCore::Config::Set(*Resolved, Equals + 1);
    std::printf("[host-layer] --fex %s\n", Option);
  }

  // FEXCore defaults to 32-bit mode, and nothing complains if you leave it there: the decoder
  // takes its bitness from the CS descriptor, so 64-bit instructions still decode correctly.
  // what changes is the *register file* -- the Arm64Emitter constructor picks x32::SRA over x64::SRA,
  // which is 8 guest GPRs instead of 16 mapped to host registers. guest code touching R8-R15,
  // or holding a 64-bit value anywhere, then quietly gets 32-bit results.
  //
  // this has to be set before InitCore(), which is where the dispatcher and its register
  // allocation are built.
  FEXCore::Config::Set(FEXCore::Config::CONFIG_IS64BIT_MODE, "1");

  // has to agree with what the VMA tracker is told below, and has to be set before InitCore()
  // for the same reason as the line above: it changes how blocks are compiled. under `full`
  // FEXCore emits a byte-comparison guard into every block; under `mtrack` it instead promises to
  // call MarkGuestExecutableRange and expects the host layer to arrange the rest.
  //
  // the numbers are FEXCore::Config::ConfigSMC_{NONE,MTRACK,FULL}; Config::Set takes strings, and
  // the "none"/"mtrack"/"full" spellings are handled by FEX's own argument parser, which we do
  // not use.
  FEXCore::Config::Set(FEXCore::Config::CONFIG_SMCCHECKS, SMC == HostLayer::VMA::SMCMode::None    ? "0" :
                                                          SMC == HostLayer::VMA::SMCMode::MTrack ? "1" :
                                                                                                   "2");

  // the interrupt fault page, which is how an asynchronous signal reaches a thread that is off
  // running already-compiled guest code -- see guest_threads.h's AsyncSite.
  //
  // GDBSERVER is a strange-looking way to ask for it, and it is deliberate: inside FEXCore this
  // option does exactly one thing in ContextImpl::InitCore,
  // `Config.NeedsPendingInterruptFaultCheck = true`,
  // which is the switch that makes the JIT emit the check. everything else called gdbserver lives
  // in FEX's frontend, which we do not build. it is the only public way to reach the switch, and
  // FEX is not ours to add another one to.
  if (HostLayer::Threads::AsyncNeedsInterruptCheck()) {
    FEXCore::Config::Set(FEXCore::Config::CONFIG_GDBSERVER, "1");
  }

  // after the --fex loop above, because the probe reads FEXCore's own CPUFeatureRegisters out of
  // the config it just populated.
  const auto Features = HostLayer::HostFeatures::Build(FeatureMode);
  HostLayer::HostFeatures::Report(Features, FeatureMode);

  // told here rather than worked out there, because this is the one place that decides it. both
  // thunks read guest float arguments straight out of the spilled register file, and FEX only uses
  // the 32-byte-stride avx layout when both of these are set -- so a host whose SVE is 256 bits
  // wide moves the thunk boundary's ABI, which is the one thing in this probe that can break a
  // thunk rather than merely slow the JIT down.
  HostLayer::ThunkABI::SetAvxRegisterFile(Features.SupportsAVX && Features.SupportsSVE256);

  auto CTX = FEXCore::Context::Context::CreateNewContext(Features);
  if (!CTX) {
    std::fprintf(stderr, "[host-layer] CreateNewContext returned null\n");
    return 1;
  }

  // before the syscall handler is installed, because the very first thing FEXCore will ask it is
  // QueryGuestExecutableRange, and that is answered out of the tracker.
  HostLayer::VMA::Initialize(CTX.get(), SMC);

  SpikeSyscallHandler SpikeSyscalls;
  LinuxSyscalls.SetTrace(Trace);
  if (FileProbeRoot) {
    LinuxSyscalls.SetFileProbeRoot(FileProbeRoot);
  }
  // and the run is refused if it does not take. a mount that silently failed would hand the guest a
  // game directory in which every single file is missing, and that arrives as a broken dump rather
  // than as a host layer that was never wired up to anything.
  if (SafMount && !HostLayer::GuestFiles::SetMount(SafMount)) {
    return 2;
  }
  CTX->SetSyscallHandler(SpikeMode ? static_cast<FEXCore::HLE::SyscallHandler*>(&SpikeSyscalls) : &LinuxSyscalls);

  // InitCore() unconditionally dereferences the signal delegator -- it calls
  // SignalDelegation->SetConfig(...) with no null check -- so one must be installed first or it
  // segfaults at +0x38. a plain instance is enough to get through init; it delivers nothing,
  // which is what makes an unhandled guest fault fatal.
  FEXCore::SignalDelegator Signals;
  CTX->SetSignalDelegator(&Signals);
  GlobalSignals = &Signals;

  // before InitCore(): this changes how blocks are compiled, so it has to be set while the
  // dispatcher is being built rather than after any code has been generated.
  CTX->EnableExitOnHLT();

  if (!CTX->InitCore()) {
    std::fprintf(stderr, "[host-layer] InitCore failed\n");
    return 1;
  }
  std::printf("[host-layer] FEXCore initialised\n");

  // after InitCore, because the dispatcher bounds and the static register allocation the fault
  // handler reads out of the config are only populated once the dispatcher has been built.
  GuestSigs.Attach(CTX.get(), &GlobalSignals->GetConfig());
  HostLayer::Threads::Initialize(CTX.get(), &GuestSigs, &GlobalSignals->GetConfig());
  HostLayer::Threads::SetSummaryCallback(PrintRunSummary);
  HostLayer::Threads::InstallProcessFaultHandlers();

  const int Result =
    SpikeMode ? RunSpike(CTX.get()) : RunELF(CTX.get(), argv[ArgIndex], LibDir, TmpDir, ExtraEnv, argc - ArgIndex, argv + ArgIndex);

  FEXCore::Config::Shutdown();
  return Result;
}
