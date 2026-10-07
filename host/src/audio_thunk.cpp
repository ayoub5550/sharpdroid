// see audio_thunk.h for the design, for why a syscall is the trap and for why three of the 72
// entry points are refused rather than forwarded.

#include "audio_thunk.h"
#include "thunk_abi.h"

#include <aaudio/AAudio.h>

#include <dirent.h>
#include <dlfcn.h>
#include <sys/uio.h>
#include <time.h>
#include <unistd.h>

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <thread>
#include <vector>

namespace HostLayer::AudioThunk {

namespace {

// --- the generated command list, read three times ------------------------------------------
enum CommandId : uint32_t {
#define AACMD(name) Id_##name,
#include "aaudio_commands.inc"
#undef AACMD
  CommandCount
};

// the signatures, generated rather than taken from the header's own declarations -- see
// scripts/gen-thunks.py for why (availability attributes, and eleven entry points that are not
// in the API 28 stub library at all).
#include "aaudio_protos.inc"

bool ThunkEnabled {};
bool TraceEnabled {};
bool Complained {};

const char* LibraryPath = "libaaudio.so";
void* Library {};
std::once_flag LibraryOnce;

std::atomic<uint64_t> Calls {};
std::atomic<uint64_t> Unresolved {};
std::atomic<uint64_t> Refused {};
const char* LastUnresolvedName {};

struct Command {
  const char* Name;
  uint64_t (*Invoke)(void* Fn, ThunkABI::ArgReader& R);
};

// nothing here needs a type-level refusal the way vulkan's VkAllocationCallbacks* does: the three
// parameters that would carry a guest function pointer belong to three named commands, and naming
// three commands is cheaper than a rule. so every argument passes straight through.
template<typename PFN>
using Thunk = ThunkABI::Marshal<ThunkABI::PassThrough, PFN>;

const Command Commands[] = {
#define AACMD(name) {#name, &Thunk<PFN_##name>::Call},
#include "aaudio_commands.inc"
#undef AACMD
};
static_assert(std::size(Commands) == CommandCount, "command list and id enum disagree");

std::atomic<void*> Resolved[CommandCount] {};

uint64_t NowNanos() {
  timespec Time {};
  ::clock_gettime(CLOCK_MONOTONIC, &Time);
  return static_cast<uint64_t>(Time.tv_sec) * 1000000000ull + static_cast<uint64_t>(Time.tv_nsec);
}

void OpenLibrary() {
  Library = ::dlopen(LibraryPath, RTLD_NOW | RTLD_LOCAL);
  if (!Library) {
    std::printf("[audio] dlopen(%s) failed: %s\n", LibraryPath, ::dlerror());
    return;
  }
  std::printf("[audio] host %s opened\n", LibraryPath);
}

void* Resolve(uint32_t Id) {
  if (void* Cached = Resolved[Id].load(std::memory_order_acquire)) {
    return Cached;
  }
  std::call_once(LibraryOnce, OpenLibrary);
  if (!Library) {
    return nullptr;
  }
  // by name against the device's real libaaudio.so, so an entry point this device's android is
  // too old to have comes back null and is reported rather than failing to link.
  void* Fn = ::dlsym(Library, Commands[Id].Name);
  if (Fn) {
    Resolved[Id].store(Fn, std::memory_order_release);
  }
  return Fn;
}

template<typename PFN>
PFN Host(uint32_t Id) {
  return reinterpret_cast<PFN>(Resolve(Id));
}

// --- the streams the guest opened -----------------------------------------------------------
//
// tracked for one reason, and it is a correctness guard rather than bookkeeping: **a stream that
// opens and plays nothing looks exactly like a stream that works**. every call succeeds,
// every buffer is accepted, and the only thing that differs is that the device never consumed
// anything. AAudioStream_getFramesRead climbing at the stream's own sample rate is what separates
// the two, so it is measured here rather than hoped for.
//
// what the stream *actually* opened with is read back and printed too. AAudio negotiates: the
// sample rate, channel count and format can all differ from what the builder asked for, and
// AAUDIO_PERFORMANCE_MODE_LOW_LATENCY can change the burst size underneath. printing the
// negotiated values is the honest report and it costs three calls at open.
struct StreamInfo {
  void* Stream {};
  int32_t SampleRate {};
  int32_t ChannelCount {};
  int32_t Format {};
  int32_t FramesPerBurst {};
  int64_t FramesReadAtOpen {};
  uint64_t OpenNanos {};
  uint64_t LastReportNanos {};
  // how long the host has held this stream paused, so the share of the sample rate it reports is a
  // share of the time it was meant to be playing. a stream the guest paused itself is not counted:
  // that silence is the guest's to explain.
  uint64_t HeldNanos {};
  uint64_t HeldSinceNanos {};
  bool HeldByHost {};
};

std::mutex StreamLock;
std::vector<StreamInfo> Streams;

// --- the watchdog ---------------------------------------------------------------------------
//
// **the periodic report above rides on the write path, which makes it blind to the one failure
// that actually happens: the guest stopping.** no write means no report, and a log that simply
// goes quiet says nothing about whether the guest stopped calling, our write is stuck inside the
// device, or the stream died underneath. those are three different bugs.
//
// so this is a host thread that reports whether or not anything is being submitted. it is the
// audio equivalent of noticing that `FLIP` and `FPS` are different numbers: the interesting
// quantity is the *gap* between what the guest is doing and what the device is doing.
//
// it deliberately reads only the two cheap local counters and the cached state. AAudio's client
// drains the service's message queue inside some of its calls, so a chattier watchdog could
// perfectly well paper over the very stall it was added to find.
// counted on both sides of the host call on purpose. "the log went quiet" cannot tell a guest that
// stopped calling from a guest parked inside AAudio, and those are opposite bugs: one is ours to
// find upstream in the fork, the other is a write that ignored its timeout. Started == Finished
// means nothing is in flight and the guest is simply not calling.
std::atomic<uint64_t> WritesStarted {};
std::atomic<uint64_t> WritesFinished {};
std::atomic<uint64_t> LastWriteNanos {};
// the guest thread that last submitted, so the watchdog can ask procfs whether it still exists and
// what it is doing. a thread that has *died* takes its /proc/self/task entry with it, which
// distinguishes "the audio thread is gone" from "the audio thread is asleep somewhere else" without
// needing a debugger this device will not give us.
std::atomic<int> LastWriteTid {};
std::atomic<bool> WatchdogRunning {};
// while the guest is paused it submits nothing and that is the point, so the watchdog says nothing.
std::atomic<bool> HostPaused {};
// **on whenever audio is**, and that is deliberate rather than a debug leftover. it is silent until
// the guest stops submitting, it costs one sleeping thread and one clock read a second, and the
// failure it exists to report is one that otherwise announces itself only as "the sound went away".
// --audio-watchdog makes it chatty; it never turns it off.
bool WatchdogVerbose {};
// --audio-watchdog also dumps every thread in the process at the stall. off by default: it is a
// few dozen lines and only the first stall of a session is worth them.
bool WatchdogDumpThreads {};

void WatchdogLoop();

void StartWatchdog() {
  bool Expected = false;
  if (!WatchdogRunning.compare_exchange_strong(Expected, true)) {
    return;
  }
  std::thread(WatchdogLoop).detach();
}

const char* FormatName(int32_t Format) {
  switch (Format) {
  case AAUDIO_FORMAT_PCM_I16: return "pcm16";
  case AAUDIO_FORMAT_PCM_FLOAT: return "float32";
  case AAUDIO_FORMAT_PCM_I24_PACKED: return "pcm24";
  case AAUDIO_FORMAT_PCM_I32: return "pcm32";
  case AAUDIO_FORMAT_INVALID: return "invalid";
  case AAUDIO_FORMAT_UNSPECIFIED: return "unspecified";
  default: return "?";
  }
}

void NoteStreamOpened(void* Stream) {
  auto GetInt32 = [Stream](uint32_t Id) -> int32_t {
    auto Fn = Host<int32_t (*)(AAudioStream*)>(Id);
    return Fn ? Fn(static_cast<AAudioStream*>(Stream)) : 0;
  };
  auto GetInt64 = [Stream](uint32_t Id) -> int64_t {
    auto Fn = Host<int64_t (*)(AAudioStream*)>(Id);
    return Fn ? Fn(static_cast<AAudioStream*>(Stream)) : 0;
  };

  StreamInfo Info {};
  Info.Stream = Stream;
  Info.SampleRate = GetInt32(Id_AAudioStream_getSampleRate);
  Info.ChannelCount = GetInt32(Id_AAudioStream_getChannelCount);
  Info.Format = GetInt32(Id_AAudioStream_getFormat);
  Info.FramesPerBurst = GetInt32(Id_AAudioStream_getFramesPerBurst);
  Info.FramesReadAtOpen = GetInt64(Id_AAudioStream_getFramesRead);
  Info.OpenNanos = NowNanos();
  Info.LastReportNanos = Info.OpenNanos;

  std::printf("[audio] stream %p opened: %d Hz, %d ch, %s, burst %d frames, buffer %d of %d\n", Stream,
              Info.SampleRate, Info.ChannelCount, FormatName(Info.Format), Info.FramesPerBurst,
              GetInt32(Id_AAudioStream_getBufferSizeInFrames), GetInt32(Id_AAudioStream_getBufferCapacityInFrames));

  {
    std::lock_guard<std::mutex> Guard(StreamLock);
    Streams.push_back(Info);
  }
  StartWatchdog();
}

// one line per stream: how many frames the device has consumed against how many it should have
// consumed in the elapsed time. anything far below 100% is a stream that is not really playing,
// which is the failure this whole section exists to make visible.
void ReportStream(const StreamInfo& Info, uint64_t Now) {
  auto FramesRead = Host<int64_t (*)(AAudioStream*)>(Id_AAudioStream_getFramesRead);
  auto FramesWritten = Host<int64_t (*)(AAudioStream*)>(Id_AAudioStream_getFramesWritten);
  auto XRuns = Host<int32_t (*)(AAudioStream*)>(Id_AAudioStream_getXRunCount);
  if (!FramesRead || !Info.SampleRate) {
    return;
  }
  auto* Stream = static_cast<AAudioStream*>(Info.Stream);
  const int64_t Read = FramesRead(Stream) - Info.FramesReadAtOpen;
  const uint64_t Held = Info.HeldNanos + (Info.HeldByHost ? Now - Info.HeldSinceNanos : 0);
  const double Seconds = static_cast<double>(Now - Info.OpenNanos - Held) / 1e9;
  const double Expected = Seconds * static_cast<double>(Info.SampleRate);
  std::printf("[audio] stream %p: %lld frames read in %.2f s = %.1f%% of %d Hz, %lld written, %d xruns\n",
              Info.Stream, static_cast<long long>(Read), Seconds,
              Expected > 0.0 ? 100.0 * static_cast<double>(Read) / Expected : 0.0, Info.SampleRate,
              FramesWritten ? static_cast<long long>(FramesWritten(Stream)) : -1LL, XRuns ? XRuns(Stream) : -1);
}

// called from the write path, so the report costs nothing on a run that never plays anything and
// arrives roughly once a second on one that does.
void MaybeReportStream(void* Stream) {
  StreamInfo Copy {};
  {
    std::lock_guard<std::mutex> Guard(StreamLock);
    for (auto& Info : Streams) {
      if (Info.Stream != Stream) {
        continue;
      }
      const uint64_t Now = NowNanos();
      if (Now - Info.LastReportNanos < 1000000000ull) {
        return;
      }
      Info.LastReportNanos = Now;
      Copy = Info;
      break;
    }
  }
  if (Copy.Stream) {
    ReportStream(Copy, NowNanos());
  }
}

void ReportOneStream(void* Stream) {
  StreamInfo Copy {};
  {
    std::lock_guard<std::mutex> Guard(StreamLock);
    for (const auto& Info : Streams) {
      if (Info.Stream == Stream) {
        Copy = Info;
        break;
      }
    }
  }
  if (Copy.Stream) {
    ReportStream(Copy, NowNanos());
  }
}

const char* StateName(int32_t State) {
  switch (State) {
  case AAUDIO_STREAM_STATE_OPEN: return "OPEN";
  case AAUDIO_STREAM_STATE_STARTING: return "STARTING";
  case AAUDIO_STREAM_STATE_STARTED: return "STARTED";
  case AAUDIO_STREAM_STATE_PAUSING: return "PAUSING";
  case AAUDIO_STREAM_STATE_PAUSED: return "PAUSED";
  case AAUDIO_STREAM_STATE_FLUSHING: return "FLUSHING";
  case AAUDIO_STREAM_STATE_FLUSHED: return "FLUSHED";
  case AAUDIO_STREAM_STATE_STOPPING: return "STOPPING";
  case AAUDIO_STREAM_STATE_STOPPED: return "STOPPED";
  case AAUDIO_STREAM_STATE_CLOSING: return "CLOSING";
  case AAUDIO_STREAM_STATE_CLOSED: return "CLOSED";
  case AAUDIO_STREAM_STATE_DISCONNECTED: return "DISCONNECTED";
  default: return "?";
  }
}

// --- what the stalled thread is waiting for ---------------------------------------------------
//
// the thread state says the audio thread is asleep in a futex and not inside AAudio. it does not say
// **whether anyone ever tried to wake it**, and those are opposite bugs: a producer that never
// signalled is upstream of this project entirely, while a signal that was published and not
// delivered is the host layer's own syscall forwarding.
//
// the futex word settles it, and it is readable from here for free. the address is a *guest* one --
// every stalled dump so far has it in the guest heap, not in bionic's -- and guest and host share
// one address space 1:1, so a guest pointer is a host pointer. glibc's condition variable parks its
// waiters on a word that a signaller bumps *before* calling FUTEX_WAKE, so:
//
//   the word never moves  -> nobody signalled. the producer is the bug, and it is not ours.
//   the word moves        -> a signal was published and the sleeper did not act on it. we are.
//
// read through process_vm_readv rather than by dereferencing it. an address that turns out not to be
// mapped then comes back as EFAULT instead of killing the process we are in the middle of
// diagnosing, which matters more than usual for a fault that takes 17 runs to reproduce.
bool ReadGuestWord(uint64_t Address, uint32_t& Out) {
  iovec Local {&Out, sizeof(Out)};
  iovec Remote {reinterpret_cast<void*>(static_cast<uintptr_t>(Address)), sizeof(Out)};
  return ::process_vm_readv(::getpid(), &Local, 1, &Remote, 1, 0) == static_cast<ssize_t>(sizeof(Out));
}

// /proc/self/task/<tid>/syscall is "nr arg0 arg1 ... sp pc", or "running". 98 is futex on arm64 --
// the *host* number, because the guest's futex is forwarded to the host's one for one.
bool ParseFutexWait(const char* Syscall, uint64_t& Address, uint64_t& Operation, uint64_t& Value) {
  unsigned long long A {}, O {}, V {};
  if (std::sscanf(Syscall, "98 %llx %llx %llx", &A, &O, &V) != 3) {
    return false;
  }
  Address = A;
  Operation = O;
  Value = V;
  return true;
}

// **passive by default, and that is the whole design.** a loop that calls
// getState/getFramesRead/getXRunCount every second makes the stall stop happening -- which is not a
// fix, it is an observer effect: AAudio's client drains the service's up-message queue inside its
// own calls, so a chatty watchdog does the draining the guest has stopped doing. an instrument that
// cures the disease cannot measure it.
//
// so the loop reads nothing but our own two counters, and only once the gap is real does it ask
// the stream a single round of questions -- by which point perturbing it no longer matters.
void WatchdogLoop() {
  bool Reported = false;
  double QuietAtReport = 0.0;
  for (;;) {
    ::usleep(1000000);
    if (HostPaused.load(std::memory_order_acquire)) {
      continue;
    }
    std::vector<StreamInfo> Copy;
    {
      std::lock_guard<std::mutex> Guard(StreamLock);
      Copy = Streams;
    }
    if (Copy.empty()) {
      continue;
    }

    const uint64_t Last = LastWriteNanos.load(std::memory_order_relaxed);
    if (!Last) {
      continue;
    }
    const double Quiet = static_cast<double>(NowNanos() - Last) / 1e9;
    const uint64_t Started = WritesStarted.load(std::memory_order_relaxed);
    const uint64_t Finished = WritesFinished.load(std::memory_order_relaxed);

    if (Quiet < 2.0) {
      // **a recovery is reported as loudly as the stall was**, and that is not politeness: without
      // it a game that legitimately goes quiet -- a menu, a silent scene, a stream it keeps open and
      // stops feeding -- leaves one STALL line in the log and nothing after it, which reads exactly
      // like audio that never came back. one line each way makes a gap self-describing.
      if (Reported) {
        std::printf("[audio-wd] recovered: the guest is submitting again after %.2f s\n", QuietAtReport);
      }
      Reported = false;
      if (WatchdogVerbose) {
        std::printf("[audio-wd] %llu writes, last %.2f s ago\n", static_cast<unsigned long long>(Finished), Quiet);
      }
      continue;
    }

    // the gap is real. say so once, with everything the stream will tell us -- which of the three
    // possible bugs this is turns entirely on `state` and on whether `read` is still advancing.
    if (Reported) {
      continue;
    }
    Reported = true;
    QuietAtReport = Quiet;

    // where the guest's audio thread is, asked of procfs rather than of a debugger this device
    // will not give us. a missing directory means the thread died; 'S' means it is asleep
    // somewhere; 'R' means it is running and simply not coming back here.
    const int Tid = LastWriteTid.load(std::memory_order_relaxed);
    char ThreadState[512] = "no thread recorded";
    // hoisted out of the block below because the futex sampling further down needs both: the name to
    // find the thread's siblings, and the syscall line to get the address it is parked on.
    char Name[64] = "?";
    // "num arg0 .. arg5 sp pc" for a thread stopped in a syscall, or "running". this is the one
    // that says *what* it is waiting for: a futex is a lock or a condition variable someone else
    // has to signal, and that is a very different bug from a sleep or a poll.
    char Syscall[256] = "?";
    auto Slurp = [](int Which, const char* What, char* Out, size_t Size) -> bool {
      char Path[64];
      std::snprintf(Path, sizeof(Path), "/proc/self/task/%d/%s", Which, What);
      std::FILE* File = std::fopen(Path, "re");
      if (!File) {
        return false;
      }
      Out[0] = 0;
      if (std::fgets(Out, static_cast<int>(Size), File)) {
        for (char* p = Out; *p; ++p) {
          if (*p == '\n') {
            *p = 0;
            break;
          }
        }
      }
      std::fclose(File);
      return true;
    };
    if (Tid) {
      char Stat[512] {};
      if (!Slurp(Tid, "stat", Stat, sizeof(Stat))) {
        std::snprintf(ThreadState, sizeof(ThreadState), "tid %d is GONE (the guest audio thread died)", Tid);
      } else {
        Slurp(Tid, "comm", Name, sizeof(Name));
        Slurp(Tid, "syscall", Syscall, sizeof(Syscall));
        const char* Close = std::strrchr(Stat, ')');
        std::snprintf(ThreadState, sizeof(ThreadState), "tid %d '%s' state %c syscall [%s]", Tid, Name,
                      Close && Close[1] && Close[2] ? Close[2] : '?', Syscall);
      }

      // and every other thread in the process, once. the audio thread waits on a condition variable
      // with no timeout, and the only way to tell "waiting for a garbage collection that is stuck"
      // from "waiting for something else entirely" is what the other threads are doing at the same
      // instant: a stalled GC parks *every* managed thread on the same address.
      if (WatchdogDumpThreads) {
        if (DIR* Tasks = ::opendir("/proc/self/task")) {
          while (dirent* Entry = ::readdir(Tasks)) {
            const int Other = std::atoi(Entry->d_name);
            if (Other <= 0) {
              continue;
            }
            char OtherStat[512] {};
            char OtherName[64] = "?";
            char OtherSyscall[256] = "?";
            if (!Slurp(Other, "stat", OtherStat, sizeof(OtherStat))) {
              continue;
            }
            Slurp(Other, "comm", OtherName, sizeof(OtherName));
            Slurp(Other, "syscall", OtherSyscall, sizeof(OtherSyscall));
            const char* Close = std::strrchr(OtherStat, ')');
            std::printf("[audio-wd]   tid %d '%s' state %c syscall [%s]%s\n", Other, OtherName,
                        Close && Close[1] && Close[2] ? Close[2] : '?', OtherSyscall,
                        Other == Tid ? "   <-- the audio thread" : "");
          }
          ::closedir(Tasks);
        }
      }
    }
    std::printf("[audio-wd] STALL: writes started %llu finished %llu -> %s. %s\n",
                static_cast<unsigned long long>(Started), static_cast<unsigned long long>(Finished),
                Started == Finished ? "nothing in flight, the guest is not calling"
                                    : "A WRITE IS STILL IN FLIGHT inside AAudio",
                ThreadState);

    auto State = Host<int32_t (*)(AAudioStream*)>(Id_AAudioStream_getState);
    auto FramesRead = Host<int64_t (*)(AAudioStream*)>(Id_AAudioStream_getFramesRead);
    auto FramesWritten = Host<int64_t (*)(AAudioStream*)>(Id_AAudioStream_getFramesWritten);
    auto XRuns = Host<int32_t (*)(AAudioStream*)>(Id_AAudioStream_getXRunCount);
    for (const auto& Info : Copy) {
      auto* Stream = static_cast<AAudioStream*>(Info.Stream);
      std::printf("[audio-wd] STALL: no write for %.2f s after %llu. %p state=%s read=%lld written=%lld xruns=%d\n",
                  Quiet, static_cast<unsigned long long>(Finished), Info.Stream,
                  State ? StateName(State(Stream)) : "?",
                  FramesRead ? static_cast<long long>(FramesRead(Stream)) : -1LL,
                  FramesWritten ? static_cast<long long>(FramesWritten(Stream)) : -1LL, XRuns ? XRuns(Stream) : -1);
    }

    // and then the question the dump above cannot answer: is anything still *trying* to wake this
    // thread? sampled rather than read once, because a single value distinguishes nothing -- a word
    // that is already non-zero may simply be a stale count from before the wait began.
    //
    // the siblings are sampled alongside it because the producer, on every stall recorded so far, is
    // a second thread with the same truncated name that is alive and running. whether *it* is
    // progressing decides which end of a producer/consumer pair to look at next.
    uint64_t Address {}, Operation {}, Value {};
    if (Tid && ParseFutexWait(Syscall, Address, Operation, Value)) {
      uint32_t Word {};
      if (!ReadGuestWord(Address, Word)) {
        std::printf("[audio-wd] STALL: futex word at 0x%llx is not readable\n", static_cast<unsigned long long>(Address));
      } else {
        struct Sibling {
          int Tid;
          char Syscall[256];
        };
        std::vector<Sibling> Before;
        if (DIR* Tasks = ::opendir("/proc/self/task")) {
          while (dirent* Entry = ::readdir(Tasks)) {
            const int Other = std::atoi(Entry->d_name);
            char OtherName[64] = "?";
            if (Other <= 0 || Other == Tid || !Slurp(Other, "comm", OtherName, sizeof(OtherName)) ||
                std::strcmp(OtherName, Name) != 0) {
              continue;
            }
            Sibling S {Other, {}};
            Slurp(Other, "syscall", S.Syscall, sizeof(S.Syscall));
            Before.push_back(S);
          }
          ::closedir(Tasks);
        }

        const uint32_t First = Word;
        uint32_t Changes = 0;
        uint32_t Last = First;
        for (int Sample = 0; Sample < 20; ++Sample) {
          ::usleep(100000);
          uint32_t Now {};
          if (ReadGuestWord(Address, Now) && Now != Last) {
            ++Changes;
            Last = Now;
          }
        }
        // **the decisive case is `First != Value`, not `Changes`.** FUTEX_WAIT_BITSET sleeps only
        // while the word equals what the caller passed, so a word that no longer equals it has
        // already been changed by somebody -- and by futex convention whoever changed it owed this
        // thread a FUTEX_WAKE. that state is *static*: the signal was published, the sleeper did not
        // act on it, and nothing further needs to happen for the log to be conclusive. sampling adds
        // the live case on top of it, where a producer is still going round its loop.
        const bool Published = First != static_cast<uint32_t>(Value) || Changes > 0;
        std::printf("[audio-wd] STALL: futex 0x%llx op 0x%llx slept while it read %llu. word %u -> %u over 2 s, "
                    "%u changes -> %s\n",
                    static_cast<unsigned long long>(Address), static_cast<unsigned long long>(Operation),
                    static_cast<unsigned long long>(Value), First, Last, Changes,
                    Published ? "SIGNALLED AND NOT WOKEN: a wake was published and lost on our side"
                              : "nobody signalled it: the producer never published, and is upstream of us");

        for (const auto& S : Before) {
          char After[256] = "?";
          Slurp(S.Tid, "syscall", After, sizeof(After));
          std::printf("[audio-wd] STALL: sibling tid %d '%s' %s\n  before [%s]\n  after  [%s]\n", S.Tid, Name,
                      std::strcmp(After, S.Syscall) == 0 ? "UNMOVED" : "progressing", S.Syscall, After);
        }
      }
    }
  }
}

void ForgetStream(void* Stream) {
  std::lock_guard<std::mutex> Guard(StreamLock);
  for (auto It = Streams.begin(); It != Streams.end(); ++It) {
    if (It->Stream == Stream) {
      Streams.erase(It);
      return;
    }
  }
}

// --- the bounded write timeout ---------------------------------------------------------------
//
// the host layer delivers asynchronous signals at syscall exits only, and CoreCLR suspends every thread with
// SIGRTMIN to collect. a guest thread parked indefinitely inside AAudioStream_write is a thread
// that cannot acknowledge a GC suspension, and the collector waits for all of them -- a way to
// stall the whole managed runtime from a place nothing would think to look.
//
// so the guest's timeout is clamped rather than trusted. a short write is a legal AAudio result
// the caller retries, which is what the fork's IHostAudioStream.Submit contract already says it
// does; an unbounded one is not something the guest gets to ask for.
constexpr int64_t MaxWriteTimeoutNanos = 20000000; // 20 ms, comfortably over a burst period
bool ClampedOnce {};

} // namespace

uint64_t CallCount() {
  return Calls.load(std::memory_order_relaxed);
}

uint64_t UnresolvedCount() {
  return Unresolved.load(std::memory_order_relaxed);
}

uint64_t RefusedCount() {
  return Refused.load(std::memory_order_relaxed);
}

const char* LastUnresolved() {
  return LastUnresolvedName;
}

void SetEnabled(bool Value) {
  ThunkEnabled = Value;
}

bool Enabled() {
  return ThunkEnabled;
}

void SetTrace(bool Value) {
  TraceEnabled = Value;
}

void SetWatchdog(bool Value) {
  WatchdogVerbose = Value;
  WatchdogDumpThreads = Value;
}

void SetLibraryPath(const char* Path) {
  if (Path && *Path) {
    LibraryPath = Path;
  }
}

namespace {

// --- pausing the streams with the guest -----------------------------------------------------
//
// **on a thread of its own, because AAudio's answer can take seconds.** a stream that has sat paused
// for a while can have been taken back by the audio server, and the request to start it again then
// waits on the server before reporting the stream disconnected -- 2.2 s was measured, on the app's UI
// thread, before this moved. a disconnected stream needs nothing from here: the guest's next write is
// refused, and the emulator reopens the stream on the same device, which it already does for a
// headset unplugged mid-game.
//
// **one thread, applying the state last asked for**, so that a pause and a resume asked for in quick
// succession arrive in the order they were asked rather than in whichever order two threads happened
// to reach the stream lock -- the wrong order being a game running over streams left paused.

std::mutex ControlLock;
std::condition_variable ControlChanged;
bool WantHeld {};
bool ControlStarted {};

void HoldStreams() {
  auto GetState = Host<aaudio_stream_state_t (*)(AAudioStream*)>(Id_AAudioStream_getState);
  auto RequestPause = Host<aaudio_result_t (*)(AAudioStream*)>(Id_AAudioStream_requestPause);
  if (!GetState || !RequestPause) {
    return;
  }
  unsigned Held = 0;
  std::lock_guard<std::mutex> Guard(StreamLock);
  for (auto& Info : Streams) {
    auto* Stream = static_cast<AAudioStream*>(Info.Stream);
    const aaudio_stream_state_t State = GetState(Stream);
    if (Info.HeldByHost || (State != AAUDIO_STREAM_STATE_STARTED && State != AAUDIO_STREAM_STATE_STARTING)) {
      continue;
    }
    if (RequestPause(Stream) == AAUDIO_OK) {
      Info.HeldByHost = true;
      Info.HeldSinceNanos = NowNanos();
      ++Held;
    }
  }
  std::printf("[audio] paused %u of %zu stream(s) with the guest\n", Held, Streams.size());
  std::fflush(stdout);
}

void ReleaseStreams() {
  auto RequestStart = Host<aaudio_result_t (*)(AAudioStream*)>(Id_AAudioStream_requestStart);
  auto ToText = Host<const char* (*)(int32_t)>(Id_AAudio_convertResultToText);
  std::lock_guard<std::mutex> Guard(StreamLock);
  for (auto& Info : Streams) {
    if (!Info.HeldByHost) {
      continue;
    }
    Info.HeldByHost = false;
    Info.HeldNanos += NowNanos() - Info.HeldSinceNanos;
    if (!RequestStart) {
      continue;
    }
    const aaudio_result_t Result = RequestStart(static_cast<AAudioStream*>(Info.Stream));
    if (Result != AAUDIO_OK) {
      std::printf("[audio] stream %p did not start again after the pause: %s (%d). the guest reopens it on "
                  "its next write\n",
                  Info.Stream, ToText ? ToText(Result) : "?", Result);
      std::fflush(stdout);
    }
  }
}

void ControlLoop() {
  bool Held = false;
  std::unique_lock Lock(ControlLock);
  for (;;) {
    ControlChanged.wait(Lock, [&Held] { return WantHeld != Held; });
    const bool Target = WantHeld;
    Lock.unlock();
    if (Target) {
      HoldStreams();
    } else {
      ReleaseStreams();
    }
    Lock.lock();
    Held = Target;
  }
}

} // namespace

void SetPaused(bool Paused) {
  if (!ThunkEnabled) {
    return;
  }
  // the watchdog is told at once, from here, so that it neither reports the pause as a stall nor
  // counts the time paused against the guest once it is resumed.
  if (!Paused && LastWriteNanos.load(std::memory_order_relaxed)) {
    LastWriteNanos.store(NowNanos(), std::memory_order_relaxed);
  }
  HostPaused.store(Paused, std::memory_order_release);
  {
    std::lock_guard Lock(ControlLock);
    WantHeld = Paused;
    if (!ControlStarted) {
      ControlStarted = true;
      std::thread(ControlLoop).detach();
    }
  }
  ControlChanged.notify_all();
}

void ReportStreams() {
  std::vector<StreamInfo> Copy;
  {
    std::lock_guard<std::mutex> Guard(StreamLock);
    Copy = Streams;
  }
  const uint64_t Now = NowNanos();
  for (const auto& Info : Copy) {
    ReportStream(Info, Now);
  }
}

uint64_t Handle(FEXCore::Core::CpuStateFrame* Frame, HostLayer::SyscallArguments* Args) {
  const uint32_t Id = static_cast<uint32_t>(Args->Argument[0] & 0xFFFF);

  // the guest half of the thunk is a file staged next to glibc, so it is found and loaded whether
  // or not --audio was given; there is no version of "not staged" to fall back to. so an unenabled
  // thunk answers every call with a failure the guest can act on, rather than by letting the magic
  // number fall through to the syscall table as an unhandled number. the fork's backend then
  // degrades to silent exactly as it did before the thunk existed.
  if (!ThunkEnabled) {
    if (!Complained) {
      Complained = true;
      std::printf("[audio] guest asked for AAudio and the thunk is not enabled (pass --audio)\n");
    }
    return static_cast<uint64_t>(static_cast<int64_t>(AAUDIO_ERROR_UNAVAILABLE));
  }

  if (Id >= CommandCount) {
    std::printf("[audio] call to unknown command id %u\n", Id);
    return static_cast<uint64_t>(static_cast<int64_t>(AAUDIO_ERROR_UNAVAILABLE));
  }

  Calls.fetch_add(1, std::memory_order_relaxed);
  if (TraceEnabled) {
    std::printf("[audio] %s(0x%llX, 0x%llX, 0x%llX, ...)\n", Commands[Id].Name,
                static_cast<unsigned long long>(Args->Argument[1]), static_cast<unsigned long long>(Args->Argument[2]),
                static_cast<unsigned long long>(Args->Argument[3]));
  }

  // --- the boundary ---------------------------------------------------------------------------
  //
  // the three entry points that take a guest function pointer for a host thread to call. refused
  // and said out loud, rather than accepted and stored: accepting one would produce a stream that
  // opens cleanly and then crashes the moment the device asks for data, from an arm64 thread that
  // jumped into x86-64. blocking writes are the other AAudio feeding model and the one the fork's
  // seam is already shaped for, so nothing is lost by having none of these.
  switch (Id) {
  case Id_AAudioStreamBuilder_setDataCallback:
  case Id_AAudioStreamBuilder_setErrorCallback:
  case Id_AAudioStreamBuilder_setPresentationEndCallback: {
    if (Refused.fetch_add(1, std::memory_order_relaxed) < 3) {
      std::printf("[audio] %s refused: the thunk is one-way, so a host thread cannot call guest code. "
                  "use blocking AAudioStream_write instead\n",
                  Commands[Id].Name);
    }
    return 0; // all three return void
  }
  default:
    break;
  }

  // the guest's own timeout, clamped. rewritten in the argument array rather than in the reader,
  // because the reader is shared with the vulkan thunk and this is not an ABI question -- it is a
  // policy about how long a guest thread may be unreachable. same shape as the swapchain
  // preTransform override in vulkan_thunk.cpp, and for a comparable reason.
  if (Id == Id_AAudioStream_write || Id == Id_AAudioStream_read) {
    const auto Requested = static_cast<int64_t>(Args->Argument[4]);
    if (Requested > MaxWriteTimeoutNanos) {
      Args->Argument[4] = static_cast<uint64_t>(MaxWriteTimeoutNanos);
      if (!ClampedOnce) {
        ClampedOnce = true;
        std::printf("[audio] %s timeout %lld ns clamped to %lld ns: a guest thread parked longer than "
                    "that cannot acknowledge a GC suspension\n",
                    Commands[Id].Name, static_cast<long long>(Requested),
                    static_cast<long long>(MaxWriteTimeoutNanos));
      }
    }
  }

  void* Fn = Resolve(Id);
  if (!Fn) {
    Unresolved.fetch_add(1, std::memory_order_relaxed);
    if (LastUnresolvedName != Commands[Id].Name) {
      LastUnresolvedName = Commands[Id].Name;
      std::printf("[audio] no host implementation of %s (this android is older than the entry point)\n",
                  Commands[Id].Name);
    }
    return static_cast<uint64_t>(static_cast<int64_t>(AAUDIO_ERROR_UNIMPLEMENTED));
  }

  // the closing report has to happen *before* the call, not after: once close or release returns
  // the stream is gone, and asking a freed handle how many frames it played is a use-after-free
  // rather than a diagnostic. this is also the only report a short run is guaranteed to produce.
  const bool Closing = Id == Id_AAudioStream_close || Id == Id_AAudioStream_release;
  if (Closing) {
    ReportOneStream(reinterpret_cast<void*>(Args->Argument[1]));
  }

  if (Id == Id_AAudioStream_write) {
    WritesStarted.fetch_add(1, std::memory_order_relaxed);
    LastWriteTid.store(static_cast<int>(::gettid()), std::memory_order_relaxed);
  }

  ThunkABI::ArgReader Reader(&Args->Argument[1], Frame->State);
  const uint64_t Result = Commands[Id].Invoke(Fn, Reader);

  // the three places the stream table has to learn something. openStream writes the handle into
  // guest memory, which is host memory, so it can simply be read back out of it.
  if (Id == Id_AAudioStreamBuilder_openStream && static_cast<int32_t>(Result) == AAUDIO_OK) {
    if (auto* Out = reinterpret_cast<void**>(Args->Argument[2])) {
      NoteStreamOpened(*Out);
    }
  } else if (Id == Id_AAudioStream_write) {
    WritesFinished.fetch_add(1, std::memory_order_relaxed);
    LastWriteNanos.store(NowNanos(), std::memory_order_relaxed);
    // a failed write is never silent. a stream that has gone away answers every submission with a
    // negative result, and without this the only symptom is that the log goes quiet.
    const auto Status = static_cast<int32_t>(Result);
    if (Status < 0) {
      static uint64_t Complaints = 0;
      if (Complaints++ < 8) {
        auto ToText = Host<const char* (*)(int32_t)>(Id_AAudio_convertResultToText);
        std::printf("[audio] AAudioStream_write failed: %s (%d)\n", ToText ? ToText(Status) : "?", Status);
      }
    }
    MaybeReportStream(reinterpret_cast<void*>(Args->Argument[1]));
  } else if (Closing) {
    ForgetStream(reinterpret_cast<void*>(Args->Argument[1]));
  }

  return Result;
}

} // namespace HostLayer::AudioThunk
