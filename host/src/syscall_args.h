// sharpdroid host layer -- a guest syscall's number and arguments, as one value, and how a syscall
// that returns is finished.
//
// up to FEX-2608 the JIT marshalled these itself: OS_LINUX64 told it which guest registers to load,
// and the handler was given them as FEXCore::HLE::SyscallArguments with its return value stored back
// into RAX by the JIT. FEX-2609 removed all of that ("Removes syscall optimization"): the handler
// gets the frame alone and both halves are its own -- read the registers out of CPUState, write the
// result into RAX.
//
// this is the old struct kept on our side, so the syscall table and the thunks keep reading
// Argument[0..6] exactly as before and the API change stays one function wide. it is built the one
// way FEX's own frontend builds it (LinuxSyscalls/Syscalls.cpp GetArg, 64-bit mode): RAX for the
// number, then RDI, RSI, RDX, R10, R8, R9 -- R10 rather than RCX, because `syscall` itself
// overwrites RCX with the return address.
//
// reading them out of CPUState is exact rather than a snapshot of something stale: the JIT spills
// every guest register before it calls the handler and fills them all back afterwards
// (JIT/BranchOps.cpp DEF_OP(Syscall), GPRSpillMask = FPRSpillMask = ~0U), which is also what makes a
// write to RAX here the value the guest sees.
//
// **why the pin is FEX-2609.1 and not FEX-2609.** on linux FEX-2609 left `syscall` in the middle of
// its JIT block, and its register allocator had no way to know the call rewrote RAX: a value it had
// already put in RAX's static register before the call was treated as still being there after the
// refill. the visible shape is two syscalls in one block, both `mov eax, 1` -- the second load is
// elided, so the second write(2) goes out with the first one's return value as its number. the
// regression set caught it as signals/smc/pause/vkrender/aaudio failures (FEX issue #5942).
// FEX-2609.1 is FEX-2609 plus the upstream fix, which makes `syscall` end its block: the JIT exits
// to the dispatcher after every syscall and resumes from CPUState::rip, so stepping RIP past the
// instruction is now the handler's job on every path -- which is what CompleteSyscall does.

#pragma once

#include <FEXCore/Core/CoreState.h>
#include <FEXCore/Core/X86Enums.h>

#include <cstddef>
#include <cstdint>

namespace HostLayer {

struct SyscallArguments {
  static constexpr std::size_t MAX_ARGS = 7;
  uint64_t Argument[MAX_ARGS];

  static SyscallArguments FromFrame(const FEXCore::Core::CpuStateFrame* Frame) {
    const auto& G = Frame->State.gregs;
    return SyscallArguments {{
      G[FEXCore::X86State::REG_RAX],
      G[FEXCore::X86State::REG_RDI],
      G[FEXCore::X86State::REG_RSI],
      G[FEXCore::X86State::REG_RDX],
      G[FEXCore::X86State::REG_R10],
      G[FEXCore::X86State::REG_R8],
      G[FEXCore::X86State::REG_R9],
    }};
  }
};

// both instructions that reach a syscall handler are two bytes: `syscall` is 0F 05, `int 0x80` is
// CD 80. while the handler runs, CPUState::rip still points *at* the instruction -- the frontend
// stores it there before the call (OpDispatchBuilder::SyscallOp).
inline constexpr uint64_t SyscallInstructionSize = 2;

// how every syscall that returns ends: the result where the guest reads it, and RIP past the
// instruction, because since FEX-2609.1 the JIT resumes from CPUState::rip rather than carrying on
// inside the block. the paths that do not return -- rt_sigreturn, a signal delivered at the
// syscall's exit, a thread's exit, a clone child's first instruction -- set RIP themselves.
inline void CompleteSyscall(FEXCore::Core::CpuStateFrame* Frame, uint64_t Result) {
  Frame->State.gregs[FEXCore::X86State::REG_RAX] = Result;
  Frame->State.rip += SyscallInstructionSize;
}

} // namespace HostLayer
