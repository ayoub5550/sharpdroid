# the path toward GTA 6 on a phone

what it would take for GTA 6 to run through sharpdroid, which part of that this repository can change, and what it cannot. written on 2026-10-08, six weeks before the game ships. the markers mean what they mean in [`performance-roadmap.md`](performance-roadmap.md): **[measured]**, **[source]**, **[estimate]**.

**the short answer: not at launch, and not on a 2026–27 phone at a playable frame rate.** the emulator side can be made several times faster, and this branch does part of that. the hardware gap and the gates below are not engineering problems this repository can solve.

## the target

- GTA 6 ships on **2026-11-19 on PS5 and Xbox Series only**, capped at **30 fps** on consoles. no PC version has been announced [source]. so Winlator / GameHub (windows games on android) is not a route until a PC port exists. GTA V's PC port came about 18 months after the console release [source].
- a PS5 game gets about 12.5 GB of memory, 7 Zen 2 cores, a 10.28 TFLOPS RDNA2 GPU with 448 GB/s of bandwidth, and hardware Kraken decompression that Cerny priced at about 9 Zen 2 cores of work [source].

## the gap, per axis

| axis | phone vs PS5 (Snapdragon 8 Elite Gen 5 class) | why |
| --- | --- | --- |
| CPU | **0.3–0.6×** [estimate] | about 11–12k Geekbench 6 multi-core natively, × 0.35–0.6 for FEX on AVX2-heavy, multithreaded code, × 0.65–0.75 sustained clocks; plus Kraken in software |
| GPU | **0.2–0.35×** [estimate] | Adreno 840 at about 3.7 TFLOPS [source], before throttling and before the cost of translating Gen5 shaders to SPIR-V |
| memory bandwidth | **0.15–0.19×** [source] | 85 vs 448 GB/s. this is the hardest limit for an open world |
| memory capacity | **marginal**, 24 GB phones only [estimate] | 12.5 GB game + 2–4 GB emulator + 4–6 GB android |

**put together [estimate]: 5–12 fps, well below 720p, with perfect compatibility.** playable needs about three to five more phone generations (around 2030), or a PC port that Winlator / GameHub can run.

## the gates nobody in this repository controls

1. **SharpEmu compatibility.** 76 of 9,176 titles are tested upstream and 24 are playable [source]. the heaviest PS5 games are not yet playable in any PS5 emulator, even on a desktop. GTA 6 will start at zero.
2. **a decrypted dump.** SharpEmu refuses encrypted images (the bench's own fake eboot is refused with exactly that message). a dump needs a PS5 on exploitable firmware, and GTA 6 will likely require firmware nobody can dump from yet. this repository does not and will not help with decryption.
3. **translation limits.** x86 TSO on arm64 needs LRCPC3 hardware to get cheap, and no phone SoC is confirmed to have it [source]. no phone has 256-bit vectors, so every AVX2 op is at least two NEON ops [source].
4. **android GPU drivers.** Turnip and vendor Vulkan quality decides whether the translated shaders run at all.
5. **legal.** Rockstar / Take-Two enforce their copyrights. only a dump of a copy you own, made by you, is in scope.

## what this repository can change

the emulator itself is the part under our control. today the whole of SharpEmu — CoreCLR, its JIT, the HLE, the shader translator — runs translated through FEXCore, so every speed-up of the emulator's own code is multiplied by the translation tax it no longer pays.

| step | status | what it buys | effort |
| --- | --- | --- | --- |
| **NativeAOT payload** (fork branch `perf/android/nativeaot-v2`, `package-build.py --nativeaot`) | built, VM- and phone-measured on 2026-10-08 | **Xiaomi 14 boot 2.41 → 0.91 s cold (2.6×), 1.59 → 0.64 s warm** [measured]; VM 91.6–94.8 s → 25.4 s; FEX code invalidations 7,513 → 785, SMC write faults 2,999 → 0 [measured, VM]. no JIT under translation also means no tiering rejits mid-game, so less stutter [estimate] | done, needs real-game validation |
| **lean HLE warm-up + parallel NID catalog** (`perf/android/lean-warmup`, also inside the AOT branch) | built and measured | R2R boot −5 % cold, −17 % warm on the Xiaomi 14; −9 to −11 % in the VM [measured] | done |
| **native arm64 SharpEmu**, FEXCore only for PS5 code (the Arm64EC model) | design: [`native-arm64-backend.md`](native-arm64-backend.md) | emulator-side code 1.7–2.9×, cold boot 2.5–5×, HLE-heavy frames 1.1–1.4×, render/shader-bound frames 1.7–2.5× [estimate] | 26–40 engineer-weeks; proof of concept 2–3 weeks |
| GPU: cache translated SPIR-V, async pipelines, render scale + FSR1 | roadmap #9–#11 | shader stutter; GPU 1.5–2.5× effective [estimate] | M |
| CPU: per-game memory-order profiles, LRCPC detection | roadmap #7–#8 | 10–30 % on eligible titles [estimate] | S, risky |

none of these moves the GTA 6 estimate out of single digits on its own. together they decide whether *lighter* PS5 games play well, which is the realistic target until the hardware catches up.

## how to know where we stand: GTA V as the calibration game

GTA V (the PS5 version) is on SharpEmu's compatibility list, and v0.0.5-nexus carries patches for it [source]. it is the same engine family, and it is the closest thing to GTA 6 that can run before GTA 6 exists. the plan:

1. bring the fork up to upstream v0.0.5-nexus (the GTA V and Astro Bot patches) with the NativeAOT branch on top — done locally as `perf/android/nexus-aot`, which boots to the same refusal on the Xiaomi 14 in 0.89 s; it still needs a game run.
2. once GTA V reaches in-game in sharpdroid, record fps, frame-time percentiles, CPU vs GPU bound, and memory peak on one reference phone, with your own legally made dump.
3. scale by GTA 6's console budget (30 fps cap, heavier CPU and streaming) to get a measured, not guessed, GTA 6 estimate, and re-run it on each new phone generation.

until step 2 exists, every GTA 6 frame rate in this document is an estimate.
