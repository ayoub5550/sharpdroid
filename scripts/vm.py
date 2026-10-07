# runs the host layer on a linux x86-64 machine with no phone attached, inside a qemu-system-aarch64
# virtual machine booted straight into android's own bionic.
#
#   py scripts/vm.py fetch                       qemu, an arm64 kernel, busybox, android's bionic
#   py scripts/vm.py regression                  the host layer's regression set, in the VM
#   py scripts/vm.py run -- ./sharpdroid-host-layer ./hello-libc
#
# **system emulation, not qemu-user, and that is the whole point of this script.** qemu-user runs a
# bionic binary perfectly well, right up to the first block FEXCore writes into a page qemu has
# already translated: qemu then depends on the host kernel reporting the faulting access as a *write*
# so it can drop its own protection, and a sandboxed or virtualised host that does not (gVisor is
# one) turns every such write into a SIGSEGV delivered to the guest. FEXCore writes code and runs it
# all day, so under qemu-user the host layer dies inside the JIT's own memcpy on its first guest. a
# 15-line C program that writes, runs and rewrites one page reproduces it with no FEX involved.
# qemu-system has a softmmu and a real arm64 kernel, so page protection, signals, mprotect and
# self-modifying code all behave the way they do on a phone.
#
# **the VM is android's userspace on a stock linux kernel.** the bionic linker, libc and every
# library the host layer links (libandroid, liblog and what they pull in) come from google's own
# arm64 system image, so a bionic difference cannot hide here; the kernel is debian's arm64 one.
# what the VM does not have is a GPU or an audio device, so the vulkan and aaudio modes fail in it
# and are reported apart from the rest -- the same three-plus-one a phone without a working vulkan
# driver would fail.
#
# **the CPU is `max,sve=off` by default.** qemu's `max` offers 512-bit SVE, which no phone does --
# the Snapdragon 8 Gen 3 and 8 Elite expose none -- and FEXCore takes its SVE256 AVX path on it,
# which is a path no device of ours runs. `--cpu` names another model when that path is the point.
#
# everything fetched lands in `toolchain/vm/`, and everything a run writes in `build/vm/`. the VM
# sees two directories over virtio-9p: the android tree read-only, and the run directory
# read-write, mounted at the same absolute paths they have here so that the symlinks in the android
# tree resolve on both sides.

import gzip
import io
import os
import re
import shutil
import stat
import struct
import subprocess
import sys
import tarfile
import urllib.request
import zipfile
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))

from sharpdroid import paths
from sharpdroid import toolchain as tc
from sharpdroid.shell import Refusal, main, say, step, warn
from sharpdroid.vocabulary import Parser

VM = paths.ROOT / "toolchain" / "vm"
RUN = paths.BUILD / "vm" / "run"

# the pins. the system image is google's, versioned by its own revision, and is what the bionic in
# the VM is; API 34 is new enough to have every library the host layer links and old enough that
# the image is still a plain ext4 inside a super partition.
SYSTEM_IMAGE = "https://dl.google.com/android/repository/sys-img/android/arm64-v8a-34_r04.zip"
DEBIAN_POOL = "http://deb.debian.org/debian/pool/main/"
KERNEL_SERIES = "6.1.0"

# the kernel's modules the VM needs for its two shares, in load order. virtio itself is built in.
MODULES = [
    "drivers/virtio/virtio_pci_legacy_dev", "drivers/virtio/virtio_pci_modern_dev",
    "drivers/virtio/virtio_pci", "fs/netfs/netfs", "fs/fscache/fscache",
    "net/9p/9pnet", "net/9p/9pnet_virtio", "fs/9p/9p",
]

# the regression modes that need hardware the VM does not have. they still run, and they are still
# counted -- separately, so that a red run is never explained away by them without saying so.
NEEDS_DEVICE = {"vulkan", "vkrender", "vkswap", "aaudio"}


