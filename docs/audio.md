# the audio thunk

guest x86-64 AAudio onto the host's AAudio, in the same process and the same address space. 72 entry points, no socket, no protocol, no serialisation: a guest call arrives, and one host call later the real audio server has it, on the same thread.

it rides in on the syscall boundary [`host-layer.md`](host-layer.md) describes, in a magic number range of its own. that document owns the mechanism and the two invariants; this one starts where a magic number in the audio range has been recognised and owns everything outward from there — the two generated halves, the callback boundary, the write timeout, the stream table and the watchdog.

**it is the vulkan thunk with two orders of magnitude less surface**, and a reader who has read [`vulkan.md`](vulkan.md) should recognise every structural piece here. what differs is worth stating once, because each difference is what makes this the smaller of the two:

| | vulkan | audio |
| --- | --- | --- |
| entry points | 623 | **72**, of which about a dozen are used |
| attach call and `.init_array` | needed, so that a proc-address lookup can return a callable address | **none.** AAudio has no procedure-address API |
| a window system to invent | two of them | none. nothing is answered here instead of forwarded, except the refusals |
| from the java side | the `ANativeWindow` | **nothing at all** |
| refused at the boundary | `VkAllocationCallbacks*`, by type | three commands, by name |

that fourth row is the one worth sitting with. **AAudio is a pure NDK C API** — no JNI, no looper, and no permission, because `RECORD_AUDIO` gates input and this only ever plays. so the host layer opens an audio stream exactly the way it opens vulkan, and the activity never learns audio exists.

## why AAudio, and not a linux audio stack

the guest is linux x86-64 code that already knows how to find an audio backend, so the obvious cheaper answer is to stage the client libraries it wants into `guest-libs/` the way glibc is staged and let it find one. **that path is closed rather than expensive**, and the device says so twice.

**what SDL is willing to try is in the binary.** the staged `libSDL3.so` names its audio features and its driver list, and the list is complete rather than a guess: pipewire, pulseaudio, jack, alsa, `disk` and `dummy`. the last two are not output devices, so there are exactly three sound servers and one kernel interface to consider.

**and all four terminate at the same permission bit.** the three sound servers each need a daemon android does not run, and a daemon that did run would still have to open the kernel device. that device is `crw-rw---- system:audio`, and an app's uid is not in group `audio` — it is in a dozen other groups and not that one — so the open fails on the discretionary bit before SELinux is consulted at all. android routes application audio through AudioFlinger over binder, and ALSA is not an interface apps are given.

so staging `libasound.so.2` buys a library that fails at its first `open()`, and the other three buy libraries that fail to find a socket. the same reasoning applies to the fork's own `PosixAlsaAudioStream`, which already exists and ends at that same `open()`: *"wire up the ALSA backend that is already there"* is the same dead end in fewer lines.

## the two halves

`scripts/gen-thunks.py` generates both from the NDK's `aaudio/AAudio.h`, together, in one pass, from one list — the vulkan generator with a different regex, a different magic number and a different header. neither half is hand-editable, and they have to agree on a command id for every entry point:

| | |
| --- | --- |
| `generated/aaudio_commands.inc` | the host half's list. one `AACMD(name)` per line, included several times with different definitions of `AACMD` to build the id enum and the dispatch table |
| `generated/aaudio_protos.inc` | the host half's types. one `PFN_` typedef per command |
| `generated/aaudio_stubs.S` | the guest half. one x86-64 stub per command, each 16 bytes, in the same order |

the command list comes from the `AAUDIO_API` declarations, matched from the start of a line to the first semicolon across however many lines a prototype takes. nullability qualifiers are stripped because `_Nonnull` and `_Nullable` are clang extensions that mean nothing to a function pointer typedef.

### the typedefs are generated, and vulkan's are not

`vulkan_core.h` ships a `PFN_` typedef for every command, so the vulkan generator emits only the *list* and the host template deduces the rest. `AAudio.h` ships none.

**taking `decltype(&AAudioStream_release)` instead would drag in the header's availability attributes.** the host layer builds at API 28 and eleven of these entry points are `__INTRODUCED_IN(29..36)`, so referencing the declarations is a diagnostic waiting to happen and linking against them is impossible. a generated typedef carries the signature and never names the symbol — which is the same fact that makes the host half resolve everything with `dlsym` rather than `-laaudio`.

### the stub

every stub is identical but for one immediate, and that is the entire point:

```
    movq %rcx, %r10           # the syscall instruction destroys RCX, which holds C argument 3 —
                              # AAudioStream_write's timeout, as it happens
    movl $0x5341'00nn, %eax   # "SA" << 16 | command id
    syscall
    ret
```

