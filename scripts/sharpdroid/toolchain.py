# resolves the build toolchain, and is the only place that knows a version number or a path to one.
#
# **no other script contains either.** the versions live in `toolchain.json` and the layout lives
# here, so moving to a new NDK is one line of JSON and a fetch rather than a sweep. a script that
# spelled out a path would keep working after the pin moved, against the wrong compiler, and say
# nothing about it.
#
# **the toolchain is the repository's own.** it is fetched into `toolchain/`, never installed
# machine-wide, and nothing here modifies `PATH` for anything but a child process it launches. a
# machine that already has a JDK or an android SDK it prefers points `SHARPDROID_JDK`,
# `SHARPDROID_SDK`, `SHARPDROID_NDK` or `SHARPDROID_DOTNET` at it and that wins.
#
# **every variable this project reads is prefixed `SHARPDROID_`, and that is a namespace rather
# than a flourish.** the emulator itself reads `SHARPEMU_HOST_AUDIO`, `SHARPEMU_HOST_WINDOW`,
# `SHARPEMU_SAVEDATA_DIR` and several more out of `SHARPEMU_`, and it is somebody else's project --
# so a build-tooling variable of ours sitting in that namespace is a collision waiting for whichever
# of the two adds a name the other already has.
#
# **the interpreter is the one exception, and it is a prerequisite rather than a resolution.**
# these scripts are Python and something has to run them before anything can be fetched. they use
# only the standard library, so there is no version-resolved content for two machines to disagree
# about -- the floor below is the whole of the requirement.

import json
import os
import platform
import re
import sys
from pathlib import Path

from . import paths
from .shell import Refusal, warn

# the floor, checked in one place. 3.9 is where `dict | dict`, `list[str]` in annotations and the
# `zoneinfo` module arrive; nothing here needs any of them, so the floor is really "old enough that
# a distribution still ships security fixes" rather than a language requirement.
MINIMUM_PYTHON = (3, 9)

# what the NDK calls this machine in its prebuilt path. android only ships x86-64 prebuilts, so the
# host architecture is not part of the question -- an arm64 windows or mac runs the x86-64 one.
_HOST_TAG = {
    "Windows": "windows-x86_64",
    "Linux": "linux-x86_64",
    "Darwin": "darwin-x86_64",
}

IS_WINDOWS = platform.system() == "Windows"
_WINDOWS = IS_WINDOWS


def check_interpreter():
    """refuse an interpreter too old for these scripts, by name and by version.

    a syntax error from a module three imports deep names the file it was parsing rather than the
    thing that is wrong, so the check is explicit and happens before anything else.
    """
    if sys.version_info < MINIMUM_PYTHON:
        raise Refusal(
            "python {}.{} or newer is required; this is {}.{} at {}".format(
                MINIMUM_PYTHON[0], MINIMUM_PYTHON[1],
                sys.version_info[0], sys.version_info[1], sys.executable))