def entry():
    parser = Parser(description="run the host layer inside a qemu-system-aarch64 VM")
    sub = parser.add_subparsers(dest="command", required=True)
    sub.add_parser("fetch", help="fetch qemu, a kernel, busybox and android's bionic into toolchain/vm")
    for name in ("regression", "run"):
        one = sub.add_parser(name)
        one.add_argument("--smp", type=int, default=min(16, os.cpu_count() or 4))
        one.add_argument("--memory", type=int, default=16384, help="MiB")
        one.add_argument("--cpu", default="max,sve=off")
        one.add_argument("--timeout", type=int, default=120,
                         help="seconds per regression mode, or for the whole command under `run`")
        one.add_argument("--no-stage", action="store_true")
        if name == "run":
            one.add_argument("--payload", type=Path,
                             help="a packaged build (build/builds/<id>) to copy into the run directory as ./payload")
            one.add_argument("argv", nargs="+")
    arguments = parser.parse_args()

    if arguments.command == "fetch":
        fetch()
        return
    if not arguments.no_stage:
        stage()
    if arguments.command == "regression":
        regression(arguments)
    else:
        if arguments.payload:
            stage_payload(arguments.payload)
        command = "timeout {} {}".format(arguments.timeout, " ".join(arguments.argv))
        status, output = boot(arguments, command)
        say(output.rstrip())
        sys.exit(status)


# --- fetching -------------------------------------------------------------------------------------

def fetch():
    if sys.platform != "linux":
        raise Refusal("the VM loop is linux x86-64 only")
    if not shutil.which("debugfs"):
        raise Refusal("debugfs is needed to read the android image's ext4 without root. install e2fsprogs")
    VM.mkdir(parents=True, exist_ok=True)
    debs = VM / "debs"
    debs.mkdir(exist_ok=True)

    step("qemu-system-aarch64")
    if os.environ.get("SHARPDROID_QEMU"):
        say("  SHARPDROID_QEMU names one, nothing fetched")
    elif not (VM / "qemu" / "usr" / "bin" / "qemu-system-aarch64").exists():
        fetch_qemu(debs)

    step("the arm64 kernel and busybox")
    kernel = newest(DEBIAN_POOL + "l/linux-signed-arm64/",
                    r"linux-image-{}-\d+-arm64_[^\"]*_arm64\.deb".format(re.escape(KERNEL_SERIES)))
    unpack_deb(download(DEBIAN_POOL + "l/linux-signed-arm64/" + kernel, debs), VM / "kernel")
    busybox = newest(DEBIAN_POOL + "b/busybox/", r"busybox-static_[^\"]*_arm64\.deb")
    unpack_deb(download(DEBIAN_POOL + "b/busybox/" + busybox, debs), VM / "busybox")

    step("android's bionic, out of google's arm64 system image")
    android = VM / "android"
    if (android / "system" / "bin" / "linker64").is_symlink() or (android / "system" / "bin" / "linker64").exists():
        say("  already there")
    else:
        extract_android(download(SYSTEM_IMAGE, VM), android)
    say("")
    say("ready. next: py scripts/vm.py regression")


def fetch_qemu(debs):
    """qemu-system-arm and its libraries, unpacked rather than installed -- no root is asked for."""
    if not shutil.which("apt-get"):
        raise Refusal("no apt-get to fetch qemu with. install qemu-system-aarch64 (7.2 or newer) and "
                      "point SHARPDROID_QEMU at it")
    packages = ["qemu-system-arm", "qemu-system-data"]
    depends = subprocess.run(["apt-cache", "depends", "--recurse", "--no-recommends", "--no-suggests",
                              "--no-conflicts", "--no-breaks", "--no-replaces", "--no-enhances",
                              "qemu-system-arm"], capture_output=True, text=True).stdout
    for line in depends.splitlines():
        line = line.strip()
        if line.startswith("Depends:") and "<" not in line:
            packages.append(line.split()[1])
    packages = sorted(set(packages))
    subprocess.run(["apt-get", "download"] + packages, cwd=str(debs), capture_output=True)
    for deb in sorted(debs.glob("*.deb")):
        unpack_deb(deb, VM / "qemu")