`0x53410000` is the magic — one range along from vulkan's, deliberately distinct so the two stay decodable apart in a trace and in a crash. the top 16 bits are what make a thunk call unmistakable: real linux x86-64 syscall numbers are all below 1000, and this FEXCore has no table indexed by syscall number, so the whole upper range is free and an unrecognised number can never be mistaken for one of ours.

**the stubs are emitted 16 bytes apart in command-id order**, the same as vulkan's — not because anything indexes them, but because a disassembly of one is then a disassembly of all of them and an id is readable straight out of the immediate.

there is **no attach call and no `.init_array`**. vulkan needs the guest to hand over its stub table address so that a proc-address lookup can return something the guest may call; AAudio has no procedure-address API at all, so the guest's own ld.so resolving these names by symbol is the entire mechanism.

### the guest library

`scripts/build-thunks.py` assembles the stubs into an x86-64 `libaaudio.so`, staged into `guest-libs/x86_64/` alongside glibc rather than built into anything — it is what the guest's own ld.so finds on `LD_LIBRARY_PATH` when the payload's `DllImport("libaaudio.so")` is resolved.

**it links nothing at all** — no libc, no crt, no `DT_NEEDED` — which is why the NDK's x86-64 clang can build it even though that target is bionic and the guest set is glibc. what comes out is 72 stubs and a `DT_SONAME`, and glibc's loader has no opinion about either.

## reading the arguments

**shared with the vulkan thunk, and described there.** `host/src/thunk_abi.h` is both thunks' — `ArgReader` walking the spilled state in ABI order, the cut-down SysV classification, the brace-initialised argument tuple, and which half of `CPUState`'s XMM union the register file is in. [`vulkan.md`](vulkan.md) owns all of it under *"reading the arguments"*, and none of it is restated here.

what is audio's own is that **there is nothing to refuse at the type level**. vulkan expresses its one refusal as a rule over a parameter type because ~90 commands take one; the three parameters here that would carry a guest function pointer belong to three named commands, and naming three commands is cheaper than a rule. so every argument passes straight through, and the thunk instantiates the marshaller with the default pass-through reader.

## the boundary: three commands refused

`AAudioStreamBuilder_setDataCallback`, `setErrorCallback` and `setPresentationEndCallback` each take a guest function pointer for a **driver-owned host thread** to call. all three are refused, and that refusal is the documented boundary of the thunk.

**the thunk is one-way by construction.** every guest thread reaches the host through a syscall trap it made itself, and a host thread entering FEXCore to execute x86-64 code is a reverse path nothing in the host layer is built for. accepting a callback would produce a stream that opens cleanly and then crashes the moment the device asks for data, from an arm64 thread that jumped into x86-64.

**it costs nothing**, because AAudio's other feeding model is an ordinary write from the guest's own thread, and the fork's `IHostAudioStream.Submit` is already specified as *"may block briefly while the device drains its queue (this is what paces the guest's audio loop)"*. the interface the payload implements is already shaped for a caller that does its own waiting.

each refusal returns void, says so out loud the first three times, and increments a counter the run summary reports. **refusing silently would be the worse failure**: a builder that quietly ignored a callback would open a stream that never asks for data, which is indistinguishable from a stream that works until nothing is heard.

## the write timeout, clamped

`AAudioStream_write` and `AAudioStream_read` take a timeout, and whatever the guest asks for is **clamped to 20 ms** — comfortably over a burst period — in the argument array before the call is forwarded.

it is there for a reason that has nothing to do with audio. the host layer delivers asynchronous signals at syscall exits only, and CoreCLR suspends every managed thread with `SIGRTMIN` to collect, waiting for all of them. **a guest thread parked indefinitely inside a driver call cannot acknowledge a garbage collection suspension**, which is a way to stall the whole managed runtime from a place nothing would think to look.

so this is **a net rather than a knob**. a short write is a legal AAudio result that the caller retries, which is what the seam's contract already describes; an unbounded park is not something a payload gets to ask for. a payload that asks for a long timeout gets a short one and is told once.

the rewrite happens in the argument array rather than in the reader, because the reader is shared with the vulkan thunk and this is not an ABI question — it is a policy about how long a guest thread may be unreachable.

**the payload asks for none of it anyway.** the fork's stream passes a zero timeout and does its own bounded sleep between retries, so the thread stays managed and stays interruptible; the clamp is what holds when something else asks.

## resolution

**per command id, once, then cached**, by name against the device's real `libaaudio.so`.

the library is `dlopen`'d and `dlsym`'d rather than linked, and that is not a style choice: eleven of these entry points are not in the API 28 stub library at all, so `-laaudio` could not resolve them. resolving by name at run time gets everything the device actually has and turns everything it does not into a null that can be reported honestly.

what an incoming call answers with, in the order the checks happen:

