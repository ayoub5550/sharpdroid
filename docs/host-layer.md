# the host layer

what runs an x86-64 linux program inside an android app: the ELF loader, the address space it is laid out in, syscall dispatch onto bionic, a `/proc/self` that describes the guest, threads, signal delivery, and the VMA and self-modifying-code tracking FEXCore needs to stay correct.

it is a library, not a process. `HostLayer::RunMain(argc, argv)` in `host/src/host_layer.h` is the whole entry point, and two translation units sit on top of it and do nothing else: `entry_exe.cpp` is a `main()` for the shell binary, `entry_jni.cpp` is the JNI surface the app calls plus the `ANativeWindow` it hands down. **the argument vector is the interface in both cases** — the app passes the flags a shell would, so a measurement taken through one is comparable with the other, and a JIT problem can be bisected outside an app.

what it is *not* is a container, a rootfs or a second process. one process, one address space, one set of file descriptors. [`repo-structure.md`](repo-structure.md) says where all of this is built; [`build-format.md`](build-format.md) says what the guest payload has to be.

## the two invariants everything else rests on

**a guest pointer is a host pointer.** FEXCore does not translate guest addresses — it maps the 64-bit guest address space 1:1 onto the host's — so "loading into the guest address space" is `mmap`, a guest buffer handed to a syscall is passed straight through with no marshalling, and a guest fault arrives as a real host `SIGSEGV` from inside JIT-generated code. everything cheap about this design follows from that one fact, including both thunks.

**`PROT_EXEC` never reaches the host kernel.** FEX does not execute guest memory: it reads those bytes and emits arm64 into code buffers of its own. so `VMA::HostProt` takes the bit off every guest mapping before it reaches bionic, and nothing is lost. what is gained is that this works inside an app at all — SELinux denies `execute` on an app's own `app_data_file`, so a host layer that forwarded the bit honestly would run from a shell directory and fail the day it moved into an APK. a conventional dynamic linker mapping a library's text segment is exactly that case.

the guest cannot tell. it never reads its own protections back, `mprotect` reports success, and the VMA tracker remembers what was really asked for so the decoder is not fooled either. the visible consequence is that `/proc/self/maps` has no `x` column anywhere — see below, where that is the one place the honesty runs out.

## the address space

**userspace is 39-bit on this class of device.** an arm64 linux kernel built for a 39-bit VA gives a process **512 GiB**, not the 128 TiB an x86-64 desktop gives, and every layout in the stack has to fit inside it. the budget for the whole process:

| | |
| --- | --- |
| 0 .. 32 GiB | the host layer, FEXCore, .NET, and the guest program — must stay clear of the guest image window |
| 32 .. 36 GiB | the PS5 guest image and module search window |
| 128 GiB .. | guest direct and flexible memory, growing upward |
| 208 .. 240 GiB | SharpEmu's own high bookkeeping: import stubs, guest stacks, TLS, the allocation arena |
| ~458 .. 512 GiB | the android linker, the app's libraries and the process stack, descending from the mmap base |

the two ends of that are chosen by different code and by the same method.

**the payload's own layout is chosen at startup from the address space the host actually has.** `HostAddressSpace` in the fork reads the highest mapping out of `/proc/self/maps` — the kernel puts the process stack just under the ceiling, so that is the measurement — and where the classic 47-bit layout fits it is used unchanged, bit for bit, which is every desktop host. where it does not, the whole family shifts down as one block into the middle of whatever space there is, preserving every relative offset and every downward probe window. the result is rounded to 16 GiB so it does not wobble under ASLR, and floored at 160 GiB so it stays clear of the image window and of the address the kernel compatibility layer hands out mappings from.

**the host layer's own guest base is probed rather than declared.** an app process is not an empty process: ART is there first, with the dalvik heaps, two JIT code caches and the boot image spread through the bottom 2 GiB. so the loader tries a list of candidate bases by reserving and releasing with `MAP_FIXED_NOREPLACE` — asking the kernel the exact question it is about to ask for real, rather than parsing a map it might disagree with — and takes the first that answers. **zero is tried first and it is not a formality**: it always wins on a shell process, so the shell binary puts the guest at one fixed set of addresses that two runs can be diffed against each other. the rest are 4 GiB apart and all stop short of 32 GiB.

relative to whichever base is chosen:

| | |
| --- | --- |
| +512 MiB | the guest program, if it is `ET_DYN`. an `ET_EXEC` goes where its program headers say |
| +2 GiB | the program interpreter |
| the page past the image | the `brk` arena — 512 MiB reserved `PROT_NONE` up front |

the interpreter clears both the program image and the arena because **the arena cannot move**: `brk` must return memory contiguous with the image, and in an address space shared with FEXCore, .NET and bionic there is no guarantee the next page is still free by the time the guest asks for it. reserving it costs address space and no memory; `brk` hands pages out of the reservation by `mprotect` and gives them back the same way, and each of those re-records the range so the untouched tail of the heap stays unreadable rather than quietly executable.

the guest stack is 8 MiB, mapped wherever the kernel likes.

## the ELF loader

`host/src/elf_loader.{h,cpp}`. it accepts a little-endian 64-bit `EM_X86_64` image of type `ET_EXEC` or `ET_DYN`, and it maps by hand rather than with file-backed mappings.

**the whole image span is reserved in one `PROT_NONE` mapping and then filled in segment by segment.** two reasons, and both are the kind of bug that surfaces far from its cause: adjacent segments whose page-rounded ranges touch cannot collide, and the pages arrive zeroed, so `.bss` and the partial tail page of a file-backed segment need no special handling. the reservation is `MAP_FIXED_NOREPLACE`, so something of the host's already living at the guest's load address is an error rather than the silent destruction of a host mapping.

**protections are applied in a second pass**, after every segment's bytes are in place: a segment that ends up read-only must not be sealed while a later one sharing its tail page is still to be written. that pass is also where each segment is reported to the VMA tracker with `PF_X` intact — the `mprotect` beside it deliberately carries no `PROT_EXEC`, so the tracker's record is the only one that will ever exist, and without it the guest's own text is not executable as far as the decoder is concerned and nothing runs at all. the reservation is recorded `PROT_NONE` first, so whatever gap alignment leaves between two segments stays unreadable instead of inheriting a neighbour's executability.

