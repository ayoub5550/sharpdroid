# a native arm64 SharpEmu with FEXCore as a guest-only CPU backend

status: design and cost estimate; **step 0 built and measured on a phone** (2026-10-08, *step 0: measured* below). it is roadmap item #15 in [`performance-roadmap.md`](performance-roadmap.md). **[measured]**, **[source]** and **[estimate]** mean what they mean in that document: an estimate is a reason to measure, not a result.

## executive summary

- **today FEXCore translates the whole emulator.** that includes CoreCLR, its JIT and GC, every line of C# HLE, the Agc→Vulkan command processor, the shader recompiler, Silk.NET, and the x86 trampolines SharpEmu emits. the PS5 code is only one part of what gets translated. at boot, nearly all of what FEX compiles is the emulator: the phone reaches *Calling guest entry* in 2.4 s cold and 1.5 s warm before any PS5 instruction has run **[measured]**.
- **the alternative** is to build SharpEmu as a native arm64 .NET app (NativeAOT `linux-bionic-arm64`) and add a `FexCpuBackend` that uses FEXCore as a library to run **only** guest x86-64. this is the same split Arm64EC/`arm64ecfex` makes on Windows. it is feasible because of the property the host layer already depends on: **a guest pointer is a host pointer**. the C# HLE dereferences guest addresses directly (`DispatchImport` reads `*(ulong*)argPackPtr`), and with FEXCore in-process that stays true, so the ~111K lines in `SharpEmu.HLE` + `SharpEmu.Libs` (plus the 23K-line shader compiler) do not change.
- **FEXCore already exposes most of what is needed**: `CreateThread`/`ExecuteThread`, `SyscallHandler` (full guest state spilled, the handler may rewrite RIP), `HandleCallback` + `CALLBACKRET` for host→guest→host nesting, `QueryGuestExecutableRange`, `LookupExecutableFileSection` for the DiskCache, `InvalidateCodeBuffersCodeRange`, and `RestoreRIPFromHostPC`. the one recommended patch is a **non-clobbering "HLE call" opcode**. without it a proof of concept can use the `syscall`-with-a-magic-number boundary the Vulkan thunk already uses.
- **the expected gain is large on the emulator side and zero on the PS5 side** (all estimates): boot to guest entry **about 2.5–5× faster cold and 1.7–3× warm**; CPU-bound frames **about 1.1–1.4×**, because the guest code is still translated, TSO and AVX2 included; frames limited by SharpEmu's own render, command or shader-translation threads **about 1.7–2.5×**. it does not change the GTA 6 conclusion, which is set by the GPU and memory bandwidth.
- **cost**: about **26–40 engineer-weeks**, high risk, and a lasting fork from upstream's x64-only `DirectExecutionBackend`. **the proof of concept is about 2–3 weeks**. step 0 — the emulator side native with no guest — is done and measured (below): it took hours rather than the estimated 1–2 weeks, because the NativeAOT payload already existed.

## step 0: measured

**built** (fork branch `perf/android/arm64-step0`, patch `0005` in `patches/sharpemu/`): SharpEmu published with NativeAOT for `linux-bionic-arm64` — a 36 MB arm64 executable whose interpreter is `/system/bin/linker64` and whose only dependencies are bionic's `libc`, `libm`, `libdl`, `liblog` and `libz`. it runs with no host layer and no FEX, and does everything up to the first guest instruction: the export registry, the HLE warm-up, the NID catalog, guest memory, the loader, and the refusal of the fake eboot (rc 3, the same as every other payload). the changes it took are small: `HostPlatform` accepts linux-arm64 (its services are plain POSIX), `CpuDispatcher` refuses guest execution in a non-x64 process before any x86-64 stub is emitted (the null backend), the CLI warns instead of exiting, and the warm-up leaves the crypto initializers to first use (below).

```
dotnet publish src/SharpEmu.CLI/SharpEmu.CLI.csproj -c Release -r linux-bionic-arm64 \
  -p:SharpEmuHeadless=true -p:SharpEmuNativeAot=true -p:PublishAotUsingRuntimePack=true \
  -p:DisableUnsupportedError=true -p:CppCompilerAndLinker=$NDK_BIN/aarch64-linux-android30-clang \
  -p:ObjCopyName=$NDK_BIN/llvm-objcopy
```

