# DOOM as a PS5 test title

A free, legal PS5 program to run SharpEmu against: [doomgeneric](https://github.com/ozkl/doomgeneric) (GPL-2.0), built as a native PS5 app with the [PS5 native-app boilerplate](https://github.com/blackbearreloaded/ps5-native-app-boilerplate) (GPL-3.0, a clean-room `libc.prx` runtime and FSELF tooling over the public ps5-payload-sdk), playing the DOOM v1.9 shareware IWAD.

No commercial game, dump or key is involved anywhere. The IWAD is id Software's shareware episode, which its licence lets anyone pass on unmodified. `build.sh` fetches it pinned by sha256 and never commits it here. `IWAD=freedoom` builds with Freedoom (BSD-3) instead.

## what is here

| file | what it is |
| --- | --- |
| `doomgeneric_ps5.c` | the PS5 platform layer: VideoOut (two tiled 1920×1080 RGBA8 buffers in direct memory, CPU-drawn), the pad, the clock, `assets/args.txt` as the command line |
| `doomgeneric_bench.c` | its host twin: the same per-frame conversion with no emulator, the native baseline |
| `param.json` | title `PPSA96660`, content id `UP9000-PPSA96660_00-DOOMGENERIC00001` |
| `build.sh` | fetches the pinned sources, then builds `build/ps5-doom/{PPSA96660, doom-td, native/}` |
| `run-device.sh` | the phone run: the timedemo under SharpEmu, the x86-64 twin under FEX, the arm64 twin, then 60 s of attract mode with the Android presenter |

With no input, the game plays its attract-mode demos, which is real gameplay. `doom-td` is the same app with `-timedemo demo1`: 5,026 tics of recorded play, rendered as fast as possible, ending in `timed N gametics in M realtics (F fps)`.

## build and run

```sh
SHARPDROID_NDK=… bash poc/ps5-doom/build.sh   # NDK r29; also g++, make, python3, wget, unzip
# x86-64 Linux, no phone (the headless or NativeAOT payload from scripts/package-build.py):
SHARPEMU_NO_FLIP_PACING=1 <payload>/SharpEmu build/ps5-doom/doom-td/eboot.bin
SHARPEMU_DUMP_VIDEOOUT=1 SHARPEMU_DUMP_VIDEOOUT_EVERY=60 <payload>/SharpEmu build/ps5-doom/PPSA96660/eboot.bin
```

The boilerplate wants `clang-18` and `llvm-config`. `build.sh` points both at the NDK's clang 21, which builds a byte-identical `libc.prx`; the boilerplate checks its sha256.

SharpEmu switches used with it: `SHARPEMU_NO_FLIP_PACING=1` (flips do not wait for 60 Hz), `SHARPEMU_LOG_FPS=1` (flips per second every 2 s), and `SHARPEMU_DUMP_VIDEOOUT=1` with `_EVERY=N`, `_MAX=N` and `_DIR=path` (BMP frame dumps; tiled buffers are detiled).

## what it took in SharpEmu

These are on the fork branch `compat/homebrew`, as a patch in [`patches/sharpemu/`](../../patches/sharpemu/). Homebrew links differently from retail titles, and each item below was a crash or a wrong result:

- **Library-aware LLE redirects.** The runtime's `libc.prx` exports stub `malloc`/`free` that return 0. An import is now sent to a loaded module's export only when that module exports it under the library the import names; otherwise it goes to HLE. Set `SHARPEMU_IGNORE_IMPORT_LIBRARY=1` for the old behaviour.
- **HLE data symbols** `__stdinp`/`__stdoutp`/`__stderrp`/`__isthreaded`, and `sceKernelSendNotificationRequest` (logged as `[KERNEL][NOTIFY]`).
- **The HLE libc heap is visible to HLE functions.** `fread`/`fgets`/`fwrite`/`memset` into a `malloc`'d block work, including host pointers above 2⁴⁷ on 48-bit arm64 hosts (FEX). Without this, `W_Init` fails in the arm64 VM.
- **Direct memory and the map search base** no longer overlap.
- **Twelve libc HLE exports:** `strncasecmp`, `puts`, `putc`, `putchar`, `mkdir`, `system`, `toupper`, `atoi`, `atof`, `strdup`, `remove`, `__swbuf`. Also printf integer precision (`%.3d`).

## results

[`docs/dry-lab.md`](../../docs/dry-lab.md) has the decision rule, written before the phone run, and the ledger.

- **x86-64 sandbox (NativeAOT payload):** plays; the timedemo runs at 97–99 % of the same C code built natively with clang.
- **arm64 VM under FEX:** plays (title screen, demo gameplay); times not read.
- **Phone:** see the ledger.