class Toolchain:
    """the resolved toolchain. every piece is found on first use and refused by name if absent.

    on demand rather than up front so that a native build is never stopped by a missing JDK, and so
    that no script has to declare what it needs and then drift from what it uses. a script that
    would rather fail early calls `require` with the names it is about to touch.
    """

    def __init__(self):
        if not paths.TOOLCHAIN_JSON.exists():
            raise Refusal("missing {}".format(paths.relative(paths.TOOLCHAIN_JSON)))
        self.spec = json.loads(paths.TOOLCHAIN_JSON.read_text(encoding="utf-8"))
        self.root = paths.ROOT
        self._resolved = {}

    # --- the versions, straight out of the spec -------------------------------------------------

    @property
    def ndk_version(self):
        return self.spec["ndk"]

    @property
    def cmake_version(self):
        return self.spec["cmake"]

    @property
    def build_tools_version(self):
        return self.spec["buildTools"]

    @property
    def platform_version(self):
        return self.spec["platform"]

    @property
    def jdk_version(self):
        return self.spec["jdk"]

    @property
    def dotnet_version(self):
        return self.spec["dotnetSdk"]

    def source(self, name):
        return self.spec["sources"][name]

    # --- the pieces ----------------------------------------------------------------------------

    @property
    def android_sdk(self):
        """the SDK root, identified by any one package inside it rather than by a chosen one.

        **the marker cannot be `platform-tools` alone.** every SDK package is installed by running
        `sdkmanager --sdk_root=<this>`, so a root that only counts as one once a package is inside
        it is a root that can never acquire its first package -- and the fetch would refuse on an
        empty machine while succeeding on every machine that already had an SDK.

        either name identifies a root: `cmdline-tools` is what the fetch unpacks before it can run
        `sdkmanager` at all, and an SDK acquired any other way has `platform-tools` whether or not
        it was given the command-line tools.
        """
        return self._piece(
            "android sdk", "SHARPDROID_SDK", paths.TOOLCHAIN / "android-sdk",
            marker=(Path("cmdline-tools"), Path("platform-tools")))

    @property
    def ndk(self):
        """the NDK the pin names, or one an override points at.

        the override is a whole NDK rather than a version, because a machine that has its own has
        it somewhere of its own choosing. the revision is checked either way.
        """
        override = os.environ.get("SHARPDROID_NDK")
        if override:
            ndk = Path(override)
            if not ndk.is_dir():
                raise Refusal("SHARPDROID_NDK does not name a directory: {}".format(ndk))
        else:
            ndk = self.android_sdk / "ndk" / self.ndk_version
            if not ndk.is_dir():
                raise Refusal(
                    "missing NDK {}. run: py scripts/fetch-toolchain.py --install --what ndk"
                    .format(self.ndk_version))
        self._check_ndk_revision(ndk)
        return ndk

    @property
    def ndk_prebuilt(self):
        tag = _HOST_TAG.get(platform.system())
        if tag is None:
            raise Refusal("no NDK prebuilt for {}".format(platform.system()))
        prebuilt = self.ndk / "toolchains" / "llvm" / "prebuilt" / tag
        if not prebuilt.is_dir():
            raise Refusal("the NDK has no {} prebuilt: {}".format(tag, prebuilt))
        return prebuilt

    @property
    def ndk_bin(self):
        return self.ndk_prebuilt / "bin"

    @property
    def ndk_sysroot(self):
        return self.ndk_prebuilt / "sysroot"

    @property
    def cmake_bin(self):
        """the directory holding both cmake and ninja.

        they are one piece rather than two: the android SDK ships ninja inside its cmake package,
        and handing cmake a make program from somewhere else is how a build ends up configured
        against one generator and built by another.

        **an override needs no SDK behind it.** the default is spelled out only when there is no
        override, because resolving it resolves the SDK -- and a native-only machine (a linux box
        building the host layer for the VM loop, `docs/vm.md`) has an NDK and a cmake and no SDK at
        all, which made `SHARPDROID_CMAKE` refuse for a reason that had nothing to do with cmake.
        """
        default = None if os.environ.get("SHARPDROID_CMAKE") else (
            self.android_sdk / "cmake" / self.cmake_version / "bin")
        return self._piece("cmake", "SHARPDROID_CMAKE", default, marker=Path(_exe("cmake")))

    @property
    def cmake(self):
        return self.cmake_bin / _exe("cmake")

    @property
    def ninja(self):
        return self.cmake_bin / _exe("ninja")

    @property
    def jdk(self):
        return self._piece(
            "jdk", "SHARPDROID_JDK", paths.TOOLCHAIN / "jdk-{}-temurin".format(self.jdk_version),
            marker=Path("bin") / _exe("java"))

    @property
    def dotnet_root(self):
        return self._piece(
            "dotnet sdk", "SHARPDROID_DOTNET", paths.TOOLCHAIN / "dotnet-sdk",
            marker=Path(_exe("dotnet")))

    @property
    def dotnet(self):
        return self.dotnet_root / _exe("dotnet")

    @property
    def workspace(self):
        """the directory this repository sits in, where its neighbours are.

        it is where a checkout of the fork or of libadrenotools is looked for when the submodule
        that should hold it is empty. `SHARPDROID_WORKSPACE` points that somewhere else.
        """
        override = os.environ.get("SHARPDROID_WORKSPACE")
        return Path(override) if override else paths.ROOT.parent

    @property
    def fork(self):
        """the SharpEmu fork checkout a build is packaged from. two places, and only two.

        **`external/sharpemu` is a pin and never a workspace.** it is what an ordinary build
        resolves to and what a clone gets, so a checkout of your own is reached by
        `SHARPDROID_SHARPEMU` and wins when it is set. a machine without it builds the pin, which
        is the configuration everything has to work in.

        **a checkout sitting beside this repository is deliberately not in the order**, unlike the
        other two submodules. it would make the pin the one path no machine of ours ever takes:
        every build here would resolve to the sibling, the pointer would go stale with nothing to
        notice, and the first person to find out would be somebody cloning this repository. an
        uninitialised submodule is a refusal naming the command that fixes it, which is the answer
        that leaves the pin doing its job.
        """
        override = os.environ.get("SHARPDROID_SHARPEMU")
        if override:
            fork = Path(override)
            if not (fork / "Directory.Build.props").exists():
                raise Refusal(
                    "SHARPDROID_SHARPEMU does not point at a SharpEmu checkout: {}".format(fork))
            return fork
        pin = paths.ROOT / "external" / "sharpemu"
        if (pin / "Directory.Build.props").exists():
            return pin
        raise Refusal(
            "{} is empty, so there is no SharpEmu to build from. run: git submodule update --init "
            "--recursive, or set SHARPDROID_SHARPEMU at a checkout of your own".format(
                paths.relative(pin)))

    @property
    def build_tools(self):
        return self._piece(
            "build-tools", None, self.android_sdk / "build-tools" / self.build_tools_version)

    @property
    def platform_jar(self):
        return self._piece(
            "platform", None, self.android_sdk / "platforms" / self.platform_version,
            marker=Path("android.jar"))

    @property
    def adb(self):
        adb = self.android_sdk / "platform-tools" / _exe("adb")
        if not adb.exists():
            raise Refusal(
                "missing adb. run: py scripts/fetch-toolchain.py --install --what platform-tools")
        return adb

    @property
    def sdkmanager(self):
        name = "sdkmanager.bat" if _WINDOWS else "sdkmanager"
        manager = self.android_sdk / "cmdline-tools" / "latest" / "bin" / name
        if not manager.exists():
            raise Refusal(
                "missing the android command-line tools. run: py scripts/fetch-toolchain.py --install")
        return manager

    def cross_compiler(self, arch, api_level):
        """the NDK's clang for one target, as a program that can be run directly.

        the NDK ships each target as a `.cmd` wrapper on windows and a shell script elsewhere; both
        are the same compiler with the target triple already applied, which is why nothing here has
        to assemble one out of `--target` and a sysroot.
        """
        stem = "{}-linux-android{}-clang".format(arch, api_level)
        compiler = self.ndk_bin / (stem + ".cmd" if _WINDOWS else stem)
        if not compiler.exists():
            raise Refusal("the NDK has no {} at API {}: {}".format(arch, api_level, compiler))
        return compiler

    @property
    def clang(self):
        return self.ndk_bin / _exe("clang")

    @property
    def readelf(self):
        return self.ndk_bin / _exe("llvm-readelf")

    @property
    def strip(self):
        return self.ndk_bin / _exe("llvm-strip")

    # --- helpers -------------------------------------------------------------------------------

    def require(self, *names):
        """resolve some pieces now, so a long step does not fail on a missing tool at the end."""
        for name in names:
            getattr(self, name)
        return self

    def describe(self):
        """every piece and where it resolved to, or why it did not. what `--list` prints."""
        rows = []
        for name, label in (
            ("android_sdk", "android sdk"), ("ndk", "ndk " + self.ndk_version),
            ("cmake_bin", "cmake " + self.cmake_version), ("jdk", "jdk " + str(self.jdk_version)),
            ("dotnet_root", "dotnet " + self.dotnet_version),
            ("build_tools", "build-tools " + self.build_tools_version),
            ("platform_jar", self.platform_version), ("adb", "adb"),
        ):
            try:
                rows.append((label, str(getattr(self, name))))
            except Refusal as refusal:
                rows.append((label, "-- {}".format(refusal)))
        return rows

    def _piece(self, label, override_name, default, marker=None):
        """one piece, from its override or from `toolchain/`, and a refusal naming it if absent.

        `marker` is what proves the directory is the thing rather than merely a directory of that
        name. **several may be given, and any one of them is enough** -- that is for a directory
        whose contents arrive in stages, where insisting on a particular one would make the piece
        unresolvable until after something that needs it resolved has run.
        """
        cached = self._resolved.get(label)
        if cached is not None:
            return cached
        override = os.environ.get(override_name) if override_name else None
        path = Path(override) if override else default
        markers = (marker,) if isinstance(marker, (str, Path)) else tuple(marker or ())
        if not path.exists() or (markers and not any((path / one).exists() for one in markers)):
            if override:
                raise Refusal("{} does not point at a usable {}: {}".format(
                    override_name, label, path))
            raise Refusal(
                "missing {}. run: py scripts/fetch-toolchain.py --install".format(label))
        self._resolved[label] = path
        return path

    def _check_ndk_revision(self, ndk):
        """r29 is a floor rather than a preference, and the exact build is an alignment.

        FEXCore's spin wait uses `std::atomic_ref`, which libc++ did not implement until LLVM 19,
        so anything on clang 18 or older fails to compile FEXCore at all -- which is what the r27
        that ships with the SDK does. the exact build is also the one the GPU driver packages this
        project uses were compiled with, so a different r29 is worth a warning and not a refusal.
        """
        if self._resolved.get("ndk revision"):
            return
        properties = ndk / "source.properties"
        revision = None
        if properties.exists():
            match = re.search(r"Pkg\.Revision\s*=\s*([\w.]+)",
                              properties.read_text(encoding="utf-8", errors="replace"))
            if match:
                revision = match.group(1)
        if revision is None:
            revision = ndk.name
        major = int(revision.split(".")[0]) if revision.split(".")[0].isdigit() else 0
        floor = int(self.spec.get("ndkMinRevision", 0))
        if major < floor:
            raise Refusal(
                "NDK r{} is too old: FEXCore needs the libc++ that ships with r{} or newer. "
                "set SHARPDROID_NDK at a newer one, or run: "
                "py scripts/fetch-toolchain.py --install --what ndk".format(major, floor))
        if revision != self.ndk_version:
            warn("using NDK {} rather than the pinned {}".format(revision, self.ndk_version))
        self._resolved["ndk revision"] = revision
        self.revision = revision


def resolve():
    check_interpreter()
    return Toolchain()


def _exe(name):
    return name + ".exe" if _WINDOWS else name


def java_home_environment(toolchain, extra=None):
    """the environment a JVM tool needs, without touching this process's own.

    gradle finds a JDK through `JAVA_HOME` and an android SDK through `ANDROID_HOME`, and the
    second is precisely the disagreement this resolver exists to prevent -- so both are set for the
    child rather than left to whatever the machine has.
    """
    env = dict(os.environ)
    env["JAVA_HOME"] = str(toolchain.jdk)
    env["ANDROID_HOME"] = str(toolchain.android_sdk)
    env["ANDROID_SDK_ROOT"] = str(toolchain.android_sdk)
    if extra:
        env.update({k: str(v) for k, v in extra.items()})
    return env


def sdk_version_of(toolchain):
    """the numeric API level of the pinned platform, for a caller that has to compare one."""
    match = re.search(r"(\d+)$", toolchain.platform_version)
    if not match:
        raise Refusal("cannot read an API level out of {}".format(toolchain.platform_version))
    return int(match.group(1))


def which(name):
    """a program on PATH, or None. used only where a missing one is survivable."""
    from shutil import which as _which
    found = _which(name)
    return Path(found) if found else None
