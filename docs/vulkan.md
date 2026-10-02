# the vulkan thunk

guest x86-64 vulkan onto the host's arm64 vulkan, in the same process and the same address space. 623 entry points, no socket, no protocol, no serialisation: a guest call arrives, and one host call later the real driver has it, on the same thread.

it rides in on the syscall boundary [`host-layer.md`](host-layer.md) describes, in a magic number range far above any real syscall. that document owns the mechanism and the two invariants; this one starts where a magic number has been recognised and owns everything outward from there — the two generated halves, argument reading, resolution, both window systems, and custom driver injection.

## what makes this small

**a guest pointer is a host pointer**, which is the first of `host-layer.md`'s invariants and the single biggest thing this design buys. there is **no pointer translation anywhere**: every `VkStructure` the guest fills in is read by the driver in place, every array is walked where it lies, and every out-parameter is written straight back into guest memory. that is why the whole marshaller is one template rather than a code generator per structure.

**vulkan passes nothing by value that is not a scalar.** every command takes handles, enums, integers, floats and pointers, and returns void or an integer. so SysV argument classification collapses to "SSE if floating point, INTEGER otherwise" with the return value always in RAX, and no aggregate ever has to be laid out. the reader static_asserts that, so a future header growing a by-value struct parameter is a compile error rather than silent corruption.

**and the arguments are all live when they are read.** FEXCore spills the entire guest state, GPRs and FPRs alike, before every syscall — `GPRSpillMask` and `FPRSpillMask` are both `~0U` at `DEF_OP(Syscall)` in FEXCore's branch ops — so at a trap `CPUState` is a complete and exact description of the guest's registers at the call. that is what lets one stub shape serve 623 entry points with different signatures: the *host* reads the arguments according to the ABI, so no stub ever has to know anything about its own prototype.

## the two halves

`scripts/gen-thunks.py` generates both from the NDK's `vulkan_core.h`, together, in one pass, from one list. neither is hand-editable, and they have to agree on a command id for every entry point:

| | |
| --- | --- |
| `generated/vulkan_commands.inc` | the host half. one `VKCMD(name)` per line, included several times with different definitions of `VKCMD` to build the id enum and the dispatch table |
| `generated/vulkan_stubs.S` | the guest half. one x86-64 stub per command, each 16 bytes, in the same order |

**the command list comes from the prototype declarations rather than from the `PFN_` typedefs.** `vulkan_core.h` has 633 `PFN_` typedefs and 623 commands; the difference is callbacks — `PFN_vkAllocationFunction`, `PFN_vkVoidFunction` and friends — which are not entry points at all.

### the stub

every stub is identical but for one immediate, and that is the entire point:

```
    movq %rcx, %r10           # the syscall instruction destroys RCX, which holds C argument 3
    movl $0x564B00nn, %eax    # "VK" << 16 | command id
    syscall
    ret
```

`0x564B0000` is the magic, `0xFFFF` is reserved for attach, and the top 16 bits are what make a thunk call unmistakable: real linux x86-64 syscall numbers are all below 1000, and this FEXCore has no table indexed by syscall number, so the whole upper range is free and an unrecognised number can never be mistaken for one of ours.