**measured** — the same source built twice, linux-x64 NativeAOT under the host layer and FEX against linux-bionic-arm64 NativeAOT run directly; `SHARPEMU_BENCH=shader` times decode + SPIR-V translation of a synthetic Gen5 compute shader inside the process. Xiaomi 14 (Snapdragon 8 Gen 3, Android 15), Firebase Test Lab, 6 interleaved rounds:

| | x86-64 under FEX | native arm64 | ratio |
| --- | --- | --- | --- |
| boot to the refusal, cold | 0.87 s | **0.31 s** | **2.9×** |
| — the same, FEX with a warm DiskCache | 0.63 s | 0.31 s | 2.1× |
| — inside the process, `main` → refusal | 556 ms | 157 ms | 3.5× |
| — of that, registry + warm-up (`main` → HLE initialisers) | 534 ms | 74 ms | 7.2× |
| first translation of a 1,000-instruction shader (cold code) | 93 ms | 24 ms | **3.9×** |
| — FEX with a warm DiskCache | 41 ms | 24 ms | 1.7× |
| first translation of a 250-instruction shader | 26.4 ms | 6.5 ms | 4.1× |
| steady state, 250 instructions (median of 60) | 7.0 ms | 4.1 ms | 1.7× |
| steady state, 1,000 instructions (median of 30; 2.7 MB of SPIR-V each, allocation-bound) | 32.3 ms | 27.9 ms | 1.16× |

Test Lab's arm64 Android 15 emulator (`MediumPhone.arm`) agrees: boot 0.82 → 0.16 s, first shader 82–111 → 17–39 ms.

what it says: **the largest losses under FEX are cold code** — boot and the first run of any path, which is exactly when a game hitches (a new shader, a new menu, a new area). there native is 3–7× faster, and a warm DiskCache only closes part of it. **hot, compute-bound emulator code gains about 1.7×, allocation- and memory-bound code very little**, because FEX translates a hot loop well and memory is memory. that lands inside section 3's estimate (1.7–2.9× on the emulator side) with the shape it predicted, and it is now measured rather than estimated. it says nothing about PS5 code, which stays translated in this design.

**what step 0 found that the design had not:**
- **.NET crypto on Android.** the framework's crypto initializers load the OpenSSL shim, which finds the system's BoringSSL `libssl.so`, misses `a2d_ASN1_OBJECT` and **aborts the process** (a fail-fast, not an exception). the warm-up skips them on a non-x64 host; SharpEmu's own hashing (SHA-1 NIDs, SHA-256 SPIR-V digests, `RandomExports`) needs a bundled OpenSSL 3 or managed implementations before guest code can run natively.
- **the NID catalog becomes the critical path**: under FEX the warm-up hides its 154k-entry parse; natively the warm-up takes 74 ms and the boot then waits about 80 ms for the catalog. a precomputed hash table in the binary (or a lazy lookup) removes it.
- **the VM is the wrong instrument for this ratio.** under TCG the native build boots 2× faster than FEX (12.7 s against 25.6 s) but translates shaders *slower* (about 1.0 s against 0.57 s a shader) — qemu's cost per arm64 instruction is not a CPU's. the VM stays the correctness check; native-against-FEX ratios come from Test Lab.

## 1. how guest code runs today (`DirectExecutionBackend`, x64 only)

`HostPlatform.Create()` (`SharpEmu.HLE/Host/HostPlatform.cs`) throws unless `ProcessArchitecture == X64`. `CpuDispatcher` builds a `DirectExecutionBackend` (`CpuDispatcher.cs:293`). **guest code is executed natively, in place**, at the PS5's fixed addresses (`SelfLoader`: the main image at `0x8_0000_0000`) inside SharpEmu's own address space. the only translation is a set of x86-64 byte sequences the backend emits (about 495 emit sites in `Cpu/Native/*.cs`).

