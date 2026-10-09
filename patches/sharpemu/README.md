# SharpEmu fork changes behind the 2026-10-08 measurements

the SharpEmu side of [`docs/performance-roadmap.md`](../../docs/performance-roadmap.md)'s NativeAOT and lean-warm-up rows. they belong in the fork, and they are carried here as patches until there is a fork branch the `external/sharpemu` submodule can point at.

| file | applies to | what it is |
| --- | --- | --- |
| `0001-…boot-phase-trace…` | `external/sharpemu` at `f44351f` (`android-v0.0.3-release.4`) | `SHARPEMU_BOOT_TRACE=1`: one `[BOOT] uptime=… t=…ms <phase>` line per boot phase, free when off |
| `0002-…metadata-driven-warm-up…` | after 0001 | the lean HLE warm-up and the NID catalog parsed on its own thread; `SHARPEMU_WARMUP=legacy`, `SHARPEMU_WARMUP_THREADS=N`, `SHARPEMU_AEROLIB_PRELOAD=0` |
| `0003-…headless-NativeAOT-publish…` | after 0002 | `-p:SharpEmuHeadless=true -p:SharpEmuNativeAot=true` in the CLI csproj |
| `0004-…full-HLE-warm-up-coverage…` | after 0003 | the warm-up made NativeAOT-aware (rooted HLE assemblies, loaded-assembly walk, the shader translators' initializers) |
| `0005-…arm64-step-0…` | after 0004 | SharpEmu as a native `linux-bionic-arm64` process with a null CPU backend, and `SHARPEMU_BENCH=shader` (see `docs/native-arm64-backend.md`, *step 0: measured*) |
| `nexus-aot-on-v0.0.5-nexus.diff` | upstream tag `v0.0.5-nexus` | the android fork **and** 0001–0004 carried onto upstream v0.0.5-nexus: the android hosts, `HostAddressSpace`, the presenter changes moved into nexus' partials, nexus' guest-space pre-reservation turned off on compact (39-bit VA) hosts, and an ILC workaround in `GpuMemorySlabs.Block` |
| `0006-compat-run-PS5-native-homebrew…` | after `nexus-aot-on-v0.0.5-nexus.diff` (fork branch `compat/homebrew`, `fa518cc`, on `perf/android/nexus-aot` `06fd2f0`) | what PS5 homebrew needed: library-aware LLE redirects, HLE data symbols, the HLE heap reachable from HLE functions (also above 2⁴⁷ under FEX), twelve libc exports, VideoOut FPS log and detiled dumps. DOOM and Hello World run with it ([`poc/ps5-doom`](../../poc/ps5-doom/README.md)) |

```
git -C external/sharpemu am ../../patches/sharpemu/000[1-4]*.patch   # 0001-0004 on the pin (0005: the arm64 step 0)
py scripts/package-build.py --nativeaot                             # the NativeAOT payload
```

0001–0004 applied to `f44351f` give exactly the tree of the branch the measurements were taken from (`perf/android/nativeaot-v2`, `3628d90`).
