# the dry lab: decide with cheap compute, confirm with one real run

> **الخلاصة.** الحوسبة هنا رخيصة: 17 نواة x86، و VM بمعمارية arm64، وساعات من التشغيل. أمّا النادر فهو تشغيلات الهاتف (حصّة
> Test Lab اليومية)، ونُسخ الألعاب الشرعية التي لا يشغّلها إلا صاحب المستودع، وأسابيع الهندسة. الطريقة مأخوذة من المختبر الجافّ في
> مستودعَي `bagel` و`azrak-thabet`، وخطواتها أربع:
> 1. نشغّل كل ما يمكن تشغيله حاسوبيًّا.
> 2. نرتّب المجاهيل بتحليل حساسية (Sobol).
> 3. نكتب قاعدة القرار **قبل** التجربة.
> 4. لا نصرف تشغيلًا حقيقيًّا إلا على السؤال الذي يحسم.
>
> ولكل أداة حدود: الـ VM يحكم على الصحّة، ولا يحكم أبدًا على النسبة بين native والمترجَم. الهاتف وحده يحكم على الزمن.

status: method, first applied on 2026-10-08 to the FexCpuBackend proof of concept ([`native-arm64-backend.md`](native-arm64-backend.md)). **[measured]**, **[source]** and **[estimate]** mean what they mean in [`performance-roadmap.md`](performance-roadmap.md).

## why

The scarce things in this project are:
- phone runs (Firebase Test Lab counts physical-device runs per day);
- the owner's own legally dumped games, which nobody else can run;
- engineer-weeks: the native arm64 backend alone is estimated at 26–40.

The cheap things are a 17-core x86 sandbox, an arm64 VM, and compute time. So:
1. Compute everything that can be computed.
2. Rank the unknowns.
3. Spend the expensive run only on the unknown that decides.

The method comes from the owner's research repositories (`bagel`'s *dry lab* and `azrak-thabet`'s computational lab). There, Monte Carlo plus Sobol sensitivity ranks what to measure first, and the result is usually "the next step is one cheap physical measurement, not more computation".

## the instruments, and what each may decide

| instrument | what it is | may decide | may **not** decide |
| --- | --- | --- | --- |
| **real x86** (the sandbox) | the x64 model runs natively; .NET, gcc and clang are all here | whether the guest blob and the C# side are correct (the oracle); x86-native reference times | anything about arm64 |
| **the VM** ([`vm.md`](vm.md), QEMU TCG, `-cpu max,sve=off`, 16 vCPU) | an arm64 Linux with bionic, where FEX's JIT and NativeAOT `linux-bionic-arm64` both run | correctness of every arm64 path: FEX JIT, signals, GC, threads, unaligned atomics. Also the cost of two variants of the *same* path (FEX knob A vs B), and regressions | absolute times. **TSO barrier cost**: TCG makes `dmb` and `ldapr` almost free. **Native vs translated ratios**: TCG's cost per instruction class differs from a core's, and indirect branches and calls are especially expensive under TCG |
| **Test Lab virtual** (`MediumPhone.arm`, API 35) | a real Android userland: bionic linker, SELinux, `/data/local/tmp` | packaging, linking, permissions, start-up failures | times and ratios: it runs as root on an emulator, and its step-0 ratio was off too (below) |
| **Test Lab physical** (`houji`, Xiaomi 14, Snapdragon 8 Gen 3) | the phone | **time**, every ratio, and TSO cost | — |

**Measured reasons for the "may not" column:**
- Step 0, the emulator side with no guest: native boot took 0.15–0.16 s on the virtual device against 0.74–0.98 s under FEX, a ratio of about 4.6–6.5×. The phone's ratio was 0.305 s against 0.87 s, about 2.9× **[measured]**.
- The PoC import boundary in the VM measured 2.1–3.3 µs under the FEX model and 0.69–0.73 µs under the translated model. TCG puts most of that in the reverse-P/Invoke transition (*ledger* below). Only the phone can say whether this survives on silicon.

## the rules