| boundary | mechanism today (file / function) |
| --- | --- |
| **entry** | `ExecuteEntry` / `ExecuteGuestThreadEntry` emit a stub. it pushes the host callee-saved GPRs and XMM6–15, stores host RSP in a slot, loads guest RSP/RBP/RDI/RSI/RDX/RCX and `call`s the entry. the guest returns to a sentinel / `CreateGuestReturnStub`, which fetches host RSP through a TLS slot and unwinds |
| **HLE imports** | every import gets a 16-byte slot (`SelfLoader.ImportStubSlotSize`) patched to `movabs rax, tramp; jmp rax` (`PatchImportStub`). `CreateImportHandlerTrampoline(idx)` pushes the 12 argument and callee-saved GPRs **onto the guest stack** as an "arg pack" (RDI first, return address at +0x60), and saves RAX, R10, R11, MXCSR, x87 CW and XMM0–7 below it. it then switches to the host stack (host RSP from `TlsGetValue(_hostRspSlotTlsIndex)`) and calls `ImportGatewayPtr`, a **Win64-ABI** pointer that `PosixHostStubs.CreateWin64ToSysVThunk` bridges to `ImportDispatchGatewayManaged(backend, importIndex, argPack)`. `DispatchImport` (`…Imports.cs:184`) copies the pack into `CpuContext`, runs the export (`SysAbiFunction(CpuContext)` → int), and the trampoline reloads RAX/XMM0/XMM1 and `ret`s to the guest. some NIDs bypass this: `TryCreateNativeImportIntrinsic` emits x86 directly (`rdtsc` for `sceKernelReadTsc`, and calls through **host** pointers to QueryPerformanceCounter/SwitchToThread/Sleep stubs), and `TryResolveDirectImportTarget` binds some libc exports to LLE guest code |
| **syscalls** | none from the guest. libkernel is HLE, so every OS service is an import. the backend has no `syscall` handling at all |
| **TLS / fs** | on Linux and Windows the host libc owns FS, so the guest's `mov reg, fs:[0]` is **patched at load** into a 9-byte `call` to a TLS handler (`PatchTlsPatterns`, `TryPatchTlsLoadInstruction`, `CreateTlsHandler`). the handler returns the per-host-thread guest TLS base from `TlsGetValue`. `BindTlsBase`/`SeedTlsLayout` fill in the PS5 TCB fields |
| **callbacks HLE→guest** | `IGuestThreadScheduler.TryCallGuestFunction` (`DirectExecutionBackend.cs:4102`) builds a fresh `CpuContext` on a cached callback stack and runs `ExecuteGuestThreadEntry` **nested on the same host thread**. it supports blocking inside the callback (`ResumeBlockedNestedGuestCallback`) |
| **guest threads** | `TryStartThread` → `GuestThreadState`, run on dedicated raw OS threads with an emitted native run loop (`…NativeWorker.cs`). this keeps CLR frames from interleaving with guest frames, which crashed CoreCLR's stack walker. blocking imports (`RequestCurrentThreadBlock`) unwind the guest to the scheduler and save a `GuestCpuContinuation` (all GPRs, RIP/RSP, FS/GS, MXCSR, FPU CW), which `ExecuteGuestContinuationEntry` resumes later. fibers and longjmp use the same continuation mechanism (`RequestCurrentContextTransfer`) |
| **faults / exceptions** | `…PosixSignals.cs`: a sigaction handler for SIGSEGV/SIGBUS/SIGILL rebuilds a **Win64 CONTEXT** from the x86 mcontext and runs the VEH chain in `…Exceptions.cs` (1,825 lines): unresolved-import sentinels, demand commit of lazily committed PRT pages (`TryHandleLazyCommittedPage`), BMI/ABM and SSE4a/MONITORX emulation on #UD (`…IllegalInstruction.cs`, `…Amd64Compat.cs`), guest-exception delivery (`TryRaiseGuestException`, used by IL2CPP stop-the-world). it writes the registers back and forwards anything else to .NET's handler |
| **memory** | `HostMemory` (Win32 semantics over mmap), `PhysicalVirtualMemory`, and `HostAddressSpace`, which already moves SharpEmu's high bookkeeping down for **39-bit VA** kernels (compact layout, 160 GiB floor) |

under the host layer **all of the above is itself x86 code that FEX translates**: the trampolines, the Win64→SysV thunk, the reverse P/Invoke, and the HLE body. an HLE call never leaves the JIT. it just runs about 100 translated x86 instructions plus the C# body at translated speed, with TSO on.

## 2. design: `FexCpuBackend`

### components

