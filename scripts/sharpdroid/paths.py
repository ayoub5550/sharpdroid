# where everything in this repository is, resolved in one place.
#
# **a script resolves its own location.** every entry point sits in `scripts/`, so the repository
# root is one directory up from this package -- which makes every script safe to run from anywhere,
# and makes an absolute path something the caller never has to supply.
#
# **one rule lives in one place.** the artefact paths below are the rule for where a build step
# writes and where the next one looks, and two copies of that rule drift silently. every script
# imports these rather than joining its own.

from pathlib import Path

# scripts/sharpdroid/paths.py -> scripts/sharpdroid -> scripts -> the repository
ROOT = Path(__file__).resolve().parent.parent.parent

SCRIPTS = ROOT / "scripts"
APP = ROOT / "app"
HOST = ROOT / "host"
GUESTS = ROOT / "guests"
GUEST_LIBS = ROOT / "guest-libs"
TOOLCHAIN = ROOT / "toolchain"
TOOLCHAIN_JSON = ROOT / "toolchain.json"

# the three submodules. two of them are read for the terms their code is redistributed under, which
# is the only reason packaging looks in here at all.
EXTERNAL = ROOT / "external"
FEX = EXTERNAL / "FEX"
ADRENOTOOLS = EXTERNAL / "libadrenotools"

# the license texts this repository keeps a copy of, for terms that are not derivable from anything
# else here -- a dependency that states a license by name and URL rather than shipping its text.
LICENSE_TEXTS = ROOT / "LICENSES"

# the x86-64 set the guest's own ld.so searches: debian glibc, plus the two generated thunk
# libraries that are built rather than downloaded.
GUEST_LIBS_X86_64 = GUEST_LIBS / "x86_64"

# every build output lives under one directory, so a clean is one path and nothing a build step
# writes ever lands in the source tree.
BUILD = ROOT / "build"
BUILD_ADRENOTOOLS = BUILD / "adrenotools"
BUILD_HOST = BUILD / "host"
BUILD_GUESTS = BUILD / "guests"
BUILD_VULKAN = BUILD / "vulkan"
BUILD_BUILDS = BUILD / "builds"
BUILD_BUNDLE = BUILD / "bundle"

# the two artefacts the host layer produces. the library is what the app loads; the executable is
# the same host layer reached from a shell, which is how the regression modes run without an APK.
HOST_LIBRARY = BUILD_HOST / "libsharpdroid-host-layer.so"
HOST_SHELL = BUILD_HOST / "sharpdroid-host-layer"

# FEXCore's own generated header, naming the FEX the two above were compiled from. it is written
# when the host layer is configured and read by the compiler that builds it, so it moves only when
# they are rebuilt -- which is why the APK step reads its version here rather than describing the
# submodule, whose checkout says what is true now rather than what was compiled.
HOST_FEX_VERSION_HEADER = BUILD_HOST / "generated" / "git_version.h"

# what the host cmake project will not configure without.
ADRENOTOOLS_LIBRARY = BUILD_ADRENOTOOLS / "libadrenotools.a"

# the guest halves of the two thunks, staged beside glibc rather than built into anything.
GUEST_VULKAN = GUEST_LIBS_X86_64 / "libvulkan.so.1"
GUEST_VULKAN_SONAME = GUEST_LIBS_X86_64 / "libvulkan.so"
GUEST_AAUDIO = GUEST_LIBS_X86_64 / "libaaudio.so"

# the generated halves of the two thunks. both are committed: the host half is compiled into the
# host layer and the guest half is assembled into the libraries above, and regenerating them is
# something you do when the NDK headers move rather than on every build.
VULKAN_THUNK = HOST / "thunks" / "vulkan"
AUDIO_THUNK = HOST / "thunks" / "audio"


# where the APKs land, one file per application id.
#
# **per application id rather than per build type.** a renamed id is a different app to android, and
# two of them writing to one path makes "which APK is this" a question about timestamps. naming the
# file after the identity makes it a question about the name.
BUILD_APK = BUILD / "apk"


def apk(application_id):
    return BUILD_APK / "{}.apk".format(application_id)


def relative(path):
    """a path as it reads from the repository root, for output. absolute if it is somewhere else."""
    path = Path(path)
    try:
        return str(path.resolve().relative_to(ROOT))
    except ValueError:
        return str(path)
