# performance roadmap

where the speed of a PS5 game on a phone is lost, what has been measured, and the decisions ranked by what they buy for what they cost. it was written on 2026-10-07 from the repository as it stood after the R2R / FEX-2609 / uncompressed-payload work, from public sources on the translators and drivers involved, and from the VM loop in [`vm.md`](vm.md).

**[measured]** means a number from a device or from the VM loop, **[source]** means a cited public source, and **[estimate]** means an inference that nothing here has measured yet. an estimate is a reason to measure, not a result.

## first, the ceiling

**a phone is not a PS5, and no emulator closes that gap.** the honest numbers, before any decision below:

| | PS5 | a 2025–26 flagship phone |
| --- | --- | --- |
| GPU, FP32 | 10.28 TFLOPS, RDNA2, 36 CU at 2.23 GHz [source] | Adreno 830: about 1.8–3.7 TFLOPS, sources disagree [source] |
| memory bandwidth | 448 GB/s GDDR6 | about 77–85 GB/s LPDDR5X [estimate] |
| CPU | 8 Zen 2 cores, 16 threads, up to 3.5 GHz | Oryon / Cortex-X4 class: *native* single-thread is faster than Zen 2 [source] |
| CPU under translation | — | roughly 40–70 % of native for integer code, worse for AVX2 on 128-bit NEON [estimate] |

so the GPU is a third to a fifth of the console's with a fifth of its bandwidth, and the CPU is a few Zen 2 cores at best once x86-64 is translated. **GTA 6 is out of reach**: it ships on 2026-11-19 on PS5 and Xbox Series only [source], it is the heaviest console title of its generation, and SharpEmu itself is an accuracy-first emulator whose tested titles are Demon's Souls (boot imagery), Dreaming Sarah, Void Terrarium and Dead Cells. the realistic target for everything below is **2D and light 3D PS5 titles, booting fast and holding a steady frame**.

## where the time goes today

- **boot** [measured, Xiaomi 14 / 8 Gen 3, FEX-2609]: an R2R payload reaches *Calling guest entry* in 2.4–2.5 s cold and 1.50–1.56 s with a warm DiskCache; an IL-only payload takes 5.5 s (4.3–4.6 s warm); single-file compression costs about 0.45 s.
- **the whole emulator runs translated.** SharpEmu is a linux-x64 .NET program: CoreCLR, its JIT and GC, the HLE, the Gen5 → SPIR-V shader translator and the Vulkan presenter all run through FEXCore, not only the PS5 code. every method CoreCLR jits is guest code FEXCore then translates again, and every one of those writes is SMC the tracker has to see.
- **memory ordering.** x86 is TSO, arm64 is not, and FEXCore pays for TSO on every load and store it cannot prove private. the Intermediate rung (TSO on, vector and memcpy TSO off, unaligned half-barrier on) is the configuration FEX itself recommends on LRCPC/LRCPC2 hardware [source].
- **AVX2.** PS5 code is Zen 2 code, so AVX2 is everywhere. FEXCore's fast AVX path needs 256-bit SVE, which no phone has; on a phone every 256-bit op is two 128-bit NEON ops [source, estimate].

## the decisions, ranked