1. **`libsharpfex.so`** (C++, arm64, links FEXCore statically). a C ABI shim built mostly from code `host/src` already has: `fex_threads.cpp` (the thread factory), `guest_threads.cpp` (GDT, call-return shadow stack, `sigsetjmp` escape hatch, pause), `guest_signals.cpp` (the fault-handler order: SMC page → unaligned-atomic backpatch → deliver), `vma_tracker.cpp` (executable ranges + SMC invalidation across thread lookup caches), and `host_features.cpp` (CPU features, TSO rung). what goes away: the ELF loader, `linux_syscalls.cpp`, `/proc/self`, the guest file layer, and both thunks.
   ```
   sfx_init(cfg) / sfx_set_hle_handler(fn) / sfx_mark_exec(base,len,file_id) / sfx_invalidate(base,len)
   sfx_thread_new(const SfxState*) -> h / sfx_run(h) -> {Returned, Blocked, Exited, Fault}
   sfx_get_state(h, SfxState*) / sfx_set_state(h, const SfxState*) / sfx_call(h, rip, args…) -> rax
   ```
2. **`FexCpuBackend : INativeCpuBackend, IGuestThreadScheduler`** in `SharpEmu.Core/Cpu/Fex/`. `INativeCpuBackend` (one `TryExecute`) is thin. **the real contract is `IGuestThreadScheduler`'s 13 members** (`GuestThreadExecution.cs:40`) plus the statics in `GuestThreadExecution`. the scheduler, stall watchdog, import tables, LLE resolution and diagnostics should be moved out of `DirectExecutionBackend` into a shared base, so that only "enter, leave and resume guest" is backend-specific.
3. **`HostPlatform`** accepts arm64 when a FEX backend is present. `KernelRuntimeCompatExports` falls back from CPUID/rdtsc to `CNTFRQ_EL0`/`CNTVCT_EL0`. `PosixHostStubs` (`libc.so.6` → `libc.so`) is not needed at all on this path.

### guest entry
`sfx_thread_new` with RIP = entry, RSP = the guest stack from `CpuDispatcher`, FS = GS = the guest TLS base. `SharpEmu.Core` then calls `sfx_run` through a plain P/Invoke. while guest code runs, the thread is in preemptive mode with no managed frames above the P/Invoke, which is the property `NativeWorker.cs` currently gets by hand-writing a native run loop.

**TLS becomes free**: FEXCore keeps the FS base in `CPUState`, so `PatchTlsPatterns`, the TLS handler and the 9-byte call patches are switched off. that also removes their SMC writes and their risk inside short jumps.

### HLE import dispatch
the import slot becomes a trap instead of a jump to a trampoline:

- **PoC, no FEX patch**: `mov [rsp-8], rcx; mov eax, 0x5345nnnn; syscall; ret` (13 bytes, fits in the 16-byte slot). FEX's `SyscallOp` stores RCX = next RIP and R11 = RFLAGS, then spills **every** GPR and FPR into `CPUState` (`DEF_OP(Syscall)`, `GPRSpillMask = ~0`) and calls `SyscallHandler::HandleSyscall(Frame)`. the shim recognises the magic, reconstructs RCX from the red zone and calls the C# handler. RAX is already clobbered by today's `movabs rax` slot, so nothing is lost there. **R11 is lost**, and the trampoline preserves it today for the libSceFiber contract.
- **production, a FEX patch (~150 lines, carried in `host/fex-patches/` like `0001-…`)**: an `HLECALL imm32` opcode in the reserved `0F 3x` space beside `CALLBACKRET` (`0F 3E`) and `OP_THUNK` (`0F 3F`). it emits `SyscallOp`'s full spill and call but writes neither RCX nor R11, then ends the block. `OP_THUNK` itself is not suitable: it hands the host only RDI and pops RIP itself, so the host cannot redirect the guest.

the C# side does not need a new argument model. the shim writes the 12 GPRs to `guestRSP-0x60` in today's arg-pack layout, and RAX/R10/R11/MXCSR/XMM0–7 below that, which is exactly what the x86 trampoline pushes. `ImportDispatchGatewayManaged(backend, idx, argPack)` then runs **unchanged** as an `[UnmanagedCallersOnly]` function. on return the shim copies RAX, RDX, XMM0 and XMM1 back, and the stub's `ret` returns to the caller. 2609.1 ends the block at every syscall and resumes from `CPUState::rip`, which is the exact property the host layer's syscall step already relies on (`host-layer.md`).

