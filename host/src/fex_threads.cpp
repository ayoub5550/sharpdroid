// sharpdroid host layer -- FEXCore's internal threads. see fex_threads.h.

#include "fex_threads.h"

#include <FEXCore/Utils/Threads.h>
#include <FEXCore/fextl/memory.h>

#include <atomic>
#include <csignal>
#include <cstdio>
#include <pthread.h>
#include <sys/resource.h>
#include <sys/syscall.h>
#include <unistd.h>

namespace HostLayer::FEXThreads {
namespace {

std::atomic<unsigned> CreatedCount {};

class PThread final : public FEXCore::Threads::Thread {
public:
  PThread(FEXCore::Threads::ThreadFunc Func, void* Arg, FEXCore::Threads::Flags Flags)
    : Func(Func)
    , Arg(Arg)
    , LowPriority(Flags.LowPriority) {
    // the mask a thread starts with is its creator's, so the way to give a new thread a fully
    // blocked mask with no window in which it is not is to block everything here, create it,
    // and put ours back. the thread body never unblocks anything.
    sigset_t All, Old;
    sigfillset(&All);
    pthread_sigmask(SIG_SETMASK, &All, &Old);
    Started = pthread_create(&Handle, nullptr, Entry, this) == 0;
    pthread_sigmask(SIG_SETMASK, &Old, nullptr);

    if (!Started) {
      // FEXCore has no way to be told, and the only caller today is the disk cache writer, whose
      // queue then simply never drains: the run goes on, compiling as it would have with the
      // cache off. said once, loudly, rather than dying for an optimisation.
      std::fprintf(stderr, "[host-layer] could not start a FEXCore worker thread; its work will not happen\n");
    } else {
      CreatedCount.fetch_add(1, std::memory_order_relaxed);
    }
  }

  bool joinable() override {
    return Started && !Joined && !Detached;
  }

  bool join(void** Ret) override {
    if (!joinable()) {
      return false;
    }
    Joined = pthread_join(Handle, Ret) == 0;
    return Joined;
  }

  bool detach() override {
    if (!joinable()) {
      return false;
    }
    Detached = pthread_detach(Handle) == 0;
    return Detached;
  }

  bool IsSelf() override {
    return Started && pthread_equal(Handle, pthread_self());
  }

private:
  static void* Entry(void* Self) {
    auto* T = static_cast<PThread*>(Self);
    if (T->LowPriority) {
      // PRIO_PROCESS with a tid is per-thread on linux, which is exactly what is wanted and is
      // also exactly what FEX's frontend does.
      setpriority(PRIO_PROCESS, static_cast<id_t>(syscall(SYS_gettid)), 19);
    }
    return T->Func(T->Arg);
  }

  FEXCore::Threads::ThreadFunc Func;
  void* Arg;
  bool LowPriority;
  pthread_t Handle {};
  bool Started {};
  bool Joined {};
  bool Detached {};
};

fextl::unique_ptr<FEXCore::Threads::Thread> Create(FEXCore::Threads::ThreadFunc Func, void* Arg, FEXCore::Threads::Flags Flags) {
  return fextl::make_unique<PThread>(Func, Arg, Flags);
}

void CleanupAfterFork() {
  // see fex_threads.h: no fork ever inherits one of these.
}

} // namespace

void Install() {
  FEXCore::Threads::Thread::SetInternalPointers({
    .CreateThread = Create,
    .CleanupAfterFork = CleanupAfterFork,
  });
}

unsigned Created() {
  return CreatedCount.load(std::memory_order_relaxed);
}

} // namespace HostLayer::FEXThreads