def newest(index, pattern):
    listing = urllib.request.urlopen(index).read().decode("utf-8", "replace")
    found = sorted(set(re.findall(pattern, listing)), key=lambda n: [int(x) if x.isdigit() else x
                                                                    for x in re.split(r"(\d+)", n)])
    if not found:
        raise Refusal("nothing matching {} at {}".format(pattern, index))
    return found[-1]


def download(url, into):
    target = Path(into) / url.rsplit("/", 1)[1]
    if not target.exists():
        say("  fetching {}".format(url))
        partial = target.with_suffix(target.suffix + ".partial")
        with urllib.request.urlopen(url) as response, open(partial, "wb") as out:
            shutil.copyfileobj(response, out, 1 << 20)
        partial.rename(target)
    return target


def unpack_deb(deb, into):
    """a .deb is an ar archive holding data.tar.*; read with the standard library alone."""
    data = Path(deb).read_bytes()
    if data[:8] != b"!<arch>\n":
        raise Refusal("not a deb: {}".format(deb))
    offset = 8
    while offset < len(data):
        name = data[offset:offset + 16].decode().strip().rstrip("/")
        length = int(data[offset + 48:offset + 58].decode().strip())
        body = data[offset + 60:offset + 60 + length]
        if name.startswith("data.tar"):
            with tarfile.open(fileobj=io.BytesIO(body)) as archive:
                archive.extractall(into, filter="tar") if hasattr(tarfile, "data_filter") else archive.extractall(into)
            return
        offset += 60 + length + (length & 1)
    raise Refusal("no data.tar in {}".format(deb))


def debugfs_dump(image, inside, target):
    target.mkdir(parents=True, exist_ok=True)
    # debugfs tries to chown what it writes and says so for every file; the files are fine.
    subprocess.run(["debugfs", "-R", "rdump {} {}".format(inside, target), str(image)],
                   capture_output=True)


def extract_android(archive, android):
    """system/{bin,lib64,etc} and every apex's payload, out of the image's super partition."""
    work = VM / "image"
    work.mkdir(exist_ok=True)
    with zipfile.ZipFile(archive) as z:
        name = next(n for n in z.namelist() if n.endswith("/system.img"))
        disk = work / "system.img"
        if not disk.exists():
            with z.open(name) as source, open(disk, "wb") as out:
                shutil.copyfileobj(source, out, 1 << 20)

    system = work / "system.ext4"
    with open(disk, "rb") as f:
        super_offset = gpt_partition(f, "super")
        for partition, sectors, first in lp_partitions(f, super_offset):
            if partition == "system":
                f.seek(super_offset + first * 512)
                with open(system, "wb") as out:
                    left = sectors * 512
                    while left:
                        chunk = f.read(min(left, 1 << 24))
                        out.write(chunk)
                        left -= len(chunk)

    for part in ("/system/bin", "/system/lib64", "/system/etc", "/system/apex"):
        debugfs_dump(system, part, android / "system")
    for apex in sorted((android / "system" / "apex").iterdir()):
        name = re.sub(r"\.c?apex$", "", apex.name)
        with zipfile.ZipFile(apex) as z:
            if "original_apex" in z.namelist():
                z = zipfile.ZipFile(io.BytesIO(z.read("original_apex")))
            if "apex_payload.img" not in z.namelist():
                continue
            payload = work / (name + ".img")
            payload.write_bytes(z.read("apex_payload.img"))
        debugfs_dump(payload, "/", android / "apex" / name)
        payload.unlink()

    # the image's symlinks are absolute (/system/lib64/libc.so -> /apex/...). the tree is mounted
    # at this same path inside the VM, so pointing them into it makes them resolve on both sides.
    for link in android.rglob("*"):
        if link.is_symlink():
            target = os.readlink(link)
            if target.startswith("/") and not target.startswith(str(android)):
                link.unlink()
                link.symlink_to(str(android) + target)
    shutil.rmtree(work)


