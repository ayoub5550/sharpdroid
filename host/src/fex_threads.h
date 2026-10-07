// sharpdroid host layer -- FEXCore's internal threads.
//
// FEXCore does not create threads itself. it asks the frontend to, through a pair of function
// pointers that start out pointing at `ERROR_AND_DIE("Frontend didn't setup thread creation!")`,
// and FEX's own frontend fills them in from LinuxEmulation's Utils/Threads.cpp. we do not build
// that frontend, so until something here fills them in, the first FEXCore feature that wants a
// worker thread kills the process with a SIGILL (ForcedAssert is a `hlt`) before any guest code
// has run.
//
// guest threads are not these. those are ours end to end -- guest_threads.h -- and FEXCore never
// asks for one. what does ask is FEXCore's own background work, which as of FEX-2609 means the
// DiskCache writer: ContextImpl's constructor calls DiskCache::Init, which with `DiskCache=1`
// starts a WorkQueueThread to write compiled blocks out behind the JIT's back. that is the one
// caller today, and the reason this file exists.
//
// the threads are plain pthreads with two properties copied from FEX's frontend, both of which
// matter more here than they look:
//
//   - every signal is blocked. the host layer routes signals by asking which guest thread the
//     receiving host thread is (guest_threads.cpp), and an internal thread is none of them. a
//     process-directed signal that the kernel happened to hand to the cache writer would arrive
//     in a handler with no guest to give it to. blocked, the kernel picks a guest thread instead.
//     synchronous faults cannot be blocked, and a fault on this thread is a host-layer bug that
//     the process fault handler reports as one.
//   - `LowPriority` becomes nice 19 for that thread alone (setpriority on its tid, which on
//     linux is per-thread). the writer serialises blobs the JIT has already finished with; it
//     must never be the thing a game's render thread waits behind.
//
// there is no fork cleanup to do: the host layer refuses clone without CLONE_THREAD
// (guest_threads.cpp), so no process ever inherits a FEXCore worker it does not have.

#pragma once

namespace HostLayer::FEXThreads {

// installs the pthread implementation behind FEXCore::Threads::Thread::Create. has to run before
// FEXCore::Context::Context::CreateNewContext, whose constructor is where DiskCache starts its
// writer.
void Install();

// how many internal threads FEXCore has asked for, for the exit summary.
unsigned Created();

} // namespace HostLayer::FEXThreads