**control-flow results** come back as an action code, because **C# must never `longjmp` and a managed exception must never cross FEX frames** (the gateway already catches everything). the C++ shim acts on the code once the managed frame has returned:
- *context transfer* (fibers, longjmp, `GuestCpuContinuation`): rewrite `CPUState` and return. the guest resumes at the new RIP.
- *block / yield*: `siglongjmp` to the thread's escape hatch in `sfx_run`, which returns `Blocked`. the FEX `InternalThreadState` **is** the continuation, so resuming is calling `sfx_run(h)` again. that is simpler than today's continuation entry stubs.
- *exit*: the same escape hatch, `Exited`.

### guest→HLE→guest re-entrancy
`TryCallGuestFunction` runs inside `HandleSyscall`. the backend saves the outer `CPUState`, sets RIP/RSP/args on the callback stack, pushes a return address that points at a guest-memory stub containing `CALLBACKRET` (`0F 3E`), and calls `Context::HandleCallback(thread, rip)`. FEX's thunk callbacks already work this way: the dispatcher runs until `CallbackReturn`. the outer state is then restored. **the hard case** is a callback that blocks, or that context-switches out (`ResumeBlockedNestedGuestCallback`). it needs one escape hatch per nesting depth, and possibly a separate `InternalThreadState` per callback. this is the main correctness risk in the backend.

### guest threads
one guest thread is one host thread, which is the host layer's own rule. `TryStartThread` creates a raw pthread in the shim (or a .NET `Thread`; with the guest in preemptive mode either works) that calls `sfx_thread_new` + `sfx_run`. the priority and affinity mapping (`MapGuestThreadAffinity`) is reused unchanged. this is also where roadmap item #6 becomes trivial: the emulator's own threads are real arm64 threads that can be placed on mid cores.

### signals and faults
the shim installs FEX's fault handler **after** the .NET runtime and **chains** to it. NativeAOT turns null dereferences into `SIGSEGV` and must keep receiving its own. in the JIT, host PC ≠ guest RIP, so the shim uses `RestoreRIPFromHostPC` + `ReconstructCompactedEFLAGS` / `ReconstructXMMRegisters`, as `guest_signals.cpp` does now. an adapter fills a **Win64 CONTEXT** from `CPUState`, so the `…Exceptions.cs` recovery chain is reused. the paths then split:
- lazy-commit demand paging needs no state change: commit the page and retry the host instruction.
- #UD for SSE4a/MONITORX arrives on FEX's "JIT-generated fault" path with RIP already exact (EXTRQ and INSERTQ have no `OpDispatch` in `SecondaryTables.cpp`). emulate it, write back through `CPUState`, and re-enter at RIP+len.
- guest-exception delivery to another thread uses the host layer's `SIGRTMAX` poke + safe-point design. CoreCLR uses `SIGRTMIN` for activation injection; NativeAOT needs a check.
- **BMI/ABM software emulation becomes dead code**, because FEX implements BMI1/2.

### memory, VA and SMC
- address space: still one shared space at 1:1 addresses. `HostAddressSpace`'s compact layout already handles 39-bit. FEX `VirtualMemSize` must match the measured ceiling, as the host layer sets it on phones today. **new risk**: the arm64 GC's region reservation must land clear of 32–36 GiB (the image), 128 GiB+ (direct memory) and the 208–240 GiB family. pin it with `GCRegionRange`/`GCHeapHardLimit` if it does not.
- executable ranges: `PROT_EXEC` never needs to reach the kernel, so guest pages stay RW. every producer of guest code (`SelfLoader`, PRT lazy commit, `sceKernelMapDirectMemory`/`mprotect` with EXEC) reports to `sfx_mark_exec`, which answers `QueryGuestExecutableRange`.
- **SMC**: `mtrack` on guest pages only. today's 7,517 invalidations and 3,031 SMC write faults per boot **[measured]** come almost entirely from CoreCLR's JIT and R2R fixups, so with no .NET code under FEX they should drop to whatever PS5 titles self-patch, which is rarely anything. `DOTNET_EnableWriteXorExecute=0` stops mattering.
- **code cache**: `LookupExecutableFileSection` returns `{eboot/sprx file id, section base}`, so FEX's DiskCache (FEX-2609 `CodeMap`) can cache **guest** code per title. PS5 modules load at fixed addresses, so cached code needs few relocations. roadmap #1 (prewarm) then applies to the game, not to .NET.

