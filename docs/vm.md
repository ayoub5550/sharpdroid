# the VM loop

running the host layer on a linux x86-64 machine with no phone attached: `scripts/vm.py` boots a qemu-system-aarch64 virtual machine whose userspace is android's own bionic, and runs the shell binary and its regression set inside it.

```
SHARPDROID_NDK=… SHARPDROID_CMAKE=… python3 scripts/build.py --only guest-libs   # and adrenotools, thunks, host, guests
python3 scripts/vm.py fetch
python3 scripts/vm.py regression
python3 scripts/vm.py run -- ./sharpdroid-host-layer ./hello-libc
```

**the payload runs in it too.** `run --payload build/builds/<id>` copies a packaged SharpEmu build into the run directory as `./payload`, with `scripts/vm-boot-bench.sh` beside it:

```
python3 scripts/vm.py run --timeout 5400 --payload build/builds/<id> -- sh ./vm-boot-bench.sh
```

the bench hands SharpEmu an eboot that is four kilobytes of zeros, so the runtime is created in full — every HLE module registered and warmed, the loader and the CPU backend constructed — and then refuses the file. that is the part of a boot that is the emulator rather than the game, and it prints one `BENCH <config> run=N rc=3 seconds=S` line per run. `rc=3` is the refusal and is the expected status. the whole bench is about 40 minutes; a subset is one quoted command, since `run` joins its arguments into one shell line:

```
python3 scripts/vm.py run --timeout 2400 --payload build/builds/<id> -- "sh -c 'BENCH_ONLY=baseline,diskcache-ram sh ./vm-boot-bench.sh'"
```

`BENCH_RUNS` sets the runs per row (3). the output appears when qemu exits; `build/vm/run/vm-out.log` fills while it runs.

**it is the loop for anything that is not graphics or audio**: the JIT, the syscall layer, signals, SMC tracking, threads, the pause, the pad bridge, and boot-time work on the payload. a phone is still the only place a number about speed is a number about a phone — see *what a VM measurement is worth* below.

## what is in the VM

| | |
| --- | --- |
| the CPU | qemu's `max` with SVE turned off, 16 vCPUs on multi-threaded TCG. `--cpu` and `--smp` change either |
| the kernel | debian's arm64 6.1 kernel, booted directly with an initramfs that holds busybox and the virtio-9p modules and nothing else |
| bionic | the linker, libc and every library the host layer links (libandroid, liblog and what they pull in), out of google's arm64 API 34 system image, apexes included |
| the files | two virtio-9p shares at the same absolute paths they have on the host: the android tree read-only, `build/vm/run/` read-write |
| no GPU, no audio | so `vulkan`, `vkrender`, `vkswap` and `aaudio` fail. `vm.py regression` counts them apart and does not let them turn a run red, and says which they were |

`vm.py fetch` needs no root. qemu is unpacked from debian packages (`apt-get download`) unless `SHARPDROID_QEMU` names a qemu-system-aarch64 of your own, 7.2 or newer; the android image is read with `debugfs`, so e2fsprogs is the one host package it asks for. everything lands under `toolchain/vm/`, and the regression run under `build/vm/`.

on a machine without a phone, three overrides are all a native build needs: `SHARPDROID_NDK` at an r29 NDK, `SHARPDROID_CMAKE` at a directory holding cmake **3.22** and ninja, and nothing else — no android SDK and no JDK. the cmake version is not a preference: a newer cmake (3.28+) turns on C++20 module scanning, and fmt's module unit then fails to compile.

## how it is built, piece by piece

everything below is what `vm.py fetch` and `vm.py run`/`regression` do, in order, written out so that the VM can be rebuilt by hand, moved to another machine, or debugged when one of the steps stops working. nothing needs root, and nothing is installed system-wide.

### 1. qemu-system-aarch64, unpacked rather than installed

`fetch_qemu()` asks `apt-cache depends --recurse` for `qemu-system-arm`'s dependency closure, `apt-get download`s every package in it into `toolchain/vm/debs/`, and unpacks each `.deb` into `toolchain/vm/qemu/`. a `.deb` is an `ar` archive holding a `data.tar.*`, and `unpack_deb()` reads it with the python standard library alone — no `dpkg`, no root.

the unpacked tree holds a whole debian base, glibc included. `qemu_library_path()` asks `ldd` which libraries the qemu binary cannot find, adds only the directories that hold those, and asks again until nothing is missing; the result is passed to qemu alone, never exported. on debian 12 that comes to two directories, `toolchain/vm/qemu/lib/x86_64-linux-gnu` and `…/usr/lib/x86_64-linux-gnu`, for `libfdt`, `libslirp`, `libcapstone`, `libpixman`, `liburing` and about forty more. **those directories hold the unpacked `libc6` too**, so qemu runs against the packaged glibc, which is only safe because the packages come from the host's own release (`apt-get download` fetches for the machine it runs on). on a host where that is not true, install qemu yourself and use `SHARPDROID_QEMU`.

