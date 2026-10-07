#include "pad_bridge.h"

#include <jni.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstring>
#include <iterator>
#include <mutex>
#include <thread>

namespace HostLayer::PadBridge {

namespace {

// the java side of rumble. it holds the vibrators and decides which of them a port drives; nothing
// below this line knows what a VibrationEffect is.
constexpr const char* HelperClass = "com/mircowuffwuff/sharpdroid/PadRumble";

JavaVM* VM {};
jclass Helper {};
jmethodID RumbleMethod {};

bool BridgeEnabled {};
bool TraceEnabled {};
bool SelfTestEnabled {};
bool Complained {};
std::atomic<bool> ComplainedAboutPort {};

std::atomic<uint64_t> Reads {};
std::atomic<uint64_t> ConnectedReads {};
// requests counted where the guest makes them and deliveries where the platform takes them, because a
// gap between the two is the whole failure mode of an asynchronous path: the guest asking and the
// vibrator moving are different claims, and one number could not tell them apart.
std::atomic<uint64_t> Requests {};
std::atomic<uint64_t> Rumbles {};
std::atomic<uint64_t> Refused {};

constexpr int64_t Refusal = -1;

// **the state: one PortState per port, and nothing else.** every format a read can ask for is written
// from it, so there is no second copy for an older format to drift out of step with.
//
// a mutex rather than a seqlock because the write rate is a person's thumbs and the read rate is a
// thousand a second at the very most, so there is no contention to design around and a lock is the
// version that is obviously correct.
std::mutex StateGate;
PortState Ports[PortCount] {};

// --- the formats -----------------------------------------------------------------------------------
//
// **every format a read can ask for, each one a projection of the state above, in one table.** a read
// finds its row by the number it sent, checks the size, and the row says which port it is answered
// from and writes it. supporting a format is a row here, and nothing else in this file knows which
// formats exist.
struct Format {
  // the hostContract generation that introduced the format, which is also its name.
  uint32_t Contract;
  // what a read in this format sends as its first argument.
  uint32_t Number;
  uint64_t Size;
  // the port a read is answered from, given its port argument, or -1 when that names no port.
  int (*PortOf)(uint64_t PortArgument);
  // writes the format, from that port's state, into the guest's own buffer.
  void (*Write)(const PortState& State, void* Out);
};

int Port1Only(uint64_t) {
  return 0;
}

int NamedPort(uint64_t PortArgument) {
  return PortArgument < PortCount ? static_cast<int>(PortArgument) : -1;
}

void WriteContract3(const PortState& State, void* Out) {
  Contract3State Pad {};
  Pad.Buttons = State.Buttons;
  Pad.LeftX = State.LeftX;
  Pad.LeftY = State.LeftY;
  Pad.RightX = State.RightX;
  Pad.RightY = State.RightY;
  Pad.LeftTrigger = State.LeftTrigger;
  Pad.RightTrigger = State.RightTrigger;
  Pad.Connected = State.Connected;
  std::memcpy(Out, &Pad, sizeof(Pad));
}

void WriteContract4(const PortState& State, void* Out) {
  std::memcpy(Out, &State, sizeof(State));
}

constexpr Format Formats[] = {
  // **the contract 3 format sends 1 rather than 3, the one row where the two differ**: the payloads
  // that read it are already in people's hands and send 1, so 1 is what identifies it. it takes no
  // port argument, because those payloads read a single pad, and it is answered from port 1. this row
  // is needed only while the app's contract range includes 3.
  {3, 1, sizeof(Contract3State), Port1Only, WriteContract3},
  {4, 4, sizeof(PortState), NamedPort, WriteContract4},
};

// set by the first read in each format, so the log says which one a payload speaks.
std::atomic<bool> FormatSeen[std::size(Formats)] {};

int FindFormat(uint32_t Number) {
  for (size_t Index = 0; Index < std::size(Formats); ++Index) {
    if (Formats[Index].Number == Number) {
      return static_cast<int>(Index);
    }
  }
  return -1;
}

// --- rumble delivery ------------------------------------------------------------------------------
//
// one host thread, created on the first request and idle on a condition variable otherwise. it exists
// because the guest's thread must not do this waiting; see the header.
std::mutex RumbleGate;
std::condition_variable RumbleWake;
struct Wanted {
  uint8_t Large;
  uint8_t Small;
  // a generation rather than a boolean, so that two requests arriving between deliveries collapse to
  // the newest instead of the older one winning or both being sent.
  uint64_t Generation;
};
Wanted WantedRumble[PortCount] {};
uint64_t DeliveredGeneration[PortCount] {};
bool DeliveryStarted {};

bool AnyRumbleDue() {
  for (uint32_t Port = 0; Port < PortCount; ++Port) {
    if (WantedRumble[Port].Generation != DeliveredGeneration[Port]) {
      return true;
    }
  }
  return false;
}

void DeliveryThread() {
  // attached once for the life of the thread and detached when it ends, rather than through the
  // per-thread attachment the file layer needs. there is exactly one of these and it is ours, so
  // there is nothing to keep in a thread_local and no guest thread to detach on an exit path this
  // file cannot see.
  JNIEnv* E {};
  JavaVMAttachArgs Args {JNI_VERSION_1_6, "sharpdroid-rumble", nullptr};
  if (!VM || VM->AttachCurrentThread(&E, &Args) != JNI_OK) {
    std::printf("[pad] the rumble thread could not attach to the runtime; rumble is dropped\n");
    std::fflush(stdout);
    return;
  }

  for (;;) {
    Wanted Due[PortCount] {};
    bool IsDue[PortCount] {};
    {
      std::unique_lock<std::mutex> Lock(RumbleGate);
      RumbleWake.wait(Lock, AnyRumbleDue);
      for (uint32_t Port = 0; Port < PortCount; ++Port) {
        if (WantedRumble[Port].Generation != DeliveredGeneration[Port]) {
          Due[Port] = WantedRumble[Port];
          IsDue[Port] = true;
          DeliveredGeneration[Port] = WantedRumble[Port].Generation;
        }
      }
    }

    for (uint32_t Port = 0; Port < PortCount; ++Port) {
      if (!IsDue[Port] || !Helper || !RumbleMethod) {
        continue;
      }
      const jboolean Took = E->CallStaticBooleanMethod(Helper, RumbleMethod, static_cast<jint>(Port),
                                                       static_cast<jint>(Due[Port].Large),
                                                       static_cast<jint>(Due[Port].Small));
      // an exception left pending would be delivered at this thread's next JNI call, which is the
      // next rumble and a different request entirely. the java side catches its own; this is for one
      // thrown before it could.
      if (TraceEnabled) {
        std::printf("[pad] delivered port %u's rumble (large=%u small=%u): the app %s it\n", Port + 1,
                    Due[Port].Large, Due[Port].Small, Took ? "took" : "did not take");
      }
      if (E->ExceptionCheck()) {
        E->ExceptionDescribe();
        E->ExceptionClear();
      } else if (Took) {
        // **counted on the platform's answer rather than on the call returning.** a void method
        // reported success for anything that did not crash, so a request refused for want of the
        // VIBRATE permission counted as delivered -- and that permission is not consulted by any of the
        // capability checks, so nothing earlier would have contradicted it.
        if (Rumbles.fetch_add(1, std::memory_order_relaxed) == 0) {
          // said out loud, because the alternative evidence is the *absence* of the failure line
          // above, and an absence is not a measurement.
          std::printf("[pad] the platform accepted a rumble, so the delivery path works end to end\n");
          std::fflush(stdout);
        }
      }
    }
  }
}

void RequestRumble(uint32_t Port, uint8_t Large, uint8_t Small) {
  {
    std::lock_guard<std::mutex> Lock(RumbleGate);
    WantedRumble[Port].Large = Large;
    WantedRumble[Port].Small = Small;
    ++WantedRumble[Port].Generation;
    if (!DeliveryStarted) {
      DeliveryStarted = true;
      // detached, and never joined. it waits forever by design and the process ends by _exit on the
      // guest's own exit_group, so there is no shutdown path for it to participate in.
      std::thread(DeliveryThread).detach();
    }
  }
  RumbleWake.notify_one();
}

// **the self-test runs on a short-lived thread of its own**, so that the pause between one port's
// buzz and the next is never spent on the guest's thread, which only started it.
void SelfTest() {
  for (uint32_t Port = 0; Port < PortCount; ++Port) {
    std::printf("[pad] self-test: requesting one rumble at full strength on port %u. a buzz now means "
                "the delivery path works for that port and the game did not ask for it\n",
                Port + 1);
    std::fflush(stdout);
    RequestRumble(Port, 255, 0);
    std::this_thread::sleep_for(std::chrono::milliseconds(700));
  }
}

uint64_t Read(HostLayer::SyscallArguments* Args) {
  const uint32_t Number = static_cast<uint32_t>(Args->Argument[1]);
  void* Out = reinterpret_cast<void*>(Args->Argument[2]);
  const uint64_t Size = Args->Argument[3];

  // **the check that replaces a shared structure layout.** the guest says which format it expects
  // and how many bytes it has room for, and a disagreement is refused and named rather than written
  // into. this is what makes two repositories safe to release independently.
  const int Index = FindFormat(Number);
  if (Index < 0 || Size != Formats[Index].Size) {
    if (Refused.fetch_add(1, std::memory_order_relaxed) < 3) {
      std::printf("[pad] refusing a read: the guest asked for format %u at %llu bytes, and this host "
                  "layer reads",
                  Number, static_cast<unsigned long long>(Size));
      for (size_t Known = 0; Known < std::size(Formats); ++Known) {
        std::printf("%s the contract %u format (number %u, %llu bytes)", Known ? " and" : "",
                    Formats[Known].Contract, Formats[Known].Number,
                    static_cast<unsigned long long>(Formats[Known].Size));
      }
      std::printf(". the payload and the host layer are out of step\n");
      std::fflush(stdout);
    }
    return static_cast<uint64_t>(Refusal);
  }
  const Format& Answer = Formats[Index];

  const int Port = Answer.PortOf(Args->Argument[4]);
  if (Port < 0 || !Out) {
    if (Refused.fetch_add(1, std::memory_order_relaxed) < 3) {
      std::printf("[pad] refusing a read in the contract %u format: %s\n", Answer.Contract,
                  Out ? "it names a port that does not exist" : "it has nowhere to write to");
      std::fflush(stdout);
    }
    return static_cast<uint64_t>(Refusal);
  }

  // no pointer translation: guest and host share one address space 1:1, so the guest's buffer is
  // written in place.
  PortState State {};
  {
    std::lock_guard<std::mutex> Lock(StateGate);
    State = Ports[Port];
  }
  Answer.Write(State, Out);

  // **one line on the first read in each format, whether or not anything is being traced.** the run
  // summary is the only other place a count appears and it prints when the guest *returns* -- which a
  // game never does, since a run ends by exit_group or by the app killing the process. so without this
  // there is no evidence anywhere that the guest ever asked, and "the pad does nothing" would look
  // identical to "the payload never polled".
  if (!FormatSeen[Index].exchange(true, std::memory_order_relaxed)) {
    std::printf("[pad] the guest is polling pad state in the contract %u format\n", Answer.Contract);
    std::fflush(stdout);
  }
  if (Reads.fetch_add(1, std::memory_order_relaxed) == 0 && SelfTestEnabled) {
    // **this is the only place anything here fabricates a request**, and every one it makes says so in
    // the log, so that a buzz can never be mistaken for a game's own.
    std::thread(SelfTest).detach();
  }
  if (State.Connected) {
    // and one on the first read that finds a pad, which is the question the line above cannot
    // answer: a guest polling a bridge nobody has pushed to gets a valid answer of "no pad".
    if (ConnectedReads.fetch_add(1, std::memory_order_relaxed) == 0) {
      std::printf("[pad] a pad is connected and its state is reaching the guest\n");
      std::fflush(stdout);
    }
  }
  if (TraceEnabled) {
    std::printf("[pad] read port %d, contract %u format: buttons=0x%05X sticks=%u,%u/%u,%u "
                "triggers=%u,%u connected=%u\n",
                Port + 1, Answer.Contract, State.Buttons, State.LeftX, State.LeftY, State.RightX,
                State.RightY, State.LeftTrigger, State.RightTrigger, State.Connected);
  }
  // whether a pad was there, which is what the fork's seam asks for.
  return State.Connected ? 1u : 0u;
}

uint64_t Rumble(uint64_t PortArgument, uint64_t LargeArgument, uint64_t SmallArgument) {
  if (PortArgument >= PortCount) {
    if (Refused.fetch_add(1, std::memory_order_relaxed) < 3) {
      std::printf("[pad] refusing a rumble on port index %llu: there are %u ports\n",
                  static_cast<unsigned long long>(PortArgument), PortCount);
      std::fflush(stdout);
    }
    return static_cast<uint64_t>(Refusal);
  }
  const uint32_t Port = static_cast<uint32_t>(PortArgument);
  const uint8_t Large = static_cast<uint8_t>(LargeArgument & 0xFF);
  const uint8_t Small = static_cast<uint8_t>(SmallArgument & 0xFF);
  if (TraceEnabled) {
    std::printf("[pad] rumble port %u: large=%u small=%u\n", Port + 1, Large, Small);
  }
  // the same reasoning as the first read: a game that never asks for rumble and a rumble path that
  // is broken are the same silence otherwise.
  if (Requests.fetch_add(1, std::memory_order_relaxed) == 0) {
    std::printf("[pad] the guest asked for rumble (port %u, large=%u small=%u)\n", Port + 1, Large, Small);
    std::fflush(stdout);
  }
  // returns as soon as the request is recorded. the waiting is the delivery thread's.
  RequestRumble(Port, Large, Small);
  return 0;
}

} // namespace

void SetEnabled(bool Enable) {
  BridgeEnabled = Enable;
}

bool Enabled() {
  return BridgeEnabled;
}

void SetTrace(bool Enable) {
  TraceEnabled = Enable;
}

void SetSelfTest(bool Enable) {
  SelfTestEnabled = Enable;
}

void OnLoad(JavaVM* Vm) {
  VM = Vm;
  JNIEnv* E {};
  if (Vm->GetEnv(reinterpret_cast<void**>(&E), JNI_VERSION_1_6) != JNI_OK || !E) {
    return;
  }

  // **FindClass here and nowhere else**, for the reason the file layer's bridge spells out: JNI_OnLoad
  // runs with the app's class loader in scope, and on a thread this library attached itself FindClass
  // searches the system loader instead and has never heard of anything in the APK.
  jclass Local = E->FindClass(HelperClass);
  if (!Local) {
    E->ExceptionClear();
    std::printf("[pad] %s not found -- rumble is dropped, and pad reads are unaffected\n", HelperClass);
    std::fflush(stdout);
    return;
  }
  Helper = static_cast<jclass>(E->NewGlobalRef(Local));
  E->DeleteLocalRef(Local);

  // boolean rather than void, so that a refusal on the platform's side is a false here and not a
  // successful call. see the delivery thread.
  RumbleMethod = E->GetStaticMethodID(Helper, "rumble", "(III)Z");
  if (!RumbleMethod) {
    E->ExceptionClear();
    Helper = nullptr;
    std::printf("[pad] %s is missing rumble(III)Z -- rumble is dropped\n", HelperClass);
    std::fflush(stdout);
  }
}

void SetControls(uint32_t Port, const Controls& Pad) {
  if (Port >= PortCount) {
    if (!ComplainedAboutPort.exchange(true, std::memory_order_relaxed)) {
      std::printf("[pad] the app pushed a pad for port index %u, and there are %u ports\n", Port, PortCount);
      std::fflush(stdout);
    }
    return;
  }
  std::lock_guard<std::mutex> Lock(StateGate);
  PortState& State = Ports[Port];
  State.Buttons = Pad.Buttons;
  State.LeftX = Pad.LeftX;
  State.LeftY = Pad.LeftY;
  State.RightX = Pad.RightX;
  State.RightY = Pad.RightY;
  State.LeftTrigger = Pad.LeftTrigger;
  State.RightTrigger = Pad.RightTrigger;
  State.Connected = Pad.Connected ? 1 : 0;
}

uint64_t Handle(FEXCore::Core::CpuStateFrame*, HostLayer::SyscallArguments* Args) {
  const uint32_t Id = static_cast<uint32_t>(Args->Argument[0] & 0xFFFF);

  // an unenabled bridge answers rather than letting the magic number fall through to the syscall
  // table as an unhandled number, which is what the audio thunk does and for the same reason: the
  // guest reaches this through libc's own syscall wrapper, so there is no version of "not staged"
  // for it to discover. the fork then reports no pad and the run is the one it was before.
  if (!BridgeEnabled) {
    if (!Complained) {
      Complained = true;
      std::printf("[pad] guest asked for pad state and the bridge is not enabled (pass --pad)\n");
      std::fflush(stdout);
    }
    Refused.fetch_add(1, std::memory_order_relaxed);
    return static_cast<uint64_t>(Refusal);
  }

  switch (Id) {
  case Command_Read:
    return Read(Args);
  case Command_Rumble:
    return Rumble(0, Args->Argument[1], Args->Argument[2]);
  case Command_RumblePort:
    return Rumble(Args->Argument[1], Args->Argument[2], Args->Argument[3]);
  default:
    if (Refused.fetch_add(1, std::memory_order_relaxed) < 3) {
      std::printf("[pad] call to unknown command id %u\n", Id);
      std::fflush(stdout);
    }
    return static_cast<uint64_t>(Refusal);
  }
}

uint64_t ReadCount() {
  return Reads.load(std::memory_order_relaxed);
}

uint64_t ConnectedReadCount() {
  return ConnectedReads.load(std::memory_order_relaxed);
}

uint64_t RumbleRequestCount() {
  return Requests.load(std::memory_order_relaxed);
}

uint64_t RumbleCount() {
  return Rumbles.load(std::memory_order_relaxed);
}

uint64_t RefusedCount() {
  return Refused.load(std::memory_order_relaxed);
}

void Report() {
  if (!BridgeEnabled) {
    return;
  }
  // **the zero is the interesting reading, which is why the line prints unconditionally.** a payload
  // that never polls and a pad that was never touched produce identical silence otherwise, and this
  // project has read a zero as success before.
  std::printf("[pad] %llu reads, %llu of them with a pad connected, %llu rumbles asked and %llu "
              "delivered, %llu refused\n",
              static_cast<unsigned long long>(ReadCount()),
              static_cast<unsigned long long>(ConnectedReadCount()),
              static_cast<unsigned long long>(RumbleRequestCount()),
              static_cast<unsigned long long>(RumbleCount()),
              static_cast<unsigned long long>(RefusedCount()));
  if (ReadCount() == 0) {
    std::printf("[pad]   the guest never asked. is the payload's contract generation new enough to "
                "read SHARPEMU_HOST_INPUT?\n");
  }
  std::fflush(stdout);
}

} // namespace HostLayer::PadBridge