## 3. what becomes native, and the expected gain (all estimates)

| native arm64 after the change | still translated |
| --- | --- |
| .NET runtime (NativeAOT: no JIT at all), GC, all HLE (kernel, pthreads, fibers, audio, Agc command processing), the Gen5→SPIR-V shader recompiler, Silk.NET → **libvulkan/Turnip called directly**, AAudio, input | the PS5 eboot and sprx modules, LLE libc exports, guest callbacks; TSO and AVX2→NEON costs unchanged |

the reasoning: FEX runs integer code at about 35–60 % of native on phones **[source/estimate, gta6_research.md §4]**, and the emulator's C# also pays TSO on every load and store (the Intermediate rung). native .NET has neither cost. emulator-side code should therefore run **about 1.7–2.9× faster**; 2.0–2.5× is the working figure.

- **(a) boot to *Calling guest entry*** is all emulator. it is 2.4 s cold / 1.5 s warm on the phone today **[measured]**. the cold−warm gap (~0.9 s) is FEX compile time and disappears. the rest runs about 2× faster, and NativeAOT also removes the 24,076 startup-jitted methods **[measured count]**. estimate **0.4–0.9 s** both cold and warm: **2.5–5× cold, 1.7–3× warm**. the guest's own first-run translation still costs until the guest DiskCache is warm.
- **(b) CPU-bound, HLE-heavy frames on guest threads**: by Amdahl's law, with HLE share *h* of a guest thread's time and emulator speedup ×2.2, the frame speedup is 1/((1−h)+h/2.2). that gives **1.10× at h = 0.2, 1.37× at h = 0.5**. *h* has never been measured on a phone; `SHARPEMU_PERF_HLE=1` and `SHARPEMU_PROFILE_GUEST_RIP` can measure it now. **boundary cost**: every HLE call becomes a JIT exit (full spill/fill of ~16 GPRs + 16 vector registers, dispatcher re-entry, a lost block link on return, and a reverse-P/Invoke transition), roughly 60–150 ns natively. that is probably no worse than today's ~100 translated trampoline instructions plus the translated C# prologue, but **hot leaf imports** (strlen, memcpy, memset; see `TryDispatchHotMemoryLeaf`) should stay inside the translation as x86 or LLE code, or become FEX thunks to bionic's NEON routines.
- **(c) frames bound by SharpEmu's own render, command-processor or shader-translation threads**: those threads are 100 % emulator, so they get **~1.7–2.5×**. the per-call Vulkan thunk cost (a syscall exit and a marshaller per `vk*` call) also disappears. shader-compile stutter should shrink by the same factor. frames bound by the Adreno GPU or by memory bandwidth gain **nothing**.
- **memory**: no x86 glibc, no x86 CoreCLR, and no FEX code buffers for .NET code. that saves likely hundreds of MB, which matters against the 18–22 GB budget **[estimate]**.
- **GTA 6**: the gain is on the emulator side. the GTA 6 limits are the GPU (0.2–0.35×), bandwidth (0.15–0.19×) and translated guest CPU, so the 5–12 fps estimate in `gta6_research.md` moves very little.

## 4. effort and risk (one engineer)