on debian 12 this is qemu **7.2** (`1:7.2+dfsg-7+deb12u18`). `SHARPDROID_QEMU=/path/to/qemu-system-aarch64` skips the step and uses yours; anything 7.2 or newer works.

### 2. the kernel: debian's stock arm64 6.1

`newest()` reads the directory listing of `deb.debian.org/debian/pool/main/l/linux-signed-arm64/`, picks the newest `linux-image-6.1.0-*-arm64` package, and unpacks it into `toolchain/vm/kernel/`. two things are taken from it:

- `boot/vmlinuz-6.1.0-*-arm64`, booted directly with `-kernel`. no bootloader, no firmware, no disk image
- eight modules out of `lib/modules/*/kernel/`, because debian builds the virtio-pci transport and 9p as modules: `virtio_pci_legacy_dev`, `virtio_pci_modern_dev`, `virtio_pci`, `netfs`, `fscache`, `9pnet`, `9pnet_virtio`, `9p`, in that load order

a stock kernel rather than an android one is deliberate: the host layer talks to the kernel only through ordinary linux syscalls (mmap, mprotect, signals, futex, `/proc/self/maps`), and those are the same. what an android kernel adds — binder, ashmem, the GPU driver — is nothing the host layer's shell binary touches.

### 3. busybox, for the VM's only userspace that is not android's

`busybox-static` for arm64, same pool, same unpacking, into `toolchain/vm/busybox/usr/bin/busybox`. it is the VM's `init`, its shell, `mount`, `insmod`, `timeout` and `poweroff`. it is statically linked, so it needs nothing from the android tree.

### 4. android's bionic, out of google's own system image

the point of the whole VM: **the host layer runs against the same bionic linker and libraries it runs against on a phone**, not against glibc and not against the NDK's stubs. `extract_android()`:

1. downloads `arm64-v8a-34_r04.zip`, google's arm64 API 34 emulator system image (673 MB), and pulls `system.img` out of it
2. `system.img` is a GPT disk. `gpt_partition()` reads the GPT header and finds the partition named `super`
3. `super` holds android's dynamic partitions. `lp_partitions()` parses its liblp metadata (magic `0x414C5030`, at 4 KiB + 2 × 4 KiB into the partition) and yields each logical partition's name and linear extent. the `system` partition's extent is copied out as a plain ext4 image
4. `debugfs -R "rdump …"` (from e2fsprogs, the one host package the VM asks for) copies `/system/bin`, `/system/lib64`, `/system/etc` and `/system/apex` out of the ext4 **without mounting it**, which is why no root is needed
5. every `.apex` (and `.capex`, whose real apex is the `original_apex` inside it) is a zip holding `apex_payload.img`, another ext4. each is extracted with `debugfs` into `android/apex/<name>/`. that is where `libc.so`, `libm.so`, `libdl.so` (`com.android.runtime`), `libc++.so` and the rest actually live on API 34
6. the image's symlinks are absolute (`/system/lib64/libc.so -> /apex/com.android.runtime/lib64/bionic/libc.so`). each is rewritten to point under `toolchain/vm/android/`, and the tree is mounted inside the VM **at that same absolute path**, so every link resolves identically on both sides

the result is 832 MB under `toolchain/vm/android/`.

### 5. the run directory

`stage()` does what `stage.py --shell` does for a phone, into `build/vm/run/` instead of `/data/local/tmp/sharpdroid`: the shell binary `sharpdroid-host-layer`, the NDK's `libc++_shared.so`, every x86-64 test guest, the x86-64 glibc set under `guest-libs/`, and `host/regression.sh` with one line changed — on a phone the linker's namespace config finds the apex libraries, the VM has no generated config, so the apex `lib64` directories are put on `LD_LIBRARY_PATH` instead (`library_path()`, exported to the script as `$VM_LIBS`).

`run --payload build/builds/<id>` additionally copies a packaged SharpEmu build to `build/vm/run/payload/` and `scripts/vm-boot-bench.sh` beside it. the payload is copied rather than shared because the run directory is the VM's only writable share, and .NET writes beside itself.

`boot()` then writes `vm-cmd.sh`, the one command the VM will run:

```
export VM_LIBS=<every apex lib64, and system/lib64>
export LD_LIBRARY_PATH=$PWD:$VM_LIBS
export REGRESSION_TIMEOUT=<--timeout>
timeout <--timeout> <your command>
```

### 6. the initramfs, written by hand