def gpt_partition(f, wanted):
    f.seek(512)
    header = f.read(92)
    lba, count, size = struct.unpack("<QII", header[72:88])
    f.seek(lba * 512)
    for _ in range(count):
        record = f.read(size)
        first = struct.unpack("<Q", record[32:40])[0]
        if record[56:128].decode("utf-16le").rstrip("\0") == wanted:
            return first * 512
    raise Refusal("no {} partition in the system image".format(wanted))


def lp_partitions(f, base):
    """android's dynamic-partition metadata (liblp): names and their one linear extent each."""
    f.seek(base + 4096 + 2 * 4096)
    header = f.read(256)
    if struct.unpack("<I", header[:4])[0] != 0x414C5030:
        raise Refusal("the super partition has no liblp metadata where it should")
    header_size = struct.unpack("<I", header[8:12])[0]
    tables_size = struct.unpack("<I", header[44:48])[0]
    descriptors = [struct.unpack("<III", header[80 + i * 12:92 + i * 12]) for i in range(2)]
    f.seek(base + 3 * 4096 + header_size)
    tables = f.read(tables_size)
    (p_off, p_count, p_size), (e_off, e_count, e_size) = descriptors
    extents = [struct.unpack("<QIQI", tables[e_off + i * e_size:e_off + i * e_size + 24])
               for i in range(e_count)]
    for i in range(p_count):
        record = tables[p_off + i * p_size:p_off + (i + 1) * p_size]
        name = record[:36].rstrip(b"\0").decode()
        first_extent, extent_count = struct.unpack("<II", record[40:48])
        for sectors, _, start, _ in extents[first_extent:first_extent + extent_count]:
            yield name, sectors, start


# --- staging and booting --------------------------------------------------------------------------

def stage():
    """what `stage.py --shell` pushes to a phone, copied into the run directory instead."""
    step("staging the shell binary, its guests and its libraries")
    toolchain = tc.resolve().require("ndk")
    if not paths.HOST_SHELL.exists():
        raise Refusal("no shell binary. run: py scripts/build-host.py")
    if RUN.exists():
        shutil.rmtree(RUN)
    (RUN / "guest-libs").mkdir(parents=True)
    stl = toolchain.ndk_sysroot / "usr" / "lib" / "aarch64-linux-android" / "libc++_shared.so"
    for source in [paths.HOST_SHELL, stl] + [p for p in sorted(paths.BUILD_GUESTS.iterdir()) if p.is_file()]:
        shutil.copy2(source, RUN)
    for source in sorted(paths.GUEST_LIBS_X86_64.iterdir()):
        if source.is_file():
            shutil.copy2(source, RUN / "guest-libs")
    binaries = paths.GUEST_LIBS / "bin"
    if binaries.is_dir():
        for source in binaries.iterdir():
            shutil.copy2(source, RUN)
    # the script is the device's own, with the one line that differs: on a phone the linker's
    # namespace config finds the apex libraries, and the VM has no generated config, so the apex
    # library directories are added to the search path the script sets.
    script = (paths.HOST / "regression.sh").read_text(encoding="utf-8")
    script = script.replace("export LD_LIBRARY_PATH=.\n", "export LD_LIBRARY_PATH=.:$VM_LIBS\n")
    (RUN / "regression.sh").write_text(script, encoding="utf-8")


def stage_payload(build):
    """a packaged build as ./payload, and the boot bench beside it. the build is copied rather than
    shared because the run directory is the VM's only writable share, and .NET writes beside itself."""
    if not (build / "SharpEmu").is_file():
        raise Refusal("{} has no SharpEmu in it. it wants a directory under build/builds".format(build))
    step("staging the payload")
    target = RUN / "payload"
    if target.exists():
        shutil.rmtree(target)
    shutil.copytree(build, target, symlinks=True)
    shutil.copy2(paths.ROOT / "scripts" / "vm-boot-bench.sh", RUN)


