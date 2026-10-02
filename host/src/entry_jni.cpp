// the host layer as a library -- the JNI surface the app calls, and nothing else.
//
// deliberately thin. everything the app can ask for is expressed as the argument vector RunMain
// already took, so the app passes the same flags a shell would and no measurement stops being
// comparable to the ones every earlier milestone recorded. the one thing that is *not* an
// argument is the window, because it is a live object rather than a string.

#include "audio_thunk.h"
#include "boot_progress.h"
#include "guest_threads.h"
#include "host_layer.h"
#include "log_ring.h"
#include "pad_bridge.h"
#include "saf_bridge.h"
#include "vulkan_thunk.h"

#include <android/log.h>
#include <android/native_window.h>
#include <android/native_window_jni.h>
#include <jni.h>

#include <cstring>
#include <pthread.h>
#include <string>
#include <unistd.h>
#include <vector>

namespace {

constexpr const char* LogTag = "sharpdroid";

ANativeWindow* Window {};

// everything the host layer and the guest print goes to stdout and stderr, and in an app both are
// /dev/null. rather than convert a few thousand printf calls -- several of which are load-bearing
// milestone evidence quoted verbatim in the docs -- put a pipe under the two descriptors and pump
// it into logcat. a run in the app then produces exactly the log a run in a shell does, which is
// the only reason `adb logcat` output can be compared against every earlier milestone's.
int LogPipe[2] {-1, -1};

void* LogPump(void*) {
  std::string Line;
  char Buffer[1024];
  for (;;) {
    const ssize_t Got = ::read(LogPipe[0], Buffer, sizeof(Buffer));
    if (Got <= 0) {
      break;
    }
    for (ssize_t i = 0; i < Got; ++i) {
      if (Buffer[i] == '\n') {
        // **the boot tap goes here and not under the write syscall**, which is the other place every
        // guest line passes. that one runs on the guest's own thread, so its cost would sit in the
        // boot's critical path; this thread exists to drain a pipe and is already off it. it sees
        // the host layer's own lines as well as the guest's, which costs a few comparisons and
        // means a checkpoint could be either side's without this loop knowing the difference.
        HostLayer::BootProgress::Observe(Line.data(), Line.size());
        // and the window a viewer inside the app reads, kept at the same point and for the same
        // reason: this thread exists to drain a pipe, so it is the one place every line passes that
        // is not the guest's own. it costs a lock and a copy against the socket write below it.
        HostLayer::LogRing::Push(Line.data(), Line.size());
        __android_log_write(ANDROID_LOG_INFO, LogTag, Line.c_str());
        Line.clear();
      } else {
        Line.push_back(Buffer[i]);
        // logcat drops a message past ~4000 bytes. the guest can produce a line longer than that
        // -- a .NET stack trace does -- and losing it silently is exactly the sort of thing that
        // costs a debugging round, so break it up rather than let it vanish.
        if (Line.size() >= 3800) {
          // the piece is kept as a line of its own, so a stack trace reaches a reader in the shape
          // logcat gets it in rather than being whole in one place and broken in the other.
          HostLayer::LogRing::Push(Line.data(), Line.size());
          __android_log_write(ANDROID_LOG_INFO, LogTag, Line.c_str());
          Line.clear();
        }
      }
    }
  }
  return nullptr;
}

// **the redirection happens on the caller's thread and only the draining is the pump's**, which is
// the whole reason it is not simply the first thing the pump does. anything printed between
// pthread_create and a new thread reaching dup2 goes to the original stdout, and in an app that is
// /dev/null -- a window a caller printing seconds later never notices, and a total loss for one that
// asks the host layer a question and ends the process on the answer.
void StartLogPump() {
  static bool Started = false;
  if (Started) {
    return;
  }
  Started = true;
  if (::pipe(LogPipe) != 0) {
    return;
  }
  ::dup2(LogPipe[1], STDOUT_FILENO);
  ::dup2(LogPipe[1], STDERR_FILENO);
  ::close(LogPipe[1]);
  pthread_t Thread {};
  ::pthread_create(&Thread, nullptr, LogPump, nullptr);
  ::pthread_detach(Thread);
}

} // namespace