| component | weeks | risk |
| --- | --- | --- |
| NativeAOT `linux-bionic-arm64` build of SharpEmu: static HLE registry instead of `ModuleManager`'s `Assembly.Load` (**shared with roadmap #2**), trim warnings, Silk.NET, FFmpeg.AutoGen arm64 libs | 3–5 | medium |
| `libsharpfex.so` shim (threads, escape hatch, exec ranges, SMC, faults, pause), mostly lifted from `host/src` | 4–6 | medium |
| FEX `HLECALL` opcode patch + test, rebased monthly with FEX | 1–2 | low–medium |
| `FexCpuBackend`: extract the shared scheduler from `DirectExecutionBackend`, imports via the arg-pack adapter, continuations, context transfer | 6–9 | high |
| nested callbacks that block / switch, guest exceptions, fibers | 3–5 | high |
| faults: CONTEXT adapter, lazy commit, SSE4a, runtime signal chaining | 2–3 | medium |
| Android: app loads `libsharpemu.so` (argv stays the interface), adrenotools handle to Silk.NET, AAudio/pad/SAF via P/Invoke | 3–4 | medium |
| stabilise on the tested titles (Demon's Souls boot, Dreaming Sarah, Void Terrarium, Dead Cells) | 4–6+ | high |
| **total** | **26–40** | |

### the smallest proof of concept (~2–3 weeks)
1. **step 0, no FEX (1–2 weeks, and the strongest single number)**: build `SharpEmu.CLI` for arm64 with a null CPU backend and run the existing `vm-boot-bench.sh` zeros-eboot. that bench stops before any guest instruction. compare against the 91.6 s VM baseline (spread 89.7–106.4 s, so only differences above ~10 % count) and against a NativeAOT x64 payload under FEX (lever C) to separate "AOT" from "native". the VM is bionic-only, so the build is either NativeAOT `linux-bionic-arm64` or CoreCLR `linux-arm64` with an arm64 glibc staged into the run share.
2. **PoC**: a NativeAOT arm64 console app (`FexPoc`) plus a minimal `libsharpfex.so`. it maps a hand-assembled x86-64 blob (built with NDK `clang -target x86_64`) at `0x8_0000_0000` and enters it with a guest stack and FS base. the guest calls an import slot with six integer args, a float in XMM0 and a pointer to a guest string. the C# handler prints the string **by dereferencing the guest pointer**, calls back into a guest function via `HandleCallback`, and returns a value in RAX that the guest checks. then the guest loops the call 10⁶ times and exits through the magic.
3. **measure in the VM (TCG)**: (i) ns per HLE round trip, native PoC vs the same loop in an x64 .NET process under the host layer (today's model); (ii) a C# compute microbench (the shader recompiler on a fixed corpus) x64-under-FEX vs arm64-native. TCG emulates both FEX output and native code as arm64, so **instruction-count ratios carry over but absolute times and TSO-barrier cost do not**. the phone has the final say on both.

## 5. what makes this harder than it looks

- **host function pointers inside emitted x86**: the TLS handler (`_tlsGetValueAddress`), the import trampolines (`ImportGatewayPtr` via the Win64→SysV thunk), the native intrinsics (`_queryPerformanceCounterAddress`, `_switchToThreadAddress`, `_sleepAddress`), the worker run loop's event stubs and `CreateExceptionHandlerTrampoline`. under FEX each would be an x86 `call` into arm64 code. every one has to become a trap, or move into C#. they are all in `Cpu/Native` and `HLE/Host/Posix`; game code never sees them. sceKernelDlsym-style returns are guest-memory stub addresses and stay valid.
- **memory ordering moves from TSO to arm64's weak model for the C# side.** today the HLE runs under TSO, inherited from x64 or from FEX's TSO emulation. natively, C# code that polls guest-written flags or rings (event flags, Agc labels and DCB rings, `WaitOnAddress`-style waits) with plain loads gets no ordering, while the translated guest still assumes TSO. the fork has 339 `Volatile`/`Interlocked` uses in `SharpEmu.Libs`, but plain-access assumptions are the classic x86-to-ARM porting bug, and they show up as rare hangs. this needs an audit.
- **the backend is 15.5K lines, the upstream is moving and x64-only**, and `INativeCpuBackend` hides almost nothing. without refactoring the scheduler into a shared layer, every upstream fix has to be ported twice.
- **NativeAOT on bionic** is not a mainstream .NET target, and this session did not verify it. CoreCLR-on-Android needs the Android workload or an APK host. the local NuGet cache has no bionic or arm64 runtime packs.
- **signal coexistence**: FEX, NativeAOT and the pause machinery each use SIGSEGV/SIGBUS and real-time signals. handler order and chaining are a correctness requirement, not a tuning choice.
- **what becomes unnecessary**: the Vulkan thunk (2,281 lines plus 623 generated stubs), the audio thunk, `linux_syscalls.cpp`, the guest file layer, `/proc/self`, the x86 glibc staging, and the `0001` SMC-full patch's main use case. that simplifies things, but it is a second product line next to today's working host layer, and both need to be kept until the native one passes the regression set.
- **unchanged costs**: AVX2 on 128-bit NEON, TSO on guest memory, and the GPU and bandwidth gap. this change makes the emulator fast; it does not make the PS5 code faster.