| | |
| --- | --- |
| the thunk is not enabled | `AAUDIO_ERROR_UNAVAILABLE`, and one line, once, naming the flag that is missing |
| an id past the end of the table | `AAUDIO_ERROR_UNAVAILABLE` |
| one of the three callbacks | refused, above |
| not on this device | `AAUDIO_ERROR_UNIMPLEMENTED`, named once, counted. the name is printed only when it changes, so an entry point missing on every call costs one line rather than a firehose |
| resolved | forwarded |

**an unenabled thunk still answers**, for the reason [`vulkan.md`](vulkan.md) gives: the guest half is staged next to glibc and is loaded whether or not the thunk is switched on, so there is no version of "not staged" to fall back to. the payload's audio port then degrades to silent exactly as it does where no thunk exists.

## the stream table

every stream the guest opens is recorded, and it is measurement rather than bookkeeping.

**a stream that opens and plays nothing looks exactly like a stream that works.** every call succeeds, every buffer is accepted, and the only thing that differs is that the device never consumed anything. `AAudioStream_getFramesRead` climbing at the stream's own sample rate is the one thing that separates them, so it is measured rather than hoped for.

**what a stream actually opened with is read back and printed**, because AAudio negotiates: sample rate, channel count and format can all differ from what the builder asked for, and low-latency performance mode can change the burst size underneath. printing the negotiated values is the honest report and it costs three calls at open.

the table learns something in three places, all of them cheap because a guest pointer is a host pointer: `openStream` writes its handle into guest memory, which is host memory, so the handle is read back out of it; a write updates the counters; a close or release forgets the stream.

the periodic report rides on the write path, so it costs nothing on a run that never plays and arrives about once a second on one that does. one line per stream: frames the device has consumed against frames it should have consumed in the elapsed time, with frames written and the xrun count beside them. **anything far below 100% is a stream that is not really playing.**

two reports are not periodic:

- **the closing report happens before the call, not after.** once close or release returns the stream is gone, and asking a freed handle how many frames it played is a use-after-free rather than a diagnostic. it is also the only report a short run is guaranteed to produce
- **the run summary reports every live stream's frame count**, which is why a run that ends without ever closing a stream still says whether anything was heard

a write that fails is never silent either. a stream that has gone away answers every submission with a negative result, and without a complaint on that path the only symptom is a log that goes quiet. the first eight are printed with the result converted to text.

## pausing with the guest

**when the guest is paused, every stream it had playing is paused with it** — see [`host-layer.md`](host-layer.md) for the pause itself. the device then goes quiet at once rather than playing its buffer out into an underrun, and the audio hardware can go to standby for as long as the game is put away. on the way back each one is started again. a stream the guest had paused or stopped itself is left alone both times, and the host's hold is counted separately from the guest's, so the stream table's percentage is a share of the time the stream was meant to be playing.

**this happens on a thread of the thunk's own**, applying whichever state was asked for last, for two reasons. AAudio's answer can take seconds: a stream that has sat paused for a while can have been taken back by the audio server, and the request to start it again then waits on the server before reporting the stream disconnected — which needs nothing from here, since the guest's next write is refused and the emulator reopens the stream, exactly as it does for a headset unplugged mid-game. and one thread applying the latest state means a pause and a resume asked for in quick succession arrive in that order, rather than in whichever order two threads reached the stream lock; the wrong order is a game running over streams left paused.

**the watchdog says nothing while the guest is paused**, since a guest submitting nothing is then the point, and it counts the quiet from the moment of resuming rather than from the last write before the pause.

## the watchdog

**a host thread that notices the guest has stopped submitting.** it runs whenever audio does; `--audio-watchdog` only makes it chatty and adds one thread dump.

it exists because **the periodic report is blind to the one failure that actually happens**. that report rides on the write path, so no write means no report — and a log that simply goes quiet cannot distinguish three different bugs:

| | |
| --- | --- |
| the guest stopped calling | writes started equals writes finished: nothing is in flight |
| a write is stuck inside AAudio | started exceeds finished |
| the stream died underneath | the state and the frame counters say so |

so writes are counted on **both sides** of the host call, and the submitting thread's id is recorded with them, so that procfs can be asked what that thread is doing. a thread that has died takes its `/proc/self/task` entry with it, which separates *"the audio thread is gone"* from *"the audio thread is asleep somewhere else"* without a debugger this device does not offer.

**the loop is passive by default, and that is the whole design.** a version that polls the stream every second makes the stall stop happening — which is not a fix but an observer effect: AAudio's client drains the service's up-message queue inside its own calls, so a chatty watchdog does the draining the guest had stopped doing. **an instrument that cures the disease cannot measure it.** the loop therefore reads nothing but its own two counters, and only once the gap is real does it ask the stream a single round of questions, by which point perturbing it no longer matters.

**a recovery is reported as loudly as the stall.** a game that legitimately goes quiet — a menu, a silent scene, a stream it keeps open and stops feeding — would otherwise leave one stall line and nothing after it, which reads exactly like audio that never came back. one line each way makes a gap self-describing.

