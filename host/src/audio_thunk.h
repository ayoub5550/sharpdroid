// sharpdroid host layer -- guest x86-64 AAudio onto the host's AAudio, in-process.
//
// this is the vulkan thunk again with two orders of magnitude less surface: 72 entry points
// instead of 623, the same 16-byte guest stub shape, the same one-template marshaller, and the
// same reason it can be that small -- guest and host share one address space 1:1, so there is no
// pointer translation anywhere. AAudioStream_write reads the guest's PCM buffer in place, and
// AAudio_createStreamBuilder writes a host pointer into guest memory that the guest then carries
// around opaquely, exactly as it carries a VkInstance.
//
// the trap is a syscall with a magic number, "SA" in the top 16 bits -- one range along from
// vulkan's "VK", deliberately distinct so the two stay decodable apart in a trace and in a crash.
// the guest side is host/thunks/audio/generated/aaudio_stubs.S built into an x86-64 libaaudio.so staged
// in guest-libs/, where every entry point is the same 16 bytes but for one immediate:
//
//     movq %rcx, %r10           ; the syscall instruction destroys RCX, which holds C arg 3 --
//                               ; AAudioStream_write's timeout, as it happens
//     movl $0x5341'00nn, %eax   ; "SA" << 16 | command id
//     syscall
//     ret
//
// three things are different from vulkan, and all three make this smaller:
//
//   - **no attach call and no .init_array.** vulkan needed the guest to hand over its stub table
//     address so vkGetInstanceProcAddr could return an address the guest may call. AAudio has no
//     procedure-address API at all, so the guest's own ld.so resolving these names by symbol is
//     the entire mechanism.
//   - **no window system to invent.** nothing here is answered by the host layer instead of being
//     forwarded, except the refusals below.
//   - **nothing at all from the java side.** AAudio is a pure NDK C API -- no JNI, no looper, and
//     no permission, because RECORD_AUDIO gates input and this only ever plays. so the app never
//     learns audio exists.
//
// **the three callback setters are refused rather than forwarded**, and that refusal is the
// documented boundary of the thunk. setDataCallback, setErrorCallback and setPresentationEndCallback
// each take a guest function pointer that a driver-owned host thread would call -- which would mean
// a host thread entering FEXCore to execute x86-64 code, a reverse path the vulkan thunk never
// needed and nothing in the host layer is built for. every guest thread reaches the host through a
// syscall trap it made itself, and that stays true.
//
// it costs nothing, because AAudio's other feeding model is an ordinary write from the guest's own
// thread and the fork's IHostAudioStream.Submit is *already* documented as "may block briefly while
// the device drains its queue (this is what paces the guest's audio loop)".
//
// **but the guest must not ask AAudio to do that waiting, and that is the hardest-won line in this
// file.** parking the guest thread inside a blocking AAudioStream_write stopped playback dead
// partway into 29% of runs -- the thread stopped coming back, the audio server saw a client that had
// gone quiet, and the stream was suspended with nothing anywhere returning an error. the fork now
// passes a zero timeout and paces itself with a bounded sleep instead, which brings it to about 6%.
// that is a mitigation and not a cure: **the bug is open**, and its mechanism is not in the audio
// path at all.
//
// the host half clamps the timeout regardless, as a net rather than a knob: the host layer delivers
// asynchronous signals at syscall exits only, and CoreCLR suspends every thread with SIGRTMIN to
// collect, so a guest thread parked indefinitely in a write is one that cannot acknowledge a GC
// suspension. see MaxWriteTimeoutNanos in the implementation. a payload that asks for a long
// timeout gets a short one and is told once.

#pragma once

#include <FEXCore/Core/CoreState.h>
#include <FEXCore/HLE/SyscallHandler.h>

#include <cstdint>

namespace HostLayer::AudioThunk {

// "SA" in the top 16 bits. same reasoning as vulkan's: real linux x86-64 syscall numbers are all
// below 1000 and this FEX has no table indexed by syscall number, so the whole upper range is free
// and an unrecognised number can never be mistaken for one of ours.
inline constexpr uint64_t Magic = 0x53410000;
inline constexpr uint64_t MagicMask = 0xFFFF0000;

inline bool IsThunkCall(uint64_t SyscallNumber) {
  return (SyscallNumber & MagicMask) == Magic;
}

// enabled by --audio. off by default, in the shape --vulkan has: with the thunk unavailable the
// guest's AAudio calls fail the way they did before the thunk existed, so a run without the flag
// behaves exactly
// as every run to date and no earlier measurement stops being comparable.
void SetEnabled(bool Enabled);
bool Enabled();
void SetTrace(bool Enabled);

// the watchdog is a host thread that notices the guest has stopped submitting. **it runs whenever
// audio does**; this only makes it chatty, reporting every second instead of only on a stall.
//
// it exists because the periodic report on the write path is blind to the one failure that
// actually happens -- the guest stopping -- since no write means no report, and a log that simply
// goes quiet cannot tell "the guest stopped calling" from "our write is stuck inside AAudio" from
// "the stream died underneath". it counts writes on both sides of the host call and asks procfs
// about the submitting thread, which separates all three.
void SetWatchdog(bool Verbose);

// the host AAudio to load. defaults to "libaaudio.so", which the platform resolves to
// /system/lib64/libaaudio.so.
//
// it is dlopen'd and dlsym'd rather than linked, and that is not a style choice: the host layer
// builds at API 28 and eleven of these entry points are __INTRODUCED_IN(29..36), so they are not
// in the API 28 stub library and -laaudio could not resolve them. resolving by name at run time
// against the *device's* real libaaudio.so gets everything the device actually has, and turns
// everything it does not into a null the thunk can report honestly.
void SetLibraryPath(const char* Path);

// the dispatch entry, called from LinuxSyscallHandler::Dispatch for any magic number.
uint64_t Handle(FEXCore::Core::CpuStateFrame* Frame, FEXCore::HLE::SyscallArguments* Args);

// how far the device has actually played, sampled from the streams the guest opened.
//
// this is a guard rather than a nice-to-have: a stream that opens and plays nothing looks *exactly*
// like a stream that works. AAudioStream_getFramesRead climbing at the stream's sample rate is the
// only thing that distinguishes them. it is the same shape as a GPU driver package that fails to
// load and falls back to the platform's own -- every call succeeds and the measurement is of
// something else entirely.
void ReportStreams();

// the guest being paused and resumed. every stream it had playing is paused with it, so the device
// goes quiet at once instead of playing its buffer out into an underrun, and is started again on the
// way back; a stream the guest paused or stopped itself is left alone both times. the watchdog says
// nothing while the guest is paused, since a guest that submits nothing is then the point.
//
// it returns at once: the streams are paused and started on a thread of the thunk's own, because the
// audio server can take seconds to answer for a stream that has been paused a while.
void SetPaused(bool Paused);

// counters, for the run summary.
uint64_t CallCount();
uint64_t UnresolvedCount();
const char* LastUnresolved();
uint64_t RefusedCount();

} // namespace HostLayer::AudioThunk