def library_path():
    android = VM / "android"
    apexes = sorted(str(p) for p in (android / "apex").glob("*/lib64") if "vndk" not in p.parent.name)
    return ":".join([str(android / "system" / "lib64")] + apexes)


def boot(arguments, command):
    """boot the VM, run `command` in the run directory, and return its status and output."""
    qemu = Path(os.environ.get("SHARPDROID_QEMU") or VM / "qemu" / "usr" / "bin" / "qemu-system-aarch64")
    kernel = next((VM / "kernel" / "boot").glob("vmlinuz-*"), None)
    if not qemu.exists() or kernel is None:
        raise Refusal("the VM is not fetched. run: py scripts/vm.py fetch")
    modules = next((VM / "kernel" / "lib" / "modules").iterdir()) / "kernel"
    android = VM / "android"

    (RUN / "vm-cmd.sh").write_text(
        "export VM_LIBS={0}\nexport LD_LIBRARY_PATH=$PWD:{0}\nexport REGRESSION_TIMEOUT={1}\n{2}\n".format(
            library_path(), arguments.timeout, command), encoding="utf-8")
    for stale in ("vm-out.log", "vm-status"):
        (RUN / stale).unlink(missing_ok=True)
    initramfs = paths.BUILD / "vm" / "initramfs.gz"
    write_initramfs(initramfs, modules, android)

    environment = dict(os.environ)
    if not os.environ.get("SHARPDROID_QEMU"):
        environment["LD_LIBRARY_PATH"] = qemu_library_path(qemu)
    argv = [
        str(qemu), "-M", "virt,gic-version=3", "-cpu", arguments.cpu,
        "-accel", "tcg,thread=multi,tb-size=1024", "-smp", str(arguments.smp), "-m", str(arguments.memory),
        "-nographic", "-nic", "none", "-no-reboot",
        "-kernel", str(kernel), "-initrd", str(initramfs),
        "-append", "console=ttyAMA0 quiet loglevel=3 rdinit=/init",
        "-fsdev", "local,id=a,path={},security_model=none,readonly=on".format(android),
        "-device", "virtio-9p-pci,fsdev=a,mount_tag=android,romfile=",
        "-fsdev", "local,id=r,path={},security_model=none".format(RUN),
        "-device", "virtio-9p-pci,fsdev=r,mount_tag=run,romfile=",
    ]
    share = VM / "qemu" / "usr" / "share" / "qemu"
    if share.is_dir():
        argv += ["-L", str(share)]
    say("  booting: {} cpu(s), {} MiB, -cpu {}".format(arguments.smp, arguments.memory, arguments.cpu))
    subprocess.run(argv, env=environment, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL,
                   stdin=subprocess.DEVNULL)
    status_file = RUN / "vm-status"
    if not status_file.exists():
        raise Refusal("the VM never reported back. boot it by hand to see its console")
    output = (RUN / "vm-out.log").read_text(encoding="utf-8", errors="replace")
    return int(status_file.read_text().strip() or 1), output


def qemu_library_path(qemu):
    """only the directories holding libraries this machine is actually missing.

    the unpacked dependency set includes libc6 and the rest of a debian base, and putting all of it on
    the search path would load one distribution's glibc beside another's dynamic linker. so ask
    `ldd` what is not found, add the directory that has it, and ask again until nothing is.
    """
    found = {}
    for library in (VM / "qemu").rglob("*.so*"):
        found.setdefault(library.name, str(library.parent))
    directories = []
    for _ in range(8):
        environment = dict(os.environ, LD_LIBRARY_PATH=":".join(directories))
        listing = subprocess.run(["ldd", str(qemu)], capture_output=True, text=True, env=environment).stdout
        missing = re.findall(r"^\s*(\S+) => not found", listing, re.M)
        if not missing:
            return ":".join(directories)
        added = [found[name] for name in missing if name in found and found[name] not in directories]
        if not added:
            raise Refusal("qemu needs {} and the fetched packages do not have it".format(", ".join(missing)))
        directories += sorted(set(added))
    return ":".join(directories)