**`PT_INTERP` is an absolute path into a filesystem layout android does not have.** anything built on a normal linux distribution names `/lib64/ld-linux-x86-64.so.2`. if that path is readable it is used unchanged; otherwise the basename is looked for in the directory the guest libraries are staged in, which is the same directory the guest is handed as `LD_LIBRARY_PATH`. keeping the redirect in one place is what stops path rewriting from spreading through the syscall layer. an interpreter with a `PT_INTERP` of its own is refused, as the kernel refuses it.

control goes to the interpreter's entry point when there is one. the program's own entry still reaches the guest as `AT_ENTRY`, because that is what ld.so jumps to when it has finished relocating.

### the initial stack

`BuildGuestStack` writes what the x86-64 kernel hands `_start`: `argc`, `argv`, `envp`, the auxiliary vector, and the strings they point into, with **RSP 16-byte aligned**. that alignment is not a crash at `_start` when it is wrong — it is a fault hundreds of instructions later inside the first SSE store to a stack local.

the auxv entries all describe the *program*, never the interpreter, with one exception:

| | |
| --- | --- |
| `AT_PHDR` `AT_PHENT` `AT_PHNUM` | the program headers as mapped, which is where the guest looks for them |
| `AT_ENTRY` | the program's entry point |
| `AT_BASE` | **the exception** — the interpreter's load address, and 0 for a static program, which is how the kernel says "there is no interpreter" |
| `AT_RANDOM` | 16 bytes of entropy at a valid guest address, which the guest libc uses to seed its stack canary and its pointer mangling |
| `AT_PLATFORM` `AT_EXECFN` | `"x86_64"`, and the guest program's path |
| `AT_HWCAP` `AT_HWCAP2` | **0, deliberately.** glibc and bionic both issue `CPUID` for feature detection, which FEXCore answers, so a value fabricated here could only ever disagree with that |
| `AT_UID` `AT_EUID` `AT_GID` `AT_EGID` `AT_PAGESZ` `AT_CLKTCK` `AT_SECURE` | the host's own answers |

the page size is read at runtime rather than assumed: android also ships 16k-page configurations.

## syscall dispatch