`write_initramfs()` builds a gzipped `newc` cpio archive in python, byte by byte (the `070701` header, 4-byte padding, `TRAILER!!!`), so there is no dependency on `cpio` or `mkinitramfs`. it holds directories, `bin/busybox`, the eight modules, and `/init`:

```
#!/bin/busybox sh
/bin/busybox --install -s /bin
mount proc, sysfs, devtmpfs
insmod the eight modules, in order
mount -t 9p -o trans=virtio,version=9p2000.L,msize=1048576,cache=loose android <toolchain/vm/android>
mount -t 9p -o trans=virtio,version=9p2000.L,msize=1048576             run     <build/vm/run>
ln -s <android>/system /system; ln -s <android>/apex /apex; ln -s <run> /data/local/tmp/sharpdroid
cd <run>
sh ./vm-cmd.sh > ./vm-out.log 2>&1
echo $? > ./vm-status
sync; poweroff -f
```

`/system` and `/apex` are symlinked at the root because bionic's linker looks for itself and its config there. `cache=loose` is only on the read-only android share, where nothing changes under it; the run share is uncached so that what the VM writes is on the host the moment it exits.

### 7. the boot

```
qemu-system-aarch64 -M virt,gic-version=3 -cpu max,sve=off \
  -accel tcg,thread=multi,tb-size=1024 -smp 16 -m 16384 \
  -nographic -nic none -no-reboot \
  -kernel toolchain/vm/kernel/boot/vmlinuz-6.1.0-*-arm64 -initrd build/vm/initramfs.gz \
  -append "console=ttyAMA0 quiet loglevel=3 rdinit=/init" \
  -fsdev local,id=a,path=<toolchain/vm/android>,security_model=none,readonly=on \
  -device virtio-9p-pci,fsdev=a,mount_tag=android,romfile= \
  -fsdev local,id=r,path=<build/vm/run>,security_model=none \
  -device virtio-9p-pci,fsdev=r,mount_tag=run,romfile= \
  -L toolchain/vm/qemu/usr/share/qemu
```

- `virt,gic-version=3`: the generic arm64 board, with the interrupt controller a modern SoC has
- `-cpu max,sve=off`: every armv8/v9 feature qemu implements (atomics, LRCPC, the lot) except SVE — see the findings below for why
- `tcg,thread=multi`: one host thread per vCPU, so 16 vCPUs really run in parallel; `tb-size=1024` gives TCG a 1 GiB translation cache, because FEXCore's own JIT output is code TCG has to translate again and a small cache thrashes
- `-nic none`: the VM needs no network; `-no-reboot` with `poweroff -f` makes qemu exit when the command is done
- `security_model=none` on the 9p shares: files keep the host user's ownership and no xattrs are needed, which is what lets all of it run without root
- `romfile=` on each virtio-9p device gives it no PCI option ROM; there is no firmware to run one, and the unpacked qemu would otherwise go looking for the file

qemu's own console goes to `/dev/null`. the command's output comes back through `build/vm/run/vm-out.log` and its exit status through `build/vm/run/vm-status`; a missing status file means the VM never got as far as running it, and `vm.py` says so.

### booting it by hand

when a run reports nothing, boot it with the console visible. `vm.py run` leaves `build/vm/initramfs.gz` and `build/vm/run/vm-cmd.sh` behind; run the qemu line above yourself without redirecting its output, and drop `quiet loglevel=3` from `-append` to see the kernel. putting `exec sh` in place of `sh ./vm-cmd.sh …` in the init (edit `write_initramfs()`) gives an interactive busybox shell inside the VM with the android tree mounted. the qemu binary needs the `LD_LIBRARY_PATH` `qemu_library_path()` computes — print it with `python3 -c "import sys; sys.path.insert(0,'scripts'); import vm; print(vm.qemu_library_path(vm.VM/'qemu/usr/bin/qemu-system-aarch64'))"`.

### building everything with no phone and no android SDK

the native steps need three things, all fetched to a scratch directory and pointed at with the resolver's overrides:

| | |
| --- | --- |
| `SHARPDROID_NDK` | android NDK **r29** |
| `SHARPDROID_CMAKE` | a directory holding cmake **3.22** and ninja (1.12). not 3.28+: see above |
| `SHARPDROID_DOTNET` | a .NET 10 SDK, only for `package-build.py` producing a SharpEmu payload |

then `build.py --only guest-libs`, `--only adrenotools`, `--only thunks`, `--only host` and `--only guests`, followed by `vm.py fetch` once and `vm.py regression`. the android SDK and the JDK are only needed for the APK. on a machine with many cores, check `OMP_NUM_THREADS` and similar limits in your environment: some sandboxes set them low, and ninja and .NET honour the core count they are shown.