1. **Write the decision rule before the run.** Put it in this file or in the PR: the metric, the threshold, and what each outcome changes. A run without a rule gets interpreted afterwards to fit what we already wanted.
2. **Correctness gates speed.** Every bench checks an exact result. A configuration that fails any check is out, whatever its speed.
   - `TSOEnabled=0` is never a candidate: it corrupts silently.
   - Knobs that change guest-visible semantics (`X87ReducedPrecision=1`, `HalfBarrierTSOEnabled=0`) go only into per-game profiles with evidence (roadmap #7). They never become a global default.
3. **Noise has a number.**
   - The VM boot bench spreads about 10 %, so smaller differences do not count.
   - Benches report min and median over ≥ 3 timed rounds, after one untimed warm-up round.
   - On the phone, variants run interleaved round by round, so heat drifts across all of them alike.
4. **The device budget.**
   - Virtual device first, to catch packaging problems.
   - Physical runs only for the top 3–5 candidates, all in **one bundle per run**.
   - Every device run gets a line in the ledger saying what it decided.
5. **The VM never ranks native against translated, and never ranks TSO variants.** Those questions go straight to the device list.

## sensitivity before search (the autotuner)

The knob space is about 20 knobs. The FEX names below are the json names from `external/FEX/FEXCore/Source/Interface/Config/Config.json.in`.

| group | knobs | VM can rank? |
| --- | --- | --- |
| FEX JIT | `Multiblock`, `MaxInst`, `DisableL2Cache`, `DynamicL1Cache`, `DynamicL1CacheIncreaseCountHeuristic`, `VolatileMetadata`, `DiskCache` | yes (same path, different code) |
| FEX memory order | `VectorTSOEnabled`, `MemcpySetTSOEnabled`, `HalfBarrierTSOEnabled` | **no**: TCG barriers are free; device only |
| NativeAOT | `OptimizationPreference` (Speed/Size), `IlcInstructionSet` (arm64 baseline: LSE, RCPC, …), GC gen0 budget, conserve-memory, concurrent GC | partly. Code-size and instruction-count effects yes; instruction-set effects device only |
| the shim | `-O2`/`-O3`, LTO, `-mcpu` tuning | partly |
| the phone | thread placement (prime/mid cores), ADPF hints | **no**: device only |

**How:**
1. Morris screening first: (k + 1) · r runs, so 21 × 10 = 210 runs for k = 20.
2. Then Sobol (Saltelli sampling via SALib) on the ~6 survivors: N · (2k + 2) runs, so 128 × 14 = 1,792 runs.
3. The VM runs them one at a time, so no run perturbs another's timing. The sandbox builds variants in parallel while the VM measures.
4. The metrics are boot to guest entry, `hle_add`, `hle_callback`, `guest_compute`, and the shader bench.
5. The outputs are S1/ST indices per knob per metric. Knobs with ST < 0.05 are frozen at their default.
6. The best 3–5 configurations that pass every correctness check go to the phone, in one bundle.

## experiment 0.5: the HLE share *h*

The native backend speeds up only the emulator side. Guest code stays translated, so `guest_compute` is the same in both models **[measured, VM]**. On CPU-bound guest threads, Amdahl's law sets the frame-time gain:

> speedup = 1 / ((1 − h) + h / s)

- *h* is the share of a guest thread's time spent in HLE (C#);
- *s* is the emulator-side speedup, measured on the phone (step 0's boot was 2.9×, and `csharp_body` in the PoC measures it directly).

| *h* | speedup at s = 2.9 | decision |
| --- | --- | --- |
| < 0.15 | < 1.11× | do not build the 26–40-week backend for frame rate. Keep the boot win, which NativeAOT x64 already gives (roadmap #2) |
| 0.15–0.35 | 1.11–1.30× | build only if the phone shows the import boundary no worse than today's (PoC rule below) |
| ≥ 0.35 | ≥ 1.30× | strong case: go |

*h* has never been measured on a real game. It is cheap to measure: `SHARPEMU_PERF_HLE=1` (in `DirectExecutionBackend.Diagnostics.cs`) on the owner's own legally dumped title, first on an x86 desktop as a proxy, then on the phone. **This is the experiment the whole backend decision hangs on.** Nothing here supplies, downloads or decrypts games.

## the ledger

| date | question | instrument | rule written before | result | decided |
| --- | --- | --- | --- | --- | --- |
| 2026-10-08 | does a NativeAOT x64 payload boot faster under FEX? | VM → houji | — | 2.41 → 0.91 s cold, 1.59 → 0.64 s warm **[measured]** | built as roadmap #2 (PR #6) |
| 2026-10-08 | step 0: native emulator vs FEX | VM, virtual, houji | — (written after; the reason rule 1 exists) | the virtual device gave ~4.6–6.5× where the phone gave 2.9× | rule 5: the VM never ranks native vs translated |
| 2026-10-08 | PoC: does FEXCore-as-a-library meet SharpEmu's contract? | x86, VM | "all checks pass, or the design changes" | **PASS** in the VM: args, regs, nested callback, transfer, block/resume, 4 threads × 10⁶ imports under 3,997 forced GCs, unaligned atomics, .NET null-ref | go to the phone |
| 2026-10-08 | PoC: does it run on real Android (bionic linker, `/data/local/tmp`, dlopen, the fixed guest mapping)? | Test Lab virtual | "all checks pass, or fix before spending a phone run" | **PASS**, every check, 3 interleaved rounds. Times are not read (rule 5) | go to houji |
| 2026-10-08 | PoC: is the import boundary affordable? | houji | see *PoC rule* | **B_fex ≈ B_x64: 41.4 vs 40.3 ns** (ratio 1.03). Callbacks 2.8× *slower*. All checks PASS on the phone | rule row 2: proceed; leaf imports via slot functions; the callback path and FEX's boundary are the work (*PoC: measured* below) |

### PoC rule (written before the phone run)

- *B_fex* is `hle_add` ns/op in the fex model (native arm64 process, guest through libsharpfex).
- *B_x64* is `hle_add` ns/op in the x64 model under the host layer (today's design). Both are measured on houji in the same bundle, interleaved.

| outcome | what it changes |
| --- | --- |
| B_fex ≤ B_x64 | the boundary is no obstacle. The backend's value is set by *h* alone: measure it next |
| B_x64 < B_fex ≤ 2 · B_x64 | proceed. Hot leaf imports go through slot functions (`sfx_set_slot_fn`, no copy) or stay in translation as x86/LLE |
| B_fex > 2 · B_x64 | before any further backend work, the boundary itself is the project: FEX's `HLECALL` opcode (no block exit, a partial spill) and a cheaper managed transition |

### PoC: measured on houji (2026-10-08)

The rule above was committed (`0eb6a9e`) before this run. The run was 3 interleaved rounds × 3 processes (fex, x64, fex with `sfx.CallSave=full`), 10⁶ imports, 5 timed rounds each, with medians across processes **[measured]**:

| ns per op | fex model | x64 model (today) | ratio |
| --- | --- | --- | --- |
| `hle_add` (B) | 41.4 (40.1–48.1; one 67.8 outlier) | 40.3 (40.2–42.8) | **1.03: a tie** |
| `hle_add_builtin`: FEX's boundary alone | 28.3–30.8 | — | 70 % of B |
| `hle_add_direct`: plus the managed transition | 34.3–37.1 | — | +6 ns |
| `hle_callback` | 140.6–158.1 | 54.0–54.2 | **2.6–2.9× slower** |
| `hle_hash64` (latency-bound C# body) | 95.5–104.5 | 93.1–95.0 | 1.0 |
| `csharp_body`: C# alone | 2.4–3.0 | 6.7–6.9 | **2.2–2.9× faster** |
| `guest_compute` (the control) | 4.1–4.4 | 4.1 | 1.0 |

- **The virtual device would have misled us.** It measured `hle_add` at 28.5 vs 77.7 ns, a 2.7× win for the fex model; on silicon it is a tie. That is rule 5 again, now on a third instrument: the virtual device exaggerates what translation costs the C# side, as it did in step 0.
- **The import boundary is a wash.** Of its 40 ns, 28 are FEX's own boundary (spill everything, end the block, look up the `ret`). The C# transition adds 6 ns and the register copy 5 ns. So the lever is FEX's `HLECALL` op from the design: no block exit, a partial spill **[estimate: ~10–15 ns]**.
- **Callbacks regressed**, by ~100 ns per host→guest call (`HandleCallback`'s dispatcher entry and exit). `sfx.CallSave=light` buys 2–10 % of it. This has to be fixed before the backend; the target is a re-entry that skips the dispatcher prologue.
- **The C# side is 2.2–2.9× faster natively**, matching step 0's 2.9× boot. Guest code costs the same in both models.
- **Correctness on a real phone** (Android 15, SELinux enforcing, shell uid): every check passes. That includes 4 guest threads × 5·10⁶ imports under 88 forced GCs, 8 threads × 2·10⁶ under 43, and the unaligned-atomic backpatch.

**What it changes for experiment 0.5.** The boundary costs the same in both models, so the frame-time gain on a guest thread comes only from time spent *inside* HLE bodies, not from the HLE share as a whole. *h* should therefore be measured as body time. Imports dominated by their entry cost gain nothing until `HLECALL` lands.

How to read the split: `hle_add_builtin` is FEX's boundary alone (C++), `hle_add_direct` adds the managed transition with no copy, and `hle_add` adds the register copy. Together they say which of the three to attack.

## how to run

- PoC build: `poc/fexpoc/build.sh` builds, into `build/fexpoc/`:
  - the guest blob;
  - both FexPoc publishes;
  - `libsharpfex.so` (stripped);
  - a VM payload;
  - `device/bundle.tar` plus `device/run.sh` (a copy of `poc/fexpoc/run-device.sh`).
- VM: `scripts/vm.py run --payload build/fexpoc/vm -- "sh ./payload/poc.sh"` ([`vm.md`](vm.md)).
- Phone: push `bundle.tar` and `run.sh` to `/data/local/tmp/sharpdroid/`, extract, and run `run.sh`. Test Lab's instrumentation test does exactly that; `results/summary.txt` is the output.
