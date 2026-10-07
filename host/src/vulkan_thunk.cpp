// see vulkan_thunk.h for the design and for why a syscall is the trap.

#include "vulkan_thunk.h"
#include "boot_progress.h"
#include "thunk_abi.h"

#define VK_NO_PROTOTYPES
#include <vulkan/vulkan_core.h>

#include <android/native_window.h>
// included directly rather than through vulkan.h with VK_USE_PLATFORM_ANDROID_KHR, because that
// header pulls in every other platform's too. it needs exactly the two things already included
// above: the core types, and `struct ANativeWindow`.
#include <vulkan/vulkan_android.h>

#include <adrenotools/driver.h>

#include <dlfcn.h>
#include <limits.h>
#include <sys/stat.h>
#include <time.h>

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string>
#include <string_view>
#include <thread>
#include <tuple>
#include <type_traits>
#include <unordered_map>
#include <vector>

namespace HostLayer::VulkanThunk {

namespace {

// --- the generated command list, read three times ----------------------------------------
enum CommandId : uint32_t {
#define VKCMD(name) Id_##name,
#include "vulkan_commands.inc"
#undef VKCMD
  CommandCount
};

bool ThunkEnabled {};
bool TraceEnabled {};
bool Complained {};

const char* LibraryPath = "libvulkan.so";
// both null unless --vulkan-driver and --vulkan-hooks were given, which is what makes the stock
// path identical to every earlier milestone's rather than merely equivalent to it.
const char* DriverPath {};
const char* HookLibDir {};
std::vector<std::string> DriverEnv;
void* Library {};
PFN_vkGetInstanceProcAddr HostGetInstanceProcAddr {};
PFN_vkGetDeviceProcAddr HostGetDeviceProcAddr {};
std::once_flag LibraryOnce;

// the guest address of the generated stub table, handed over by the guest library's own
// .init_array as soon as its dynamic linker maps it. nothing else knows this address.
uint64_t StubBase {};

// the instance and device the guest created, kept so that extension entry points can be
// resolved through vkGetInstanceProcAddr the way a normal vulkan client would.
VkInstance GuestInstance {};
VkDevice GuestDevice {};

// --- the profile ---------------------------------------------------------------------------
//
// wall time and call count per command id, dumped periodically. this answers "which call blocks",
// which is a different question from --trace-vulkan's "which calls happen" and is the one a stall
// needs. relaxed atomics: several guest threads record into this and none of them read it, so the
// only ordering that matters is that the dump eventually sees the writes.
bool ProfileEnabled {};
struct CommandProfile {
  std::atomic<uint64_t> Nanos {};
  std::atomic<uint64_t> Calls {};
  // the worst single call in the interval, which is what separates a command that *blocks* from
  // one that is merely called a lot. 7 ms spread over 22 uniform calls is per-call overhead worth
  // removing; 7 ms in one call and 21 cheap ones is a wait, and moving it elsewhere buys nothing.
  std::atomic<uint64_t> MaxNanos {};
  // CPU time actually consumed inside the command, against the wall time above.
  std::atomic<uint64_t> CpuNanos {};
};
CommandProfile Profile[CommandCount] {};

uint64_t NowNanos() {
  timespec Time {};
  ::clock_gettime(CLOCK_MONOTONIC, &Time);
  return static_cast<uint64_t>(Time.tv_sec) * 1000000000ull + static_cast<uint64_t>(Time.tv_nsec);
}

// this thread's CPU time, so the profile can say whether a command *burns* the frame or *waits*
// inside it. wall time alone cannot: a driver that sleeps in an ioctl and one that spins look
// identical, and only one of them is worth removing -- the other is where idle goes, and taking it
// away just moves the idle somewhere else.
uint64_t ThreadCpuNanos() {
  timespec Time {};
  ::clock_gettime(CLOCK_THREAD_CPUTIME_ID, &Time);
  return static_cast<uint64_t>(Time.tv_sec) * 1000000000ull + static_cast<uint64_t>(Time.tv_nsec);
}

// --- how long the GPU actually takes -------------------------------------------------------
//
// the profile can say a fence check *waited*, and cannot say what it waited for. two readings fit
// equally: the guest is idle and the wait is where idle happens to land, or the GPU genuinely
// takes longer and the wait is real. at a frame budget the game sets rather than the GPU, those
// are indistinguishable from wall time alone -- and they are opposite answers to "is this driver as
// fast".
//
// so: note when a submission's fence is handed to the queue, and how long until a fence check
// first reports it signalled. that is submit-to-complete turnaround, measured without touching
// SharpEmu and without a timestamp query.
//
// it is an **upper bound**, because completion is only observed when the guest asks. the guest
// asks ~22 times a frame, so the granularity is well under a millisecond -- fine for telling a
// 4 ms difference from none. the minimum is reported alongside the mean for the same reason: it is
// the sample least inflated by polling latency.
std::mutex FenceLock;
std::unordered_map<uint64_t, uint64_t> FenceSubmitNanos;
std::atomic<uint64_t> GpuTurnaroundNanos {};
std::atomic<uint64_t> GpuTurnaroundCount {};
std::atomic<uint64_t> GpuTurnaroundMinNanos {~0ull};

void NoteFenceSubmitted(uint64_t Fence) {
  if (!Fence) {
    return;
  }
  const uint64_t Now = NowNanos();
  std::lock_guard<std::mutex> Guard(FenceLock);
  // submissions and completions balance in practice -- 13.0 of each per frame -- but a fence the
  // guest submits and never asks about again would sit here for the life of the run. bounded
  // rather than trusted, because this is a diagnostic and a diagnostic that leaks is worse than
  // one that occasionally forgets.
  if (FenceSubmitNanos.size() > 4096) {
    FenceSubmitNanos.clear();
  }
  FenceSubmitNanos[Fence] = Now;
}

void NoteFenceSignalled(uint64_t Fence) {
  if (!Fence) {
    return;
  }
  uint64_t Submitted = 0;
  {
    std::lock_guard<std::mutex> Guard(FenceLock);
    auto Entry = FenceSubmitNanos.find(Fence);
    if (Entry == FenceSubmitNanos.end()) {
      return;
    }
    Submitted = Entry->second;
    FenceSubmitNanos.erase(Entry);
  }
  const uint64_t Elapsed = NowNanos() - Submitted;
  GpuTurnaroundNanos.fetch_add(Elapsed, std::memory_order_relaxed);
  GpuTurnaroundCount.fetch_add(1, std::memory_order_relaxed);
  uint64_t Best = GpuTurnaroundMinNanos.load(std::memory_order_relaxed);
  while (Elapsed < Best &&
         !GpuTurnaroundMinNanos.compare_exchange_weak(Best, Elapsed, std::memory_order_relaxed)) {
  }
}

std::atomic<void*> Resolved[CommandCount] {};
std::atomic<uint64_t> Calls {};
std::atomic<uint64_t> Unresolved {};
const char* LastUnresolvedName {};

using ThunkABI::ArgReader;

struct Command {
  const char* Name;
  uint64_t (*Invoke)(void* Fn, ArgReader& R);
};

// --- one template, 623 entry points -------------------------------------------------------
//
// the SysV classification and the marshaller both live in thunk_abi.h, shared with the audio
// thunk. what is vulkan's own is the one parameter type that must not be forwarded.
//
// VkAllocationCallbacks* is consumed so the remaining arguments still line up, and then dropped
// on the floor: a guest allocation callback is an x86-64 function pointer and the driver would
// enter it as arm64. vulkan makes the whole structure optional precisely so it can be ignored,
// and expressing the refusal at the type level covers all ~90 commands taking one without naming
// any of them.
template<typename T>
struct Read : ThunkABI::PassThrough<T> {};

template<>
struct Read<const VkAllocationCallbacks*> {
  static const VkAllocationCallbacks* From(ArgReader& R) {
    R.SkipInteger();
    return nullptr;
  }
};

template<typename PFN>
using Thunk = ThunkABI::Marshal<Read, PFN>;

const Command Commands[] = {
#define VKCMD(name) {#name, &Thunk<PFN_##name>::Call},
#include "vulkan_commands.inc"
#undef VKCMD
};
static_assert(std::size(Commands) == CommandCount, "command list and id enum disagree");

// --- resolution ---------------------------------------------------------------------------

// --- the custom driver, via libadrenotools -------------------------------------------------
//
// what adrenotools does is not "load this .so instead". it creates an isolated linker namespace,
// preloads a hook into it, and then opens **the platform loader** -- /system/lib64/libvulkan.so --
// inside that namespace, so that when the loader goes looking for a driver the hook answers with
// ours. that is the whole reason it exists rather than a dlopen: the loader keeps being the
// loader, so WSI, the surface extensions and the ICD negotiation are all still the platform's,
// and only the driver underneath changes.
//
// that also means the swapchain work at 1a is not invalidated by swapping the driver: WSI never
// lived in the driver to begin with.
//
// it must happen before the first vulkan call in the process, which it does -- the whole thunk
// resolves through Resolve(), and Resolve() is what calls this, once.

// checked before calling adrenotools rather than after, because adrenotools_open_libvulkan
// returns a bare nullptr for about ten distinct reasons and dlerror() is only meaningful for the
// last of them. everything checkable from out here is checked out here, so that the message names
// the actual problem instead of "could not load".
bool DriverPreconditionsMet() {
  struct ::stat Info {};
  if (::stat(DriverPath, &Info) != 0) {
    std::printf("[vulkan] driver %s: %s\n", DriverPath, std::strerror(errno));
    return false;
  }
  // the hooks are opened by soname out of this directory and nowhere else. getting this wrong is
  // adrenotools' own documented failure mode and it is a quiet one: the call still succeeds, the
  // stock driver loads as a fallback, and the only symptom is that nothing got faster.
  for (const char* Hook : {"libmain_hook.so", "libhook_impl.so"}) {
    const std::string Full = std::string(HookLibDir) + "/" + Hook;
    if (::stat(Full.c_str(), &Info) != 0) {
      std::printf("[vulkan] hook %s: %s\n", Full.c_str(), std::strerror(errno));
      return false;
    }
  }
  return true;
}

void* OpenCustomDriver() {
  if (!DriverPreconditionsMet()) {
    return nullptr;
  }

  // before the driver is loaded, because mesa reads its debug options once at initialisation and
  // never looks again. setenv on the host process, deliberately: the guest environment is a
  // different array entirely and turnip is not the guest.
  for (const auto& Assignment : DriverEnv) {
    const auto Equals = Assignment.find('=');
    if (Equals == std::string::npos) {
      std::printf("[vulkan] --vulkan-driver-env wants NAME=VALUE, ignoring '%s'\n", Assignment.c_str());
      continue;
    }
    const std::string Name = Assignment.substr(0, Equals);
    const std::string Value = Assignment.substr(Equals + 1);
    const int Set = ::setenv(Name.c_str(), Value.c_str(), 1);
    // read back through getenv rather than trusting setenv's return, because the question this
    // has to answer is not "did we call it" but "is it visible to whoever looks next". a sweep of
    // TU_DEBUG values that all did nothing is indistinguishable from a sweep that never arrived,
    // and that cost a full round of measurements before it was checked.
    const char* Readback = ::getenv(Name.c_str());
    std::printf("[vulkan] driver env: %s=%s (setenv=%d, getenv=%s)\n", Name.c_str(), Value.c_str(), Set,
                Readback ? Readback : "<null>");
  }

  // adrenotools concatenates these two without a separator, so the directory half must end in a
  // slash or it stats a path that does not exist and returns null with nothing to say about it.
  const std::string Path = DriverPath;
  const auto Slash = Path.find_last_of('/');
  const std::string Dir = Path.substr(0, Slash + 1);
  const std::string Name = Path.substr(Slash + 1);

  // tmpLibDir is for api < 29, where there is no memfd to patch the soname in; adrenotools
  // ignores it from 29 on and this device is 35. passed as null rather than inventing a directory
  // that would never be written to.
  void* Handle = ::adrenotools_open_libvulkan(RTLD_NOW | RTLD_LOCAL, ADRENOTOOLS_DRIVER_CUSTOM,
                                              nullptr, HookLibDir, Dir.c_str(), Name.c_str(), nullptr, nullptr);
  if (!Handle) {
    std::printf("[vulkan] adrenotools could not load %s: %s\n", Name.c_str(), ::dlerror());
    std::printf("[vulkan]   this needs an app process -- a shell binary has no namespace to bypass\n");
    return nullptr;
  }
  std::printf("[vulkan] adrenotools: %s injected from %s\n", Name.c_str(), Dir.c_str());
  return Handle;
}

// --- did the injection actually happen -----------------------------------------------------
//
// **a handle from adrenotools is not an answer.** what it returns is the platform loader, opened in an
// isolated namespace with a hook in front of the loader's own dlopen -- and the hook is a separate
// decision made later. read hook_android_dlopen_ext in libadrenotools: when it cannot load the custom
// driver it calls its own fallback(), which loads the system driver and returns a perfectly good
// handle. so the loud fallback above catches adrenotools refusing, and catches nothing at all when
// adrenotools agrees and the hook underneath it does not.
//
// the question that survives that is "is the .so we asked for in this process", and /proc/self/maps is
// the only thing that answers it. the hook loads the driver with an ordinary android_dlopen_ext out of
// its own directory, so a successful injection is a file-backed mapping of that library and a fallback
// is the absence of one. it needs to know nothing about which driver was chosen, which is what keeps
// it working across several turnip packages that all report the same device name.
//
// **the driver's reported identity is not this question and cannot answer it.** deviceName says turnip
// or adreno, which stops being an answer the moment two turnip packages are on the device -- and two is
// what this device has.
enum class Injection { Yes, No, Unknown };

// **Unknown until something is actually asked**, which is what makes ChosenDriverLoads() safe to call
// on a run that named no driver: nothing was attempted, so there is nothing to refuse.
Injection Verdict = Injection::Unknown;

Injection DriverIsMapped() {
  // **the two spellings of one path, and the whole check turns on them.** an app's data directory is
  // reached as /data/user/0/<package>, which is a symlink to /data/data/<package>, and the launch
  // passes the first while the kernel reports the second -- so matching the string we were handed
  // finds nothing at all on a driver that is mapped four times over. that failure is silent, it is
  // indistinguishable from the one this check exists to catch, and it condemns every driver on the
  // device. resolving here rather than where the flag is parsed keeps it beside the only thing that
  // has to agree with it.
  char Canonical[PATH_MAX];
  const bool Resolved = ::realpath(DriverPath, Canonical) != nullptr;
  const char* Wanted = Resolved ? Canonical : DriverPath;

  // "re" -- close-on-exec, because the guest forks nothing but the emulator's own child processes are
  // not this file's business to leak into.
  std::FILE* Maps = std::fopen("/proc/self/maps", "re");
  if (!Maps) {
    // **unknown, and never a failure.** the expensive direction here is the false positive: a driver
    // that loaded and is reported as broken ends a run that would have worked. a maps file we cannot
    // read says nothing about the driver, so it is not allowed to say anything about the driver.
    std::printf("[vulkan] /proc/self/maps: %s -- whether the injection took cannot be checked\n",
                std::strerror(errno));
    return Injection::Unknown;
  }
  Injection Result = Injection::No;
  // a mapping line is an address range, four short fields and a path, so this is generous. a path
  // long enough to be split across two reads would be split at the same place every time and would
  // read as absent, which is why the size is not the tight one.
  char Line[1024];
  while (std::fgets(Line, sizeof(Line), Maps)) {
    if (std::strstr(Line, Wanted)) {
      Result = Injection::Yes;
      break;
    }
  }
  std::fclose(Maps);
  if (Result == Injection::No) {
    // named here rather than at the caller, because what was looked for is the resolved path and the
    // one every other line in this log names is the launch's. a search that found nothing has to say
    // which string it searched for or it cannot be checked at all.
    std::printf("[vulkan] no mapping of %s\n", Wanted);
    if (!Resolved) {
      // the string that was searched for is the unresolved one, and finding nothing under it is
      // exactly what a working driver looked like before the line above existed. so this is the one
      // remaining route to condemning a driver that loaded, and it answers unknown instead.
      std::printf("[vulkan] realpath(%s) failed, so that is not an answer about the driver\n",
                  DriverPath);
      return Injection::Unknown;
    }
  }
  return Result;
}

// the loader binds an ICD on its first entry point rather than at dlopen, so nothing is mapped yet at
// the moment the injection is set up and a check there would condemn every driver ever loaded. this
// forces the binding with the cheapest call that does it -- a pure query, taking no instance, that the
// guest itself makes moments later -- and then asks maps.
void VerifyInjection() {
  auto Enumerate = reinterpret_cast<PFN_vkEnumerateInstanceExtensionProperties>(
    ::dlsym(Library, "vkEnumerateInstanceExtensionProperties"));
  if (!Enumerate) {
    std::printf("[vulkan] the loader has no vkEnumerateInstanceExtensionProperties -- whether the"
                " injection took cannot be checked\n");
    return;
  }
  uint32_t Count = 0;
  Enumerate(nullptr, &Count, nullptr);

  Verdict = DriverIsMapped();
  if (Verdict == Injection::Yes) {
    std::printf("[vulkan] the injection took: %s is mapped into this process\n", DriverPath);
  } else if (Verdict == Injection::No) {
    // adrenotools accepted it and the driver is not here, which is the quiet failure this whole
    // check exists for.
    std::printf("[vulkan] adrenotools accepted the injection and %s is not mapped into this process."
                " the hook fell back and the driver underneath is the system one\n", DriverPath);
  }
}

void OpenLibrary() {
  // the custom path first, and a **loud** fallback rather than a silent one. a run that quietly
  // reverted to the stock driver looks exactly like a successful injection that did not help,
  // which is the one way this measurement could lie about itself.
  bool Injected = false;
  if (DriverPath && HookLibDir) {
    Library = OpenCustomDriver();
    Injected = Library != nullptr;
    if (!Library) {
      std::printf("[vulkan] falling back to the platform loader -- this is NOT the custom driver\n");
      // a refusal from adrenotools and a missing file are both definite: the chosen driver is not
      // what this process will render through. the *quiet* case is settled below instead.
      Verdict = Injection::No;
    }
  }
  if (!Library) {
    Library = ::dlopen(LibraryPath, RTLD_NOW | RTLD_LOCAL);
  }
  if (!Library) {
    std::printf("[vulkan] dlopen(%s) failed: %s\n", LibraryPath, ::dlerror());
    return;
  }
  HostGetInstanceProcAddr = reinterpret_cast<PFN_vkGetInstanceProcAddr>(::dlsym(Library, "vkGetInstanceProcAddr"));
  HostGetDeviceProcAddr = reinterpret_cast<PFN_vkGetDeviceProcAddr>(::dlsym(Library, "vkGetDeviceProcAddr"));
  std::printf("[vulkan] host loader %s opened, vkGetInstanceProcAddr=%p\n", LibraryPath,
              reinterpret_cast<void*>(HostGetInstanceProcAddr));
  // last, and only for a run that asked for a driver: a stock run must reach this point having done
  // exactly what every earlier measurement's did, which means not calling into the loader at all.
  if (Injected) {
    VerifyInjection();
  }
}

void* Resolve(uint32_t Id) {
  if (void* Cached = Resolved[Id].load(std::memory_order_acquire)) {
    return Cached;
  }
  std::call_once(LibraryOnce, OpenLibrary);
  if (!Library) {
    return nullptr;
  }

  const char* Name = Commands[Id].Name;
  // dlsym first, because the android loader exports the core entry points directly and that
  // path skips its per-command dispatch trampoline. extensions are not exported and have to be
  // asked for by name, which is the same thing any vulkan client does.
  void* Fn = ::dlsym(Library, Name);
  if (!Fn && HostGetDeviceProcAddr && GuestDevice) {
    Fn = reinterpret_cast<void*>(HostGetDeviceProcAddr(GuestDevice, Name));
  }
  if (!Fn && HostGetInstanceProcAddr) {
    Fn = reinterpret_cast<void*>(HostGetInstanceProcAddr(GuestInstance, Name));
  }
  if (Fn) {
    Resolved[Id].store(Fn, std::memory_order_release);
  }
  return Fn;
}

// defined down in the WSI section, where the window and the mode live. declared here because
// ImplementedHere is the first thing that needs to know.
bool UseAndroidWsi();

// the commands the thunk answers itself rather than forwarding. kept as one list because two
// places need it and they must not drift: the dispatch switch, and the proc-address lookup that
// has to hand these out without asking the host whether it has them.
//
// **the list is much shorter with a real window.** under android WSI everything below except the
// surface constructor is a genuine loader entry point that the host can resolve and that does the
// right thing, so forwarding is not merely allowed but the entire point -- those commands stop
// being ours and go back to being vulkan's.
bool ImplementedHere(uint32_t Id) {
  // the one command that is always ours, in both modes and for opposite reasons. headless: there
  // is no window, so the surface is a token we invent. android: there *is* a window, and this is
  // where VK_EXT_headless_surface is translated into a real VK_KHR_android_surface -- which is what
  // lets the fork keep asking for a headless surface and need no change at all.
  if (Id == Id_vkCreateHeadlessSurfaceEXT) {
    return true;
  }
  if (UseAndroidWsi()) {
    return false;
  }
  switch (Id) {
  case Id_vkDestroySurfaceKHR:
  case Id_vkGetPhysicalDeviceSurfaceSupportKHR:
  case Id_vkGetPhysicalDeviceSurfaceCapabilitiesKHR:
  case Id_vkGetPhysicalDeviceSurfaceFormatsKHR:
  case Id_vkGetPhysicalDeviceSurfacePresentModesKHR:
  case Id_vkCreateSwapchainKHR:
  case Id_vkDestroySwapchainKHR:
  case Id_vkGetSwapchainImagesKHR:
  case Id_vkAcquireNextImageKHR:
  case Id_vkQueuePresentKHR: return true;
  default: return false;
  }
}

const std::unordered_map<std::string_view, uint32_t>& NameToId() {
  static const auto* Map = [] {
    auto* M = new std::unordered_map<std::string_view, uint32_t>();
    M->reserve(CommandCount * 2);
    for (uint32_t i = 0; i < CommandCount; ++i) {
      M->emplace(Commands[i].Name, i);
    }
    return M;
  }();
  return *Map;
}

// what vkGetInstanceProcAddr and vkGetDeviceProcAddr have to return: an address in the *guest*
// stub table, not the host function. SharpEmu's presenter casts the result of GetDeviceProcAddr
// straight to a delegate* and calls it, so handing back a host arm64 pointer would be a jump
// into arm64 from x86-64.
uint64_t GuestProcAddr(const char* Name) {
  if (!Name || !StubBase) {
    return 0;
  }
  const auto& Map = NameToId();
  const auto It = Map.find(std::string_view(Name));
  if (It == Map.end()) {
    // newer than the header this was generated from, or not a command at all. a null return is
    // what a real loader gives for an entry point it does not have, so the guest's own feature
    // detection sees the truth.
    if (TraceEnabled) {
      std::printf("[vulkan] procaddr miss: %s\n", Name);
    }
    return 0;
  }
  // the WSI commands have no host implementation by construction -- they are the ones the thunk
  // answers itself -- so they must not be put through the host-resolution gate below. this is
  // how SharpEmu's android window finds vkCreateHeadlessSurfaceEXT: the platform loader has
  // never heard of it, and it is still perfectly callable.
  if (ImplementedHere(It->second)) {
    return StubBase + 16 * static_cast<uint64_t>(It->second);
  }

  // and if the *host* cannot provide it, say so rather than handing back a stub that would
  // fail later, at a call site with no name attached to it.
  if (!Resolve(It->second)) {
    if (TraceEnabled) {
      std::printf("[vulkan] procaddr unresolved on host: %s\n", Name);
    }
    return 0;
  }
  return StubBase + 16 * static_cast<uint64_t>(It->second);
}

// the stub table's shape is a contract between two generated files; this checks it rather than
// trusting it. a silently misaligned table would send every call to the wrong entry point.
bool Attach(uint64_t Base) {
  const auto* Bytes = reinterpret_cast<const uint8_t*>(Base);
  for (uint32_t i = 0; i < CommandCount; ++i) {
    const uint8_t* Stub = Bytes + 16 * i;
    uint32_t Immediate {};
    std::memcpy(&Immediate, Stub + 4, sizeof(Immediate));
    const bool Shape = Stub[0] == 0x49 && Stub[1] == 0x89 && Stub[2] == 0xCA && // movq %rcx, %r10
                       Stub[3] == 0xB8 &&                                       // movl $imm32, %eax
                       Stub[8] == 0x0F && Stub[9] == 0x05 &&                    // syscall
                       Stub[10] == 0xC3;                                        // ret
    if (!Shape || Immediate != Magic + i) {
      std::printf("[vulkan] stub table rejected at id %u (%s): shape=%d imm=0x%08X expected=0x%08llX\n", i,
                  Commands[i].Name, Shape ? 1 : 0, Immediate, static_cast<unsigned long long>(Magic + i));
      return false;
    }
  }
  StubBase = Base;
  std::printf("[vulkan] guest thunk attached: %u commands at 0x%llX\n", CommandCount,
              static_cast<unsigned long long>(Base));
  return true;
}

// --- WSI, the invented half ------------------------------------------------------------------
//
// **everything below runs only under `--vulkan-wsi headless`.** with a window the swapchain is the
// driver's and these commands are forwarded -- see UseAndroidWsi() and ImplementedHere().
//
// it exists because `VK_KHR_surface` needs a real window on android and a binary run as the shell
// user cannot have one: `ANativeWindow` only exists once there is an app. so rather than wait for
// the app, the thunk *is* the window system -- it hands out a surface, owns the images, and turns
// vkQueuePresentKHR into whatever the host wants a present to mean: a frame counter, an optional
// PPM, or a copy into a window buffer.
//
// **this is not the path a game runs on**, since the app supplies a window and the swapchain goes
// back to the driver. it is kept, selectable and covered by the regression set for two reasons: a
// shell binary has no window and needs it, and a graphics regression under a real swapchain -- or
// under a driver other than the one a result was proved against -- needs something known-good to
// bisect against.
//
// the guest asks for this through `VK_EXT_headless_surface`, which is a real vulkan extension
// meaning exactly "a surface with no window". that keeps the fork's side of it honest vulkan
// rather than a bespoke entry point, and it is also what let the android path arrive without the
// fork changing at all: the same request, a different surface behind it.

constexpr uint32_t MaxSwapchainImages = 8;

struct Swapchain {
  VkDevice Device;
  VkExtent2D Extent;
  VkFormat Format;
  uint32_t Count;
  uint32_t Next;
  VkImage Images[MaxSwapchainImages];
  VkDeviceMemory Memory[MaxSwapchainImages];
};

// the display, as far as the guest is concerned. 1080p by default because that is SharpEmu's own
// normalised HostVideoOptions and the Odin 3's panel, and because a client that finds the surface
// a different size from the drawable it asked for recreates its swapchain every frame forever --
// which is exactly what the presenter did while this said 1280x720.
//
// note this is the *presentation* size and has nothing to do with what a game renders at:
// Dreaming Sarah draws into its own 1280x720 targets and SharpEmu blits up to the swapchain, on
// any host. --vulkan-size overrides it when there is no window; a window overrides both, and then
// refuses --vulkan-size. under android WSI the driver answers this question and none of these
// three are consulted at all.
uint32_t SurfaceWidth = 1920;
uint32_t SurfaceHeight = 1080;
const char* DumpDirectory {};
std::atomic<uint64_t> PresentedFrames {};

// the app's surface. null wherever there is no app -- including the whole regression set, which
// still runs as a shell binary with no window anywhere -- so every branch on this is also the line
// between "WSI can be invented" and "WSI has somewhere to go".
std::atomic<::ANativeWindow*> AppWindow {};
// set once a window has been seen. the *size* has to stop being overridable at that point, or a
// --vulkan-size left on a command line quietly contradicts the buffer frames land in -- and an
// extent mismatch does not error, it recreates the swapchain forever and never renders.
bool WindowOwnsSize {};

WsiMode RequestedWsi {WsiMode::Auto};
// latched, not recomputed. ImplementedHere() is consulted at proc-address time, vkCreateInstance
// decides an extension list from it, and the dispatch switch reads it on every call -- an answer
// that changed halfway through would mean an instance created for one window system serving
// commands belonging to the other.
bool AndroidWsiLatched {};
bool AndroidWsiDecided {};
// set once the swapchain's preTransform has been forced to identity, which makes the driver call
// every acquire and present suboptimal from then on. see the swallow at the end of Handle().
bool TransformOverridden {};

bool UseAndroidWsi() {
  if (!AndroidWsiDecided) {
    AndroidWsiDecided = true;
    AndroidWsiLatched = RequestedWsi == WsiMode::Android ||
                        (RequestedWsi == WsiMode::Auto && AppWindow.load(std::memory_order_acquire) != nullptr);
    std::printf("[vulkan] wsi: %s\n", AndroidWsiLatched ? "android surface, driver swapchain"
                                                        : "headless surface, host swapchain");
    if (AndroidWsiLatched && !AppWindow.load(std::memory_order_acquire)) {
      // asking for it without one is not fatal here, but it will be at vkCreateAndroidSurfaceKHR,
      // and saying so at the point of the decision is far more useful than a VK_ERROR later.
      std::printf("[vulkan] --vulkan-wsi android was asked for and there is no window\n");
    }
  }
  return AndroidWsiLatched;
}

VkPhysicalDevice GuestPhysicalDevice {};
uint32_t GuestQueueFamily {};
std::mutex WsiLock;
uint32_t SurfaceToken = 0x5EA1;

template<typename PFN>
PFN Host(const char* Name) {
  std::call_once(LibraryOnce, OpenLibrary);
  void* Fn = Library ? ::dlsym(Library, Name) : nullptr;
  if (!Fn && HostGetInstanceProcAddr) {
    Fn = reinterpret_cast<void*>(HostGetInstanceProcAddr(GuestInstance, Name));
  }
  return reinterpret_cast<PFN>(Fn);
}

// --- a surface that outlives its window ------------------------------------------------------
//
// **under android WSI the guest's surface is a real one, made on the app's window, and the window
// does not live as long as the guest.** android takes a SurfaceView's surface away whenever the
// activity stops -- home, the screen going off, anything opening over the game -- because the
// view's surface goes with its window's, whatever lifecycle the view asks for. the driver then
// answers the next acquire with VK_ERROR_SURFACE_LOST_KHR, the guest's presenter treats that as
// fatal, and the run ends.
//
// so the handle the guest holds stays the one it was given and what it stands for moves. every
// command that names it is handed the real surface on the window that is current now, made the
// first time anything asks; and a lost surface is reported to the guest as
// VK_ERROR_OUT_OF_DATE_KHR, which its presenter already answers by recreating the swapchain --
// against the same handle, which by then means the new window.
//
// **the handle the guest was given is the first real surface, and that one is kept until the guest
// destroys it.** a command that reaches the driver untranslated then names a surface on a dead
// window and gets an error back, rather than naming a destroyed object. every later surface is
// destroyed once it has been replaced and the last swapchain made on it is gone, since vulkan
// requires a surface to outlive its swapchains.
struct RealSurface {
  VkSurfaceKHR Handle {};
  ///< which window it was made on, as the count SetAndroidWindow keeps rather than as a pointer. the
  ///< count moves whenever the window does, through null included, so a window freed and a new one
  ///< allocated at the same address cannot compare equal.
  uint64_t Generation {};
  uint32_t Swapchains {};
  bool Retired {};
};

// a leaf: held across the one driver call that makes a surface and never while waiting.
std::mutex SurfaceLock;
std::condition_variable WindowChanged;
uint64_t WindowGeneration {};
// the generation a surface was lost on. nothing is made on it again, because the window it names may
// still be the dead one -- the driver reports the loss before the app is told the window went away.
uint64_t LostGeneration {};
VkSurfaceKHR GuestSurface {};
std::vector<RealSurface> RealSurfaces;
std::unordered_map<uint64_t, VkSurfaceKHR> SwapchainSurface;
uint64_t SurfaceRebuilds {};

RealSurface* FindReal(VkSurfaceKHR Handle) {
  for (auto& Real : RealSurfaces) {
    if (Real.Handle == Handle) {
      return &Real;
    }
  }
  return nullptr;
}

// destroys every replaced surface whose swapchains are gone, except the one whose value the guest
// holds. called with SurfaceLock held.
void DropRetiredSurfaces() {
  auto Destroy = Host<PFN_vkDestroySurfaceKHR>("vkDestroySurfaceKHR");
  for (auto It = RealSurfaces.begin(); It != RealSurfaces.end();) {
    if (It->Retired && It->Swapchains == 0 && It->Handle != GuestSurface) {
      if (Destroy) {
        Destroy(GuestInstance, It->Handle, nullptr);
      }
      It = RealSurfaces.erase(It);
    } else {
      ++It;
    }
  }
}

// makes a real surface on the current window, waiting for one if there is none. called with
// SurfaceLock held through Held.
//
// **the wait is reached only by a guest that keeps rendering while nothing is on screen.** a paused
// guest never asks, and one that is not paused has nowhere to draw: the alternative to waiting is an
// error, and every error a surface query can return is one the guest's presenter dies on.
VkResult MakeRealSurface(std::unique_lock<std::mutex>& Held, RealSurface& Out) {
  auto Create = Host<PFN_vkCreateAndroidSurfaceKHR>("vkCreateAndroidSurfaceKHR");
  if (!Create) {
    return VK_ERROR_INITIALIZATION_FAILED;
  }
  bool Said = false;
  for (;;) {
    ::ANativeWindow* Window = AppWindow.load(std::memory_order_acquire);
    if (Window && WindowGeneration > LostGeneration) {
      VkAndroidSurfaceCreateInfoKHR Info {};
      Info.sType = VK_STRUCTURE_TYPE_ANDROID_SURFACE_CREATE_INFO_KHR;
      Info.window = Window;
      const VkResult Result = Create(GuestInstance, &Info, nullptr, &Out.Handle);
      Out.Generation = WindowGeneration;
      return Result;
    }
    if (!Said) {
      Said = true;
      std::printf("[vulkan] the guest wants a surface and there is no window; waiting for one\n");
      std::fflush(stdout);
    }
    WindowChanged.wait(Held);
  }
}

// what the guest's surface stands for now. a handle that is not the guest's surface is returned as
// it is and leaves Ours false, which is every handle under headless WSI.
VkSurfaceKHR CurrentSurface(VkSurfaceKHR Asked, bool& Ours) {
  std::unique_lock Held(SurfaceLock);
  Ours = GuestSurface && Asked == GuestSurface && !RealSurfaces.empty();
  if (!Ours) {
    return Asked;
  }
  RealSurface& Current = RealSurfaces.back();
  if (!Current.Retired && Current.Generation == WindowGeneration && AppWindow.load(std::memory_order_acquire)) {
    return Current.Handle;
  }
  RealSurface Next {};
  const VkResult Result = MakeRealSurface(Held, Next);
  if (Result != VK_SUCCESS) {
    std::printf("[vulkan] could not remake the surface on the new window: %d\n", static_cast<int>(Result));
    return RealSurfaces.back().Handle;
  }
  RealSurfaces.back().Retired = true;
  RealSurfaces.push_back(Next);
  ++SurfaceRebuilds;
  std::printf("[vulkan] surface remade on a new window (%llu so far): %ux%u\n",
              static_cast<unsigned long long>(SurfaceRebuilds), SurfaceWidth, SurfaceHeight);
  DropRetiredSurfaces();
  return Next.Handle;
}

// a real surface the driver has called lost. the next command naming the guest's surface waits for
// a window newer than the one it was made on.
void SurfaceWasLost(VkSurfaceKHR Real) {
  std::lock_guard Held(SurfaceLock);
  if (RealSurface* Found = FindReal(Real)) {
    if (!Found->Retired) {
      std::printf("[vulkan] the driver lost the surface; the guest is told its swapchain is out of date\n");
    }
    Found->Retired = true;
    if (Found->Generation > LostGeneration) {
      LostGeneration = Found->Generation;
    }
  }
}

// the real surface a swapchain was made on, or null for one this file did not see made.
VkSurfaceKHR SurfaceOfSwapchain(uint64_t Swapchain) {
  std::lock_guard Held(SurfaceLock);
  const auto It = SwapchainSurface.find(Swapchain);
  return It == SwapchainSurface.end() ? VK_NULL_HANDLE : It->second;
}

void NoteSwapchainMade(uint64_t Swapchain, VkSurfaceKHR Real) {
  std::lock_guard Held(SurfaceLock);
  if (RealSurface* Found = FindReal(Real)) {
    ++Found->Swapchains;
    SwapchainSurface[Swapchain] = Real;
  }
}

void NoteSwapchainGone(uint64_t Swapchain) {
  std::lock_guard Held(SurfaceLock);
  const auto It = SwapchainSurface.find(Swapchain);
  if (It == SwapchainSurface.end()) {
    return;
  }
  if (RealSurface* Found = FindReal(It->second); Found && Found->Swapchains) {
    --Found->Swapchains;
  }
  SwapchainSurface.erase(It);
  DropRetiredSurfaces();
}

// the guest destroying a surface. when it is the guest's surface, everything made in its name goes
// with it and the answer is true; any other surface is the driver's to destroy.
bool DestroyGuestSurface(VkSurfaceKHR Asked) {
  std::lock_guard Held(SurfaceLock);
  if (!GuestSurface || Asked != GuestSurface) {
    return false;
  }
  auto Destroy = Host<PFN_vkDestroySurfaceKHR>("vkDestroySurfaceKHR");
  for (auto& Real : RealSurfaces) {
    if (Destroy) {
      Destroy(GuestInstance, Real.Handle, nullptr);
    }
  }
  RealSurfaces.clear();
  SwapchainSurface.clear();
  GuestSurface = VK_NULL_HANDLE;
  return true;
}

// where a forwarded command names a surface by value, as an index into the syscall's arguments --
// Argument[N + 1] is the command's argument N -- or 0 for a command that names none that way. the
// two that name one inside a structure, and the swapchain constructor, are handled where they are
// forwarded.
unsigned SurfaceArgument(uint32_t Id) {
  switch (Id) {
  case Id_vkGetPhysicalDeviceSurfaceSupportKHR: return 3;
  case Id_vkGetPhysicalDeviceSurfaceCapabilitiesKHR:
  case Id_vkGetPhysicalDeviceSurfaceFormatsKHR:
  case Id_vkGetPhysicalDeviceSurfacePresentModesKHR:
  case Id_vkGetDeviceGroupSurfacePresentModesKHR:
  case Id_vkGetPhysicalDevicePresentRectanglesKHR:
  case Id_vkGetPhysicalDeviceSurfaceCapabilities2EXT: return 2;
  default: return 0;
  }
}

// a present that lost one of the guest's surfaces, reported as out of date: per swapchain where the
// guest asked for per-swapchain results, and as the call's own result.
uint64_t ReportLostAsOutOfDate(const VkPresentInfoKHR* Info, uint64_t Result) {
  if (!Info) {
    return Result;
  }
  const bool Lost = static_cast<VkResult>(static_cast<int32_t>(Result)) == VK_ERROR_SURFACE_LOST_KHR;
  bool Reported = false;
  // the lookup takes the surface lock, so it is asked only of a swapchain that was actually lost and
  // an ordinary present costs a comparison or two.
  for (uint32_t i = 0; i < Info->swapchainCount; ++i) {
    const bool ThisLost = Info->pResults ? Info->pResults[i] == VK_ERROR_SURFACE_LOST_KHR : Lost;
    if (!ThisLost) {
      continue;
    }
    const VkSurfaceKHR Real = SurfaceOfSwapchain(reinterpret_cast<uint64_t>(Info->pSwapchains[i]));
    if (!Real) {
      continue;
    }
    SurfaceWasLost(Real);
    if (Info->pResults) {
      Info->pResults[i] = VK_ERROR_OUT_OF_DATE_KHR;
    }
    Reported = true;
  }
  return Lost && Reported ? static_cast<uint64_t>(VK_ERROR_OUT_OF_DATE_KHR) : Result;
}

// the two-call idiom, with our extension appended to whatever the host reported. the guest asks
// once for a count and once for the array, and both answers have to agree.
uint64_t AppendExtension(const char* Extra, VkResult (*Query)(uint32_t*, VkExtensionProperties*), uint32_t* Count,
                         VkExtensionProperties* Properties) {
  uint32_t HostCount = 0;
  Query(&HostCount, nullptr);
  if (!Properties) {
    *Count = HostCount + 1;
    return VK_SUCCESS;
  }
  const uint32_t Room = *Count;
  uint32_t Take = Room < HostCount ? Room : HostCount;
  Query(&Take, Properties);
  if (Take < Room) {
    VkExtensionProperties& Slot = Properties[Take];
    std::memset(&Slot, 0, sizeof(Slot));
    std::strncpy(Slot.extensionName, Extra, sizeof(Slot.extensionName) - 1);
    Slot.specVersion = 1;
    *Count = Take + 1;
    return VK_SUCCESS;
  }
  *Count = Take;
  return VK_INCOMPLETE;
}

// a queue to submit our own bookkeeping work on. acquire has to signal a semaphore and a fence,
// and the only way to signal either from outside the guest's own submissions is an empty submit.
VkQueue BookkeepingQueue(VkDevice Device) {
  static VkQueue Queue {};
  if (!Queue) {
    if (auto Get = Host<PFN_vkGetDeviceQueue>("vkGetDeviceQueue")) {
      Get(Device, GuestQueueFamily, 0, &Queue);
    }
  }
  return Queue;
}

uint64_t SignalEmpty(VkDevice Device, VkSemaphore Semaphore, VkFence Fence) {
  auto Submit = Host<PFN_vkQueueSubmit>("vkQueueSubmit");
  VkQueue Queue = BookkeepingQueue(Device);
  if (!Submit || !Queue) {
    return VK_ERROR_INITIALIZATION_FAILED;
  }
  VkSubmitInfo Info {};
  Info.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
  Info.signalSemaphoreCount = Semaphore ? 1 : 0;
  Info.pSignalSemaphores = Semaphore ? &Semaphore : nullptr;
  return Submit(Queue, 1, &Info, Fence);
}

uint64_t CreateSwapchain(const VkSwapchainCreateInfoKHR* Info, VkDevice Device, VkSwapchainKHR* Out) {
  auto CreateImage = Host<PFN_vkCreateImage>("vkCreateImage");
  auto Requirements = Host<PFN_vkGetImageMemoryRequirements>("vkGetImageMemoryRequirements");
  auto Allocate = Host<PFN_vkAllocateMemory>("vkAllocateMemory");
  auto Bind = Host<PFN_vkBindImageMemory>("vkBindImageMemory");
  auto MemoryProperties = Host<PFN_vkGetPhysicalDeviceMemoryProperties>("vkGetPhysicalDeviceMemoryProperties");
  if (!CreateImage || !Requirements || !Allocate || !Bind || !MemoryProperties || !GuestPhysicalDevice) {
    return VK_ERROR_INITIALIZATION_FAILED;
  }

  auto* Chain = new Swapchain {};
  Chain->Device = Device;
  Chain->Extent = Info->imageExtent;
  Chain->Format = Info->imageFormat;
  Chain->Count = Info->minImageCount < 2 ? 2 : Info->minImageCount;
  if (Chain->Count > MaxSwapchainImages) {
    Chain->Count = MaxSwapchainImages;
  }

  VkPhysicalDeviceMemoryProperties Memory {};
  MemoryProperties(GuestPhysicalDevice, &Memory);

  for (uint32_t i = 0; i < Chain->Count; ++i) {
    VkImageCreateInfo Image {};
    Image.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    Image.imageType = VK_IMAGE_TYPE_2D;
    Image.format = Chain->Format;
    Image.extent = {Chain->Extent.width, Chain->Extent.height, 1};
    Image.mipLevels = 1;
    Image.arrayLayers = 1;
    Image.samples = VK_SAMPLE_COUNT_1_BIT;
    Image.tiling = VK_IMAGE_TILING_OPTIMAL;
    // TRANSFER_SRC on top of whatever the guest asked for, because present has to be able to
    // read the image back out. the guest never sees this and cannot be broken by it.
    Image.usage = Info->imageUsage | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
    Image.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    Image.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    if (CreateImage(Device, &Image, nullptr, &Chain->Images[i]) != VK_SUCCESS) {
      return VK_ERROR_INITIALIZATION_FAILED;
    }

    VkMemoryRequirements Needs {};
    Requirements(Device, Chain->Images[i], &Needs);
    uint32_t Type = UINT32_MAX;
    for (uint32_t t = 0; t < Memory.memoryTypeCount; ++t) {
      if ((Needs.memoryTypeBits & (1u << t)) &&
          (Memory.memoryTypes[t].propertyFlags & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT)) {
        Type = t;
        break;
      }
    }
    if (Type == UINT32_MAX) {
      return VK_ERROR_INITIALIZATION_FAILED;
    }
    VkMemoryAllocateInfo Request {};
    Request.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    Request.allocationSize = Needs.size;
    Request.memoryTypeIndex = Type;
    if (Allocate(Device, &Request, nullptr, &Chain->Memory[i]) != VK_SUCCESS ||
        Bind(Device, Chain->Images[i], Chain->Memory[i], 0) != VK_SUCCESS) {
      return VK_ERROR_INITIALIZATION_FAILED;
    }
  }

  *Out = reinterpret_cast<VkSwapchainKHR>(Chain);
  std::printf("[vulkan] swapchain %ux%u, %u images, format %d\n", Chain->Extent.width, Chain->Extent.height, Chain->Count,
              static_cast<int>(Chain->Format));
  return VK_SUCCESS;
}

// pulling a presented frame back out -- a full device-to-host copy and a stall, into a linear image
// the CPU can read. it exists so that "the game renders" can be looked at rather than inferred from
// a frame counter, and the headless present is the same copy into an ANativeWindow buffer instead
// of a file.
//
// **both consumers are headless-only.** under android WSI the swapchain images belong to the
// driver and there is nothing here to copy; `adb shell screencap -p` answers the same question
// more cheaply.
struct Capture {
  VkCommandPool Pool;
  VkCommandBuffer Commands;
  VkImage Staging;
  VkDeviceMemory Memory;
  VkFence Fence;
  uint32_t Width, Height;
};
Capture Grab {};
// the capture machinery is a single set of objects reused every frame, so two threads presenting
// at once would record into the same command buffer. SharpEmu presents from one thread, but this
// is now on the path of every frame rather than every three-hundredth, and a latent race on the
// hottest path in the project is not worth leaving to a promise made elsewhere. deliberately not
// WsiLock: that one is also held across acquire, and present waits on a fence.
std::mutex CaptureLock;

bool EnsureCapture(Swapchain* Chain) {
  // a swapchain recreated at a different size leaves a staging image of the old one behind, and
  // the copy would then be reading and writing different rectangles. it cannot happen while the
  // extent comes from the window, which is the point of it coming from the window, but this is
  // the place where that assumption would fail silently rather than loudly.
  if (Grab.Staging && (Grab.Width != Chain->Extent.width || Grab.Height != Chain->Extent.height)) {
    if (auto Destroy = Host<PFN_vkDestroyImage>("vkDestroyImage")) {
      Destroy(Chain->Device, Grab.Staging, nullptr);
    }
    if (auto Free = Host<PFN_vkFreeMemory>("vkFreeMemory")) {
      Free(Chain->Device, Grab.Memory, nullptr);
    }
    Grab.Staging = VK_NULL_HANDLE;
    Grab.Memory = VK_NULL_HANDLE;
  }
  if (Grab.Staging) {
    return true;
  }
  auto CreatePool = Host<PFN_vkCreateCommandPool>("vkCreateCommandPool");
  auto AllocCommands = Host<PFN_vkAllocateCommandBuffers>("vkAllocateCommandBuffers");
  auto CreateImage = Host<PFN_vkCreateImage>("vkCreateImage");
  auto Requirements = Host<PFN_vkGetImageMemoryRequirements>("vkGetImageMemoryRequirements");
  auto Allocate = Host<PFN_vkAllocateMemory>("vkAllocateMemory");
  auto Bind = Host<PFN_vkBindImageMemory>("vkBindImageMemory");
  auto MemoryProperties = Host<PFN_vkGetPhysicalDeviceMemoryProperties>("vkGetPhysicalDeviceMemoryProperties");
  auto CreateFence = Host<PFN_vkCreateFence>("vkCreateFence");
  if (!CreatePool || !AllocCommands || !CreateImage || !Requirements || !Allocate || !Bind || !MemoryProperties ||
      !CreateFence) {
    return false;
  }

  // the pool, the command buffer and the fence do not depend on the extent, so a rebuild after a
  // resize keeps them rather than leaking one set per recreation.
  if (!Grab.Pool) {
    VkCommandPoolCreateInfo PoolInfo {};
    PoolInfo.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
    PoolInfo.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    PoolInfo.queueFamilyIndex = GuestQueueFamily;
    CreatePool(Chain->Device, &PoolInfo, nullptr, &Grab.Pool);

    VkCommandBufferAllocateInfo CommandsInfo {};
    CommandsInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    CommandsInfo.commandPool = Grab.Pool;
    CommandsInfo.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    CommandsInfo.commandBufferCount = 1;
    AllocCommands(Chain->Device, &CommandsInfo, &Grab.Commands);
  }

  VkImageCreateInfo Image {};
  Image.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
  Image.imageType = VK_IMAGE_TYPE_2D;
  Image.format = Chain->Format;
  Image.extent = {Chain->Extent.width, Chain->Extent.height, 1};
  Image.mipLevels = 1;
  Image.arrayLayers = 1;
  Image.samples = VK_SAMPLE_COUNT_1_BIT;
  Image.tiling = VK_IMAGE_TILING_LINEAR;
  Image.usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT;
  Image.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
  if (CreateImage(Chain->Device, &Image, nullptr, &Grab.Staging) != VK_SUCCESS) {
    return false;
  }

  VkMemoryRequirements Needs {};
  Requirements(Chain->Device, Grab.Staging, &Needs);
  VkPhysicalDeviceMemoryProperties Memory {};
  MemoryProperties(GuestPhysicalDevice, &Memory);
  uint32_t Type = UINT32_MAX;
  const auto Wanted = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
  for (uint32_t t = 0; t < Memory.memoryTypeCount; ++t) {
    if ((Needs.memoryTypeBits & (1u << t)) && (Memory.memoryTypes[t].propertyFlags & Wanted) == Wanted) {
      Type = t;
      break;
    }
  }
  if (Type == UINT32_MAX) {
    return false;
  }
  VkMemoryAllocateInfo Request {};
  Request.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
  Request.allocationSize = Needs.size;
  Request.memoryTypeIndex = Type;
  if (Allocate(Chain->Device, &Request, nullptr, &Grab.Memory) != VK_SUCCESS ||
      Bind(Chain->Device, Grab.Staging, Grab.Memory, 0) != VK_SUCCESS) {
    return false;
  }

  if (!Grab.Fence) {
    VkFenceCreateInfo FenceInfo {};
    FenceInfo.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
    CreateFence(Chain->Device, &FenceInfo, nullptr, &Grab.Fence);
  }
  Grab.Width = Chain->Extent.width;
  Grab.Height = Chain->Extent.height;
  return true;
}

void Transition(VkCommandBuffer Commands, VkImage Image, VkImageLayout From, VkImageLayout To) {
  auto Barrier = Host<PFN_vkCmdPipelineBarrier>("vkCmdPipelineBarrier");
  VkImageMemoryBarrier Change {};
  Change.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
  Change.srcAccessMask = VK_ACCESS_MEMORY_WRITE_BIT;
  Change.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT | VK_ACCESS_TRANSFER_WRITE_BIT;
  Change.oldLayout = From;
  Change.newLayout = To;
  Change.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
  Change.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
  Change.image = Image;
  Change.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
  Barrier(Commands, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 0, nullptr, 0, nullptr, 1,
          &Change);
}

// device-local presented image -> a linear image the CPU can read. this began as a debug aid and
// became the present path: the destination is the only difference between writing a PPM and putting the
// frame on the panel, which is what "--vulkan-dump is the shape of the real present" meant.
//
// on success the caller gets a mapped, row-pitched view of the frame and **must** call
// ReleaseFrame. it returns null rather than throwing anything, because a present that cannot be
// captured should cost a frame and not the run.
const uint8_t* CaptureFrame(Swapchain* Chain, uint32_t Index, VkDeviceSize* RowPitch) {
  if (!EnsureCapture(Chain)) {
    return nullptr;
  }
  auto Begin = Host<PFN_vkBeginCommandBuffer>("vkBeginCommandBuffer");
  auto End = Host<PFN_vkEndCommandBuffer>("vkEndCommandBuffer");
  auto Reset = Host<PFN_vkResetCommandBuffer>("vkResetCommandBuffer");
  auto Copy = Host<PFN_vkCmdCopyImage>("vkCmdCopyImage");
  auto Submit = Host<PFN_vkQueueSubmit>("vkQueueSubmit");
  auto WaitFences = Host<PFN_vkWaitForFences>("vkWaitForFences");
  auto ResetFences = Host<PFN_vkResetFences>("vkResetFences");
  auto Map = Host<PFN_vkMapMemory>("vkMapMemory");
  auto Unmap = Host<PFN_vkUnmapMemory>("vkUnmapMemory");
  auto Layout = Host<PFN_vkGetImageSubresourceLayout>("vkGetImageSubresourceLayout");
  VkQueue Queue = BookkeepingQueue(Chain->Device);
  if (!Begin || !Copy || !Submit || !Map || !Queue) {
    return nullptr;
  }

  Reset(Grab.Commands, 0);
  VkCommandBufferBeginInfo Start {};
  Start.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
  Start.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
  Begin(Grab.Commands, &Start);
  Transition(Grab.Commands, Grab.Staging, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
  Transition(Grab.Commands, Chain->Images[Index], VK_IMAGE_LAYOUT_PRESENT_SRC_KHR, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
  VkImageCopy Region {};
  Region.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
  Region.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
  Region.extent = {Grab.Width, Grab.Height, 1};
  Copy(Grab.Commands, Chain->Images[Index], VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, Grab.Staging,
       VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &Region);
  Transition(Grab.Commands, Chain->Images[Index], VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR);
  Transition(Grab.Commands, Grab.Staging, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_GENERAL);
  End(Grab.Commands);

  VkSubmitInfo Work {};
  Work.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
  Work.commandBufferCount = 1;
  Work.pCommandBuffers = &Grab.Commands;
  Submit(Queue, 1, &Work, Grab.Fence);
  WaitFences(Chain->Device, 1, &Grab.Fence, VK_TRUE, 5'000'000'000ull);
  ResetFences(Chain->Device, 1, &Grab.Fence);

  VkImageSubresource Which {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0};
  VkSubresourceLayout Placement {};
  Layout(Chain->Device, Grab.Staging, &Which, &Placement);
  void* Pixels {};
  if (Map(Chain->Device, Grab.Memory, 0, VK_WHOLE_SIZE, 0, &Pixels) != VK_SUCCESS) {
    return nullptr;
  }
  *RowPitch = Placement.rowPitch;
  return static_cast<const uint8_t*>(Pixels) + Placement.offset;
}

void ReleaseFrame(Swapchain* Chain) {
  if (auto Unmap = Host<PFN_vkUnmapMemory>("vkUnmapMemory")) {
    Unmap(Chain->Device, Grab.Memory);
  }
}

void DumpFrame(Swapchain* Chain, uint32_t Index, uint64_t Frame) {
  std::lock_guard Held(CaptureLock);
  VkDeviceSize RowPitch = 0;
  const uint8_t* Base = CaptureFrame(Chain, Index, &RowPitch);
  if (!Base) {
    return;
  }

  char Path[512];
  std::snprintf(Path, sizeof(Path), "%s-%05llu.ppm", DumpDirectory, static_cast<unsigned long long>(Frame));
  if (std::FILE* File = std::fopen(Path, "wbe")) {
    std::fprintf(File, "P6\n%u %u\n255\n", Grab.Width, Grab.Height);
    std::vector<uint8_t> Row(Grab.Width * 3);
    for (uint32_t y = 0; y < Grab.Height; ++y) {
      const uint8_t* Source = Base + RowPitch * y;
      for (uint32_t x = 0; x < Grab.Width; ++x) {
        // the swapchain is B8G8R8A8 and a PPM is RGB.
        Row[x * 3 + 0] = Source[x * 4 + 2];
        Row[x * 3 + 1] = Source[x * 4 + 1];
        Row[x * 3 + 2] = Source[x * 4 + 0];
      }
      std::fwrite(Row.data(), 1, Row.size(), File);
    }
    std::fclose(File);
    std::printf("[vulkan] wrote %s\n", Path);
  }
  ReleaseFrame(Chain);
}

// the same capture, into the app's window instead of a file.
//
// the copy is on the CPU and it is not free -- 8 MB a frame at 1080p, read back over PCIe-equivalent
// and written again with a channel swap. it is the honest shape of "the host layer owns the
// swapchain", and the way out of it is for the swapchain to stop being ours: a real
// VK_KHR_android_surface on this window would let the driver composite the guest's images with no
// copy at all. that is a follow-up and a measurement, not a milestone -- see the performance file.
// only the headless path needs this, and only it may do it: it is about to write into the window
// by hand, so it has to know the buffer's size and format rather than letting the compositor pick
// one. under android WSI the driver owns this window and setting geometry behind it would be a
// way to make the swapchain disagree with its own buffers.
void EnsureWindowGeometry(::ANativeWindow* Window) {
  static ::ANativeWindow* Configured {};
  if (Configured == Window) {
    return;
  }
  ::ANativeWindow_setBuffersGeometry(Window, static_cast<int32_t>(SurfaceWidth), static_cast<int32_t>(SurfaceHeight),
                                     WINDOW_FORMAT_RGBA_8888);
  Configured = Window;
}

void PostFrame(Swapchain* Chain, uint32_t Index, ::ANativeWindow* Window) {
  std::lock_guard Held(CaptureLock);
  EnsureWindowGeometry(Window);
  VkDeviceSize RowPitch = 0;
  const uint8_t* Base = CaptureFrame(Chain, Index, &RowPitch);
  if (!Base) {
    return;
  }

  ANativeWindow_Buffer Buffer {};
  if (::ANativeWindow_lock(Window, &Buffer, nullptr) != 0) {
    ReleaseFrame(Chain);
    return;
  }

  // the geometry was set from this window, so these agree in every configuration we ship. clamp
  // anyway: the compositor is entitled to hand back a buffer of its own choosing, and a present
  // that writes past the end of it would be a crash a long way from its cause.
  const uint32_t Width = Buffer.width < static_cast<int32_t>(Grab.Width) ? Buffer.width : Grab.Width;
  const uint32_t Height = Buffer.height < static_cast<int32_t>(Grab.Height) ? Buffer.height : Grab.Height;
  for (uint32_t y = 0; y < Height; ++y) {
    const auto* Source = reinterpret_cast<const uint32_t*>(Base + RowPitch * y);
    auto* Target = static_cast<uint32_t*>(Buffer.bits) + static_cast<size_t>(Buffer.stride) * y;
    for (uint32_t x = 0; x < Width; ++x) {
      // the swapchain image is B8G8R8A8_UNORM and the window buffer is RGBA_8888: byte 0 and byte
      // 2 change places and the other two stay. done as a word swizzle rather than four byte
      // stores because this loop runs 2 million times per frame at 60 Hz.
      const uint32_t Pixel = Source[x];
      Target[x] = (Pixel & 0xFF00FF00u) | ((Pixel >> 16) & 0x000000FFu) | ((Pixel & 0x000000FFu) << 16);
    }
  }

  ::ANativeWindow_unlockAndPost(Window);
  ReleaseFrame(Chain);
}

// counted in both modes, because under android WSI the present itself is forwarded and there is
// nothing else left that knows a frame happened. the run summary only prints on a clean exit and a
// game run is killed by a timeout, so the log line is the frame counter as far as any real run is
// concerned -- which is a finding in its own right and cost a run to notice.
// the profile dump, as a delta since the last one rather than a running total. a total is
// dominated by start-up forever -- shader compilation and the first pipeline creations are seconds
// of `vkCreateGraphicsPipelines` that never repeat -- and would bury a steady-state stall under it.
// per-interval means every dump describes the frames that just happened.
void DumpProfile(uint64_t Frame, uint64_t IntervalFrames) {
  struct Row {
    uint32_t Id;
    uint64_t Nanos;
    uint64_t Calls;
    uint64_t MaxNanos;
    uint64_t CpuNanos;
  };
  std::vector<Row> Rows;
  uint64_t TotalNanos = 0;
  uint64_t TotalCpuNanos = 0;
  static uint64_t LastNanos[CommandCount] {};
  static uint64_t LastCalls[CommandCount] {};
  static uint64_t LastCpu[CommandCount] {};

  for (uint32_t Id = 0; Id < CommandCount; ++Id) {
    const uint64_t Nanos = Profile[Id].Nanos.load(std::memory_order_relaxed);
    const uint64_t CallCount = Profile[Id].Calls.load(std::memory_order_relaxed);
    const uint64_t Cpu = Profile[Id].CpuNanos.load(std::memory_order_relaxed);
    const uint64_t DeltaNanos = Nanos - LastNanos[Id];
    const uint64_t DeltaCalls = CallCount - LastCalls[Id];
    const uint64_t DeltaCpu = Cpu - LastCpu[Id];
    LastNanos[Id] = Nanos;
    LastCalls[Id] = CallCount;
    LastCpu[Id] = Cpu;
    if (DeltaCalls) {
      // the max is per-interval, so it is reset here rather than accumulated: a single stall
      // during shader compilation would otherwise be the reported worst case forever.
      Rows.push_back({Id, DeltaNanos, DeltaCalls, Profile[Id].MaxNanos.exchange(0, std::memory_order_relaxed),
                      DeltaCpu});
      TotalNanos += DeltaNanos;
      TotalCpuNanos += DeltaCpu;
    }
  }
  std::sort(Rows.begin(), Rows.end(), [](const Row& A, const Row& B) { return A.Nanos > B.Nanos; });

  // wall time for the same interval, so the dump says what fraction of a frame is actually spent
  // inside vulkan. this is the line that matters most: a stall *inside* a command and a stall the
  // guest does to itself between commands look identical in a per-command table, and they have
  // completely different causes. turnip is the second kind and the stock driver is the first.
  static uint64_t LastWall = 0;
  const uint64_t Wall = NowNanos();
  const double WallMs = LastWall ? (Wall - LastWall) / 1e6 : 0.0;
  LastWall = Wall;

  std::printf("[vkprof] frames %llu-%llu: %.2f ms/f wall, %.2f ms/f in vulkan (%.2f of it cpu), %.2f ms/f elsewhere\n",
              static_cast<unsigned long long>(Frame - IntervalFrames + 1), static_cast<unsigned long long>(Frame),
              WallMs / IntervalFrames, TotalNanos / 1e6 / IntervalFrames, TotalCpuNanos / 1e6 / IntervalFrames,
              (WallMs - TotalNanos / 1e6) / IntervalFrames);
  // the answer to "is the wait real". a submission's own turnaround does not depend on how idle
  // the guest is, so this is comparable across drivers in a way the fence-check wall time is not.
  const uint64_t TurnCount = GpuTurnaroundCount.exchange(0, std::memory_order_relaxed);
  const uint64_t TurnNanos = GpuTurnaroundNanos.exchange(0, std::memory_order_relaxed);
  const uint64_t TurnMin = GpuTurnaroundMinNanos.exchange(~0ull, std::memory_order_relaxed);
  if (TurnCount) {
    std::printf("[vkprof] gpu turnaround: %.3f ms mean, %.3f ms best, over %llu submissions "
                "(%.1f per frame)\n",
                TurnNanos / 1e6 / static_cast<double>(TurnCount), TurnMin / 1e6,
                static_cast<unsigned long long>(TurnCount), static_cast<double>(TurnCount) / IntervalFrames);
  }

  std::printf("[vkprof] %zu commands, worst first:\n", Rows.size());
  for (size_t i = 0; i < Rows.size() && i < 12; ++i) {
    const auto& R = Rows[i];
    std::printf("[vkprof]   %-38s %7.3f ms/f wall %7.3f ms/f cpu %7.1f calls/f %8.1f us/call  worst %8.1f us\n",
                Commands[R.Id].Name, R.Nanos / 1e6 / IntervalFrames, R.CpuNanos / 1e6 / IntervalFrames,
                static_cast<double>(R.Calls) / IntervalFrames, R.Nanos / 1e3 / static_cast<double>(R.Calls),
                R.MaxNanos / 1e3);
  }
}

uint64_t CountPresentedFrame() {
  const uint64_t Frame = PresentedFrames.fetch_add(1, std::memory_order_relaxed) + 1;
  if (Frame == 1) {
    // **the end of a boot is here rather than at any line the emulator prints**, and that is the
    // point of putting it here: the last thing the emulator says before a picture appears is 50 to
    // 240 ms early depending on the title, and it is a string that upstream is free to change.
    // this is the moment the panel has something on it, and it is ours.
    HostLayer::BootProgress::FirstFrame();
  }
  if (Frame == 1 || Frame % 300 == 0) {
    std::printf("[vulkan] presented frame %llu\n", static_cast<unsigned long long>(Frame));
    if (ProfileEnabled && Frame % 300 == 0) {
      DumpProfile(Frame, 300);
    }
  }
  return Frame;
}

// present is where back-pressure comes from. the guest's frame is not finished when it calls
// this -- it is finished when the semaphores it is waiting on are signalled -- so waiting here
// is what stops the render loop free-running. without it that loop free-runs -- measured at 70,000
// dispatches a second with nothing to push back on it, which is the leading suspect for the device
// reboots.
uint64_t Present(const VkPresentInfoKHR* Info) {
  auto Submit = Host<PFN_vkQueueSubmit>("vkQueueSubmit");
  auto CreateFence = Host<PFN_vkCreateFence>("vkCreateFence");
  auto WaitFences = Host<PFN_vkWaitForFences>("vkWaitForFences");
  auto DestroyFence = Host<PFN_vkDestroyFence>("vkDestroyFence");
  if (Info->swapchainCount == 0 || !Submit || !CreateFence || !WaitFences) {
    return VK_ERROR_INITIALIZATION_FAILED;
  }

  auto* Chain = reinterpret_cast<Swapchain*>(Info->pSwapchains[0]);
  VkQueue Queue = BookkeepingQueue(Chain->Device);
  if (!Queue) {
    return VK_ERROR_INITIALIZATION_FAILED;
  }

  VkFence Done {};
  VkFenceCreateInfo FenceInfo {};
  FenceInfo.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
  CreateFence(Chain->Device, &FenceInfo, nullptr, &Done);

  VkPipelineStageFlags Stage = VK_PIPELINE_STAGE_ALL_COMMANDS_BIT;
  std::vector<VkPipelineStageFlags> Stages(Info->waitSemaphoreCount, Stage);
  VkSubmitInfo Wait {};
  Wait.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
  Wait.waitSemaphoreCount = Info->waitSemaphoreCount;
  Wait.pWaitSemaphores = Info->pWaitSemaphores;
  Wait.pWaitDstStageMask = Stages.empty() ? nullptr : Stages.data();
  Submit(Queue, 1, &Wait, Done);
  WaitFences(Chain->Device, 1, &Done, VK_TRUE, 5'000'000'000ull);
  if (DestroyFence) {
    DestroyFence(Chain->Device, Done, nullptr);
  }

  const uint64_t Frame = CountPresentedFrame();

  // and now the frame goes somewhere. read once: the surface can be taken away by the app while
  // the guest is mid-present, and a window that is null by the time we reach the copy is a frame
  // to skip rather than a crash.
  if (::ANativeWindow* Window = AppWindow.load(std::memory_order_acquire); Window && Info->pImageIndices) {
    PostFrame(Chain, Info->pImageIndices[0], Window);
  }

  if (DumpDirectory && Info->pImageIndices && (Frame == 1 || Frame % 300 == 0)) {
    DumpFrame(Chain, Info->pImageIndices[0], Frame);
  }
  return VK_SUCCESS;
}

} // namespace

uint64_t PresentedFrameCount() {
  return PresentedFrames.load(std::memory_order_relaxed);
}

void SetDumpPrefix(const char* Prefix) {
  if (Prefix && *Prefix) {
    DumpDirectory = Prefix;
  }
}

void SetWsiMode(WsiMode Mode) {
  RequestedWsi = Mode;
}

void SetSurfaceSize(uint32_t Width, uint32_t Height) {
  // --vulkan-size, which exists for the windowless case and must not be able to contradict a real
  // window. saying so is cheaper than debugging it: a size the guest is told about that differs
  // from the buffer its frames land in is the silent infinite swapchain recreation this once hit.
  if (WindowOwnsSize) {
    std::printf("[vulkan] ignoring --vulkan-size %ux%u: the window is %ux%u and it decides\n", Width, Height,
                SurfaceWidth, SurfaceHeight);
    return;
  }
  SurfaceWidth = Width;
  SurfaceHeight = Height;
}

void SetAndroidWindow(::ANativeWindow* Window) {
  if (Window) {
    const int32_t Width = ::ANativeWindow_getWidth(Window);
    const int32_t Height = ::ANativeWindow_getHeight(Window);
    if (Width > 0 && Height > 0) {
      SurfaceWidth = static_cast<uint32_t>(Width);
      SurfaceHeight = static_cast<uint32_t>(Height);
    }
    WindowOwnsSize = true;
  }

  // note what is deliberately *not* done here: ANativeWindow_setBuffersGeometry. under android WSI
  // the driver configures this window itself, and pinning a format behind its back is a good way
  // to get a swapchain that disagrees with the buffers it is handed. the headless path still needs
  // it, so it asks for it at the point it is about to write -- see EnsureWindowGeometry.
  //
  // **the swap is under the surface lock, and the caller depends on it.** the app releases the
  // window it held only after this returns, so a guest thread making a surface on that window has
  // finished with the pointer before it can be freed. the count moves only when the window does:
  // android hands over a surface it already handed over whenever the view is laid out again, and a
  // surface remade on the window the guest's swapchain already owns would be refused as in use.
  {
    std::lock_guard Held(SurfaceLock);
    if (AppWindow.load(std::memory_order_acquire) != Window) {
      ++WindowGeneration;
    }
    AppWindow.store(Window, std::memory_order_release);
  }
  WindowChanged.notify_all();

  if (!Window) {
    std::printf("[vulkan] surface released\n");
    return;
  }
  std::printf("[vulkan] surface attached: %ux%u\n", SurfaceWidth, SurfaceHeight);
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

void SetProfile(bool Value) {
  ProfileEnabled = Value;
}

void SetLibraryPath(const char* Path) {
  if (Path && *Path) {
    LibraryPath = Path;
  }
}

void SetDriver(const char* Path) {
  if (Path && *Path) {
    DriverPath = Path;
  }
}

void SetHookLibDir(const char* Dir) {
  if (Dir && *Dir) {
    HookLibDir = Dir;
  }
}

bool ChosenDriverLoads() {
  // the same once-flag the guest's first thunk call uses, so this **is** the load the run will use
  // rather than a rehearsal of it. asking early only moves when it happens; asking twice does
  // nothing the second time.
  std::call_once(LibraryOnce, OpenLibrary);
  // **flushed, because a refusal is followed by the process ending rather than by more output.**
  // stdout is a pipe here and therefore fully buffered, so everything the open just explained would
  // otherwise sit in that buffer until 4 KB of a run that is not going to happen.
  std::fflush(stdout);
  // Unknown is a yes here, and that is the whole of the safety argument. it covers a run that named
  // no driver, a maps file that could not be read and a path that would not resolve -- none of which
  // is evidence against the driver, and all of which would otherwise end a run that works.
  return Verdict != Injection::No;
}

// --- turbo -----------------------------------------------------------------------------------
std::thread TurboThread;
std::atomic<bool> TurboRunning {};

void SetTurbo(bool Enabled) {
  if (!Enabled) {
    // **self-healing, and not optional.** the pinned state is a device-global KGSL property that
    // outlives the process, and a run killed by `am force-stop` -- which is how every measurement
    // in this project ends -- never reaches the release below. so a run that does *not* ask for
    // turbo clears it on the way in, which means the next ordinary launch always puts the
    // governor back however the last one died.
    ::adrenotools_set_turbo(false);
    return;
  }
  if (TurboRunning.exchange(true)) {
    return;
  }
  TurboThread = std::thread([] {
    // 100 ms is Eden's own cadence for how recent a submission has to be for it to keep
    // re-asserting. we have no equivalent signal here and the guest submits continuously while it
    // renders, so the timer stands in for it.
    std::printf("[vulkan] turbo: pinning GPU clocks via KGSL until exit\n");
    while (TurboRunning.load(std::memory_order_relaxed)) {
      ::adrenotools_set_turbo(true);
      std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
    ::adrenotools_set_turbo(false);
    std::printf("[vulkan] turbo: released\n");
  });
}

void StopTurbo() {
  if (TurboRunning.exchange(false) && TurboThread.joinable()) {
    TurboThread.join();
  }
}

void AddDriverEnv(const char* Assignment) {
  if (Assignment && *Assignment) {
    DriverEnv.emplace_back(Assignment);
  }
}

uint64_t CallCount() {
  return Calls.load(std::memory_order_relaxed);
}

uint64_t UnresolvedCount() {
  return Unresolved.load(std::memory_order_relaxed);
}

const char* LastUnresolved() {
  return LastUnresolvedName;
}

uint64_t Handle(FEXCore::Core::CpuStateFrame* Frame, HostLayer::SyscallArguments* Args) {
  const uint32_t Id = static_cast<uint32_t>(Args->Argument[0] & 0xFFFF);

  // the guest half of the thunk is a file staged next to glibc, so it is found and loaded
  // whether or not --vulkan was given; there is no version of "not staged" to fall back to.
  // so an unenabled thunk answers every call with a failure the guest can act on, rather than
  // by letting the magic number fall through to the syscall table as an unhandled number.
  if (!ThunkEnabled) {
    if (!Complained) {
      Complained = true;
      std::printf("[vulkan] guest asked for vulkan and the thunk is not enabled (pass --vulkan)\n");
    }
    return Id == AttachId ? static_cast<uint64_t>(-1) : static_cast<uint64_t>(VK_ERROR_INITIALIZATION_FAILED);
  }

  if (Id == AttachId) {
    return Attach(Args->Argument[1]) ? 0 : static_cast<uint64_t>(-1);
  }
  if (Id >= CommandCount) {
    std::printf("[vulkan] call to unknown command id %u\n", Id);
    return static_cast<uint64_t>(VK_ERROR_INITIALIZATION_FAILED);
  }

  Calls.fetch_add(1, std::memory_order_relaxed);
  if (TraceEnabled) {
    std::printf("[vulkan] %s(0x%llX, 0x%llX, 0x%llX, ...)\n", Commands[Id].Name,
                static_cast<unsigned long long>(Args->Argument[1]), static_cast<unsigned long long>(Args->Argument[2]),
                static_cast<unsigned long long>(Args->Argument[3]));
  }

  // the two commands whose return value is an address the guest will call.
  if (Id == Id_vkGetInstanceProcAddr) {
    return GuestProcAddr(reinterpret_cast<const char*>(Args->Argument[2]));
  }
  if (Id == Id_vkGetDeviceProcAddr) {
    return GuestProcAddr(reinterpret_cast<const char*>(Args->Argument[2]));
  }

  // --- WSI, and the two extension queries that go with it ------------------------------------
  //
  // "answered here, never forwarded" was true when the thunk was the window system. under android
  // WSI most of this block stops being ours, and the right thing for those commands is to fall
  // through to the marshaller at the bottom and become ordinary forwarded vulkan again.
  //
  // which ones those are is decided in exactly one place, ImplementedHere(), because the
  // proc-address gate has to give the same answer -- a disagreement between those two has cost a
  // whole milestone before. dispatching on CommandCount matches no case and so lands on the
  // default, which is the forward.
  const bool AlwaysOurs = Id == Id_vkCreateInstance || Id == Id_vkEnumerateInstanceExtensionProperties ||
                          Id == Id_vkEnumerateDeviceExtensionProperties;
  switch (ImplementedHere(Id) || AlwaysOurs ? Id : CommandCount) {
  case Id_vkEnumerateInstanceExtensionProperties: {
    // only the unlayered query is ours to extend; a layer name means the guest is asking about
    // something we do not provide at all.
    if (reinterpret_cast<const char*>(Args->Argument[1])) {
      break;
    }
    static auto HostQuery = [](uint32_t* Count, VkExtensionProperties* Properties) {
      auto Fn = Host<PFN_vkEnumerateInstanceExtensionProperties>("vkEnumerateInstanceExtensionProperties");
      return Fn ? Fn(nullptr, Count, Properties) : VK_ERROR_INITIALIZATION_FAILED;
    };
    return AppendExtension(VK_EXT_HEADLESS_SURFACE_EXTENSION_NAME, HostQuery,
                           reinterpret_cast<uint32_t*>(Args->Argument[2]),
                           reinterpret_cast<VkExtensionProperties*>(Args->Argument[3]));
  }
  case Id_vkCreateInstance: {
    // an extension we implement ourselves must be removed before the create info reaches the
    // host loader, which has never heard of it and rejects the whole instance. advertising an
    // extension and then handing it to someone who does not have it is the obvious failure of
    // this design and it is worth the special case rather than a note.
    auto Fn = Host<PFN_vkCreateInstance>("vkCreateInstance");
    if (!Fn) {
      break;
    }
    const auto* Requested = reinterpret_cast<const VkInstanceCreateInfo*>(Args->Argument[1]);
    auto* Out = reinterpret_cast<VkInstance*>(Args->Argument[3]);
    VkInstanceCreateInfo Forwarded = *Requested;
    std::vector<const char*> Kept;
    bool HasAndroidSurface = false;
    for (uint32_t i = 0; i < Requested->enabledExtensionCount; ++i) {
      if (std::strcmp(Requested->ppEnabledExtensionNames[i], VK_EXT_HEADLESS_SURFACE_EXTENSION_NAME) != 0) {
        Kept.push_back(Requested->ppEnabledExtensionNames[i]);
      }
      HasAndroidSurface = HasAndroidSurface ||
                          std::strcmp(Requested->ppEnabledExtensionNames[i], VK_KHR_ANDROID_SURFACE_EXTENSION_NAME) == 0;
    }
    // and the mirror of the removal above: under android WSI the surface the guest asked for by
    // one name is created by another, so the instance needs the extension that other name belongs
    // to. the guest never asked for it and never learns it is there -- it is ours, in exactly the
    // way VK_EXT_headless_surface was the other way round.
    if (UseAndroidWsi() && !HasAndroidSurface) {
      Kept.push_back(VK_KHR_ANDROID_SURFACE_EXTENSION_NAME);
    }
    Forwarded.enabledExtensionCount = static_cast<uint32_t>(Kept.size());
    Forwarded.ppEnabledExtensionNames = Kept.empty() ? nullptr : Kept.data();
    const auto Result = static_cast<VkResult>(Fn(&Forwarded, nullptr, Out));
    if (Result == VK_SUCCESS) {
      GuestInstance = *Out;
    }
    return static_cast<uint64_t>(Result);
  }
  case Id_vkEnumerateDeviceExtensionProperties: {
    if (reinterpret_cast<const char*>(Args->Argument[2])) {
      break;
    }
    auto Fn = Host<PFN_vkEnumerateDeviceExtensionProperties>("vkEnumerateDeviceExtensionProperties");
    if (!Fn) {
      break;
    }
    // the adreno driver already reports VK_KHR_swapchain, because on android the *loader*
    // implements it over the driver's private extension. it is still ours to answer, since the
    // swapchain the guest gets is ours -- so this only appends when the host has not said it.
    const auto Physical = reinterpret_cast<VkPhysicalDevice>(Args->Argument[1]);
    auto* Count = reinterpret_cast<uint32_t*>(Args->Argument[3]);
    auto* Out = reinterpret_cast<VkExtensionProperties*>(Args->Argument[4]);
    uint32_t HostCount = 0;
    Fn(Physical, nullptr, &HostCount, nullptr);
    std::vector<VkExtensionProperties> All(HostCount);
    Fn(Physical, nullptr, &HostCount, All.data());
    bool Present = false;
    for (const auto& Entry : All) {
      Present = Present || std::strcmp(Entry.extensionName, VK_KHR_SWAPCHAIN_EXTENSION_NAME) == 0;
    }
    if (!Present) {
      VkExtensionProperties Extra {};
      std::strncpy(Extra.extensionName, VK_KHR_SWAPCHAIN_EXTENSION_NAME, sizeof(Extra.extensionName) - 1);
      Extra.specVersion = 1;
      All.push_back(Extra);
    }
    const uint32_t Available = static_cast<uint32_t>(All.size());
    if (!Out) {
      *Count = Available;
      return VK_SUCCESS;
    }
    const uint32_t Take = *Count < Available ? *Count : Available;
    std::memcpy(Out, All.data(), Take * sizeof(VkExtensionProperties));
    *Count = Take;
    return Take < Available ? VK_INCOMPLETE : VK_SUCCESS;
  }
  case Id_vkCreateHeadlessSurfaceEXT: {
    auto* Out = reinterpret_cast<VkSurfaceKHR*>(Args->Argument[4]);
    if (UseAndroidWsi()) {
      // **the translation point, and the reason the fork needed no change for any of this.**
      // SharpEmu's AndroidHostWindow asks for VK_EXT_headless_surface because that is what was
      // true when it was written, and it is still exactly what it wants: "a surface, and do not
      // ask me for a window". it happens that we now have one.
      //
      // **and the surface made here is the handle the guest keeps for the rest of the run**, whatever
      // window it later stands for -- see CurrentSurface. a second surface is the guest's own
      // business and is passed through as the driver made it.
      std::unique_lock Held(SurfaceLock);
      RealSurface First {};
      const VkResult Result = MakeRealSurface(Held, First);
      std::printf("[vulkan] android surface created: %d\n", static_cast<int>(Result));
      if (Result == VK_SUCCESS) {
        *Out = First.Handle;
        if (!GuestSurface) {
          GuestSurface = First.Handle;
          RealSurfaces.assign(1, First);
        }
      }
      return static_cast<uint64_t>(Result);
    }
    // the invented one carries nothing: everything a surface would describe is a property of the
    // swapchain we make later, and there is only ever one of these.
    *Out = reinterpret_cast<VkSurfaceKHR>(&SurfaceToken);
    std::printf("[vulkan] headless surface created, %ux%u\n", SurfaceWidth, SurfaceHeight);
    return VK_SUCCESS;
  }
  case Id_vkDestroySurfaceKHR: return 0;
  case Id_vkGetPhysicalDeviceSurfaceSupportKHR:
    *reinterpret_cast<VkBool32*>(Args->Argument[4]) = VK_TRUE;
    return VK_SUCCESS;
  case Id_vkGetPhysicalDeviceSurfaceCapabilitiesKHR: {
    auto* Caps = reinterpret_cast<VkSurfaceCapabilitiesKHR*>(Args->Argument[3]);
    std::memset(Caps, 0, sizeof(*Caps));
    Caps->minImageCount = 2;
    Caps->maxImageCount = MaxSwapchainImages;
    Caps->currentExtent = {SurfaceWidth, SurfaceHeight};
    Caps->minImageExtent = Caps->currentExtent;
    Caps->maxImageExtent = Caps->currentExtent;
    Caps->maxImageArrayLayers = 1;
    Caps->currentTransform = VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR;
    Caps->supportedTransforms = VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR;
    Caps->supportedCompositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
    Caps->supportedUsageFlags = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT |
                                VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
    return VK_SUCCESS;
  }
  case Id_vkGetPhysicalDeviceSurfaceFormatsKHR: {
    static const VkSurfaceFormatKHR Offered[] = {
      {VK_FORMAT_B8G8R8A8_UNORM, VK_COLOR_SPACE_SRGB_NONLINEAR_KHR},
      {VK_FORMAT_R8G8B8A8_UNORM, VK_COLOR_SPACE_SRGB_NONLINEAR_KHR},
      {VK_FORMAT_B8G8R8A8_SRGB, VK_COLOR_SPACE_SRGB_NONLINEAR_KHR},
    };
    auto* Count = reinterpret_cast<uint32_t*>(Args->Argument[3]);
    auto* Out = reinterpret_cast<VkSurfaceFormatKHR*>(Args->Argument[4]);
    const uint32_t Available = static_cast<uint32_t>(std::size(Offered));
    if (!Out) {
      *Count = Available;
      return VK_SUCCESS;
    }
    const uint32_t Take = *Count < Available ? *Count : Available;
    std::memcpy(Out, Offered, Take * sizeof(Offered[0]));
    *Count = Take;
    return Take < Available ? VK_INCOMPLETE : VK_SUCCESS;
  }
  case Id_vkGetPhysicalDeviceSurfacePresentModesKHR: {
    // FIFO first, and it is the only one that is honest: present blocks until the guest's own
    // work is done, which is the whole point of implementing this rather than forwarding it.
    static const VkPresentModeKHR Offered[] = {VK_PRESENT_MODE_FIFO_KHR, VK_PRESENT_MODE_IMMEDIATE_KHR};
    auto* Count = reinterpret_cast<uint32_t*>(Args->Argument[3]);
    auto* Out = reinterpret_cast<VkPresentModeKHR*>(Args->Argument[4]);
    const uint32_t Available = static_cast<uint32_t>(std::size(Offered));
    if (!Out) {
      *Count = Available;
      return VK_SUCCESS;
    }
    const uint32_t Take = *Count < Available ? *Count : Available;
    std::memcpy(Out, Offered, Take * sizeof(Offered[0]));
    *Count = Take;
    return Take < Available ? VK_INCOMPLETE : VK_SUCCESS;
  }
  case Id_vkCreateSwapchainKHR: {
    std::lock_guard Held(WsiLock);
    return CreateSwapchain(reinterpret_cast<const VkSwapchainCreateInfoKHR*>(Args->Argument[2]),
                           reinterpret_cast<VkDevice>(Args->Argument[1]),
                           reinterpret_cast<VkSwapchainKHR*>(Args->Argument[4]));
  }
  case Id_vkGetSwapchainImagesKHR: {
    auto* Chain = reinterpret_cast<Swapchain*>(Args->Argument[2]);
    auto* Count = reinterpret_cast<uint32_t*>(Args->Argument[3]);
    auto* Out = reinterpret_cast<VkImage*>(Args->Argument[4]);
    if (!Out) {
      *Count = Chain->Count;
      return VK_SUCCESS;
    }
    const uint32_t Take = *Count < Chain->Count ? *Count : Chain->Count;
    std::memcpy(Out, Chain->Images, Take * sizeof(VkImage));
    *Count = Take;
    return Take < Chain->Count ? VK_INCOMPLETE : VK_SUCCESS;
  }
  case Id_vkAcquireNextImageKHR: {
    std::lock_guard Held(WsiLock);
    auto* Chain = reinterpret_cast<Swapchain*>(Args->Argument[2]);
    const uint32_t Index = Chain->Next++ % Chain->Count;
    *reinterpret_cast<uint32_t*>(Args->Argument[6]) = Index;
    // the image is always ready, but the guest is entitled to be told so through the semaphore
    // and fence it passed, and an empty submit is the only way to signal either from outside
    // the guest's own submissions.
    const auto Semaphore = reinterpret_cast<VkSemaphore>(Args->Argument[4]);
    const auto Fence = reinterpret_cast<VkFence>(Args->Argument[5]);
    if (Semaphore || Fence) {
      return SignalEmpty(Chain->Device, Semaphore, Fence);
    }
    return VK_SUCCESS;
  }
  case Id_vkQueuePresentKHR: {
    return Present(reinterpret_cast<const VkPresentInfoKHR*>(Args->Argument[2]));
  }
  case Id_vkDestroySwapchainKHR: {
    std::lock_guard Held(WsiLock);
    delete reinterpret_cast<Swapchain*>(Args->Argument[2]);
    return 0;
  }
  default: break;
  }

  // --- the one thing a forwarded WSI call still needs from us -------------------------------
  //
  // **the guest is a desktop-shaped client and android surfaces rotate.** the Odin 3's panel is
  // natively portrait, so a landscape surface reports currentTransform = ROTATE_90, and
  // VulkanVideoPresenter passes `PreTransform = capabilities.CurrentTransform` -- which on every
  // desktop is IDENTITY and a no-op, and here is a *promise* that the client has already rotated
  // its own content. it has not. the first frames through the real swapchain came out on their
  // side, exactly as that promise being false predicts.
  //
  // so the promise is withdrawn here rather than in the fork: `VulkanVideoPresenter.cs` is
  // upstream and this is one line inside a shared code path, which is the shape of edit the fork
  // exists to avoid -- and more to the point, which client pre-rotates is a property of the window
  // system integration, and the window system integration is this file. the capabilities the guest
  // reads stay honest; only the promise changes.
  VkSwapchainCreateInfoKHR SwapchainOverride {};
  if (Id == Id_vkCreateSwapchainKHR) {
    const auto* Requested = reinterpret_cast<const VkSwapchainCreateInfoKHR*>(Args->Argument[2]);
    if (Requested && (Requested->preTransform != VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR || UseAndroidWsi())) {
      SwapchainOverride = *Requested;
      Args->Argument[2] = reinterpret_cast<uint64_t>(&SwapchainOverride);
    }
    if (Requested && Requested->preTransform != VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR) {
      SwapchainOverride.preTransform = VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR;
      if (!TransformOverridden) {
        std::printf("[vulkan] swapchain preTransform 0x%X -> identity: the guest does not pre-rotate\n",
                    Requested->preTransform);
      }
      TransformOverridden = true;
    }
  }

  // the guest's surface handed down as the one on the current window -- see CurrentSurface. the
  // surface the guest named is taken before anything is rewritten, so that a retry translates that
  // again rather than the real one the first attempt substituted.
  if (UseAndroidWsi() && Id == Id_vkDestroySurfaceKHR &&
      DestroyGuestSurface(reinterpret_cast<VkSurfaceKHR>(Args->Argument[2]))) {
    return 0;
  }
  VkSurfaceKHR Asked = VK_NULL_HANDLE;
  const unsigned SurfaceArg = UseAndroidWsi() ? SurfaceArgument(Id) : 0;
  VkPhysicalDeviceSurfaceInfo2KHR SurfaceInfo {};
  if (SurfaceArg) {
    Asked = reinterpret_cast<VkSurfaceKHR>(Args->Argument[SurfaceArg]);
  } else if (UseAndroidWsi() && (Id == Id_vkGetPhysicalDeviceSurfaceCapabilities2KHR ||
                                 Id == Id_vkGetPhysicalDeviceSurfaceFormats2KHR)) {
    if (const auto* Requested = reinterpret_cast<const VkPhysicalDeviceSurfaceInfo2KHR*>(Args->Argument[2])) {
      SurfaceInfo = *Requested;
      Asked = SurfaceInfo.surface;
      Args->Argument[2] = reinterpret_cast<uint64_t>(&SurfaceInfo);
    }
  } else if (UseAndroidWsi() && Id == Id_vkCreateSwapchainKHR && Args->Argument[2]) {
    Asked = SwapchainOverride.surface;
  }
  // read before the call, which is what ends the handle's life.
  const uint64_t DestroyedSwapchain = Id == Id_vkDestroySwapchainKHR ? Args->Argument[2] : 0;

  void* Fn = Resolve(Id);
  if (!Fn) {
    Unresolved.fetch_add(1, std::memory_order_relaxed);
    if (LastUnresolvedName != Commands[Id].Name) {
      LastUnresolvedName = Commands[Id].Name;
      std::printf("[vulkan] no host implementation of %s\n", Commands[Id].Name);
    }
    return static_cast<uint64_t>(VK_ERROR_INITIALIZATION_FAILED);
  }

  // **one retry, for a surface lost between the guest asking and the driver answering.** the
  // second attempt waits for a window newer than the one the first was made on, so it is not the
  // same call twice.
  uint64_t Result;
  VkSurfaceKHR Real = VK_NULL_HANDLE;
  for (unsigned Attempt = 0;; ++Attempt) {
    if (Asked) {
      bool Ours = false;
      Real = CurrentSurface(Asked, Ours);
      if (!Ours) {
        Real = VK_NULL_HANDLE;
      } else if (SurfaceArg) {
        Args->Argument[SurfaceArg] = reinterpret_cast<uint64_t>(Real);
      } else if (Id == Id_vkCreateSwapchainKHR) {
        SwapchainOverride.surface = Real;
      } else {
        SurfaceInfo.surface = Real;
      }
    }

    ArgReader Reader(&Args->Argument[1], Frame->State);

    // the host call, optionally timed. `--trace-vulkan` says which commands are *called*, which is
    // the wrong question for a stall: 95 ms across 15 draws is not spread over the calls, it is one
    // of them waiting. so the profile accumulates wall time per command id and the dump sorts by
    // it.
    //
    // one clock_gettime either side, only when asked for. off, this is the same two lines it always
    // was -- the branch is on a bool that never changes after start-up and predicts perfectly.
    if (ProfileEnabled) [[unlikely]] {
      const uint64_t Start = NowNanos();
      const uint64_t StartCpu = ThreadCpuNanos();
      Result = Commands[Id].Invoke(Fn, Reader);
      const uint64_t Elapsed = NowNanos() - Start;
      Profile[Id].CpuNanos.fetch_add(ThreadCpuNanos() - StartCpu, std::memory_order_relaxed);
      Profile[Id].Nanos.fetch_add(Elapsed, std::memory_order_relaxed);
      Profile[Id].Calls.fetch_add(1, std::memory_order_relaxed);
      // no fetch_max before C++26, so a CAS loop. it spins only when a call genuinely sets a new
      // worst time, which by construction happens a handful of times per interval.
      uint64_t Worst = Profile[Id].MaxNanos.load(std::memory_order_relaxed);
      while (Elapsed > Worst &&
             !Profile[Id].MaxNanos.compare_exchange_weak(Worst, Elapsed, std::memory_order_relaxed)) {
      }
    } else {
      Result = Commands[Id].Invoke(Fn, Reader);
    }

    if (!Real || Attempt > 0 || static_cast<VkResult>(static_cast<int32_t>(Result)) != VK_ERROR_SURFACE_LOST_KHR) {
      break;
    }
    SurfaceWasLost(Real);
  }

  // what the swapchains were made on, so that a lost one is reported as out of date and a replaced
  // surface is destroyed once nothing is made on it any more.
  if (UseAndroidWsi()) {
    const auto Status = static_cast<VkResult>(static_cast<int32_t>(Result));
    if (Id == Id_vkCreateSwapchainKHR && Status == VK_SUCCESS && Real && Args->Argument[4]) {
      NoteSwapchainMade(*reinterpret_cast<const uint64_t*>(Args->Argument[4]), Real);
    } else if (Id == Id_vkDestroySwapchainKHR) {
      NoteSwapchainGone(DestroyedSwapchain);
    } else if (Id == Id_vkAcquireNextImageKHR && Status == VK_ERROR_SURFACE_LOST_KHR) {
      if (const VkSurfaceKHR Lost = SurfaceOfSwapchain(Args->Argument[2])) {
        SurfaceWasLost(Lost);
        Result = static_cast<uint64_t>(VK_ERROR_OUT_OF_DATE_KHR);
      }
    } else if (Id == Id_vkQueuePresentKHR) {
      Result = ReportLostAsOutOfDate(reinterpret_cast<const VkPresentInfoKHR*>(Args->Argument[2]), Result);
    }
  }

  // GPU turnaround, gathered from the three commands that know about it. vkQueueSubmit's fence is
  // its last argument; vkGetFenceStatus and vkWaitForFences report completion. anything the guest
  // never asks about is simply never resolved, and the map drops it at the next reset.
  if (ProfileEnabled) {
    const auto Status = static_cast<VkResult>(static_cast<int32_t>(Result));
    if (Id == Id_vkQueueSubmit && Status == VK_SUCCESS) {
      NoteFenceSubmitted(Args->Argument[4]);
    } else if (Id == Id_vkGetFenceStatus && Status == VK_SUCCESS) {
      NoteFenceSignalled(Args->Argument[2]);
    } else if (Id == Id_vkWaitForFences && Status == VK_SUCCESS) {
      const uint32_t Count = static_cast<uint32_t>(Args->Argument[2]);
      const auto* Fences = reinterpret_cast<const VkFence*>(Args->Argument[3]);
      for (uint32_t i = 0; Fences && i < Count; ++i) {
        NoteFenceSignalled(reinterpret_cast<uint64_t>(Fences[i]));
      }
    }
  }

  // **how batchable the render passes are.** SharpEmu begins and ends a render pass per draw, and
  // whether merging them is worth anything depends entirely on how often consecutive passes target
  // the *same* framebuffer. a long run of identical framebuffers is an opportunity; alternating
  // ones are not, and no amount of batching would help.
  //
  // measured here rather than reasoned about, because it decides whether a delicate change to
  // upstream synchronisation is worth making at all.
  if (ProfileEnabled && Id == Id_vkCmdBeginRenderPass) {
    static uint64_t LastFramebuffer = 0;
    static uint64_t RunLength = 0;
    static uint64_t Passes = 0;
    static uint64_t Runs = 0;
    static uint64_t LongestRun = 0;
    const auto* Info = reinterpret_cast<const VkRenderPassBeginInfo*>(Args->Argument[2]);
    if (Info) {
      const uint64_t Framebuffer = reinterpret_cast<uint64_t>(Info->framebuffer);
      ++Passes;
      if (Framebuffer == LastFramebuffer) {
        ++RunLength;
      } else {
        if (RunLength > LongestRun) {
          LongestRun = RunLength;
        }
        LastFramebuffer = Framebuffer;
        RunLength = 1;
        ++Runs;
      }
      if (Passes % 20000 == 0) {
        std::printf("[vkprof] render passes %llu in %llu framebuffer runs: mean run %.2f, longest %llu\n",
                    static_cast<unsigned long long>(Passes), static_cast<unsigned long long>(Runs),
                    static_cast<double>(Passes) / static_cast<double>(Runs ? Runs : 1),
                    static_cast<unsigned long long>(LongestRun));
      }
    }
  }

  // and what the allocation is *for*. a size on its own says how big the per-frame allocation is
  // and not which resource it belongs to, and the guest is managed code whose call site the host
  // cannot see -- but an image created immediately before an allocation of its own size identifies
  // itself by geometry. printed on the same schedule as the allocations below so the two lines sit
  // together in the log.
  if (ProfileEnabled && Id == Id_vkCreateImage) {
    static std::atomic<uint64_t> SeenImages {};
    const uint64_t Index = SeenImages.fetch_add(1, std::memory_order_relaxed);
    if (Index < 8 || Index % 300 == 0) {
      // Argument[N+1] is function argument N, so pCreateInfo is [2] and not [1] -- [1] is the
      // VkDevice, which prints as a plausible-looking extent and nonsense everywhere else.
      const auto* Info = reinterpret_cast<const VkImageCreateInfo*>(Args->Argument[2]);
      if (Info) {
        std::printf("[vkprof] vkCreateImage #%llu: %ux%ux%u format %d usage 0x%X tiling %d mips %u layers %u\n",
                    static_cast<unsigned long long>(Index), Info->extent.width, Info->extent.height,
                    Info->extent.depth, static_cast<int>(Info->format), Info->usage,
                    static_cast<int>(Info->tiling), Info->mipLevels, Info->arrayLayers);
      }
    }
  }

  // **the per-frame allocation, named.** the profile shows one vkAllocateMemory and one
  // vkFreeMemory per frame costing more than everything else put together, on *both* drivers. a
  // time without a size is not actionable, so under --vulkan-profile the first few are printed
  // with their size and memory type -- which is what says whether this is a large allocation or an
  // expensive small one.
  if (ProfileEnabled && Id == Id_vkAllocateMemory) {
    static std::atomic<uint64_t> Seen {};
    const uint64_t Index = Seen.fetch_add(1, std::memory_order_relaxed);
    if (Index < 8 || Index % 300 == 0) {
      const auto* Info = reinterpret_cast<const VkMemoryAllocateInfo*>(Args->Argument[2]);
      if (Info) {
        std::printf("[vkprof] vkAllocateMemory #%llu: %.2f MiB, memoryTypeIndex %u\n",
                    static_cast<unsigned long long>(Index), Info->allocationSize / 1048576.0,
                    Info->memoryTypeIndex);
      }
    }
  }

  // what the real surface actually offers, said once. under headless WSI this list is ours and
  // there is nothing to learn; under android it is the driver's, it is the thing the presenter
  // negotiates against, and "which formats exist" is the first question to ask when a picture
  // comes out with its channels in the wrong order.
  if (Id == Id_vkGetPhysicalDeviceSurfaceFormatsKHR && Args->Argument[4]) {
    static bool Said = false;
    if (!Said) {
      Said = true;
      const auto* Formats = reinterpret_cast<const VkSurfaceFormatKHR*>(Args->Argument[4]);
      const uint32_t Count = *reinterpret_cast<const uint32_t*>(Args->Argument[3]);
      for (uint32_t i = 0; i < Count; ++i) {
        std::printf("[vulkan] surface format %u: format=%d colorspace=%d\n", i, static_cast<int>(Formats[i].format),
                    static_cast<int>(Formats[i].colorSpace));
      }
    }
  }

  if (Id == Id_vkGetPhysicalDeviceSurfaceCapabilitiesKHR && Args->Argument[3]) {
    static bool Said = false;
    if (!Said) {
      Said = true;
      const auto* Caps = reinterpret_cast<const VkSurfaceCapabilitiesKHR*>(Args->Argument[3]);
      std::printf("[vulkan] surface caps: extent %ux%u images %u..%u currentTransform 0x%X supported 0x%X\n",
                  Caps->currentExtent.width, Caps->currentExtent.height, Caps->minImageCount, Caps->maxImageCount,
                  Caps->currentTransform, Caps->supportedTransforms);
    }
  }

  // under android WSI the present was just forwarded, so this is the only thing left that knows a
  // frame happened. counted whatever the result is: SUBOPTIMAL is a presented frame, and a run
  // that starts returning OUT_OF_DATE forever should show a frame counter that stops rather than
  // one that never started.
  if (Id == Id_vkQueuePresentKHR) {
    CountPresentedFrame();
    if (Result != VK_SUCCESS && Result != VK_SUBOPTIMAL_KHR) {
      static uint64_t Complaints = 0;
      if (Complaints++ < 8) {
        std::printf("[vulkan] present returned %d\n", static_cast<int>(Result));
      }
    }
  }

  // **and the other half of the transform decision.** forcing preTransform to identity is legal --
  // supportedTransforms says 0x1FF, identity included -- but the driver then reports every acquire
  // and present as VK_SUBOPTIMAL_KHR, because from its point of view the client could have saved
  // it a rotation and did not. a well-behaved client treats suboptimal as "recreate the
  // swapchain", so VulkanVideoPresenter did: **a new swapchain every 26 ms, forever, rendering
  // nothing.** that is the silent-swapchain failure exactly, reached by a completely different route, and it is
  // the second time this project has watched a swapchain recreate itself to death without one call
  // returning an error.
  //
  // suboptimal is not news here. *we* chose the compositor rotation, knowingly, on the guest's
  // behalf -- so the layer that made the trade absorbs the flag that reports it, and the guest is
  // told what is true for it: the frame was presented. a genuine OUT_OF_DATE still passes through
  // untouched, because that one really does mean the swapchain must go.
  if (TransformOverridden && Result == VK_SUBOPTIMAL_KHR &&
      (Id == Id_vkQueuePresentKHR || Id == Id_vkAcquireNextImageKHR)) {
    return VK_SUCCESS;
  }

  // an extension the guest asked for and the driver does not have. this is one of the few results
  // in vulkan that names a *set* without saying which member of it failed, and the guest cannot
  // find out either -- it gets one error code for a list it already believed. the thunk is the only
  // place holding both the request and the driver, so it is the only place that can answer, and
  // the answer is the whole diagnosis when a driver is swapped underneath a working client.
  //
  // note the sign. Result is the raw 64-bit return with a 32-bit VkResult zero-extended into it,
  // so every *error* code -- all of which are negative -- compares equal to nothing at all when
  // matched against the enum directly. VK_SUCCESS and VK_SUBOPTIMAL_KHR above get away with it
  // because they are non-negative.
  if (static_cast<VkResult>(static_cast<int32_t>(Result)) == VK_ERROR_EXTENSION_NOT_PRESENT &&
      Id == Id_vkCreateDevice) {
    const auto* Info = reinterpret_cast<const VkDeviceCreateInfo*>(Args->Argument[2]);
    auto Enumerate = Host<PFN_vkEnumerateDeviceExtensionProperties>("vkEnumerateDeviceExtensionProperties");
    auto* Physical = reinterpret_cast<VkPhysicalDevice>(Args->Argument[1]);
    if (Info && Enumerate && Physical) {
      uint32_t Count = 0;
      Enumerate(Physical, nullptr, &Count, nullptr);
      std::vector<VkExtensionProperties> Have(Count);
      Enumerate(Physical, nullptr, &Count, Have.data());
      std::printf("[vulkan] vkCreateDevice: the driver has %u device extensions and the guest asked for %u\n",
                  Count, Info->enabledExtensionCount);
      for (uint32_t i = 0; i < Info->enabledExtensionCount; ++i) {
        const char* Wanted = Info->ppEnabledExtensionNames[i];
        bool Found = false;
        for (const auto& Entry : Have) {
          Found = Found || std::strcmp(Entry.extensionName, Wanted) == 0;
        }
        if (!Found) {
          std::printf("[vulkan]   MISSING: %s\n", Wanted);
        }
      }
    }
  }

  // keep the handles the rest of the resolution path needs. reading them back out of the
  // guest's own out-parameters rather than tracking them separately keeps this to two lines.
  if (Result == VK_SUCCESS) {
    if (Id == Id_vkCreateInstance) {
      GuestInstance = *reinterpret_cast<VkInstance*>(Args->Argument[3]);
    } else if (Id == Id_vkCreateDevice) {
      GuestDevice = *reinterpret_cast<VkDevice*>(Args->Argument[4]);
      // the WSI half needs both of these and cannot ask for them later: allocating swapchain
      // memory needs the physical device's memory report, and signalling an acquire needs a
      // queue, which needs the family the guest chose.
      GuestPhysicalDevice = reinterpret_cast<VkPhysicalDevice>(Args->Argument[1]);
      const auto* Info = reinterpret_cast<const VkDeviceCreateInfo*>(Args->Argument[2]);
      if (Info && Info->queueCreateInfoCount) {
        GuestQueueFamily = Info->pQueueCreateInfos[0].queueFamilyIndex;
      }
      // and say who actually answered. the fallback above reports that an injection *failed*;
      // this reports which driver is really underneath, which is the same question asked from the
      // other end and the only one a performance number can be read against. one call, once.
      if (auto Properties = Host<PFN_vkGetPhysicalDeviceProperties>("vkGetPhysicalDeviceProperties")) {
        VkPhysicalDeviceProperties Device {};
        Properties(GuestPhysicalDevice, &Device);
        std::printf("[vulkan] driver: %s, vulkan %u.%u.%u, driverVersion 0x%08X\n", Device.deviceName,
                    VK_API_VERSION_MAJOR(Device.apiVersion), VK_API_VERSION_MINOR(Device.apiVersion),
                    VK_API_VERSION_PATCH(Device.apiVersion), Device.driverVersion);
      }
      // **the memory types, spelled out.** the guest picks a type by index and the index means
      // nothing across drivers -- the same allocation is type 6 on the stock driver and type 0 on
      // turnip. what matters is the flags behind it, because a mapped allocation that is HOST
      // VISIBLE but not HOST CACHED is uncached memory as far as the CPU is concerned, and every
      // guest store into it costs a bus transaction instead of a cache line. that time is *guest*
      // time, so it lands nowhere in a per-command profile.
      if (auto MemProperties = Host<PFN_vkGetPhysicalDeviceMemoryProperties>("vkGetPhysicalDeviceMemoryProperties")) {
        VkPhysicalDeviceMemoryProperties Memory {};
        MemProperties(GuestPhysicalDevice, &Memory);
        for (uint32_t i = 0; i < Memory.memoryTypeCount; ++i) {
          const VkMemoryPropertyFlags F = Memory.memoryTypes[i].propertyFlags;
          std::printf("[vulkan] memory type %2u: heap %u %.2f GiB  %s%s%s%s%s\n", i, Memory.memoryTypes[i].heapIndex,
                      Memory.memoryHeaps[Memory.memoryTypes[i].heapIndex].size / 1073741824.0,
                      (F & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT) ? "DEVICE_LOCAL " : "",
                      (F & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT) ? "HOST_VISIBLE " : "",
                      (F & VK_MEMORY_PROPERTY_HOST_COHERENT_BIT) ? "HOST_COHERENT " : "",
                      (F & VK_MEMORY_PROPERTY_HOST_CACHED_BIT) ? "HOST_CACHED " : "",
                      (F & VK_MEMORY_PROPERTY_LAZILY_ALLOCATED_BIT) ? "LAZILY_ALLOCATED " : "");
        }
      }
    }
  }
  return Result;
}

} // namespace HostLayer::VulkanThunk
