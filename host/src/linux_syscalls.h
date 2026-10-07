// sharpdroid host layer -- guest linux x86-64 syscalls onto bionic/android.
//
// FEX's JIT hands us the guest's frame; we read RAX/RDI/RSI/RDX/R10/R8/R9 out of it into
// SyscallArguments::Argument[0..6] and put our return value back in guest RAX (syscall_args.h).
// everything else is ours to do.
//
// the two translations that matter, and are easy to get wrong by assuming they are no-ops:
//   - *values*, where a guest constant differs from the arm64 one. PROT_*, MAP_* and errno are
//     identical across both architectures; O_* and `struct stat` are not.
//   - *structures*, where a guest layout differs from the host's. `struct stat` again.
// anything not translated is passed through to bionic, which issues the arm64 syscall.

#pragma once

#include "audio_thunk.h"
#include "guest_procfs.h"
#include "guest_signals.h"
#include "pad_bridge.h"
#include "syscall_args.h"
#include "vma_tracker.h"
#include "vulkan_thunk.h"

#include <FEXCore/Core/CodeCache.h>
#include <FEXCore/HLE/SyscallHandler.h>

#include <atomic>
#include <cstdint>
#include <mutex>

namespace HostLayer {

class LinuxSyscallHandler final : public FEXCore::HLE::SyscallHandler {
public:
  LinuxSyscallHandler();

  void SetSignals(GuestSignals* Signals) {
    Guest = Signals;
  }
  // where the guest heap begins: the first page past the loaded image.
  void SetBrkBase(uint64_t Base);
  GuestProcFS& ProcFS() {
    return Proc;
  }
  void SetTrace(bool Enabled) {
    Trace = Enabled;
  }
  // count what the guest asks of one directory subtree -- opens, stats, directory listings, reads,
  // and how many of each land on a descriptor that came from there. off unless this is called.
  // the reasoning, and why the counts matter beyond curiosity, is at FileProbe in linux_syscalls.cpp.
  void SetFileProbeRoot(const char* Root);

  uint64_t UnhandledCount() const {
    return Unhandled.load(std::memory_order_relaxed);
  }
  uint64_t LastUnhandledNumber() const {
    return LastUnhandled.load(std::memory_order_relaxed);
  }

  // FEX-2609's entry point: the frame alone, with the arguments, the result and the step past the
  // instruction all ours to do. it reads the one, runs Handle, and does the others -- see
  // syscall_args.h.
  void HandleSyscall(FEXCore::Core::CpuStateFrame* Frame) override;

  // the three halves of the contract with FEXCore's SMC tracking. all of them are backed by
  // vma_tracker.{h,cpp}, which is where the reasoning lives.
  FEXCore::HLE::ExecutableRangeInfo QueryGuestExecutableRange(FEXCore::Core::InternalThreadState*, uint64_t Address) override;
  void MarkGuestExecutableRange(FEXCore::Core::InternalThreadState*, uint64_t Start, uint64_t Length) override;
  void InvalidateGuestCodeRange(FEXCore::Core::InternalThreadState*, uint64_t Start, uint64_t Length) override;

  std::optional<FEXCore::ExecutableFileSectionInfo> LookupExecutableFileSection(FEXCore::Core::InternalThreadState*, uint64_t) override {
    return std::nullopt;
  }

private:
  ///< the syscall with its signal and pause brackets, returning what goes in RAX. this is what
  ///< HandleSyscall was until FEX-2609 moved the register marshalling out of the JIT.
  uint64_t Handle(FEXCore::Core::CpuStateFrame* Frame, HostLayer::SyscallArguments* Args);

  ///< the syscall table proper. Handle is a wrapper around it that turns the return into a
  ///< delivery point for asynchronous signals -- see guest_threads.h.
  uint64_t Dispatch(FEXCore::Core::CpuStateFrame* Frame, HostLayer::SyscallArguments* Args);

  uint64_t HandleBrk(uint64_t NewBreak);

  GuestSignals* Guest {};
  GuestProcFS Proc {};
  bool Trace {};

  // the break is process-wide and every guest thread's malloc can move it. glibc serialises brk
  // behind its own arena lock, so this is belt and braces -- but the arena lock is the guest's, and
  // a host layer that assumes the guest is well-behaved about its own locking is one bug away from
  // handing two threads the same pages.
  std::mutex BrkLock;
  uint64_t BrkBase {};
  uint64_t BrkCurrent {};
  uint64_t BrkArenaEnd {};

  std::atomic<uint64_t> Unhandled {};
  std::atomic<uint64_t> LastUnhandled {};
};

} // namespace HostLayer