### the futex word

the thread state can say the audio thread is asleep in a futex rather than inside AAudio. it cannot say **whether anyone ever tried to wake it**, and those are opposite bugs: a producer that never signalled is upstream of this project entirely, while a signal that was published and not delivered is the host layer's own forwarding.

the futex word settles it, and it is readable from here for nothing. the address is a *guest* one, and guest and host share one address space 1:1, so a guest pointer is a host pointer. a condition variable parks its waiters on a word that a signaller bumps *before* calling `FUTEX_WAKE`:

| | |
| --- | --- |
| the word still reads what the sleeper slept on | nobody signalled. the producer is the bug, and it is not ours |
| the word has moved | a wake was published and lost on our side |

**the decisive case is the static one.** `FUTEX_WAIT_BITSET` sleeps only while the word equals what the caller passed, so a word that no longer equals it has already been changed by somebody — and by futex convention whoever changed it owed this thread a wake. nothing further needs to happen for that to be conclusive. sampling over two seconds is added on top of it for the live case, where a producer is still going round its loop.

the word is read through `process_vm_readv` rather than by dereferencing it, so an address that turns out not to be mapped comes back as `EFAULT` instead of killing the process being diagnosed.

the submitting thread's siblings — the threads sharing its name — are sampled either side of the same window and reported as moving or unmoved, which says which end of a producer/consumer pair to look at next.

### what the watchdog is for

**playback can stop permanently, part way into a run, silently.** the picture and the frame rate carry on and nothing returns an error.

**it is not an audio bug.** it is a cooperative-resume fault in the emulator, where a guest thread resumed from a blocking call arrives at an instruction it was not sent to; audio is where a rare mis-resume happens to have a visible, permanent and silent consequence. the audio server's own complaint about a client that went quiet is a consequence of the last write, not a cause of it.

so nothing in this file is the fix, and the watchdog is not a workaround: it is the instrument that reports every occurrence with enough state attached to say which layer to look at.

## the payload's side

the payload opens audio through AAudio when the launcher selects it with `SHARPEMU_HOST_AUDIO` — [`build-format.md`](build-format.md) owns that contract and the latency knob beside it. from this thunk's point of view the payload is an ordinary AAudio client, and that is deliberate: the guest library exports the same 72 names the NDK header declares, so the fork's android backend reads as ordinary android code and is reviewable by anyone who has written one.

what it asks for is 48 kHz, 2 channels, float32, shared, low latency, game usage — and float32 is exactly `AAUDIO_FORMAT_PCM_FLOAT`, so guest samples pass through without a conversion. three simultaneous ports play without underruns and at no measurable cost to the frame rate.

## the switches

everything is off unless it is asked for, so two runs differ by exactly the flags between them. **`--audio-lib` enables the thunk on its own**; the others do not.

| | |
| --- | --- |
| `--audio` | enable the thunk. off by default, so that without it the guest's AAudio calls fail the way they do where the thunk is absent and every measurement taken without audio still reproduces |
| `--audio-lib <so>` | the host AAudio to load. defaults to `libaaudio.so`, which the platform resolves to the device's own |
| `--trace-audio` | every thunked call, with its first three arguments |
| `--audio-watchdog` | make the watchdog chatty — a line a second whether or not anything is wrong — and dump every thread in the process at the first stall |

calls thunked, calls unresolved with the last name, callbacks refused and every live stream's frame count are reported in the run summary [`host-layer.md`](host-layer.md) describes.

## what exercises it

`guests/aaudio.c`, an x86-64 test guest that links against nothing but the generated `libaaudio.so`, so the whole chain is under test rather than only the marshaller: the guest's own ld.so finds the library, a PLT call lands in a 16-byte stub, and the host reads the arguments out of the spilled state and calls the real AAudio.

it is `-nostdlib` like every other guest here, which means no libm — so the 440 Hz tone comes out of a two-term recurrence rather than out of `sinf()`, a digital resonator stable to about 1e-10 over the few seconds it runs.

**what makes it a test rather than a demonstration is the pass condition.** a stream that opens and plays nothing succeeds at every call and accepts every buffer, so the check is that `AAudioStream_getFramesRead` advanced at the stream's own sample rate — against a clock **the guest reads itself**, because a check that used the host's clock would be checking the host against itself.

it also asks for an error callback, so the refusal path is exercised rather than merely written, and it plays in two phases with a pause between them long enough for the audio server to give up on a silent client — so going quiet and coming back is part of what passes.

`scripts/regression.py` runs it among the host layer's other modes, **with a negative control**: the same guest without `--audio`, which must fail, because that is what says the pass is the thunk working rather than something else providing audio. [`scripts.md`](scripts.md) has the commands.