| # | decision | buys | effort | risk | where |
| --- | --- | --- | --- | --- | --- |
| 1 | **prewarm the FEX DiskCache for a payload** at install or first idle, keyed by payload hash — Rosetta 2's AOT idea | cold boot → about warm (2.4 → ~1.5 s on a phone; warm 0.88× in the VM) [measured] | M | low | `scripts/package-build.py`, the launcher, `docs/app.md` |
| 2 | **NativeAOT linux-x64 payload** in the fork — built: `perf/android/nativeaot-v2`, `package-build.py --nativeaot` (see *measured so far*) | **Xiaomi 14 boot 2.41 → 0.91 s cold (2.6×), 1.59 → 0.64 s warm** [measured]; VM 3.6–3.7×, SMC write faults 2,999 → 0; no CoreCLR JIT under translation, steadier pacing | M | medium (needs a real-game run) | fork csproj, `ModuleManager.LeanWarmup.cs`, `Program.cs` |
| 3 | until #2: **.NET runtime knobs** — composite R2R with the framework, `TieredPGO=0`, larger gen0, fewer rejits | R2R ~2× [measured]; PGO and gen0 nothing measurable at boot [measured, VM]; composite R2R and the HLE warm-up untested | S | low | build env in `docs/build-format.md`, publish args |
| 4 | **track FEX monthly** (2610+), rebase `host/fex-patches/`, re-measure the ladder each bump | CPU 5–15 % [estimate] | S per month | low–medium | `external/FEX`, `toolchain.json` |
| 5 | **ADPF PerformanceHint** around present (`vkQueuePresentKHR` is already thunked) plus thermal headroom | frame pacing, sustained clocks | S–M | low | `host/src/vulkan_thunk.cpp`, `entry_jni.cpp` |
| 6 | **host-side thread placement**: guest main/render threads on prime cores, FEX compile and CoreCLR background threads on mid cores | frame pacing | S | low | `host/src/guest_threads.cpp`, `fex_threads.cpp` |
| 7 | **per-game memory-order profiles** with a measured compatibility list (Box64's STRONGMEM idea, on the per-game rung the app already has) | CPU 10–30 % on eligible titles [estimate] | S | medium (silent corruption) | per-game settings, `docs/app.md` |
| 8 | **verify LRCPC / LRCPC2 / LRCPC3 / LSE2** detection on 8 Gen 2 / 3 / Elite and that TSO loads use LDAPR | the TSO cost | S | low | `host/src/host_features.cpp` |
| 9 | **key the pipeline cache by `pipelineCacheUUID` and driver** (system driver and Turnip caches are not interchangeable) | shader stutter | S | low | the launcher's `SHARPEMU_VK_PIPELINE_CACHE_PATH` |
| 10 | **cache translated SPIR-V**, compile pipelines asynchronously (GPL / shader object where Turnip has them) | stutter, translated CPU time | M | low–medium | fork `SharpEmu.ShaderCompiler`, presenter |
| 11 | **render scale plus FSR1** in the presenter, driven by #5's thermal headroom | GPU 1.5–2.5× effective [estimate] | M | low | fork presenter, or a host-side blit |
| 12 | **thunk glibc's memcpy / memset / str\*** to bionic's NEON versions | CPU, a few % [estimate] | M | low | `host/thunks`, `scripts/gen-thunks.py` |
| 13 | **`MADV_HUGEPAGE`** on FEX code buffers and large guest mappings where the kernel allows | iTLB, a few % [estimate] | S | low | `vma_tracker.cpp`, mmap paths |
| 14 | **ship current Turnip A7xx / A8xx** and pick a default driver per GPU | GPU, compatibility | S | medium | `scripts/build-adrenotools.py` |
| 15 | **research spike: SharpEmu native linux-arm64**, with a FEXCore-backed `INativeCpuBackend` (Arm64EC's model: the emulator native, only PS5 code translated) | every axis; the largest win available, 2×+ on emulator-side work [estimate] | months | high (diverges from upstream) | fork `SharpEmu.Core/Cpu/Native/*`, `HostPlatform.cs` |
| 16 | frame generation, after pacing is solved | perceived fps | L | medium | presenter |
| 17 | 256-bit SVE2 phone cores: watch, nothing to do yet | AVX2 | — | — | — |

### why this order

**#1–#3 remove work, so they are safe and the VM can see them.** they are boot and hitch wins with no correctness risk, and every one is a change to how the payload is built or launched rather than to the JIT.

**#2 and #15 are the two real step changes**, and they are the same idea at two sizes. today the emulator's own .NET code is the majority of what FEXCore translates at boot. NativeAOT removes the JIT from that picture; a native arm64 SharpEmu removes translation of the emulator altogether and leaves FEXCore only the PS5 code — which is what Arm64EC does on windows. both live in the fork. the seams are already there: `SharpEmu.HLE/Host/HostPlatform.cs` is the one place that refuses a non-x64 process, and `SharpEmu.Core/Cpu/Native/INativeCpuBackend.cs` is the interface a FEXCore backend would implement.

**#5, #6, #9–#11 are the frame, not the boot**, and none of them can be measured anywhere but a phone with a game running.

**#7 and #8 are the CPU's biggest dial and its biggest risk.** a title that is single-threaded where it matters can run with TSO off and gain a lot; one that is not corrupts memory silently. that is why it is a per-game, measured decision and never a default.

## what the VM can and cannot decide

the VM loop ([`vm.md`](vm.md)) runs the host layer under qemu-system-aarch64 with android's bionic. it measures **ratios between configurations that change how much work is done** — #1, #2, #3, #4, #12 — and nothing about the speed of an individual instruction, so #7, #8 and #13 need a device. every graphics and audio item needs a device.

## measured so far

results land here as they are produced, with where and how.

- **VM, regression set** (16 vCPUs, `-cpu max,sve=off`): every mode that does not need a GPU or an audio device passes, 15 of 19; `vulkan`, `vkrender`, `vkswap` and `aaudio` fail as they must with no hardware behind them.

- **VM, boot bench** (`scripts/vm-boot-bench.sh`, 16 vCPUs, `-cpu max,sve=off`, payload `android-0.0.3-release.4`, 3 runs a row, the median against the baseline's). the baseline's own three runs spread 89.7–106.4 s, so **anything within about ±10 % is noise**:

  | row | median | vs baseline |
  | --- | --- | --- |
  | baseline (R2R, the launcher's env) | 91.6 s | 1.00 |
  | `DOTNET_TieredPGO=0` | 97.6 s | 1.07 — noise, not a win |
  | `DOTNET_GCgen0size=64 MiB` | 103.0 s | 1.12 — noise or slightly worse |
  | both | 96.1 s | 1.05 — noise |
  | `DOTNET_ReadyToRun=0` (IL only) | 179.0 s | **1.95** |
  | FEX DiskCache on the run share, cold then warm | 122.4 s, then 124.5 / 118.5 s | 1.34, then 1.33 |
  | FEX DiskCache in RAM (second session, its own baseline 94.0 s), cold then warm | 103.6 s, then 83.8 / 81.9 s | 1.10 cold, **0.88 warm** |

  what it decides: **R2R is the one .NET knob that matters** — the VM's 1.95× is the device's 2.3× (5.5 s → 2.4 s) seen through TCG, which is the first check that the VM points the right way. **#3's PGO and gen0 knobs buy nothing measurable at boot**; they stay out of the launcher until a device frame-time run says otherwise. the first DiskCache row measures 9p, not the cache: the cache sat on the run share, which is uncached 9p under TCG, so every lookup was a round trip to the host (`vm.md`). in RAM the cache does what it does on a phone — a cold run pays about 10 % to fill it and every warm run is about 12 % faster, the device's 0.62 seen through TCG — which is the case for **#1, prewarming it so that no user ever pays the cold boot**.
- **correction (2026-10-08): R2R is not being bypassed at boot.** the warm-up's *JIT-compiled 24076 methods* counts `RuntimeHelpers.PrepareMethod` calls, and for a method R2R already compiled that call just binds the precompiled code. the JIT's own log (`DOTNET_JitStdOutFile` + `DOTNET_JitDisasmSummary=1`, the bench's `jit-summary` row) shows **694 methods actually jitted** in a boot: 238 instrumented Tier1, 194 Tier0, 58 instrumented Tier0, 55 Tier1, 44 FullOpts, 35 Tier1 with synthesized PGO, 35 MinOpts, 34 Tier1 with dynamic PGO (84 of them Silk.NET, 16 SharpEmu.Libs) — generic instantiations and stubs R2R cannot pregenerate, plus tiering rejits. the SMC numbers (7,513 code invalidations, 2,999 SMC write faults per boot) are those 694 methods, their rejits and CoreCLR's stubs.
- **where an R2R boot's seconds go** (VM, `SHARPEMU_BOOT_TRACE=1`, fork branch `perf/android/boot-trace`, one run of 101.9 s, uptime deltas): before `Main` about 26.8 s (FEX, the 9p payload read, CoreCLR start), `Main` → runtime creation 5.5 s, the generated export registry 6.1 s, registration 0.6 s, the warm-up's dependency walk 3.1 s, **framework class constructors 13.4 s, the HLE warm-up 26.2 s** (reflection 9.1, class constructors 7.6, `PrepareMethod` 9.3), the 154k-entry NID catalog 8.0 s, the refusal 0.8 s, exit 5.8 s. **about 42 % of a boot is the warm-up**, which is what the next two rows go after.
- **lean warm-up** (fork `perf/android/lean-warmup`: the work list from metadata instead of reflection, only types with something to initialise, `PrepareMethod` on N threads, the NID catalog parsed on its own thread; `SHARPEMU_WARMUP=legacy` and `SHARPEMU_AEROLIB_PRELOAD=0` restore the old path). VM, 2 runs a row:

  | row | runs | mean | `Main` → end |
  | --- | --- | --- | --- |
  | legacy warm-up, catalog inline | 103.1 / 106.4 s | 104.8 s | 69.8 / 74.4 s |
  | lean, 6 threads | 86.8 / 99.0 s | 92.9 s (−11 %) | 50.6 / 71.5 s |
  | lean, 1 thread | 96.0 / 93.9 s | 94.9 s (−9 %) | 63.6 / 63.6 s |

  a modest, real win, mostly the NID catalog leaving the critical path (8–15 s) and 3,728 → 217 HLE initialisers; the metadata read of the framework assemblies costs about 4 s more than reflection did, and parallel `PrepareMethod` is a wash under TCG (one run 26.8 s). natively it is 0.62 → 0.48 s of warm-up.
- **NativeAOT payload: the step change** (fork `perf/android/nativeaot-v2` = `-p:SharpEmuHeadless=true -p:SharpEmuNativeAot=true`, linux-x64, 35 MB, no JIT, no `libcoreclr`): VM, the same bench, the same refusal (rc 3):

  | payload | runs | median | FEX code invalidations | SMC write faults | unaligned backpatches |
  | --- | --- | --- | --- | --- | --- |
  | R2R (baseline) | 3 + 1 | 91.6 s (the table above); 94.8 s on 2026-10-08 | 7,513 | 2,999 | 2,766 |
  | NativeAOT v1 | 4 | 27.6 s | 783–971 | **0** | 251–255 |
  | NativeAOT v2 (lean warm-up merged, full HLE coverage) | 6 | **25.4 s (3.6–3.7×)** | 781–910 | **0** | — |

  `Main` → end is about 12 s against R2R's 65–72 s (5–6×); what is left before `Main` is mostly the VM's 9p read of the binary. natively (x86-64, no FEX) it is 0.66 → 0.25 s. one v2 run of six took 55 s, the VM's known outliers. what it does not show yet: a phone, and a game. NativeAOT has no tiered PGO, so steady-state managed code may be up to ~10–15 % slower or faster than Tier1+PGO [estimate], against no JIT and no rejit SMC mid-game. under NativeAOT a static class has no runtime type handle unless code names it with `typeof`, so the warm-up's reflection sweep cannot run those initializers; the shader translators are warmed explicitly and `SHARPEMU_BOOT_TRACE=1` lists the rest (11 harmless ones on 0.0.3, 23 on 0.0.5-nexus — a source-generated `typeof` list is the real fix).
- **on a phone: Xiaomi 14 (Snapdragon 8 Gen 3, Android 15, 39-bit VA, SELinux enforcing, shell uid), FEX-2609, Firebase Test Lab, 2026-10-08.** the same fake eboot to the same refusal (rc 3 every run); cold rows interleaved round by round, 8 rounds; DiskCache rows 4 runs on a fresh cache, the median of runs 2–4:

  | payload | cold, median (range) | warm DiskCache | DiskCache size |
  | --- | --- | --- | --- |
  | R2R, legacy warm-up (today's launcher payload) | 2.41 s (2.36–2.51) | 1.59 s | 43 MB |
  | R2R, lean warm-up | 2.30 s (2.23–2.42), −5 % | 1.32 s, −17 % | 40 MB |
  | **NativeAOT v2** | **0.91 s (0.80–0.93), 2.6×** | **0.64 s, 2.5×** | 15 MB |
  | NativeAOT on upstream v0.0.5-nexus (fork `perf/android/nexus-aot`) | 0.89 s (0.86–0.98) | 0.67 s | 16 MB |

  the legacy row reproduces the history above (2.4–2.5 s cold, 1.50–1.56 s warm), so the harness measures what the earlier runs did. **NativeAOT cold is faster than ReadyToRun with a warm code cache.** where the 0.91 s goes: about 0.2 s before `Main` (FEX, the loader), 0.06 s to the runtime, the export registry 0.09 s, framework initialisers 0.18 s, HLE initialisers 0.21 s, then the refusal and exit. the same bench on Test Lab's arm64 Android 15 emulator (`MediumPhone.arm`) gives 2.07 / 1.77 / 0.67 s cold and 1.25 / 0.98 / 0.47 s warm for the first three rows. the nexus row also shows the compact address-space layout carrying the new memory model: `layout=compact top=0x3C00000000`, with nexus' 64–1008 GiB pre-reservation off on such hosts.
- **two x86-64 syscalls the host layer did not pass through**: `getcpu` (309, three calls per boot, from glibc's `sched_getcpu()` because rseq is refused) and `getgroups` (115). both now pass through, the regression set is still 15 of 19 with them, and a payload boot is down to one unhandled syscall, `get_mempolicy` (239), which .NET's NUMA probe asks and reads ENOSYS as *no NUMA* — the right answer on a phone, so it stays unhandled.