**the stubs are emitted 16 bytes apart in command-id order**, so the host resolves any guest entry point as `__sharpdroid_vk_stubs + 16 * id`. that is what lets `vkGetInstanceProcAddr` return an address the guest may actually call — see [below](#the-two-commands-that-return-an-address).

### attach

**only the guest knows where its dynamic linker put the stub table**, so the guest tells the host. `__sharpdroid_vk_attach` is registered in `.init_array` and runs as soon as the library is mapped; it passes the table's address through the reserved id.

the host **checks the table's shape rather than trusting it**: every entry is verified byte for byte against the four instructions above and against its own expected immediate. a silently misaligned table would send every call to the wrong entry point, which is the kind of failure that surfaces nowhere near its cause.

### the guest library

`scripts/build-thunks.py` assembles the stubs into an x86-64 `libvulkan.so.1`, staged into `guest-libs/x86_64/` alongside glibc rather than built into anything — it is what the guest's own ld.so finds on `LD_LIBRARY_PATH` when .NET asks for vulkan. a bare `libvulkan.so` is copied beside it, because Silk.NET asks for the versioned soname and anything probing by bare soname asks for the other.

**it links nothing at all** — no libc, no crt, no `DT_NEEDED` — which is why the NDK's x86-64 clang can build it even though that target is bionic and the guest set is glibc. what comes out is 623 stubs, an `.init_array` entry and a `DT_SONAME`, and glibc's loader has no opinion about any of that.

## reading the arguments

`host/src/thunk_abi.h`, shared with the audio thunk because there is exactly one right answer and two copies of it would be two chances to be wrong.

`ArgReader` walks the spilled state in ABI order: integer-class arguments out of the six registers the syscall handler already has, then off the guest stack. **the stub does not touch the stack**, so at the trap RSP still points at the return address the guest's call pushed and stack-passed arguments start one slot above it. floating-point arguments come out of the XMM file.

**which half of `CPUState`'s XMM union holds that file is asked, not assumed.** FEXCore picks the 32-byte-stride avx layout only when it has both AVX and SVE256; with AVX alone — which is this device, whose cores have no SVE256 — spills go to the 16-byte-stride sse layout instead. reading the wrong one is not a crash: it is every float argument after the first silently belonging to a different register. it is set once at start-up from what FEXCore reports, and it lives in the shared header because both thunks read the same register file and only one of them can own the answer.

`Marshal` deduces the parameter pack from a function pointer type, which is what removes any need for a per-command generator on the host side: the compiler already knows every signature, so the only thing that has to be generated is the *list*. **the argument tuple is brace-initialised on purpose** — plain function-argument evaluation order is unspecified in C++, and the initializer clauses of a braced-init-list are guaranteed left to right, so without it the reader would be handed the arguments in whatever order the compiler felt like and every call's registers would shuffle.

### what must not be passed through

**anything that would make the driver call back into guest code**, since a guest function pointer is x86-64 and the driver would enter it as arm64.

- **`VkAllocationCallbacks*` is forced to `nullptr`**, by a type-level specialisation of the reader's customisation point. it is consumed so the remaining arguments still line up, then dropped. vulkan makes the whole structure optional precisely so it can be ignored, and expressing the refusal at the type level covers all ~90 commands taking one without naming any of them.

### the two commands that return an address

`vkGetInstanceProcAddr` and `vkGetDeviceProcAddr` return the **guest** stub address, not the host function. this is load-bearing rather than tidy: SharpEmu's presenter casts what `vkGetDeviceProcAddr` returns straight to a delegate pointer and calls it.

the lookup answers in three ways, and the order matters:

| | |
| --- | --- |
| not a known command | **0**, which is what a real loader gives for an entry point it does not have, so the guest's own feature detection sees the truth |
| implemented by the thunk | the stub address, without asking the host whether it has the command — these are the ones the host has never heard of |
| anything else | the stub address **only if the host can resolve it**, so an entry point the driver lacks fails here, with a name attached, rather than at a call site with none |

## resolution

**per command id, once, then cached.** `dlsym` on the loader first, because the android loader exports the core entry points directly and that path skips its per-command dispatch trampoline; then `vkGetDeviceProcAddr` and `vkGetInstanceProcAddr`, which is how extensions are reached and is the same thing any vulkan client does.

the handles that path needs are kept as they go past, read back out of the guest's own out-parameters rather than tracked separately: the instance and device for proc-address resolution, and — for the invented window system — the physical device, whose memory report a swapchain allocation needs, and the queue family the guest chose, which signalling an acquire needs.

**a command the host cannot provide returns `VK_ERROR_INITIALIZATION_FAILED`, names itself once, and increments a counter reported at exit.** the name is printed only when it changes, so a command missing on every frame costs one line rather than a firehose.

**an unenabled thunk still answers.** the guest half is staged next to glibc and is found and loaded whether or not the thunk is switched on, so there is no version of "not staged" to fall back to — an unenabled thunk answers every call with a failure the guest can act on, rather than letting a magic number fall through to the syscall table as an unhandled number.

## the window system

two modes, and which one is in force decides how much of this file is reached at all.

| `--vulkan-wsi` | |
| --- | --- |
| `auto` | **the default.** android when there is a window, headless when there is not |
| `android` | a real `VK_KHR_android_surface` on the app's `ANativeWindow`, and a real swapchain from the platform loader. the driver composites the guest's own images and nothing is copied anywhere |
| `headless` | the thunk invents the surface and owns the swapchain, and a present is a copy into whatever the host wants. it is the only thing a binary with no window can do |

**the answer is latched the first time it is asked, not recomputed.** the proc-address gate consults it, `vkCreateInstance` decides an extension list from it, and the dispatch switch reads it on every call; an answer that changed halfway through would mean an instance created for one window system serving commands belonging to the other.

**which commands the thunk answers itself is decided in exactly one place**, because the dispatch switch and the proc-address gate must give the same answer. under android WSI that list is one command long: everything else is a genuine loader entry point that does the right thing, so those commands stop being ours and go back to being ordinary forwarded vulkan — with the guest's surface handle swapped for the real one on the way through, see below.

### the translation point

**the guest asks for `VK_EXT_headless_surface` in both modes**, and that is what keeps the fork's side of this honest vulkan rather than a bespoke entry point. it is a real extension meaning exactly "a surface, and do not ask me for a window", which is exactly what the guest wants; whether there happens to be a window behind it is the host's business.

so `vkCreateHeadlessSurfaceEXT` is **always** the thunk's, in both modes and for opposite reasons. with a window it creates a real `VK_KHR_android_surface`; without one it hands back a token, since everything a surface would describe is a property of the swapchain made later and there is only ever one of them.

the instance extension list is rewritten in both directions to match:

- **`VK_EXT_headless_surface` is removed** before the create info reaches the host loader, which has never heard of it and would reject the whole instance. advertising an extension and then handing it to someone who does not have it is the obvious failure of this design, so it is a special case rather than a note.
- **`VK_KHR_android_surface` is added** under android WSI. the guest never asked for it and never learns it is there — the surface it asked for by one name is created by another, and the instance needs the extension that other name belongs to.

`vkEnumerateInstanceExtensionProperties` appends the headless extension to whatever the host reported, in both halves of the two-call idiom so the count and the array agree. only the unlayered query is extended: a layer name means the guest is asking about something the thunk does not provide at all. `vkEnumerateDeviceExtensionProperties` appends `VK_KHR_swapchain` **only when the host has not already named it** — on android the loader implements it over the driver's private extension, so it is usually already there.

### a surface that outlives its window

**under android WSI the guest's surface is a real one, made on the app's window, and that window does not live as long as the guest.** android takes a `SurfaceView`'s surface away whenever the activity stops — home, the screen going off, anything opening over the game — because the view's surface goes with its window's, whatever lifecycle the view asks for; `SURFACE_LIFECYCLE_FOLLOWS_ATTACHMENT` changes nothing here. the driver then answers the next acquire with `VK_ERROR_SURFACE_LOST_KHR`, the guest's presenter treats that as fatal, and the run ends.

so **the handle the guest holds stays the one it was given, and what it stands for moves.** every forwarded command that names it — the four surface queries, the swapchain constructor, the `2KHR` queries' `pSurfaceInfo`, and the rest of the commands that take a `VkSurfaceKHR` — is handed the real surface on the window that is current now, made the first time anything asks after the window changed. **a lost surface is reported to the guest as `VK_ERROR_OUT_OF_DATE_KHR`** on acquire and present, which the presenter already answers by recreating its swapchain against the same handle, which by then means the new window. the forwarded-command list is unchanged; only their surface argument is rewritten.

| | |
| --- | --- |
| **the guest's handle is the first real surface** | kept until the guest destroys it, so a command that reaches the driver untranslated names a surface on a dead window and gets an error back, rather than naming a destroyed object |
| **a later surface is destroyed when it is both replaced and unused** | vulkan requires a surface to outlive its swapchains, so each swapchain is counted against the surface it was made on |
| **a window is identified by a count, not a pointer** | the count moves whenever the window does, through null included, so a window freed and a new one allocated at the same address cannot compare equal. the same window handed over twice — android repeats one whenever the view is laid out again — moves nothing, since a surface remade on a window the swapchain still owns would be refused as in use |
| **a surface lost on a window is never remade on it** | the driver reports the loss before the app is told the window went away, so the next surface waits for a newer window |
| **with no window, a surface query waits for one** | reached only by a guest that keeps rendering while nothing is on screen, since the app pauses the guest before its window goes — see [`app.md`](app.md). the alternative is an error, and every error a surface query can return is one the presenter dies on |

**the window swap is under the same lock as the surface's creation**, and the app releases the window it held only after the swap returns, so a guest thread making a surface on that window has finished with the pointer before it can be freed.

### rotation, and the flag it costs

**the guest is a desktop-shaped client and android surfaces rotate.** a panel that is natively portrait reports `currentTransform = ROTATE_90` for a landscape surface, and the presenter passes the surface's own `currentTransform` as its swapchain's `preTransform` — which on every desktop is identity and a no-op, and here is a *promise* that the client has already rotated its own content. it has not.

**so the promise is withdrawn here rather than in the fork.** the swapchain create info is copied, `preTransform` forced to identity, and the copy forwarded. this belongs in the thunk on two counts: the alternative is one line inside a shared upstream code path, which is the shape of edit the fork exists to avoid, and more to the point *which client pre-rotates* is a property of the window system integration — and the window system integration is this file. the capabilities the guest reads stay honest; only the promise changes.

**that forces the other half of the trade.** identity `preTransform` is legal, and the driver then reports every acquire and present as `VK_SUBOPTIMAL_KHR`, because from its point of view the client could have saved it a rotation and did not. a well-behaved client treats suboptimal as "recreate the swapchain", and doing that every frame renders nothing while returning no error anywhere.

so **suboptimal is swallowed on acquire and present, and only once the transform has actually been overridden**. the layer that made the trade absorbs the flag that reports it, and the guest is told what is true for it: the frame was presented. `VK_ERROR_OUT_OF_DATE_KHR` passes through untouched, because that one really does mean the swapchain must go.

the cost is that the compositor rotates every frame. on this class of GPU that is normally a hardware overlay; the alternative is for the guest to render pre-rotated, which needs a concept of display transform the fork does not have.

### formats

**android surfaces offer no `B8G8R8A8` at all** — the ones here report `R8G8B8A8_UNORM` and `R8G8B8A8_SRGB` across several colour spaces and nothing else. a client that looks for a BGRA format first and falls through to `formats[0]` therefore gets an RGBA swapchain here and a BGRA one on every desktop, so **anything doing a raw byte copy between two images it assumed were both BGRA will exchange red and blue**.

that is why the thunk prints the surface's format list and capabilities once under android WSI: "which formats exist" is the first question to ask when a picture comes out with its channels in the wrong order. under headless WSI the list is the thunk's own and there is nothing to learn from it.

### the invented swapchain

**everything in this section runs under `--vulkan-wsi headless` only.** it exists because `VK_KHR_surface` needs a real window on android and a binary run as the shell user cannot have one: `ANativeWindow` only exists once there is an app. so the thunk *is* the window system — it hands out a surface, owns the images, and turns `vkQueuePresentKHR` into whatever the host wants a present to mean.

it is selectable and covered by the regression set for two reasons: a shell binary still has no window, and a graphics regression under a real swapchain — or under a driver this was not proved against — needs something known-good to bisect against.

what it answers:

| | |
| --- | --- |
| surface support | always true |
| capabilities | 2..8 images, `currentExtent` fixed at the presentation size, identity transform only, opaque composite alpha |
| formats | `B8G8R8A8_UNORM`, `R8G8B8A8_UNORM`, `B8G8R8A8_SRGB`, all sRGB non-linear |
| present modes | **FIFO first**, and it is the only honest one: present blocks until the guest's own work is done, which is the whole point of implementing this rather than forwarding it |
| swapchain | device-local images created by hand, with `TRANSFER_SRC` added on top of whatever the guest asked for so present can read them back. the guest never sees that and cannot be broken by it |
| acquire | the images round-robin and are always ready — but the guest is entitled to be told so through the semaphore and fence it passed, and an empty submit is the only way to signal either from outside the guest's own submissions |

**present is where back-pressure comes from.** the guest's frame is not finished when it calls present, it is finished when the semaphores it is waiting on are signalled — so the thunk submits a wait on those semaphores with a fence and blocks on it. without that the render loop free-runs, with nothing anywhere to push back on it.

**the presentation size must match what the client believes its drawable is.** a client that finds the surface a different size from the drawable it asked for recreates its swapchain every frame, forever, without ever returning an error. so the size comes from one place: the window when there is one, `--vulkan-size` when there is not. **a window takes ownership of the answer** and `--vulkan-size` is refused from then on rather than silently contradicting the buffer frames land in.

### getting a frame back out

a full device-to-host copy into a linear image the CPU can read, behind its own lock — a single set of objects is reused every frame, and this is the hottest path in the project. it has two consumers and **both are headless-only**: under android WSI the swapchain images belong to the driver, there is nothing here to copy, and `adb shell screencap` answers the same question better.

| | |
| --- | --- |
| `--vulkan-dump <prefix>` | writes `<prefix>-NNNNN.ppm` on the first frame and every 300th. it costs a full copy and a stall on the frames it captures |
| the headless present | the same copy into an `ANativeWindow` buffer instead of a file. the destination is the only difference between the two |

the window copy swizzles as it goes, because the swapchain image is `B8G8R8A8_UNORM` and the window buffer is `RGBA_8888` — done as a word operation rather than four byte stores, since the loop runs two million times a frame at 1080p. it clamps to the buffer it was actually handed: the geometry was set from this window, but the compositor is entitled to hand back a buffer of its own choosing and a present that wrote past the end of one would be a crash a long way from its cause.

**`ANativeWindow_setBuffersGeometry` is called only on this path**, and at the point of the write. the headless present has to know the buffer's size and format because it is about to write into it by hand; under android WSI the driver configures that window itself, and pinning a format behind its back is a way to get a swapchain that disagrees with the buffers it is handed.

the window may be attached before the guest starts and taken away while it is running, so it is read once per present — a window that is null by the time the copy is reached is a frame to skip rather than a crash.

## the driver

**what `--vulkan-lib` names is a loader, and a driver is not a loader.** on android, WSI lives in the *loader*, not the driver, so pointing this at a driver `.so` would open something with no `vkCreateSwapchainKHR` in it at all. the loader has to stay the loader; what changes is which driver it loads underneath.

**that is what libadrenotools does, and it is not "load this `.so` instead".** it creates an isolated linker namespace, preloads a hook into it, and opens the *platform loader* inside that namespace, so that when the loader goes looking for a driver the hook answers with ours. WSI, the surface extensions and the ICD negotiation all stay the platform's.

two things must both be set or neither is used, and with neither set the library open is byte for byte the plain `dlopen` — which is what keeps the stock-driver baseline reproducible rather than merely equivalent:

| | |
| --- | --- |
| `--vulkan-driver <so>` | absolute path to the driver. it must live on **internal storage**: adrenotools stats it and then dlopens it, and external storage is mounted `noexec`, so the executable segment cannot be mapped off it — the loader says `couldn't map … segment 2: Operation not permitted` and the hook quietly falls back to the system driver |
| `--vulkan-hooks <dir>` | the app's `nativeLibraryDir`, and nothing else. `libmain_hook.so` and `libhook_impl.so` are loaded from there by soname into the isolated namespace, so a directory that merely contains copies of them is not the same thing |

**this only works inside an app process.** adrenotools drives the bionic linker's namespace API, and a shell binary has no classloader namespace to bypass — so the shell binary and the whole regression set stay on the platform loader whatever these are set to.

**everything checkable is checked before calling adrenotools**, because it returns a bare `nullptr` for about ten distinct reasons and `dlerror()` is only meaningful for the last of them. a missing hook is its own documented failure mode and a quiet one: the call still succeeds, the stock driver loads as a fallback, and the only symptom is that nothing got faster.

**and the fallback is loud.** a run that quietly reverted to the stock driver looks exactly like a successful injection that did not help, which is the one way this measurement could lie about itself. the injection says so and the fallback says so in as many words.

**a handle from adrenotools is not an answer, though, and that is the failure this has to catch.** what it returns is the platform loader opened inside an isolated namespace with a hook in front of the loader's own `dlopen`; whether the driver underneath is the chosen one is a separate decision the hook makes later, and a hook that cannot load it falls back to the system driver and returns a perfectly good handle. so the loud fallback above catches adrenotools refusing and catches nothing at all when adrenotools agrees and the hook does not.

**so the injection is checked against `/proc/self/maps`**, once, and a driver that is not mapped refuses the launch rather than rendering through a driver nobody chose. three things about that check are not obvious:

- **it cannot run where the injection is set up.** the loader binds an ICD on its first entry point rather than at `dlopen`, so nothing is mapped yet and a check there would condemn every driver ever loaded. the binding is forced with `vkEnumerateInstanceExtensionProperties`, a pure query the guest itself makes moments later.
- **the two spellings of the path are the whole check.** an app's data directory is reached as `/data/user/0/<package>` and the kernel reports mappings under `/data/data/<package>`, so matching the string the launch passed finds nothing at all on a driver that is mapped four times over. `realpath` closes that, and a `realpath` that fails answers *unknown* rather than *no* — the expensive direction here is condemning a driver that loaded.
- **the answer is asked for rather than announced.** `ChosenDriverLoads()` opens the host loader behind the same `std::call_once` the guest's first thunk call would have used, so asking early moves *when* the driver loads and not how often, and what is checked is the load the run will use rather than a rehearsal of it. that is what lets the app ask before it starts a guest at all. it answers true for anything short of a definite failure, including a run that named no driver.

**the device's own reported name is printed once at `vkCreateDevice` and does not answer this question.** it is worth having and it is not evidence: on this device turnip reports the same `deviceName` as the proprietary driver, so what separates them there is the API and driver versions rather than the name, and neither separates two turnip packages from each other. "is *the chosen package* loaded" is a different question and the mapping is what answers it.

### the driver's environment

`--vulkan-driver-env NAME=VALUE`, repeatable, applied before the driver is loaded because mesa reads its debug options once at initialisation and never looks again.

**this is not `--env`, and the difference is the whole reason it exists.** `--env` appends to the *guest's* environment, which is what CoreCLR and SharpEmu read. a custom driver is host arm64 code loaded by the host loader, so it reads the host process's environment and never sees a guest variable at all — `TU_DEBUG` and the rest of mesa's knobs are reachable from here and nowhere else.

each assignment is **read back through `getenv`** rather than trusting `setenv`'s return, because the question is not "did we call it" but "is it visible to whoever looks next". a sweep of values that all did nothing is indistinguishable from a sweep that never arrived.

### the memory types, spelled out

printed once at `vkCreateDevice`, and it is diagnosis rather than noise. **a memory type index means nothing across drivers** — the same allocation can be type 6 on one and type 0 on another. what matters is the flags behind it: a mapped allocation that is `HOST_VISIBLE` but not `HOST_CACHED` is uncached memory as far as the CPU is concerned, so every guest store into it costs a bus transaction instead of a cache line, and a staging buffer written in a swizzled pattern is the worst case for that.

**that time is guest time.** it is spent in guest x86-64 stores under FEXCore, not inside any vulkan call, so it appears nowhere in the per-command profile below and the CPU looks busy rather than blocked. a driver that offers a cached type and is simply never asked for one is indistinguishable from a slow driver until this table is read.

### turbo

`--vulkan-turbo` pins the GPU clocks through libadrenotools' KGSL power-control ioctl.

**it is not a driver feature and does not care which driver is loaded** — it is a kernel call, so it works on any of them alike, which is what makes it a fair lever to compare them with and what makes it reachable on builds whose mesa options are compiled out.

it is **re-asserted on a timer rather than set once**, because that is what Eden's turbo mode does — a thread that re-asserts for as long as submissions are recent — and taking that at face value is cheaper than assuming one call sticks.

**and clearing it is self-healing rather than optional.** the pinned state is a device-global property that outlives the process, and a run killed rather than exited never reaches the release. so a run that does *not* ask for turbo clears it on the way in, which means the next ordinary launch always puts the governor back however the last one died. it is off by default: a thermal and battery trade, not a free win.

## the profile

`--vulkan-profile`. **a trace says which commands are called, which cannot find a stall** — 95 ms spread across 15 draws is not spread at all, it is one call waiting. so the profile accumulates per command and the dump sorts by time, and the answer is usually the first line.

one `clock_gettime` pair either side of the host call, only when asked for; off, the branch is on a bool that never changes after start-up and predicts perfectly.

dumped every 300 presented frames, **as a delta since the last dump rather than a running total**. a total is dominated by start-up forever — shader compilation is seconds of `vkCreateGraphicsPipelines` that never repeat — and would bury a steady-state stall under it.

| per command | |
| --- | --- |
| wall time | where the time goes |
| CPU time | against the wall time, so the dump can say whether a command *burns* the frame or *waits* inside it. wall time alone cannot: a driver that sleeps in an ioctl and one that spins look identical, and only one of them is worth removing |
| worst single call | what separates a command that blocks from one that is merely called a lot. reset per interval, or one stall during shader compilation is the reported worst case forever |

the header line carries the interval's wall time beside the time spent inside vulkan, which is the line that matters most: **a stall inside a command and a stall the guest does to itself between commands look identical in a per-command table and have completely different causes.**

### GPU turnaround

the profile can say a fence check *waited* and cannot say what it waited for. two readings fit equally — the guest is idle and the wait is where idle lands, or the GPU genuinely takes longer — and at a frame budget the game sets rather than the GPU those are indistinguishable from wall time, while being opposite answers to "is this driver as fast".

so a submission's fence is noted when it is handed to the queue, and again when a fence check first reports it signalled: **submit-to-complete turnaround, without touching the guest and without a timestamp query.** it is an **upper bound**, because completion is only observed when the guest asks — which it does often enough for the granularity to be well under a millisecond. the minimum is reported beside the mean for the same reason: it is the sample least inflated by polling latency.

the map is bounded rather than trusted. submissions and completions balance in practice, but a fence the guest submits and never asks about again would sit there for the life of the run, and a diagnostic that leaks is worse than one that occasionally forgets.

### three things counted for a specific argument

each answers a question that decides whether some other change is worth making:

- **render pass runs.** whether merging consecutive render passes is worth anything depends entirely on how often consecutive passes target the *same* framebuffer. a long run of identical framebuffers is an opportunity; alternating ones are not, and no amount of batching would help
- **allocations, with their size and memory type.** a time without a size is not actionable, and the size is what says whether an expensive allocation is a large one or a small one being made often
- **image creations, with their geometry.** an allocation's size says how big it is and not what it is *for*; the guest is managed code whose call site the host cannot see, but an image created immediately before an allocation of its own size identifies itself by geometry

## the switches

everything is off unless it is asked for, so two runs differ by exactly the flags between them. **`--vulkan-lib` and `--vulkan-driver` each enable the thunk on their own**; the rest do not.

| | |
| --- | --- |
| `--vulkan` | enable the thunk. off by default, so that without it the guest's `dlopen` of `libvulkan.so.1` fails the way it does when the thunk is absent |
| `--vulkan-lib <so>` | the host vulkan to load. defaults to the platform loader, `libvulkan.so`, which is what gives WSI and the stock driver. **not where a custom driver goes** |
| `--vulkan-driver <so>` | a custom driver for the platform loader to load, injected with libadrenotools. needs `--vulkan-hooks` and an app process |
| `--vulkan-hooks <dir>` | the app's `nativeLibraryDir`, where the adrenotools hooks are found by soname |
| `--vulkan-driver-env NAME=VALUE` | the *driver's* environment, not the guest's. repeatable |
| `--vulkan-turbo` | pin the GPU clocks through KGSL for the life of the run |
| `--vulkan-wsi auto\|headless\|android` | which window system the guest gets |
| `--vulkan-size WxH` | the presentation size, when there is no window. refused once there is one |
| `--vulkan-dump <prefix>` | presented frames as PPMs. headless only |
| `--trace-vulkan` | every thunked call, with its first three arguments |
| `--vulkan-profile` | the profile above |

frames presented, calls thunked and calls unresolved are reported in the run summary [`host-layer.md`](host-layer.md) describes. **the frame count is also logged as it passes**, on the first frame and every 300th, and that is not redundant: the summary only prints on a clean exit, and a run ended by a timeout or a force-stop never reaches it. presents are counted in both window systems — under android WSI the present is forwarded, so the count is the only thing left that knows a frame happened, and it counts whatever the result is, since a suboptimal present is a presented frame and a run that starts failing forever should show a counter that stops rather than one that never started.

## what exercises it

`guests/` carries three x86-64 test guests that stand between the host layer and a real game, each proving something the one before it could not:

| | |
| --- | --- |
| `vulkan.c` | the thunk marshals arguments and enumeration works |
| `vkrender.c` | guest x86-64 code makes the GPU actually execute something, and the result is read back and checked |
| `vkswap.c` | acquire, render and present as a *loop*, with the semaphores and fences a real client uses |

they run as a shell binary with no window anywhere, so **the regression set exercises the invented window system rather than the real one**. `scripts/regression.py` runs them among the host layer's other modes; [`scripts.md`](scripts.md) has the commands.

`host/thunks/vulkan/host_vk_probe.c` sits underneath all of it — a host-side probe with no FEXCore and no guest, which answers what vulkan looks like from a bionic arm64 binary at all: whether the platform loader hands one a working instance and device, which extensions exist, and what the driver reports itself as. it links nothing and dlopens everything, which is also how the thunk reaches vulkan.