`host/src/linux_syscalls.{h,cpp}`. FEXCore's JIT spills the whole guest state and hands over the frame; the host layer reads `RAX`/`RDI`/`RSI`/`RDX`/`R10`/`R8`/`R9` out of it into `SyscallArguments::Argument[0..6]` and writes the return value back into guest `RAX` itself, then steps `RIP` past the two-byte instruction (`CompleteSyscall`, `host/src/syscall_args.h`). up to FEX-2608 the JIT did the register halves; FEX-2609 removed that marshalling, and keeping the struct on this side is what kept the change one function wide. the pin is FEX-2609.1 rather than FEX-2609 because the latter left `syscall` mid-block on linux, where the register allocator could keep a pre-call `RAX` value as live across the call and elide the guest's own reload (FEX #5942: the second of two `write`s in one block went out with the first one's return value as its number). 2609.1 ends the block at every syscall and resumes from `CPUState::rip`, which is why the step is the handler's. everything else is the host layer's. **anything not named below is forwarded to bionic**, which issues the arm64 syscall, and that is an honest answer because the guest really does live in this process and at these addresses.

the two things that are easy to get wrong by assuming they are no-ops:

- **values**, where a guest constant differs from the arm64 one. `PROT_*`, `MAP_*`, `FUTEX_*`, `SCHED_*`, `PR_*`, `RLIMIT_*`, `AT_*` and errno are all identical across the two architectures. four `O_*` bits are not — `O_DIRECT`, `O_LARGEFILE`, `O_DIRECTORY` and `O_NOFOLLOW` sit at different values, and passing them through unchanged silently turns an `O_DIRECTORY` open into an `O_LARGEFILE` one. two `MAP_*` bits are x86-only placement hints and are dropped.
- **structures**, where a guest layout differs from the host's. `struct stat` is the one that does: the field order diverges after `st_ino` and the padding differs, so every stat-shaped syscall writes the 144-byte x86-64 layout by hand. `struct statx` is fixed-width and identical everywhere, which is exactly why it needs no translation — it is filled by hand in one place only, for a path the guest file layer owns, where there is no kernel answer to translate in the first place. `iovec`, `pollfd`, `epoll_event`, `timespec`, `timeval`, `sockaddr`, `msghdr`, `rusage`, `tms`, `statfs` and `rlimit64` all agree.

**arm64 has no plain `access`, `stat`, `lstat`, `open`, `mkdir`, `rename`, `unlink`, `link`, `symlink` or `chmod`** — asm-generic dropped them in favour of the `*at` forms — but on x86-64 those are the numbers glibc actually issues, so each is routed onto its `*at` equivalent by hand. a guest dynamic linker probes `/etc/ld.so.preload` with `access` before it does anything else, so this is reached immediately.

the handled set that is more than a forward:

| | |
| --- | --- |
| `brk` | backed by the reserved arena described above |
| `mmap` `mprotect` `munmap` `mremap` `madvise` | forwarded with `PROT_EXEC` stripped, then reported to the VMA tracker with the protection the *guest* asked for. `MADV_DONTNEED` additionally invalidates: the mapping stands but the contents are gone |
| `arch_prctl` | in 64-bit mode FEXCore keeps the FS and GS bases in `CPUState` rather than in a descriptor, so writing them there is the whole of it. this works from inside a syscall because the JIT spills every statically allocated register to `CPUState` before the call and refills after |
| `uname` | the six fields are ours to choose, and `machine` is the one that matters: a guest reading `aarch64` there makes wrong decisions about the code it is running |
| `rt_sigaction` `rt_sigprocmask` `sigaltstack` `rt_sigreturn` | the guest's own signal state, tracked in the host layer and never installed on the host |
| `kill` `tkill` `tgkill` | raise a guest signal on a guest thread. none of them delivers anything at the point of the call |
| `clone` `exit` `exit_group` `set_tid_address` | threads |
| `open` `openat` `stat` `lstat` `newfstatat` `statx` `access` `faccessat` `faccessat2` `readlink` `readlinkat` `getdents64` | path-taking, and therefore subject to the `/proc/self` substitution below and to the guest file layer beside it |
| `write` `writev` to fd 1 and 2 | the guest's log, optionally line-stamped |
| `poll` `ppoll` `select` `pselect6` | the `p` variants' signal mask is **dropped rather than forwarded.** guest signals are emulated entirely inside the host layer, so the guest's mask and the host's have no relationship; handing the guest's mask to the kernel would not change what the guest sees, and would blindfold the host layer's own `SIGSEGV` handler |
| `set_robust_list` | accepted and ignored. it is glibc's crash-recovery bookkeeping for threads that die holding a mutex, and guest threads only die by asking to |
| `clone3` | `-ENOSYS`, deliberately. glibc probes `clone3` first and falls back to `clone` by itself, so a second entry point into the same machinery is only a second way to get the argument marshalling wrong |
| `rseq` | `-ENOSYS`. glibc treats restartable sequences as optional and does without |

sockets forward rather than being stubbed, because a guest that cannot open one usually gives up quietly and hides whatever it was doing. `membarrier` is next to `mlock` rather than near the other memory calls for a reason worth knowing: CoreCLR's process-wide memory barrier asks `membarrier` first and falls back to touching a locked page's protection.

**an unhandled syscall returns `-ENOSYS`, prints its number, arguments and guest RIP, and increments a counter reported at exit.** `get_mempolicy` and `getcpu` are the only two a full game run reaches, and `-ENOSYS` is an honest answer for both.

**the two thunks and the pad bridge ride in on the syscall boundary**, in magic number ranges far above any real syscall, which is what guarantees their arguments are all live in `CPUState` when they are read. all three are tested before the switch and can never shadow a real number.

**the pad bridge is on that boundary without being a thunk**, and it is worth separating the two ideas. a thunk forwards a guest call to a real NDK library; there is no NDK gamepad API for an app to read, so the host layer answers those calls itself out of what the app pushed into it. what it borrows is the boundary alone. [`pad.md`](pad.md) owns it, including why input is a poll rather than a callback: it originates in java and has to reach guest code, which is the one direction nothing here crosses.

`--trace` prints every syscall with the issuing tid, and prints the *path* for the path-taking ones: a trace of bare pointers answers "how many opens" and never "which file", and which file is the whole question when a guest's dynamic linker is searching for something it cannot find.

### the guest's environment

the host layer sets these before whatever the launcher adds. the payload-facing selectors are [`build-format.md`](build-format.md)'s and are passed through from above.

| | |
| --- | --- |
| `LD_LIBRARY_PATH` | the same directory `PT_INTERP` is resolved out of — if ld.so came from there, so did everything it is about to load |
| `HOME` `TMPDIR` `DOTNET_BUNDLE_EXTRACT_BASE_DIR` | all one writable directory. android has no `/tmp`, and .NET reaches for one in three different ways; leaving any of them to a fallback lands the extraction directory at `/` |
| `DOTNET_SYSTEM_GLOBALIZATION_INVARIANT=1` | .NET links globalization against ICU at runtime and fails fast without it, from a static constructor under the first local-time conversion a logger performs. ICU is not among the staged x86-64 shared objects and is ~30 MB of them. invariant mode costs culture-aware formatting and collation, and nothing that affects running a game |
| `PATH` `LANG` | conventional values |

## `/proc/self`

`host/src/guest_procfs.{h,cpp}`. this is the one place the host layer stops being a pass-through: the guest is not the process, and a guest asking procfs who it is must not be told about the arm64 executable hosting it. .NET's apphost locates its own single-file bundle by reading `/proc/self/exe` and parsing a bundle header out of whatever comes back.

| | |
| --- | --- |
| `exe` | answered with the `realpath` of the guest payload, for `open`, `stat`, `lstat`, `access` and `readlink` alike. resolved once at load time, because the guest is usually named relatively and a path relative to a working directory the guest may later change is a trap |
| `cmdline` | synthesised into a `memfd` — NUL-separated including the trailing NUL, since it is not a string and reading it as one gets `argv[0]` alone. a memfd needs no cleanup, is invisible to everything else, and works in a sandbox where a writable path may not exist |

`/proc/<our own pid>/` is recognised as well as `/proc/self/`, because glibc and CoreCLR both build that form from `getpid()` rather than using the shorthand.

**what is deliberately not virtualised is as interesting as what is.** because guest and host share one address space, `/proc/self/maps` read straight from bionic already describes the guest's mappings at their real addresses, more accurately than anything synthesised could — with the one caveat that the permission column is the host's, and therefore has no `x` anywhere. `/proc/self/fd` is honest for the same reason. both pass through.

## the guest file layer

`host/src/guest_files.{h,cpp}` and `host/src/saf_bridge.{h,cpp}`. the same idea as `/proc/self` one level up, with a real directory behind it: a game the user granted rather than staged is not reachable by path at all, so the guest is handed an invented one — `/game/eboot.bin` — and the path-taking calls above are answered out of a content provider instead of being forwarded to bionic.

**it is off unless `--saf-mount <prefix>` names a mount**, and the flag needs the app, because there is no provider to ask on the other side of a shell. without it every path in this section is an ordinary syscall with no interception registered anywhere, which is what keeps a run through the scripts free of any alibi: a frame rate measured that way cannot have been moved by a layer that was not in the run.

only *paths* are answered. a descriptor the provider returns is a real kernel descriptor on a real file, so `read`, `pread`, `lseek`, `mmap` and `fstat` on one are not intercepted and cost nothing extra — and the guest's path-taking is over by the end of boot. the layer is read-only, a directory descriptor is a `memfd` with a hand-written `dirent64` listing beside it, and `saf_bridge.cpp` is this project's only two-way JNI. [`guest-files.md`](guest-files.md) has the design, the costs and the measured A/B against a staged path.

## threads

`host/src/guest_threads.{h,cpp}`. **one guest thread is one host thread, always.** that is not a simplification: it is what keeps the rest of the host layer honest. `gettid`, the `sched_*` family, `futex` and `/proc/self/task` all forward to bionic and give truthful answers, because the thing the guest calls a thread really is a thread of this process.

what FEXCore does and does not do here is worth stating plainly. it creates an `InternalThreadState` and runs it on whatever host thread calls `ExecuteThread`. it does **not** create the host thread, populate the GDT, allocate the call-return shadow stack, or know anything about `clone` semantics. all of that is the host layer's, exactly as it is FEX's own frontend's.

two pieces of storage FEXCore expects the owner of a thread to provide, both of which fail spectacularly when left empty:

- **the GDT.** the decoder reads the CS descriptor to decide 64-bit mode and dereferences the result unchecked. it is **copied from the parent** on a clone.
- **the call-return shadow stack**, which FEX keeps beside the guest's own stack so a guest `ret` can branch straight back to already-compiled host code instead of going through a block lookup. the JIT reaches it through a pinned register with no null check, so a zero there turns the thread's first `call` into a store near the top of the address space. it is **fresh** on a clone, because its contents describe host call frames that only ever existed on the parent's host stack. it is allocated with a guard page either side and a default position a quarter of the way in, so a thread that returns more often than it calls underflows into slack; walking off a guard page anyway resets the pointer and counts, and that count is how a run says whether the mechanism matters at all.

**`clone` is implemented for the `CLONE_THREAD` case only**, in the x86-64 kernel's argument order — which is not x86-32's, where the kernel selects `CLONE_BACKWARDS` and swaps `tls` with `child_tid`. copying that mapping puts a TLS pointer where the `CLONE_CHILD_CLEARTID` word belongs and the first `pthread_join` waits on garbage. anything without `CLONE_THREAD` is `fork`, and returns `-ENOSYS`.

`CLONE_CHILD_CLEARTID` and `set_tid_address` are honoured — the word is zeroed and futex-woken when the thread exits, which is the entire mechanism behind `pthread_join`.

**the start-up handshake between parent and child lives in the child's own storage**, not on the parent's stack: the parent returns from `clone` straight back into JIT'd code, so by the time the child re-checks its wake-up condition the parent's frame has already been overwritten by guest execution.

**`exit` ends one guest thread; `exit_group` ends the process.** when the initial guest thread calls `exit`, the host layer waits for every other guest thread rather than tearing the address space down under threads that are still using it — on linux the process outlives it. `exit_group` calls `::_exit` — the other guest threads are still inside FEXCore and unwinding past them would be racing them, and the kernel does not politely join threads on `exit_group` either. in the app that ends the process the library was loaded into, which is why the app gives a run a process of its own — see [`app.md`](app.md).

## signals

`host/src/guest_signals.{h,cpp}` builds the x86-64 signal frame a guest handler expects, redirects the guest into that handler, and puts everything back on `rt_sigreturn`. the guest ABI structures are spelled out in the header rather than taken from any host header, because the host's describe arm64 and the two disagree in ways that do not announce themselves — x86-64 puts `restorer` in the middle of `sigaction`, and its `ucontext` has a completely different `mcontext`.

**the frame carries FP state, not only GPRs, and that is load-bearing rather than thorough.** SharpEmu emulates SSE4a `EXTRQ`/`INSERTQ` on `#UD` by writing XMM state back *through the signal context*, so a layer that preserved GPRs alone would look fine on a hello world and fall apart on the real workload.

the split of what is per-process and what is per-thread is easy to get backwards. `sigaction` installs handlers for the whole process; **the blocked mask and the alternate stack belong to one thread**, so they live in the thread structure and every entry point that needs them names a thread. a host layer that shared the mask would have one thread's `pthread_sigmask` silently deafen another.

### the host fault handler

`SIGSEGV`, `SIGBUS`, `SIGILL`, `SIGFPE` and `SIGTRAP` are installed process-wide, on a per-thread alternate stack. the order the handler asks its questions in is the design:

1. **the interrupt fault page**, when that delivery site is selected — not a fault at all, but this thread arriving at a guest instruction boundary with a signal waiting for it.
2. **the call-return shadow stack guard pages** — FEX's own bookkeeping drifting off the end of its allocation, which resets the pointer rather than being a crash.
3. **a write to a page sealed for SMC tracking.** asked *before* deciding whether the PC is in JIT'd code, and on purpose: the host layer's own syscall handlers write into guest memory too, so this fault can arrive from host code with a perfectly ordinary host PC.
4. **`SIGBUS` with `BUS_ADRALN` from inside a code buffer** — routine, not a crash. x86 permits unaligned access everywhere, including on the atomic and TSO-ordered operations FEX compiles guest memory accesses into, and arm64's atomics require natural alignment. the JIT emits the fast aligned form and relies on being corrected the first time it is wrong: the faulting instruction is decoded, the code buffer is backpatched to a sequence that tolerates misalignment, and the PC moves back to re-run it.

    **which sequence is FEXCore's `HalfBarrierTSOEnabled` to decide**, read once when the handlers are installed rather than per fault. a half-barrier atomic keeps the ordering the guest expects across the misaligned access; a plain load or store is faster and can tear under another thread, and a run that asks for it is told so in the log. **the first repair announces itself and the rest are counted**, because a run that is killed rather than exited never reaches the exit summary — and without that line a knob that changes the repair cannot be told from one that never fires.
5. **anything left, delivered to the guest** if it installed a handler, and fatal if it did not.

guest registers are not in memory while JIT'd code runs — GPRs live in host arm64 registers and XMMs in host vector registers, per FEXCore's static register allocation — so a real host fault is reconstructed into `CPUState` first, because everything downstream reads `CPUState`.

**a fault the JIT generated is a different path with the opposite rule.** an invalid opcode or a #GP is not a host fault: FEXCore's decoder recognises it at compile time and emits a block that spills guest state, records what should be raised, and branches to a dispatcher trampoline. guest state is already correct on that path — RIP already points at the offending instruction — so it must *not* be reconstructed. the SSE4a emulation above arrives here.

### asynchronous signals

a signal raised on a thread other than the one raising it **cannot be delivered where it is raised**: the target's guest state is in the target's host registers, and only the target may `longjmp` out of its own dispatch loop. so raising is two steps that deliver nothing — record a bit in the target's pending mask, then poke it with `SIGRTMAX`, which carries no meaning of its own beyond "look at your pending mask". the poke is installed **without `SA_RESTART`**, deliberately: a thread parked in `futex` or `poll` has to come back out of that host call for the delivery check to run at all, and `SA_RESTART` would have the kernel silently re-enter it — which is precisely the thread that most needs waking.

**delivery only happens where abandoning the host frames is sound**, because delivery means re-entering `ExecuteThread` at the guest RIP the host PC maps back to, and that is only valid at a guest instruction boundary. land in the middle of the arm64 sequence implementing one guest instruction and the guest registers are half-updated while RIP still points at the start of it, so resuming re-runs the instruction over its own partial results.

| `--asyncsig` | |
| --- | --- |
| `syscall` | **the default.** syscall exits alone, plus `rt_sigreturn`, which restores the mask the handler was entered with and can therefore unblock something that arrived during it. sound by construction — the guest chose that boundary itself. its one honest gap is a thread that spins in translated code without calling anything, which it never reaches |
| `safepoint` | FEXCore's interrupt fault page. the JIT stores to it at every block entry and ahead of every backward branch, so arming it `PROT_NONE` turns the next of those into a deliverable fault. it closes the gap above and passes all three routes of the regression guest — **and a real game still dies under it**, which is why it is not the default. a back-edge check is not the boundary a block entry is: the host PC there maps back to a guest instruction that has already run |
| `block` | delivery anywhere inside a translated block. it is what *shows* the problem above, is not safe, and is not a supported mode |

blocked signals stay pending, because a blocked signal is not lost — it is waited for. ignored signals are dropped at the point they are taken rather than when they are raised, since a disposition can become `SIG_IGN` after the signal was already queued and linux discards it in exactly that case.

`--trace-signals` traces raise, defer, deliver and sigreturn — a handful of lines per run rather than a firehose, and separate from `--trace` for that reason. it answers one question a syscall log cannot: what the guest's handler did to the frame. CoreCLR suspends threads for its GC by **rewriting the RIP in the ucontext it was handed**, so a delivery whose sigreturn returns at a different RIP than it left at is the runtime redirecting itself.

## pausing

`Threads::Pause` and `Threads::Resume`, which the app reaches through `nativePause` and `nativeResume`. **a paused guest is every guest thread stopped where it holds no host lock, with everything it owns kept in memory**, until it is let go and carries on from exactly where it was.

**where a thread may stop is the whole design**, and it is the same two places an asynchronous signal is delivered from, for the same reason: inside translated code, where the frames below belong to FEX's dispatcher and nothing of the host layer's is held, and on the way out of a syscall, where `CPUState` describes the guest exactly and every host lock has been let go. a thread stopped anywhere else could be holding bionic's allocator, a driver's queue lock or FEXCore's code cache, and the app's own threads — which go on drawing over a paused game — would then wait on it.

**stopping inside translated code needs no redirection at all**, which is why it is sound where delivering a signal from the same place is not. the poke's handler simply does not return until the pause is over, and the thread then carries on from the instruction it was interrupted at; nothing is rebuilt, and no instruction boundary has to be found. the handler parks on a futex word, the only wait that is safe in a signal handler.

| a thread that is | when the pause starts |
| --- | --- |
| running translated code | poked with `SIGRTMAX`, and parks in the handler |
| in FEXCore's compiler, bionic or a syscall implementation | poked, and left alone by the handler. it is poked again every millisecond, then every ten, until it is caught in translated code or reaches a syscall exit |
| blocked in a syscall | **not poked**. it is already stopped, and parks on the way out if anything wakes it |
| not yet started | parks at the top of its dispatch loop, before its first guest instruction |

the poking is done by a host thread of its own, so the caller is never held, and it says once where every thread ended up: `paused: 46 guest threads, 1 parked and 45 inside a syscall, after 0.00 ms`. the resume says how many stopped in translated code and how many at a syscall exit, which is **the liveness counter for the first of the two places** — a game whose threads are all blocked at the moment it is paused never exercises the poke landing in a block, and a pause that worked only at syscall exits would otherwise look exactly like one that works everywhere. a thread still running after five seconds is named rather than poked forever.

**every guest syscall is bracketed** so the pause can tell blocked from running. the bracket opens on entry and closes on the way out, and `rt_sigreturn` and a signal delivered from inside a syscall — both of which leave through the escape hatch rather than through the exit — close it where they land. **the one race is a thread found running that enters a blocking call before its poke arrives**: the poke then brings that call back with `EINTR` the guest never asked for. the exit tells that `EINTR` from one a guest signal caused — a guest signal leaves itself pending — and issues the same syscall again once the pause is over, which is what the guest would have seen had it never been interrupted. **only a real syscall is ever issued twice**: a thunk call is a library call whose library retries its own `EINTR`, whose result can equal `-EINTR` by coincidence, and whose arguments the vulkan thunk rewrites on the way through.

**every syscall pays for the bracket, so it is plain loads and stores.** nothing about the pause needs them ordered — the thread finishing a pause walks the threads again every few milliseconds, so a stale reading either way costs one poke the exit accounts for, or one walk's delay — and ordered versions cost 15 ns on every syscall. as it is, the bracket costs nothing measurable on a thunk call, and the clock below adds about 3 ns to a `clock_gettime`.

### the guest's clock, with the pauses left out

**a paused guest sees the pause the way a linux program sees the machine being suspended**: its monotonic clocks stop, and `CLOCK_REALTIME` and `CLOCK_BOOTTIME` carry on. that is the kernel's own answer to the same question, and it is load-bearing rather than tidy — the emulator's own stall watchdog ends a run that makes no progress for twenty seconds by `CLOCK_MONOTONIC`, and on linux every clock it hands a game, `sceKernelReadTsc` and process time included, reads that one too. without this, a game paused for longer than twenty seconds is ended by that watchdog the moment it is resumed.

so the guest's `CLOCK_MONOTONIC`, `CLOCK_MONOTONIC_RAW` and `CLOCK_MONOTONIC_COARSE` read the host's less the total time paused, and **an absolute deadline on any of them is moved later by the same amount**: `clock_nanosleep` with `TIMER_ABSTIME`, and the futex operations whose timeout is an absolute monotonic time — `FUTEX_WAIT_BITSET`, `FUTEX_WAIT_REQUEUE_PI` and `FUTEX_LOCK_PI2` without `FUTEX_CLOCK_REALTIME`, which is the shape CoreCLR's own timed waits take. relative timeouts need nothing. the total moves at `Resume`, before any thread is let go, so no thread reads the clock on both sides of the step.

**the one clock this cannot reach is the cycle counter**: a guest's own `rdtsc` is translated to a read of the host's counter inside a block. the emulator does not use it on linux, and a game reading it directly sees the pause.

### audio, and the self-test

the audio streams the guest had playing are paused with it — see [`audio.md`](audio.md).

**`--pause-selftest` pauses the guest for half a second, 300 ms after it starts**, and fails the run unless a thread was stopped inside translated code and none was still running when the pause settled. `guests/pause.c` checks the rest from inside: that its monotonic clock left the half second out, against `CLOCK_BOOTTIME`, and that a thread spinning in translated code with no syscalls at all stopped for it. the regression set runs it both ways — with the self-test, where it must pass, and without, where it must fail, which is what says the guest can tell a pause from none.

## VMA and SMC tracking

`host/src/vma_tracker.{h,cpp}`. FEXCore asks the host layer two questions about guest memory and expects to be told when the answers change:

- *"is there executable guest code at this address, and how far does it run?"* — a size of 0 is how the host layer says "not executable", and it is what makes the decoder refuse to translate a data page a guest jumped into by accident.
- *"this page now holds code i have compiled."* — under `mtrack`, an invitation to arrange for a fault the next time the guest writes there.

and the other direction: whenever guest memory stops holding what FEXCore compiled — unmapped, re-protected, moved, discarded, or simply written to — the host layer must invalidate before the guest can reach the stale translation. that means dropping the block from the shared code buffers *and* from every live thread's own lookup cache, which is why the tracker walks the thread registry.

**the kernel cannot be asked any of this, and that is the whole reason the tracker exists.** because `PROT_EXEC` never reaches bionic, the host kernel does not know which guest pages the guest believes are executable and `/proc/self/maps` will never say. the guest's *requested* protection exists only here, recorded at the moment the syscall arrives — so **every producer of guest memory has to report in**, not just `mmap`: the ELF loader, the `brk` arena, the guest stack and the sigreturn trampoline all record what they made.

| `--smc` | |
| --- | --- |
| `mtrack` | **the default**, and FEXCore's. a compiled page has `PROT_WRITE` taken off the host mapping; the write faults, the fault handler drops the translations for that page, hands the permission back, and lets the instruction re-run |
| `full` | FEXCore emits a byte-comparison guard into every block. correct, and slower. the fallback if the page-protection machinery ever misbehaves |
| `none` | no detection at all. fast, and wrong for anything with a JIT — which includes any .NET payload |

the mode has to be set before the core is initialised, because it changes how blocks are compiled, and the tracker and FEXCore have to be told the same thing.

**one case is not a plain resume: the guest rewriting code inside the very block it is executing.** re-running the faulting instruction there would carry straight on into the translation that was just dropped, so guest state is spilled and the dispatcher is re-entered asking for a one-instruction block instead.

**the invalidation entry point is load-bearing under `full` as well as under `mtrack`**, which the name does not suggest: the byte-comparison guard emitted into every block calls back through it. leaving it as the base class's empty default does not merely lose an optimisation — the guard detects the change, invalidates nothing, returns to the same entrypoint, finds the same stale block, and spins forever.

**under FEX-2609, `full` spins on any instruction FEXCore could not decode — including the jump into a non-executable page that the size-0 answer above exists to refuse.** that is a FEX bug, not this tracker's, and `mtrack` never reaches it. FEX-2609 changed the guard from a byte comparison to a CRC32 over the instruction's bytes (`ValidateCode`, `FEXCore/Source/Interface/Core/JIT/BranchOps.cpp`). an undecodable instruction is emitted with a length of 0, so no CRC instruction is generated, but the result is still taken from the scratch register (`sub Dst, TMP1, crc`) that only those instructions would have written. whatever the dispatcher last left in it reads as "the code changed": the guard invalidates, exits to the same address, and the recompiled block does the same thing again forever. the one-token fix is `WorkingReg.W()` in place of `TMP1` in that `sub` (with no bytes the checksum is `wzr`, which equals the compile-time CRC of nothing). it was found on a Snapdragon 8 Gen 3, where the regression set spent its whole Test Lab slot on this one mode, and a second run on the same phone failed it on time at the same instruction while FEX-2608 passed all nineteen modes; an Android emulator run passed it, presumably because the register held zero there. **the fix is carried in this repository as [`host/fex-patches/0001-smc-full-empty-block-checksum.patch`](../host/fex-patches/0001-smc-full-empty-block-checksum.patch)**, applied at configure time to a copy of the file in the build tree (see [`repo-structure.md`](repo-structure.md)), and with it `smc-full` passes again. the day a FEX bump picks up an equivalent fix, the patch stops applying, configure says so, and the patch is deleted.

**`DOTNET_EnableWriteXorExecute=0` is a hard requirement of this mechanism**, not a tuning knob. without it the SMC tracker cannot see CoreCLR's JIT writes, and a boot costs 65x. the launcher sets it; [`build-format.md`](build-format.md) records that nothing in the payload reads it, so that nobody removes it on those grounds.

## the switches

the argument vector is the whole interface, and the app passes the same flags a shell would. **everything below is off unless it is asked for**, so two runs differ by exactly the flags between them and a measurement is attributable to one change at a time.

| | |
| --- | --- |
| `--spike` | 24 bytes of hand-assembled x86-64, no loader and no syscall table: the smoke test that the JIT still translates, executes and faults on this device. it goes through the real VMA tracker, so a miswired tracker fails here immediately rather than subtly much later |
| `--libs <dir>` | where `PT_INTERP` is resolved from, and what the guest gets as `LD_LIBRARY_PATH`. one flag for both, because they are the same directory in every case that matters and splitting them only creates a way to get them out of step |
| `--tmp <dir>` | the writable directory behind `HOME`, `TMPDIR` and the bundle extraction base |
| `--env NAME=VALUE` | appended to the guest environment. repeatable |
| `--trace` | every syscall, with the tid and, where there is one, the path |
| `--trace-signals` | the asynchronous signal path only |
| `--trace-files <prefix>` | what the guest asks of one directory subtree — opens, stats, listings, and what it then does with the descriptors. a few hundred events in a whole run against `--trace`'s millions, and it answers a question of its own: it is what makes two ways of reaching the same game comparable rather than a matter of opinion |
| `--saf-mount <prefix>` | where a game the user granted appears to the guest. **needs the app**, and the run is refused rather than half-mounted if there is no provider to ask. [`guest-files.md`](guest-files.md) |
| `--timestamps` | prefixes every line the guest writes to stdout or stderr with elapsed time since process start. **elapsed rather than time of day**, because every number worth having is a delta from the first line and a delta stays meaningful next to a run from another day. the host layer's own lines are deliberately unstamped, which makes them instantly distinguishable while their position still says when they happened |
| `--log-tids` | widens that stamp to name the host thread that wrote the line, `[+   6.402 t14053]`. implies `--timestamps`, the thread id riding in the stamp because there is nowhere else to put it. **what it is for**: a thread the emulator names in its own output cannot otherwise be matched against an in-process counter that names a thread id, since every line reaching the platform log carries the log pump's thread rather than its author's. off by default, because the boot checkpoints and anything else matching a guest line by its text see a prefix that has moved |
| `--boot-progress` | matches the guest's log against an ordered table of boot checkpoints, so that a caller with a screen in front of a booting guest can say how far along it is. below |
| `--smc none\|mtrack\|full` | above |
| `--asyncsig syscall\|safepoint\|block` | above |
| `--fex Name=Value` | one FEXCore option, repeatable. below |
| `--host-features probe\|minimal` | how FEXCore is told what this CPU can do. below |
| `--vulkan` `--audio` and their families | the two thunks, `host/src/vulkan_thunk.h` and `host/src/audio_thunk.h` |
| `--pad` `--trace-pad` `--pad-selftest` | the gamepad bridge, `host/src/pad_bridge.h`. [`pad.md`](pad.md) |

### describing the host CPU

FEXCore is created with a `HostFeatures`, which its own header calls "backend features that change how codegen is generated from IR". a field left false is a longer instruction sequence than this host needs; a field set true on a CPU without the extension is `SIGILL` wherever the emitter first uses it. **the asymmetry is why the set is read from the CPU rather than assumed** — `--host-features probe`, which is what a launch does when nothing is said, reads the `ID_AA64*` registers, `CTR_EL0` and `DCZID_EL0` and decodes them field by field.

`mrs` of an ID register is trapped and emulated by the kernel at EL0, which is what makes this readable from an ordinary process. the view it answers with is *sanitised* — fields the kernel does not want userspace acting on read as zero — and that is the right thing to describe FEXCore with, since it is also what the guest's own threads observe.

**three fields are not CPU properties and say where they come from instead.** `SupportsAVX` is not a capability at all but the switch deciding whether the decoder has a VEX table to decode with, so it is unconditional; `SupportsPreserveAllABI` is a property of the compiler that built FEXCore rather than of the processor; and the cacheline maintenance ops travel with the cacheline *sizes*, since the emitter divides by the size to work out how many lines to walk and a zero there is reached exactly when the ops are on and the size was never read.

**a CPU that advertises an extension it does not deliver is why this is a port of FEX's probe rather than a reading of the registers alone.** the errata are FEX's: Qualcomm's Oryon implements the RAND extension with `RNDRRS` never returning a number, where x86 permits `RDSEED` to fail but guarantees eventual success — so a guest that retries until it succeeds never leaves the loop, and the extension is switched off rather than the instruction special-cased, because CPUID is what a guest asks. some Cortex parts execute the unscaled `LDAPUR` forms with stricter ordering than their pseudocode describes, which costs more than the addressing mode saves. both are decided per core out of `/sys`, so a hybrid part is judged on all of its cores rather than on whichever one the probe happened to run on.

**`minimal` is the arm the probe is measured against**, and the answer for a device the probe reads wrongly: four extensions off `AT_HWCAP`, the AVX decode table, and the MIDRs. it is not a preset rung and must not become one — understating the host is wrong at every rung, since it buys no fidelity for the speed it gives up.

a feature set other than this CPU's is described with FEXCore's own `CPUFeatureRegisters`, which the probe reads: `--fex CPUFeatureRegisters=isar0=0x1021111110212120,mmfr1=0x0` overrides the named registers and leaves the rest as the hardware gave them. **FEXCore's `HostFeatures` option is a different thing and reaches nothing here**, being a bitmask of individual overrides consumed only in FEX's frontend.

the run prints both the raw registers and every resulting field, so a report about wrong codegen carries enough to recompute the second from the first away from the device.

### choosing FEXCore options

`--fex TSOEnabled=0` sets one of FEXCore's own configuration options for the run. **it exists because the routes FEX documents do not reach a process that hosts FEXCore as a library**: a `Config.json` and the `FEX_` environment variables are both read by FEX's frontend, and only the core is linked here. so a `FEX_TSOEnabled` in the environment reaches nothing, silently, which is a worse answer than no answer.

**the names are FEXCore's json spellings** — `TSOEnabled`, `Multiblock`, `X87ReducedPrecision` — and they are resolved against FEXCore's own generated option table rather than a list restated here, so a name upstream renames or removes fails to build instead of resolving to the wrong option. **an unknown name refuses the run.** FEX's own loaders skip what they do not recognise, which suits a file a person edits by hand; here the names arrive from a table in a program, and a typo that merely did nothing would be indistinguishable from a knob with no effect on this workload.

values are passed through as written, so a bool wants `0` or `1`: the `none`/`mtrack`/`full` spellings belong to FEX's argument parser, which this does not use.

**three options are the host layer's and a `--fex` naming one is overridden rather than refused.** 64-bit mode, the SMC mode and the interrupt fault page are set after every `--fex` is applied, because each is load-bearing for correctness rather than a preference — the first silently halves the guest register file, the second has to agree with what the VMA tracker is told, and the third is what makes an asynchronous signal deliverable at all. `--smc` is how the SMC mode is chosen.

**and one is a default rather than either**, set *before* the loop so that a launch naming it wins: `DynamicL1CacheDecreaseCountHeuristic=0`, which stops FEXCore's block-lookup L1 shrinking. that L1 is resized to fit a guest thread's working set, and a shrink strands every translation cached above the new size — the shrink zeroes only the region above it, invalidation clears the single index an address folds to under the mask in force now, and the inline lookup returns on a guest-address match with nothing else checked. a mask that widens again therefore reaches an entry left behind at a narrower one, and a guest thread resumed through the emulator's continuation trampoline runs the previous translation of that trampoline: it bakes all sixteen registers as immediates, so the thread lands on the right instruction holding the previous resume's operands. the audio stops permanently and sometimes the picture stops with it — [issue #1](https://github.com/mircowuffwuff/sharpdroid/issues/1).

**stopping the shrink is the whole of what the fault needs**, and the option is a threshold rather than a switch: the branch compares a rate against this number and a rate cannot fall below zero, so at 0 nothing reaches it while the growth heuristic beside it is untouched and the L1 still sizes itself upward. pinning the cache outright with `DynamicL1Cache=0` closes the same fault and costs about 150 MB more for a size change the fault does not need.

**a default and not a pin, because the unmodified arm has to stay reachable**: `--fex DynamicL1CacheDecreaseCountHeuristic=50` still asks for FEXCore's own value, which is what lets somebody reproduce the fault on a build that does not have it. the run prints the default whether or not a launch replaces it, so a log read cold says which lookup it ran on rather than leaving it to be inferred from an absent line.

**`--fex DiskCache=1 --fex DiskCachePath=<a directory>/` keeps FEXCore's compiled code across runs**, and it can start only because the host layer gives FEXCore the threads it asks for (`host/src/fex_threads.{h,cpp}`): the cache writer is a FEXCore worker thread, and the defaults behind FEXCore's thread hooks abort. there is no file-section lookup on this side, so every block takes FEX's anonymous-code path, which keys on the guest bytes and stores relocations relative to the block — which is what lets it survive ASLR. measured on a Snapdragon 8 Gen 3 phone under FEX-2609 to `Calling guest entry` of a trivial guest, a ReadyToRun payload boots in 2.4-2.5 s without it and 1.50-1.56 s with a warm cache, and the run that writes the cache was no slower than one without; an IL-only payload goes from 5.5 s to 4.3-4.6 s. **it stays opt-in, because nothing bounds it.** a ReadyToRun payload's cache settled at 44-46 MB, but an IL-only one grew from 55 to 83 MB over three runs, since what the JIT emits differs from launch to launch and every new translation is a new entry — and a new build of the payload adds a new set of entries beside the old ones rather than replacing them. a default would want a directory per build and a size limit first.

`--timestamps` detours fds 1 and 2 through a stamping writer. **the whole stamped buffer goes out in a single write**, because many guest threads share that descriptor and two writes per line would let their output interleave mid-sentence; the cost is that a short write cannot be reported back exactly, so the loop finishes the buffer and tells the guest its own length went out. a guest that saw its own write return more than it asked for would be entitled to be confused. stamps are placed by tracking line *starts* rather than writes, which is what survives a guest emitting its text and its newline separately — .NET's console writer does.

### where a boot has got to

a guest takes several seconds to reach its first frame and the panel is black for all of it. the emulator says a great deal about what it is doing meanwhile, on stdout, and `--boot-progress` turns that stream into a position: an ordered table of checkpoints, and how far along it this run has got. `host/src/boot_progress.h` is the whole of it.

**the two ends of the table belong to this project and the middle does not.** the start is the host layer's own; the end is the vulkan thunk's first presented frame, which is the moment the panel has something on it. between them is a line the emulator prints, and the emulator may rename any of those at any time — so the design is built around that being survivable:

- **entries are optional.** the scan runs forward from the current position to the end of the table, so a pattern that no longer appears is passed over the moment a later one matches. matching only the *next* entry would instead let one renamed line stall the table and lose every entry behind it.
- **the position only moves forward.** several of these lines occur more than once in a boot — `=== Execute START ===` occurs once per module — so "the first line that matches anything" and "any line that matches this" are both wrong, and neither is reachable.
- **the terminal entry is not a line.** the first presented frame advances it, and it advances to the *end*, so an entry that never matched is passed over at once and the position lands on complete exactly when the picture appears. that one rule is what makes every way this can go stale end in a finished bar rather than a stuck one.
- **it says what no longer matches.** the first frame prints how many of the patterns were seen and names each one that was not, because a table that has quietly stopped matching and a boot that is simply fast look identical from outside.

**patterns are the structural fragment of a line and never the whole sentence**, since most of these lines carry counts that differ per title. a pattern with a number in it is one that stops matching on the next game.

**the tap is in the log pump rather than under the write syscall**, which is the other place every guest line passes. that one runs on the guest's own thread and would put the cost in the boot's critical path; the pump exists to drain a pipe and is already off it. a boot produces on the order of a thousand lines, so the whole of the matching is around a millisecond spread over several seconds, and the tap disarms at the first frame — the rest of a run, which is where frame rate is measured, pays one relaxed load per line.

**a caller reads the position rather than being told it.** this is the direction every seam here runs, and the reason is the same one the pad bridge has: the pump thread must keep draining, and anything it called into could block it, at which point the pipe fills and the guest blocks in `write`.

## what a run reports at exit

printed once however the process ends, and handed to the thread layer as well as called directly, because `exit_group` can come from any guest thread and only the initial one unwinds back to the driver.

threads created and still live; guest signals delivered; signals raised asynchronously against interrupts left for a later boundary; frames presented and vulkan calls thunked; audio calls thunked and per-stream frame counts; pad polls, polls that found a pad, and rumbles asked for against rumbles delivered; call-return stack resets; unaligned accesses backpatched; the SMC mode with mappings tracked, invalidations and write faults; and unhandled syscalls with the last number.

**several of these are counted precisely because zero is the interesting answer.** a negative result with no liveness counter behind it is silence, not evidence — a run reporting no call-return resets says the mechanism is theoretical here, and one reporting thousands says something quite different. the same reasoning is why a stream that opened and never played reports its frame count: it is the one audio failure that looks exactly like success.

**and rumbles asked for are counted apart from rumbles delivered for the same reason.** delivery is asynchronous and ends in a platform call that can be refused, so "the guest asked" and "the device buzzed" are two claims; a gap between the counts is what a broken delivery path looks like, and one number could not tell them apart.
