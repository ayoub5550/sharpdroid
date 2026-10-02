# builds the static x86-64 test guests the host layer is exercised against.
#
#   py scripts/build-guests.py
#
# the NDK's x86-64 clang is used purely as a cross compiler. nothing about these binaries is
# android: they are linux x86-64 ELFs, which is exactly what the host layer claims to be able to
# run, and running them is what the regression gate does.
#
# **most of them link no libc**, which is the point rather than an economy. what is under test is
# the host layer's own ELF loading, syscall dispatch, signal delivery and memory tracking, and a
# libc in between would decide half of that and hide which side got something wrong. the two that
# do link something link one of the generated thunk libraries and nothing else, so the guest's own
# ld.so has to find it and resolve every entry point before the program is reached.

import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))

from sharpdroid import paths
from sharpdroid import toolchain as tc
from sharpdroid.shell import Refusal, capture, ensure, main, produced, run, say, step
from sharpdroid.vocabulary import Parser

# the guests are glibc-linked or nothing-linked, so this only chooses a compiler wrapper.
API_LEVEL = 21

# -nostdlib             no libc, no startup files; the entry point is the guest's own
# -no-pie               ET_EXEC, so the loader maps it where its program headers say and picks no bias
# -fno-stack-protector  the canary would be read through the thread pointer before there is one
_FREESTANDING = ["-static", "-nostdlib", "-no-pie", "-fno-stack-protector", "-fno-builtin",
                 "-Os", "-Wall", "-Wl,-e,_start"]

# the dynamic ones. still freestanding, so the interpreter has to be named by hand -- there are no
# startup files to carry it -- and the library path is relative to the program, because the host
# layer stages the guest set beside whatever it is running.
_DYNAMIC = ["-nostdlib", "-fno-stack-protector", "-fno-builtin", "-Os", "-Wall", "-Wl,-e,_start",
            "-Wl,--dynamic-linker=/lib64/ld-linux-x86-64.so.2", "-Wl,-rpath,$ORIGIN/guest-libs"]

_GUESTS = [
    ("hello-nostdlib", "the smallest thing that can be loaded and exit", _FREESTANDING, None),
    ("signals", "spells out the kernel signal ABI itself", _FREESTANDING, None),
    ("smc", "issues its own mmap, mprotect and munmap, so what is under test is VMA tracking",
     _FREESTANDING, None),
    ("asyncsig", "issues clone and tgkill itself, so one guest thread interrupts another",
     _FREESTANDING, None),
    ("pause", "is paused from outside, and checks from inside that its clock and its spinning "
     "thread both stopped", _FREESTANDING, None),
    ("vulkan", "623 entry points resolved by the guest's own ld.so before it starts",
     _DYNAMIC, "vulkan"),
    ("vkrender", "the one that makes the GPU run something", _DYNAMIC, "vulkan"),
    ("vkswap", "a swapchain, on a window system the host layer invents", _DYNAMIC, "vulkan"),
    ("aaudio", "the audio thunk, reached the same way", _DYNAMIC, "audio"),
    ("hello-libc", "a full static bionic libc, so the libc startup path is exercised too",
     ["-static", "-O1", "-Wall"], None),
]

_LINKS = {
    "vulkan": (paths.GUEST_VULKAN, "-l:libvulkan.so.1", "gen-thunks.py and build-thunks.py"),
    "audio": (paths.GUEST_AAUDIO, "-l:libaaudio.so", "gen-thunks.py and build-thunks.py"),
}


def entry():
    parser = Parser(description="build the x86-64 test guests")
    parser.add_argument("--only", metavar="NAME", nargs="+", default=None,
                        help="build only these: " + ", ".join(name for name, _, _, _ in _GUESTS))
    arguments = parser.parse_args()

    toolchain = tc.resolve().require("ndk")
    compiler = toolchain.cross_compiler("x86_64", API_LEVEL)
    output = ensure(paths.BUILD_GUESTS)

    wanted = arguments.only or [name for name, _, _, _ in _GUESTS]
    known = {name for name, _, _, _ in _GUESTS}
    unknown = [name for name in wanted if name not in known]
    if unknown:
        raise Refusal("no such guest: {}. there is {}".format(
            ", ".join(unknown), ", ".join(sorted(known))))

    for name, purpose, flags, needs in _GUESTS:
        if name not in wanted:
            continue
        step("{} -- {}".format(name, purpose))
        source = paths.GUESTS / (name + ".c")
        if not source.exists():
            raise Refusal("missing {}".format(paths.relative(source)))
        arguments_out = [compiler, "-o", str(output / name), str(source)] + list(flags)
        if needs:
            library, link, how = _LINKS[needs]
            if not library.exists():
                raise Refusal("missing {}. run: py scripts\\{}".format(
                    paths.relative(library), how))
            arguments_out += ["-L" + str(paths.GUEST_LIBS_X86_64), link]
        run(arguments_out)
        produced(output / name, name)

    step("what they are")
    for name in wanted:
        header = capture([toolchain.readelf, "-h", str(output / name)], check=False)
        interesting = [" ".join(line.split()) for line in header.splitlines()
                       if line.strip().startswith(("Type:", "Machine:", "Entry point"))]
        say("  {:<16} {}".format(name, "  ".join(interesting)))


if __name__ == "__main__":
    main(entry)