### how this loop came to be

the first attempt was **qemu-user**: run the bionic shell binary directly under `qemu-aarch64` with `-L` pointed at the extracted android tree. bionic itself ran fine. the host layer did not: it died on its first guest with `SIGSEGV`/`SEGV_ACCERR` inside the JIT's memcpy into its code buffer, on a page `/proc/self/maps` called `rwxp`. the host-side backtrace added for this (`CaptureHostFrames`, below) named `Arm64JITCore::CompileCode` as the caller, and fifteen lines of C that write a function into an rwx page, call it, and write to the page again reproduced it with no FEX at all, on qemu 7.2 and 11.1 alike — the explanation is in *why not qemu-user* below. no flag of qemu-user's changes it, so the loop moved to **qemu-system**, where a software MMU and a real arm64 kernel make page protection a non-issue. the first qemu-system boot then hit the SVE256 crash recorded under *findings*, and with `sve=off` the regression set passed: 15 of 19, with the four GPU and audio modes failing as they must.

## why not qemu-user

**qemu-user cannot run FEXCore on every host, and the failure looks like a host-layer bug.** qemu-user write-protects a guest page once it has translated code from it, and relies on the host kernel reporting the next fault on that page as a *write* so it can lift the protection. a sandboxed or virtualised host that does not report it (gVisor is one) delivers that fault to the guest instead. FEXCore writes code into its code buffer and runs it all day, so the host layer died on its first guest, in the JIT's memcpy of a compiled block into the code buffer (`Arm64JITCore::CompileCode`), with `SEGV_ACCERR` on a page `/proc/self/maps` called `rwxp`. fifteen lines of C that write a function into an rwx page, call it, and write to the page again reproduce it with no FEX anywhere. it reproduces on qemu 7.2 and on 11.1.

qemu-system emulates the MMU in software, so none of this applies to it, and a real arm64 kernel answers mprotect, signals and `/proc` the way a phone's does.

## what a VM measurement is worth

**a ratio between two configurations, never an absolute.** TCG translates arm64 to x86-64 a second time, so the VM runs the host layer roughly an order of magnitude slower than a phone and with a different cost model: a barrier, an atomic or a cache-maintenance instruction costs what TCG makes it cost, not what a Cortex-X4 does. two things follow:

- a change that removes *work* — fewer guest instructions translated, fewer methods jitted, fewer syscalls, fewer compiles — shows up in the VM in the right direction, and is worth confirming on a device
- a change whose effect is *how fast an instruction runs* — TSO barriers, LRCPC versus LDAR, atomics — is unmeasurable here and must go to a device. so are the memory-ordering rungs

## findings this loop has produced

- **FEXCore's SVE256 AVX path crashes the host layer.** under qemu's default `max` CPU (512-bit SVE, so FEXCore reports `SVE256=1` and takes it), `hello-libc` jumps to address 0 from JIT code before its first line of output; with `sve=off` every non-hardware mode passes. no phone of ours has SVE at all, let alone 256-bit, so nothing shipping takes this path today; it is recorded here because a host feature override or a future SoC would. it is why the VM's default CPU turns SVE off.
- **a host-side crash now names its callers.** the fault report used to print only the host PC, and a crash in a libc routine said `memcpy` and nothing about who called it. the handler now captures LR and up to twelve frame-pointer records when the fault is not in JIT code (`CaptureHostFrames`, `host/src/guest_threads.cpp`), and the report symbolises each the same way it does the PC. that is what located the qemu-user failure above.
- **the first boot bench** (numbers in [`performance-roadmap.md`](performance-roadmap.md#measured-so-far)): R2R off costs 1.95× in the VM against 2.3× on a phone, the first check that the VM's ratios point the way a device's do; `TieredPGO=0` and a 64 MiB gen0 are inside the ±10 % the baseline spreads by. one baseline boot to the loader's refusal is about 92 s under TCG, about 40 times a phone's 2.4 s.
- **nothing stateful belongs on the run share.** the run share is 9p with no cache, so that the host sees what the VM wrote the moment it exits — and the same property made FEX's DiskCache on it *slower* than no cache (1.33×), because every lookup crossed into the host under TCG. caches go in the initramfs's rootfs, which is RAM: there (`diskcache-ram` in the bench) a warm boot is 0.88× the baseline's, the right direction and the device's 0.62 shrunk by TCG.
- **SharpEmu's boot asks for `getcpu` and `getgroups`.** the host layer's unhandled-syscall counter named them (309 three times, 115 once) on the first payload boot; both are now passthroughs in `host/src/linux_syscalls.cpp`, and the only one left is `get_mempolicy` (239), whose ENOSYS .NET reads correctly as *no NUMA*.