def write_initramfs(target, modules, android):
    """busybox, the 9p modules, and an /init that mounts both shares and runs vm-cmd.sh."""
    entries = []

    def directory(name):
        entries.append((name, stat.S_IFDIR | 0o755, b""))

    def regular(name, data, mode=0o755):
        entries.append((name, stat.S_IFREG | mode, data))

    parents = set()
    for path in ("bin", "dev", "proc", "sys", "lib/modules", "data/local/tmp",
                 str(android).lstrip("/"), str(RUN).lstrip("/")):
        parts = path.split("/")
        for i in range(1, len(parts) + 1):
            parents.add("/".join(parts[:i]))
    for name in sorted(parents, key=lambda p: (p.count("/"), p)):
        directory(name)
    regular("bin/busybox", (VM / "busybox" / "usr" / "bin" / "busybox").read_bytes())
    names = []
    for module in MODULES:
        name = module.rsplit("/", 1)[1]
        regular("lib/modules/{}.ko".format(name), (modules / (module + ".ko")).read_bytes(), 0o644)
        names.append(name)
    init = "\n".join([
        "#!/bin/busybox sh",
        "/bin/busybox --install -s /bin",
        "export PATH=/bin",
        "mount -t proc proc /proc; mount -t sysfs sys /sys; mount -t devtmpfs dev /dev",
        "for m in {}; do insmod /lib/modules/$m.ko; done".format(" ".join(names)),
        "mount -t 9p -o trans=virtio,version=9p2000.L,msize=1048576,cache=loose android {}".format(android),
        "mount -t 9p -o trans=virtio,version=9p2000.L,msize=1048576 run {}".format(RUN),
        "ln -s {0}/system /system; ln -s {0}/apex /apex; ln -s {1} /data/local/tmp/sharpdroid".format(android, RUN),
        "cd {}".format(RUN),
        "sh ./vm-cmd.sh > ./vm-out.log 2>&1",
        "echo $? > ./vm-status",
        "sync",
        "poweroff -f",
        "",
    ])
    regular("init", init.encode())

    out = bytearray()
    for inode, (name, mode, data) in enumerate(entries + [("TRAILER!!!", 0, b"")], start=1):
        encoded = name.encode() + b"\0"
        out += b"070701" + b"".join(b"%08X" % v for v in (inode, mode, 0, 0, 1, 0, len(data), 0, 0, 0, 0,
                                                            len(encoded), 0))
        out += encoded + b"\0" * (-(110 + len(encoded)) % 4)
        out += data + b"\0" * (-len(data) % 4)
    target.parent.mkdir(parents=True, exist_ok=True)
    target.write_bytes(gzip.compress(bytes(out), 1))


def regression(arguments):
    step("the regression set, in the VM")
    status, output = boot(arguments, "sh ./regression.sh")
    say(output.rstrip())
    results = re.findall(r"(?m)^(PASS|FAIL)  (\S+)", output)
    passed = [m for r, m in results if r == "PASS"]
    failed = [m for r, m in results if r == "FAIL" and m not in NEEDS_DEVICE]
    hardware = [m for r, m in results if r == "FAIL" and m in NEEDS_DEVICE]
    say("")
    say("regression (VM): {} passed, {} failed{}".format(
        len(passed), len(failed),
        ", and {} that need a GPU or audio device the VM has not got: {}".format(len(hardware), " ".join(hardware))
        if hardware else ""))
    if not results:
        raise Refusal("no mode reported at all -- that is a failure, not a pass")
    sys.exit(1 if failed else 0)


if __name__ == "__main__":
    main(entry)