extern "C" {

// the one place a class can be looked up by name, and the reason this function exists at all.
//
// the host layer has always been one-way -- the app calls down and nothing calls back, because a
// window is an ANativeWindow* and audio is pure NDK. the guest file layer is the first thing that
// has to ask java a question, and a guest thread cannot ask it: FindClass on a thread this process
// attached itself searches the *system* class loader, which has never heard of anything in the APK.
// JNI_OnLoad runs with the app's own class loader in scope, so this is where the class and its
// method ids are resolved and held.
//
// it runs whether or not a run will ever mount anything. resolving three method ids costs
// microseconds once, and a mount that discovered here that it had nothing to talk to would have
// discovered it far too late.
JNIEXPORT jint JNICALL JNI_OnLoad(JavaVM* VM, void*) {
  // **first, because the two bridges below can fail and say so on stdout.** a redirection that is a run's
  // business leaves everything printed before a run starts going to the original stdout -- /dev/null
  // in an app. what is printed here is precisely a bridge that could not resolve its java side, so
  // the lines lost that way are the ones explaining a capability the rest of the process no longer
  // has. redirecting at library load costs a pipe and a thread once
  // and gives every later caller a stdout that goes somewhere.
  StartLogPump();
  HostLayer::SafBridge::OnLoad(VM);
  // the pad bridge resolves its own helper here for the same reason and at the same moment. it calls
  // *up* only for rumble; state comes down through nativeSetPadState below, so a failure to resolve
  // costs rumble and leaves the reading half working.
  HostLayer::PadBridge::OnLoad(VM);
  return JNI_VERSION_1_6;
}

// called from the SurfaceHolder callback, with null when the surface goes away. the window is
// handed straight to the thunk, which is what turns a presented swapchain image into pixels on
// the panel; the thunk is also where the surface *size* now comes from, so that the extent the
// guest is told about and the buffer it ends up in cannot disagree. that disagreement is the
// bug, and the constant that papered over it is what this retires.
JNIEXPORT void JNICALL Java_com_mircowuffwuff_sharpdroid_HostLayer_nativeSetSurface(JNIEnv* Env, jclass, jobject Surface) {
  ANativeWindow* Next = Surface ? ::ANativeWindow_fromSurface(Env, Surface) : nullptr;
  HostLayer::VulkanThunk::SetAndroidWindow(Next);
  if (Window) {
    ::ANativeWindow_release(Window);
  }
  Window = Next;
}

// the guest stopped where it stands, and let go again. what stops where is guest_threads.h's to say,
// and what the audio streams do is audio_thunk.h's.
//
// both return at once and both are safe to repeat: the threads are stopped and the streams paused on
// threads of the host layer's own, so the app's UI thread is never held by either. a pause asked for
// before the guest has started is kept, and the guest's first thread parks before it runs anything.
JNIEXPORT void JNICALL Java_com_mircowuffwuff_sharpdroid_HostLayer_nativePause(JNIEnv*, jclass) {
  HostLayer::AudioThunk::SetPaused(true);
  HostLayer::Threads::Pause();
}

JNIEXPORT void JNICALL Java_com_mircowuffwuff_sharpdroid_HostLayer_nativeResume(JNIEnv*, jclass) {
  HostLayer::Threads::Resume();
  HostLayer::AudioThunk::SetPaused(false);
}

// how many frames the guest has presented. a relaxed load, so it can be asked once per drawn frame --
// which is how the app tells that a resumed guest has drawn again.
JNIEXPORT jlong JNICALL Java_com_mircowuffwuff_sharpdroid_HostLayer_nativePresentedFrames(JNIEnv*, jclass) {
  return static_cast<jlong>(HostLayer::VulkanThunk::PresentedFrameCount());
}

// the app's pad state, pushed from wherever it reads a KeyEvent or a MotionEvent. scalars rather than
// a structure on purpose: the guest checks a version and a byte count for the one layout that does
// cross, and adding a second layout across the JNI boundary as well would be a second thing to keep
// in step for no gain.
//
// **cheap enough to call on every event.** it takes one uncontended lock and copies twelve bytes; the
// guest's poll takes the same lock. nothing here allocates, throws or blocks, which is what lets it be
// called straight from the input dispatch on the UI thread.
JNIEXPORT void JNICALL Java_com_mircowuffwuff_sharpdroid_HostLayer_nativeSetPadState(
  JNIEnv*, jclass, jint Buttons, jint LeftX, jint LeftY, jint RightX, jint RightY, jint LeftTrigger,
  jint RightTrigger, jboolean Connected) {
  HostLayer::PadBridge::WireState State {};
  State.Buttons = static_cast<uint32_t>(Buttons);
  State.LeftX = static_cast<uint8_t>(LeftX);
  State.LeftY = static_cast<uint8_t>(LeftY);
  State.RightX = static_cast<uint8_t>(RightX);
  State.RightY = static_cast<uint8_t>(RightY);
  State.LeftTrigger = static_cast<uint8_t>(LeftTrigger);
  State.RightTrigger = static_cast<uint8_t>(RightTrigger);
  State.Connected = Connected ? 1 : 0;
  HostLayer::PadBridge::SetState(State);
}

// whether the GPU driver the app chose is the one this process would render through, asked *before*
// a guest is started so that a launch can be refused rather than ended.
//
// **this opens the driver, and that is the point rather than a side effect.** it is the same
// `std::call_once` the guest's first vulkan call would have run, so the load happens once and simply
// happens earlier -- what is checked is the load that the run will use, not a rehearsal of it in some
// other process or namespace. see vulkan_thunk.h for why the answer cannot be had any other way.
//
// the two strings outlive the call because the thunk keeps the pointers rather than copying them, and
// RunMain later sets the same two from its own argument vector.
JNIEXPORT jboolean JNICALL Java_com_mircowuffwuff_sharpdroid_HostLayer_nativeDriverLoads(
  JNIEnv* Env, jclass, jstring Driver, jstring Hooks) {
  static std::string DriverPath;
  static std::string HookLibDir;
  const char* Chars = Env->GetStringUTFChars(Driver, nullptr);
  DriverPath = Chars ? Chars : "";
  Env->ReleaseStringUTFChars(Driver, Chars);
  Chars = Env->GetStringUTFChars(Hooks, nullptr);
  HookLibDir = Chars ? Chars : "";
  Env->ReleaseStringUTFChars(Hooks, Chars);

  HostLayer::VulkanThunk::SetDriver(DriverPath.c_str());
  HostLayer::VulkanThunk::SetHookLibDir(HookLibDir.c_str());
  return HostLayer::VulkanThunk::ChosenDriverLoads() ? JNI_TRUE : JNI_FALSE;
}

// the boot checkpoint table's names, in order, asked for once.
//
// **ids rather than indices**, so that the two sides are free to move independently: a caller maps
// an id to whatever it draws for that phase, one it has never heard of changes nothing on screen,
// and an entry that disappears from here takes its own text with it. an index would make adding a
// checkpoint a change to two files that must be made in the same breath.
JNIEXPORT jobjectArray JNICALL Java_com_mircowuffwuff_sharpdroid_HostLayer_nativeBootCheckpointIds(
  JNIEnv* Env, jclass) {
  const jint Count = HostLayer::BootProgress::Count();
  jclass StringClass = Env->FindClass("java/lang/String");
  jobjectArray Out = Env->NewObjectArray(Count, StringClass, nullptr);
  for (jint Index = 0; Index < Count; ++Index) {
    jstring Text = Env->NewStringUTF(HostLayer::BootProgress::Id(Index));
    Env->SetObjectArrayElement(Out, Index, Text);
    Env->DeleteLocalRef(Text);
  }
  Env->DeleteLocalRef(StringClass);
  return Out;
}

// how many checkpoints the run has passed, 0 up to the table's length. **the one call meant to be
// made repeatedly**: it is a single relaxed load behind the JNI boundary, which is what lets a
// caller ask once per drawn frame rather than arranging to be told.
JNIEXPORT jint JNICALL Java_com_mircowuffwuff_sharpdroid_HostLayer_nativeBootCheckpointsReached(
  JNIEnv*, jclass) {
  return HostLayer::BootProgress::Reached();
}

// when each checkpoint was passed, in milliseconds since the host layer started, and -1 for one
// that was not. asked for once a boot is over, by a caller recording this run to predict the next.
//
// **-1 is a real answer and not a zero.** an entry that no line matched and an entry reached in the
// same instant as its neighbour would otherwise be indistinguishable, and a caller averaging the
// second into its record would be averaging in a table that has stopped working.
JNIEXPORT jlongArray JNICALL Java_com_mircowuffwuff_sharpdroid_HostLayer_nativeBootCheckpointTimes(
  JNIEnv* Env, jclass) {
  const jint Count = HostLayer::BootProgress::Count();
  std::vector<jlong> Times(static_cast<size_t>(Count));
  for (jint Index = 0; Index < Count; ++Index) {
    Times[static_cast<size_t>(Index)] = HostLayer::BootProgress::ReachedAt(Index);
  }
  jlongArray Out = Env->NewLongArray(Count);
  Env->SetLongArrayRegion(Out, 0, Count, Times.data());
  return Out;
}

// the sequence one past the newest line the process has printed. **the repeatedly-made call of this
// group**, and it is a lock and a load: a caller polls it while it is showing the log and asks for
// nothing at all the rest of the time.
JNIEXPORT jlong JNICALL Java_com_mircowuffwuff_sharpdroid_HostLayer_nativeLogNext(JNIEnv*, jclass) {
  return HostLayer::LogRing::Next();
}

// the sequence of the oldest line still held. a caller opening a viewer starts here; one that has
// been away compares this against what it last saw to learn whether anything was dropped meanwhile.
JNIEXPORT jlong JNICALL Java_com_mircowuffwuff_sharpdroid_HostLayer_nativeLogOldest(JNIEnv*, jclass) {
  return HostLayer::LogRing::Oldest();
}

// the lines from From up to but not including To, clamped to what is still held.
//
// **the copy out of the ring is the whole of what the pump can be made to wait for**, which is why
// the range is the caller's to choose rather than "everything new": a viewer that polls asks for the
// handful of lines that arrived since it last looked, and only an opening viewer asks for thousands.
JNIEXPORT jobjectArray JNICALL Java_com_mircowuffwuff_sharpdroid_HostLayer_nativeLogRange(
  JNIEnv* Env, jclass, jlong From, jlong To) {
  std::vector<std::string> Held;
  HostLayer::LogRing::Range(From, To, Held);
  jclass StringClass = Env->FindClass("java/lang/String");
  auto Count = static_cast<jsize>(Held.size());
  jobjectArray Out = Env->NewObjectArray(Count, StringClass, nullptr);
  for (jsize Index = 0; Index < Count; ++Index) {
    jstring Text = Env->NewStringUTF(Held[static_cast<size_t>(Index)].c_str());
    Env->SetObjectArrayElement(Out, Index, Text);
    Env->DeleteLocalRef(Text);
  }
  Env->DeleteLocalRef(StringClass);
  return Out;
}

// one line of the app's own, kept where it arrived among the emulator's.
//
// **it is not written to logcat here**, because the caller has already written it there -- this puts
// it in the one place it would otherwise be missing from. the app's lines never touch the pipe under
// fds 1 and 2: java logging is the platform's own channel and does not pass through stdout.
JNIEXPORT void JNICALL Java_com_mircowuffwuff_sharpdroid_HostLayer_nativeLogLine(
  JNIEnv* Env, jclass, jstring Line) {
  if (!Line) {
    return;
  }
  const char* Text = Env->GetStringUTFChars(Line, nullptr);
  if (Text) {
    HostLayer::LogRing::Push(Text, ::strlen(Text));
    Env->ReleaseStringUTFChars(Line, Text);
  }
}

// blocks for the whole run, so the app must call it off the UI thread. a guest that calls
// exit_group never returns from here -- it calls _exit, which is what the syscall means and the only
// safe answer once the other guest threads are inside translated code. that is the process this
// library was loaded into, so a caller that has anything to lose runs a guest in a process it is
// willing to lose; the app gives one to each run.
JNIEXPORT jint JNICALL Java_com_mircowuffwuff_sharpdroid_HostLayer_nativeRun(JNIEnv* Env, jclass, jobjectArray Args) {
  std::vector<std::string> Storage;
  Storage.emplace_back("sharpdroid-host-layer");
  const jsize Count = Args ? Env->GetArrayLength(Args) : 0;
  for (jsize i = 0; i < Count; ++i) {
    auto Text = static_cast<jstring>(Env->GetObjectArrayElement(Args, i));
    const char* Chars = Env->GetStringUTFChars(Text, nullptr);
    Storage.emplace_back(Chars ? Chars : "");
    Env->ReleaseStringUTFChars(Text, Chars);
    Env->DeleteLocalRef(Text);
  }

  std::vector<char*> Argv;
  Argv.reserve(Storage.size());
  for (auto& Entry : Storage) {
    Argv.push_back(Entry.data());
  }

  return HostLayer::RunMain(static_cast<int>(Argv.size()), Argv.data());
}

} // extern "C"
