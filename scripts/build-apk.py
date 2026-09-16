# builds the APK, with exactly one SharpEmu build and the guest's x86-64 libraries inside it.
#
#   py scripts/build-apk.py                              build as the debug app, bundling the newest build
#   py scripts/build-apk.py --install                    build, then install over whatever is there
#   py scripts/build-apk.py --release                    build under the manifest's own id and label
#   py scripts/build-apk.py --sharpemu none              no build ships; the APK carries no asset
#   py scripts/build-apk.py --sharpemu build\builds\...  bundle that one
#
# **this is the entry point and not a wrapper you may skip.** it resolves the SDK and the JDK, writes
# `local.properties` from what it found, and passes the identity this build should carry. gradle left
# alone finds its own SDK through the environment, which on a machine with two installs is exactly
# the silent disagreement the resolver exists to prevent.
#
# **the build going in has to be the fork checkout on this machine, and a difference refuses.** the
# emulator is developed through the bundled build -- edit the fork, package it, build the APK, run it
# -- and dropping the packaging step from that loop otherwise produces a perfectly good APK carrying
# the payload from before the edit. naming one with `--sharpemu` is a choice and is reported instead.
#
# **the guest libraries always ship and there is no argument to say otherwise.** a build is chosen
# because a person picks between several; the x86-64 set the guest's own linker searches is the one
# right set for a given APK, so the only question it could answer is "should this APK be able to run
# a game at all". missing, it is a refusal naming the fetch.
#
# **the native libraries are not built here.** build the host layer and libadrenotools first; the
# gradle build collects their output.

import hashlib
import json
import re
import shutil
import subprocess
import sys
import zipfile
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))

from sharpdroid import builds, device, paths, vocabulary
from sharpdroid import toolchain as tc
from sharpdroid.shell import (Refusal, capture, ensure, fresh, main, produced, run, say, size, step,
                            tree_size, wipe, write_text)
from sharpdroid.vocabulary import Parser

# what the app's own asset packer silently drops out of `assets/`. they are dropped here instead, so
# that the listing the app walks describes the tree the APK actually carries -- a name in the listing
# that was never packaged is a first launch that aborts part way through unpacking, on the device,
# with everything it needed to know available here.
def _ignored_by_the_packer(name, is_directory):
    return name.startswith(".") or name.endswith("~") or (is_directory and name.startswith("_"))


def entry():
    parser = Parser(description="build the APK")
    parser.add_argument("--install", action="store_true",
                        help="install the APK over whatever is on the device afterwards.")
    parser.add_argument("--name", metavar="LABEL", default=None,
                        help="the name under the launcher icon. defaults to the identity's own.")
    parser.add_argument("--offline", action="store_true",
                        help="hand gradle --offline, so everything resolves from its cache or the "
                             "build fails. the way to find out whether a dependency was fetched "
                             "that nobody declared.")
    vocabulary.add_package(parser)
    vocabulary.add_sharpemu(parser)
    vocabulary.add_common(parser)
    arguments = parser.parse_args()

    toolchain = tc.resolve().require("android_sdk", "jdk", "ndk", "build_tools")
    package = device.application_id(arguments.package, arguments.release)
    label = arguments.name or device.application_label(arguments.package, arguments.release)
    apk = paths.apk(package)

    say("application id: {}".format(package))
    say("label:          {}".format(label))
    say("APK:            {}".format(paths.relative(apk)))

    check_sdk_levels(toolchain)
    # **asserted before any staging, rather than beside the signing it configures.** the three asset
    # trees take minutes to assemble, and a refusal that waits for them is one that arrives after the
    # work it makes pointless. nothing here reaches a device or writes anything.
    if arguments.release:
        release_keystore()
    # **written before anything asks gradle a question**, rather than beside the build it configures.
    # staging the notices runs gradle to find out what it resolved, and gradle cannot configure an
    # android project without being told where the SDK is.
    write_local_properties(toolchain)
    # **emptied once, here, rather than by whichever step runs first.** three asset trees are staged
    # into it and each would otherwise wipe the other's work depending on the order they were called
    # in -- a failure that would show up as an APK missing whichever one was packed first.
    fresh(paths.BUILD_BUNDLE)
    bundled = stage_bundle(arguments.sharpemu, package, toolchain)
    guest_libraries = stage_guest_libs()
    notices = stage_notices(toolchain, arguments.offline)

    keystore(toolchain, arguments.release)

    step("gradle")
    build_with_gradle(toolchain, package, label, arguments.offline, arguments.release)
    collect(apk, arguments.release)
    verify(apk, bundled, guest_libraries, notices, arguments.release)

    say("")
    say("built: {}  {}".format(paths.relative(apk), size(apk.stat().st_size)))

    if arguments.install:
        step("installing")
        attached = device.Device(toolchain, arguments.serial).require()
        attached.install(apk, package)


# --- the assertions that belong on this machine ----------------------------------------------------


def check_sdk_levels(toolchain):
    """the one place two declarations of an SDK level overlap, asserted rather than assumed.

    `toolchain.json` decides which platform is installed and the app's own build file decides what
    it compiles against. a build file compiling against a platform that was never installed fails
    inside gradle with a message about a missing platform, a long way from either declaration.
    """
    build_file = paths.APP / "build.gradle.kts"
    compile_sdk = re.search(r"compileSdk\s*=\s*(\d+)", build_file.read_text(encoding="utf-8"))
    if not compile_sdk:
        raise Refusal("no compileSdk in {}".format(paths.relative(build_file)))
    installed = tc.sdk_version_of(toolchain)
    if int(compile_sdk.group(1)) != installed:
        raise Refusal(
            "{} compiles against SDK {} and the toolchain installs android-{}. bump one to match "
            "the other".format(paths.relative(build_file), compile_sdk.group(1), installed))


# --- the bundled build -----------------------------------------------------------------------------


def stage_bundle(wanted, package, toolchain):
    """assemble the asset tree for the one build that ships inside this APK.

    **exactly one ships, and it is a plain directory tree rather than a zip.** a zip inside an APK is
    an archive inside an archive that already is one, so the payload would be compressed twice and
    the device would pay to undo both.

    **the tree is staged under `build/` and never into the source tree**, so "which build is in this
    APK" is answered by the argument and never by what somebody left lying around. it is emptied on
    every build, which is what stops yesterday's bundle riding along in today's APK.
    """
    step("the bundled build")
    asset = paths.BUILD_BUNDLE / "sharpemu"

    source = vocabulary.read(wanted)
    vocabulary.accept(source, (vocabulary.EXISTING, vocabulary.NONE, vocabulary.PC_PATH,
                               vocabulary.OMITTED), "--sharpemu")

    if source.kind == vocabulary.NONE:
        say("  no build ships in this APK, so a launch will use whatever is staged on the device")
        return None

    if source.kind in (vocabulary.EXISTING, vocabulary.OMITTED):
        # **omitted and `existing` are the same request here.** a script that never touches a device
        # has one place to look, and it is worth saying rather than assuming: this is the one word in
        # the vocabulary whose larder is on this machine rather than on the phone.
        #
        # **and nothing to bundle is a refusal.** producing a bundle-less APK quietly is the exact
        # failure that bundling by default exists to remove, so it must not happen by accident.
        found = builds.find(paths.BUILD_BUILDS)
        if not found:
            raise Refusal(
                "nothing to bundle: no packaged build under {}.\n"
                "  py scripts/package-build.py         packages the fork checkout\n"
                "  --sharpemu <build directory>        bundles one you already have\n"
                "  --sharpemu none                     ships an APK with no build in it".format(
                    paths.relative(paths.BUILD_BUILDS)))
        build = found[0]
        say("  {} is the most recent under {}".format(
            build.directory.name, paths.relative(paths.BUILD_BUILDS)))
    else:
        build = builds.open_build(Path(source.raw))

    build.check()
    check_provenance(build, package)
    check_currency(build, toolchain, named=source.kind == vocabulary.PC_PATH)

    ensure(asset)
    shutil.copytree(str(build.directory), str(asset), dirs_exist_ok=True)
    drop_unpackable(asset)
    write_bundle_meta(build, asset)

    # the listing first, then the identity -- so the identity is not in the listing, is not
    # extracted, and is not hashing itself. the guest libraries are assembled in the same order for
    # the same reason.
    count = write_contents(asset)
    identity = content_hash(asset)
    write_text(asset / "identity", identity + "\n")

    total = tree_size(asset)
    say("  bundling {} {} {}, contract {}".format(
        build.id, build.version, build.commit or "no commit", build.contract))
    say("  {} in {} files, from {}".format(size(total), count, paths.relative(build.directory)))
    say("  identity {}".format(identity[:16]))
    return build


def check_provenance(build, package):
    """the recorded submodule pointer has to name the commit a release APK's build was cut from.

    that pointer is the only thing that makes an APK reproducible from a clone: the build's own
    metadata records a commit from whatever the packager had checked out, while the submodule is what
    this repository claims shipped. shipping a build the pointer does not name publishes an APK whose
    contents nothing can reconstruct.

    **the debug app is not held to that, and the difference is publishability rather than rigour.**
    it installs under its own application id, reaches nobody, and there is no clone to reproduce it
    from -- and holding it to a release-grade gate is what would make bundling by default impossible,
    since every development build would stop until somebody had committed, pushed and moved the
    pointer. the mismatch is printed instead, so it is still known which commit is on the phone.
    """
    release = package != device.DEBUG_ID

    if not build.commit:
        if release:
            raise Refusal(
                "{} records no commit, so it was packaged from an archive and cannot say what "
                "source it came from. an APK bundling it is not reproducible from a clone -- "
                "package the build from the fork instead".format(build.directory.name))
        say("  the bundled build records no commit, so this APK is not reproducible from a clone")
        return

    # **a build published from a tree with edits in it is answered here rather than by the pointer
    # below.** its commit names a real commit and then some, so a comparison against the pointer
    # would fail with a mismatch and offer to check out a revision no repository has.
    if build.from_dirty_tree:
        if release:
            raise Refusal(
                "{} was published from a working tree with uncommitted changes in it, so no commit "
                "names the source it came from. an APK bundling it is not reproducible from a clone "
                "-- commit the changes and package the build again".format(build.directory.name))
        say("  the bundled build is {}, published from a tree with changes in it, so this APK is "
            "not reproducible from a clone".format(build.commit))
        return

    pointer = recorded_submodule_commit()
    if pointer is None:
        if release:
            raise Refusal(
                "external/sharpemu is not a submodule of this repository, so there is no recorded "
                "commit to check {} against. run: git submodule update --init --recursive".format(
                    build.commit))
        say("  external/sharpemu is not a submodule here, so the bundled commit is unchecked")
        return

    if not pointer.startswith(build.commit):
        if release:
            raise Refusal(
                "{} was cut from {} and external/sharpemu is recorded at {}. an APK bundling this "
                "build would ship a commit this repository does not name. move the submodule onto "
                "it and stage the pointer:\n"
                "  git -C external/sharpemu fetch origin\n"
                "  git -C external/sharpemu checkout {}\n"
                "  git add external/sharpemu".format(
                    build.directory.name, build.commit, pointer[:7], build.commit))
        say("  the bundled build is {}; external/sharpemu is recorded at {} -- fine for a "
            "development APK, a release one would refuse this".format(build.commit, pointer[:7]))
    else:
        say("  the bundled build is {}, which external/sharpemu is recorded at".format(build.commit))


def check_currency(build, toolchain, named):
    """the build going into this APK has to be the fork checkout on this machine.

    **this is a different question to the one above and it is the one a person hits daily.**
    provenance asks whether an APK could be rebuilt from a clone, which matters when one ships;
    currency asks whether the payload about to be sealed into an APK contains the work in the tree
    you are editing, which matters every single time. the emulator is developed through the bundled
    build here -- edit the fork, package it, build the APK, run it -- and dropping the packaging step
    from that loop produces a perfectly good APK carrying the payload from before the edit, with
    nothing anywhere saying so.

    **a build that was named is reported and bundled anyway.** naming one is a choice and it is the
    only way to bundle a branch that is not what is checked out, which is how a `perf/` branch is
    packaged by hand. omitting it means the newest under `build/builds` answered and nobody chose, so
    a difference there is the accident this exists for and it refuses.

    a build packaged from an archive is skipped: it records no commit, provenance has already said
    so, and there is nothing to compare. so is a machine with no fork checkout at all, which is a
    clone that has not set one up rather than a mistake.
    """
    if not build.commit:
        return
    try:
        fork = toolchain.fork
    except Refusal as why:
        say("  the bundled build is not compared against a fork checkout -- {}".format(why))
        return
    checkout = builds.checkout_commit(fork)
    if not checkout:
        say("  the bundled build is not compared -- {} would not say what commit it is at".format(
            fork))
        return

    verdict, why = builds.compare_commit(build.commit, checkout, fork,
                                         subject="the build being bundled")
    if verdict == builds.MATCH:
        # **the tree is named and not only the commit.** two checkouts of this fork exist on a
        # development machine and one environment variable decides which one answers -- so a line
        # naming the commit alone is agreement between two trees that may not be the one being
        # worked in, and the two would print identically whenever they sit at the same commit. every
        # other answer this makes already names the tree, because it has something to complain
        # about; this one has to name it precisely because it does not.
        say("  it is the fork checkout, {} in {}".format(checkout, fork))
        return
    if named:
        say("  ** {}".format(why))
        say("     it is bundled anyway, because it was named **")
        return
    raise Refusal(
        "{}.\n"
        "  no --sharpemu was given, so the newest build under {} answered and nobody chose.\n"
        "  py scripts/package-build.py           package the checkout, then build again\n"
        "  --sharpemu <a build directory>        bundle that one, knowing what it is\n"
        "  --sharpemu none                       ship an APK with no build in it".format(
            why, paths.relative(paths.BUILD_BUILDS)))


def recorded_submodule_commit():
    """the commit the submodule pointer names, read out of the index.

    the index entry rather than the object, because a submodule's commits do not live in this
    repository's own object database -- looking one up says nothing useful and writes to the error
    stream while doing it.
    """
    entry = capture(["git", "-C", str(paths.ROOT), "ls-files", "-s", "--", "external/sharpemu"],
                    check=False)
    found = re.match(r"^160000\s+([0-9a-f]{40})", entry.strip())
    return found.group(1) if found else None


def drop_unpackable(asset):
    """remove the names the asset packer would have declined, and say which went.

    removing them rather than filtering the listing keeps the extracted tree equal to what the APK
    carries. a build directory picks these up from whatever produced it, so this is not hypothetical.
    """
    for path in sorted(asset.rglob("*"), key=lambda p: len(p.parts), reverse=True):
        if _ignored_by_the_packer(path.name, path.is_dir()):
            say("  dropping {} -- the asset packer would not have shipped it".format(
                path.relative_to(asset).as_posix()))
            if path.is_dir():
                wipe(path)
            else:
                path.unlink()


def write_bundle_meta(build, asset):
    """the bundled build's identity, regenerated rather than copied.

    **what it drops is the packaging timestamp**, because exactly one of this build exists and
    nothing orders it against anything -- the field was provably doing no work for it, and its
    version, to a person, is its commit. **the name and the author are the other two the bundle
    decides for itself**: this build is the one the app came with, which is what a person needs to
    know and is exactly what it does not share with a staged copy of the same commit, so it is named
    for that here rather than in the build it was cut from, where the name would travel to every
    copy. the author is whoever produced the app, which the app's own screens say once.

    every value is given a default rather than allowed through as a JSON null, because the reader on
    the other side hands a null back as the four-character string "null".
    """
    meta = {
        "id": build.id,
        "name": "Bundled build",
        "sharpemuVersion": build.version,
        "hostContract": build.contract,
        "payload": build.payload.name,
        "env": build.field("env") or {},
        "notes": build.field("notes") or "",
        # the commit is what a person calls this build's version, one tag of upstream's having many
        # builds of the fork under it, and both the About screen and the build manager name it.
        # whether what is on disk is what this APK carries is a different question and the identity
        # beside this tree answers it, so a bundle recording no commit costs nothing here.
        "commit": build.commit,
        "source": build.field("source") or "",
    }
    write_text(asset / "meta.json", json.dumps(meta, indent=4))


# --- the guest libraries ---------------------------------------------------------------------------


def stage_guest_libs():
    """assemble the asset tree for the x86-64 shared objects the guest's own linker searches.

    **the APK is where these come from, and there is no way to build one without them.** reaching a
    device over `adb` alone leaves a release install unable to start a game, and puts a working
    install in that same state after a data wipe -- the platform's own wipe takes the external files
    directory, which is where a staged set lives. so the refusal here is what keeps the failure on this
    machine, where the fetch that fixes it can be named.

    **the identity is computed here rather than on the device.** the set has no natural version:
    twenty-five files come from fixed debian packages and the rest are the thunks' guest halves that
    this repository builds, so nothing but the content names the whole of it. a hash of 12 MB
    measures 15 to 20 ms on the device warm, and the app has to answer "is what is unpacked what we
    ship" on every single launch -- so it is computed once, here, and shipped beside the tree as one
    short string the app compares against the stamp its own unpack wrote.
    """
    step("the guest libraries")
    asset = paths.BUILD_BUNDLE / "guest-libs"

    source = paths.GUEST_LIBS_X86_64
    libraries = sorted(p for p in source.iterdir() if p.is_file()) if source.is_dir() else []
    if not libraries:
        raise Refusal(
            "nothing to bundle: no x86-64 guest libraries under {}. without them an installed app "
            "cannot start a game at all.\n"
            "  py scripts/fetch-guest-libs.py      fetches the glibc set out of debian packages\n"
            "  py scripts/build-thunks.py          builds the two the thunks contribute".format(
                paths.relative(source)))
    for needed in ("ld-linux-x86-64.so.2", "libc.so.6"):
        if not (source / needed).exists():
            raise Refusal("{} is not in {} -- the guest cannot start without it. run: py "
                          "scripts/fetch-guest-libs.py".format(needed, paths.relative(source)))
    # **the notice and the texts are required rather than packed if they happen to be there.** most
    # of this set is unmodified debian binaries under licenses that ask that a recipient be given the
    # terms and be able to get the source, and an APK is where they are distributed -- so a set
    # assembled before the fetch started writing these must not quietly ship without them.
    #
    # the index alone is not enough to check for: it is one small file, and the directory beside it
    # is where the copyright statements and the full license texts are. a set carrying the index and
    # an empty directory would pass a test that only asked about the index.
    # `GPL-2` is in this list because both the glibc and the gcc-12 statements refer to it, not
    # because anything here is under it alone -- a text a statement points at and the set does not
    # carry is a reference that resolves nowhere.
    missing = [name for name in ("licenses.txt", "licenses/glibc.copyright",
                                 "licenses/gcc-12.copyright", "licenses/openssl.copyright",
                                 "licenses/LGPL-2.1", "licenses/GPL-2", "licenses/GPL-3",
                                 "licenses/Apache-2.0")
               if not (source / name).exists()]
    if missing:
        raise Refusal(
            "{} is missing {} -- most of this set is redistributed debian binaries and the APK is "
            "what distributes them, so it ships the terms and the source pointers with them. run: "
            "py scripts/fetch-guest-libs.py".format(
                paths.relative(source), ", ".join(missing)))

    # **the whole directory rather than the files in it.** the set is not only shared objects -- the
    # notice sits beside them and `licenses/` under it -- and a copy that took files would drop the
    # subdirectory silently, which is how the terms these binaries are redistributed under would go
    # missing without anything failing. what the fetch writes is what ships.
    ensure(asset)
    shutil.copytree(str(source), str(asset), dirs_exist_ok=True)
    drop_unpackable(asset)

    # the listing first, then the identity -- so the identity is not in the listing, is not extracted,
    # and is not hashing itself.
    count = write_contents(asset)
    identity = content_hash(asset)
    write_text(asset / "identity", identity + "\n")

    total = tree_size(asset)
    say("  {} in {} files, from {}".format(size(total), count, paths.relative(source)))
    say("  identity {}".format(identity[:16]))
    return count


def content_hash(asset):
    """one hex string naming the whole content of an asset tree. **both trees are hashed with it.**

    every file's path and byte count go into the digest beside its bytes, so a renamed file is a
    different set rather than the same one -- which matters in the guest set, where the linker finds
    a library by the name it is filed under and two of the names are sonames that nothing else spells
    out, and in a build, where `meta.json` names the payload file it must find.
    """
    digest = hashlib.sha256()
    for path in sorted(asset.rglob("*")):
        if not path.is_file():
            continue
        digest.update(path.relative_to(asset).as_posix().encode("utf-8"))
        digest.update(b"\0")
        digest.update(str(path.stat().st_size).encode("ascii"))
        digest.update(b"\0")
        with open(str(path), "rb") as handle:
            for block in iter(lambda: handle.read(1 << 20), b""):
                digest.update(block)
    return digest.hexdigest()


def write_contents(asset):
    """the listing an unpack reads before it starts: a size and a path per line, tab-separated.

    **it is packaging's file rather than part of anything's format**, and it is what buys an unpack
    that knows how much it is about to write before it writes any of it -- an asset in the APK has no
    length to ask for, and `AssetManager.list` returns names without saying which are directories.

    it is written after the walk, so it never appears in its own listing and is therefore never
    extracted. anything else that must stay in the APK and off the device is written after this.
    """
    lines = []
    for path in sorted(asset.rglob("*")):
        if path.is_file():
            lines.append("{}\t{}".format(path.stat().st_size,
                                         path.relative_to(asset).as_posix()))
    write_text(asset / "contents", "\n".join(lines) + "\n")
    return len(lines)


# --- the terms everything else is redistributed under -----------------------------------------------


# what is compiled into the host layer or shipped beside it, and where each one's terms are kept.
#
# **membership is decided by what is in the binary, not by what the build configures.** FEX's
# `External/` holds several more libraries than this, and four of them reach no APK: two are built and
# then linked into nothing, one is header-only and never instantiated, and the allocator is a stub
# whose real implementation is not compiled. a notice for a library a recipient did not receive is
# noise in a list whose whole value is that every line of it is true, so each entry here was checked
# against the symbols in `libsharpdroid-host-layer.so` rather than against the CMake graph.
#
# the identifiers are SPDX where an SPDX identifier is accurate. `cephes` is deliberately not one: its
# terms are a relicensing permission rather than a standard text, so the name says BSD and the
# document itself is what states the grant.
HOST_NOTICES = (
    ("FEXCore", "MIT", ("FEX", "LICENSE")),
    ("{fmt}", "MIT", ("FEX", "External/fmt/LICENSE")),
    ("unordered_dense", "MIT", ("FEX", "External/unordered_dense/LICENSE")),
    ("xxHash", "BSD-2-Clause", ("FEX", "External/xxhash/LICENSE")),
    ("cephes", "BSD", ("FEX", "External/cephes/LICENSE")),
    ("libadrenotools", "BSD-2-Clause", ("ADRENOTOOLS", "LICENSE")),
)

# SoftFloat is the one that cannot be copied from a file. the vendored copy carries no license
# document at all -- upstream keeps it in a directory that was not vendored -- and states its terms in
# a header block at the top of every source file instead. so the notice is lifted out of one, and the
# extraction refuses rather than shipping something shorter than the terms it claims to be.
SOFTFLOAT_SOURCE = ("FEX", "External/SoftFloat-3e/src/extF80_add.c")
SOFTFLOAT_HEADER = re.compile(r"/\*=+\s*(.*?)\s*=+\*/", re.DOTALL)


def stage_notices(toolchain, offline):
    """assemble the asset tree holding the terms of everything the APK redistributes but did not write.

    **an APK is a binary redistribution and the notice travels with it.** the repository carrying
    `external/FEX/LICENSE` satisfies somebody who cloned the repository; a person handed an APK never
    sees `external/`, and they are the ones the permission notices are addressed to.

    three provenances land in one directory, because they are one obligation and the screen that reads
    them is one screen. what is compiled into the host layer is copied out of the submodules it was
    compiled from, so it cannot drift from the code it describes. what lands in the dex is whatever
    gradle resolved, written by the attribution plugin, because the declared dependency list is a
    fraction of the distributed one and a hand-written list would be a statement about the wrong set.
    the guest's x86-64 libraries keep their own tree and are not touched here.
    """
    step("the license notices")
    asset = ensure(paths.BUILD_BUNDLE / "licenses")
    texts = ensure(asset / "texts")
    roots = {"FEX": paths.FEX, "ADRENOTOOLS": paths.ADRENOTOOLS}

    libraries = []
    for name, license, (root, relative) in HOST_NOTICES:
        source = roots[root] / relative
        if not source.exists():
            raise Refusal(
                "{} is not there, so the terms {} is redistributed under cannot be packaged. the "
                "submodules are what these are read from:\n"
                "  git submodule update --init --recursive".format(
                    paths.relative(source), name))
        target = texts / file_name(name)
        shutil.copyfile(str(source), str(target))
        libraries.append({"name": name, "license": license,
                          "text": "texts/" + target.name})

    libraries.append(stage_softfloat_notice(texts))
    libraries.append(stage_stl_notice(toolchain, texts))
    libraries.extend(stage_guest_notices(texts))
    native = len(libraries)

    dex = stage_dex_notices(toolchain, texts, offline)
    libraries.extend(dex)
    # after the dex, because these share the texts it wrote: an icon set states terms without naming a
    # holder in them, so its row points at the same document ninety-odd others do.
    vendored = vendored_asset_notices(texts)
    libraries.extend(vendored)
    libraries.sort(key=lambda entry: entry["name"].lower())

    write_text(asset / "notices.json",
               json.dumps({"libraries": libraries}, indent=2) + "\n")

    say("  {} native: compiled into the host layer, shipped beside it, or searched by the "
        "guest".format(native))
    say("  {} resolved into the dex, from the attribution plugin".format(len(dex)))
    say("  {} committed here as source, which nothing resolves".format(len(vendored)))
    return native, len(dex), len(vendored)


def file_name(name):
    """one library's name as a filename, since two of them are punctuation a path should not carry."""
    return re.sub(r"[^A-Za-z0-9._+-]", "-", name).strip("-")


def stage_softfloat_notice(texts):
    """SoftFloat's terms, lifted out of a source file's header because it ships no license document.

    the block is the copyright line, the three BSD conditions and the disclaimer, which is the whole
    of what the license asks be reproduced. an extraction that came back short would be a notice that
    looked complete and was not, so the length is checked against the disclaimer that ends it.
    """
    source = paths.FEX / SOFTFLOAT_SOURCE[1]
    if not source.exists():
        raise Refusal("{} is not there, so SoftFloat's terms cannot be read. the submodules are what "
                      "these are read from:\n"
                      "  git submodule update --init --recursive".format(paths.relative(source)))
    found = SOFTFLOAT_HEADER.search(source.read_text(encoding="utf-8", errors="replace"))
    if not found or "THIS SOFTWARE IS PROVIDED" not in found.group(1):
        raise Refusal(
            "the license header at the top of {} is not the shape this reads. SoftFloat states its "
            "terms per file rather than in a document, so that block is the notice and packaging "
            "will not invent one.".format(paths.relative(source)))
    target = texts / "SoftFloat-3e"
    write_text(target, found.group(1) + "\n")
    return {"name": "SoftFloat-3e", "license": "BSD-3-Clause", "text": "texts/" + target.name}


def stage_stl_notice(toolchain, texts):
    """the LLVM runtime's terms, from the NDK that produced the copies in the APK.

    **the row is the project rather than one library out of it, because more than one arrives.**
    `libc++_shared.so` is the visible part and the one anybody would think to name, but the compiler's
    builtins are linked statically into the host layer by every NDK link, so a row saying `libc++`
    would name a subset of what this notice actually covers. the notice is the NDK's own and is
    over-inclusive on purpose: trimming it to the parts that apply would mean deciding which of its
    grants a reader may not need, which is not packaging's call to make.
    """
    source = toolchain.ndk_prebuilt / "NOTICE"
    if not source.exists():
        raise Refusal(
            "the NDK's notice is not at {}, so the terms the LLVM runtime in this APK is "
            "redistributed under cannot be packaged. the APK ships libc++_shared.so out of this same "
            "NDK, and links its compiler builtins into the host layer.".format(
                paths.relative(source)))
    target = texts / "LLVM"
    shutil.copyfile(str(source), str(target))
    return {"name": "LLVM", "license": "Apache-2.0 WITH LLVM-exception",
            "text": "texts/" + target.name}


# somebody else's work that lives in this repository as an ordinary source file.
#
# **nothing generated can find these.** they are not a dependency of anything -- they were taken and
# committed -- so a resolver has nothing to resolve, an attribution plugin sees nothing, and the only
# record that they are here is this table.
#
# **the icons are attributed by exception rather than by resemblance.** an icon in this app comes from
# Google's Material set unless it was drawn here, so the set is every icon drawable minus the ones
# named below -- and each of those says in its own comment that it was drawn rather than taken.
# matching a shape instead would attribute by how Material something looks, which is a judgement a
# packaging step should not be making and would get wrong in the safe-looking direction: quietly
# dropping an icon that is Google's because it was redrawn at a different grid.
VENDORED_ASSETS = (
    {"name": "Material Symbols", "license": "Apache-2.0",
     "what": "icons in the app's drawable resources, taken from Google's Material icon set",
     "icons": ("app/src/main/res/drawable", "ic_*.xml"),
     "drawn_here": ("ic_add.xml", "ic_game_placeholder.xml")},
)


# a third-party work carried *inside* one of the resolved dependencies, under terms of its own.
#
# **this is not a dependency and a row of its own would say it was.** the app asks for okhttp; the
# Public Suffix List arrives inside it as a data file. listing it as a peer of okhttp and coil would
# state a relationship this build does not have, so instead the row it arrived in names both licenses
# and the document behind that row carries both texts.
#
# `evidence` is the entry the carrier ships to say so, and it is asserted in the finished APK -- the
# claim made here is then the artefact's rather than this table's.
EMBEDDED_WORKS = (
    {"carrier": "okhttp", "work": "the Public Suffix List", "license": "MPL-2.0",
     "text": "MPL-2.0.txt", "source": "https://publicsuffix.org/list/public_suffix_list.dat",
     "evidence": "okhttp3/internal/publicsuffix/NOTICE"},
)


# one block of the guest set's index: a source package and its version, then the binary packages cut
# from it, the statement covering them and where their source is kept.
GUEST_BLOCK = re.compile(
    r"^(?P<source>\S+) (?P<version>\S+)\n"
    r"(?P<packages>(?:  package .*\n)+)"
    r"  copyright (?P<copyright>\S+)\n"
    r"  source +(?P<url>\S+)$",
    re.MULTILINE)
GUEST_PACKAGE = re.compile(r"^  package +(?P<name>\S+) +-- +(?P<license>.+?)\s*$", re.MULTILINE)

# what a debian copyright statement points at when it names a license by where it lives on a debian
# system rather than quoting it.
GUEST_REFERENCE = re.compile(r"common-licenses/([A-Za-z0-9.+-]+)")


def stage_guest_notices(texts):
    """the x86-64 set the guest's own linker searches, one row per binary package.

    **a debian binary package is the unit here, because it is what a library is everywhere else in
    this list.** a maven artefact and a `.deb` are the same kind of thing -- a named, versioned unit
    somebody redistributes -- so the rows line up and a reader is not asked to hold two ideas of what
    a row means.

    **the index is what this is built from and it cannot drift**, being generated by the fetch out of
    the same table that pins the binaries. parsing it is the alternative to a second copy of those
    pins living here, which is the drift this avoids rather than causes -- and a parse that finds
    nothing is a refusal, so the coupling fails loudly instead of quietly shipping an empty set.

    **the source pointer goes with the binary it corresponds to.** glibc is LGPL and the gcc runtime
    is GPL with an exception, and both ask that the complete corresponding source of the binary be
    available. debian's own statement points at *upstream*, which is not the corresponding source of
    a debian-patched binary -- so the snapshot URL is the only pointer that answers, and it belongs in
    front of the statement rather than in a separate document a reader has to know to open.

    **the texts are the ones that apply, intersected rather than assumed.** a statement covers a whole
    source package, so `gcc-12` refers to licenses that cover documentation and compilers this ships
    no binary of; the set beside the binaries is the set that applies, and naming any other would be a
    claim this APK cannot back.
    """
    index = paths.GUEST_LIBS_X86_64 / "licenses.txt"
    available = paths.GUEST_LIBS_X86_64 / "licenses"
    if not index.exists():
        raise Refusal("{} is not there, so the terms the guest's x86-64 libraries are redistributed "
                      "under cannot be packaged. run: py scripts/fetch-guest-libs.py".format(
                          paths.relative(index)))

    rows = []
    for block in GUEST_BLOCK.finditer(index.read_text(encoding="utf-8")):
        statement = paths.GUEST_LIBS_X86_64 / block.group("copyright")
        if not statement.exists():
            raise Refusal("{} names {} and it is not there".format(
                paths.relative(index), block.group("copyright")))
        body = statement.read_text(encoding="utf-8", errors="replace")
        applicable = sorted({name for name in GUEST_REFERENCE.findall(body)
                             if (available / name).is_file()})

        for package in GUEST_PACKAGE.finditer(block.group("packages")):
            document = [guest_preamble(package.group("name"), block, applicable), body]
            for name in applicable:
                document.append("{0}\n{1}\n{0}\n\n{2}".format(
                    "=" * 78, name, (available / name).read_text(encoding="utf-8",
                                                                 errors="replace")))
            target = texts / file_name(package.group("name"))
            write_text(target, "\n".join(document).rstrip("\n") + "\n")
            rows.append({"name": package.group("name"), "license": package.group("license"),
                         "text": "texts/" + target.name})

    if not rows:
        raise Refusal(
            "nothing was read out of {}. it is generated by scripts/fetch-guest-libs.py and this "
            "reads the shape that writes -- so either it was written by something else, or that "
            "script's format moved and this did not.".format(paths.relative(index)))
    return rows


def guest_preamble(name, block, applicable):
    """what this app can say about a debian binary that debian's own statement does not.

    the version and the source pointer, because a statement names neither; and that the directory is
    replaceable, which is what the LGPL asks be made possible and is met here by a property the design
    already has rather than by anything special.
    """
    return (
        "{name} {version}\n"
        "from debian's {source} source package\n"
        "\n"
        "the complete corresponding source of this binary is kept at\n"
        "  {url}\n"
        "\n"
        "this library is not part of sharpdroid and nothing in the application links against\n"
        "it -- the application is arm64 and this is x86-64, searched by the guest's own dynamic\n"
        "linker under emulation. the directory holding it can be replaced with any compatible set:\n"
        "it is a plain directory of files searched by name, a set placed on the application's\n"
        "external storage is preferred over the one inside it, and the launch log names whichever\n"
        "answered.\n"
        "\n"
        "debian's own copyright statement for this package follows{texts}\n".format(
            name=name, version=block.group("version"), source=block.group("source"),
            url=block.group("url"),
            texts=".\n" if not applicable else
                  ",\nthen the full text of the license it refers to.\n" if len(applicable) == 1 else
                  ",\nthen the full texts of the licenses it refers to.\n"))


def stage_dex_notices(toolchain, texts, offline):
    """what gradle actually resolved into the dex, and the terms each of those is under.

    **the declared dependency list is not the distributed one.** the app names a dozen or so libraries
    and the resolved runtime classpath is several times that, nearly all of it arriving transitively,
    so a list written by hand states something true about `build.gradle` and false about the APK. this
    asks gradle what it resolved instead.

    the export runs as its own invocation because the answer has to exist before the asset tree is
    handed to the build that packages it. it is cheap and it is cached.

    **an entry with no license is a refusal rather than a blank row.** the terms come from each
    artefact's published metadata, and metadata that is missing or names a license the generator has
    no text for is exactly how an attribution list ends up with a confident-looking gap in it.

    **the generator's own file is not what ships.** its schema belongs to a build-time plugin and
    would become something the app has to keep reading correctly across upgrades of that plugin, for
    no gain: what the screen needs is a name, a license and a document, which is the shape everything
    else here is already in. so this returns rows in that shape and the plugin's file stays behind.

    one text per license rather than one per library, because these state their terms without naming
    a holder in them -- the attribution is the entry, and ninety-odd copies of one identical document
    is bytes in the APK that no reader is better off for.
    """
    produced_json = (paths.BUILD / "gradle" / "app" / "generated" / "aboutLibraries"
                     / "aboutlibraries.json")
    # removed first, so a plugin that fails without saying so is caught by the file not being there
    # rather than by the previous build's answer being packaged as this one's.
    produced_json.unlink(missing_ok=True)
    # `--offline` reaches here too. it is asked for to find out whether anything is being fetched that
    # nobody declared, and an invocation exempt from it is one the question is not being asked of.
    run([str(gradle_launcher()), "-p", str(paths.ROOT), ":app:exportLibraryDefinitions",
         "--console=plain", "-q"] + (["--offline"] if offline else []),
        env=gradle_environment(toolchain))
    if not produced_json.exists():
        raise Refusal("the attribution plugin did not write {}".format(paths.relative(produced_json)))

    described = json.loads(produced_json.read_text(encoding="utf-8"))
    known = {name for name, text in described.get("licenses", {}).items() if text.get("content")}
    unstated = sorted(library.get("name") or library.get("uniqueId", "?")
                      for library in described.get("libraries", [])
                      if not (set(library.get("licenses") or ()) & known))
    if unstated:
        raise Refusal(
            "gradle resolved {} into the APK and the terms are unstated or have no text: {}. every "
            "line of this list has to be true, so packaging refuses rather than shipping a row with "
            "nothing behind it.".format(
                "a dependency" if len(unstated) == 1 else "dependencies", ", ".join(unstated)))

    for name in sorted(known):
        write_text(texts / file_name(name), described["licenses"][name]["content"].rstrip("\n") + "\n")

    rows = []
    for library in described.get("libraries", []):
        license = sorted(set(library.get("licenses") or ()) & known)[0]
        rows.append({"name": library.get("name") or library["uniqueId"], "license": license,
                     "text": "texts/" + file_name(license)})
    return fold_embedded_works(rows, texts)


def vendored_asset_notices(texts):
    """somebody else's work committed here as source, which no resolver can see.

    **the counting is the point of this function.** an icon taken from a set and committed leaves no
    trace a build can resolve, so the row would go on being written long after the last of those icons
    had been replaced by a drawing of our own -- an attribution for something the APK no longer
    carries, in a list whose whole value is that every line of it is true.

    **so both ends of the exception are checked.** nothing left to attribute is a refusal, and so is
    an exception naming a file that is gone: the second is what stops the list quietly shrinking as
    drawings are renamed, since a stale exception subtracts something that no longer exists and takes
    a real icon's attribution with it the day that name is reused.
    """
    rows = []
    for asset in VENDORED_ASSETS:
        where, pattern = asset["icons"]
        directory = paths.ROOT / where
        drawn_here = set(asset["drawn_here"])

        missing = sorted(name for name in drawn_here if not (directory / name).exists())
        if missing:
            raise Refusal(
                "{} is excluded from {} as drawn here and is not in {} -- an exception for a file "
                "that is gone subtracts nothing today and the wrong thing the day its name comes "
                "back.".format(", ".join(missing), asset["name"], paths.relative(directory)))

        found = [path for path in sorted(directory.glob(pattern))
                 if path.name not in drawn_here]
        if not found:
            raise Refusal(
                "every {} under {} is excluded as drawn here, so {} describes work this APK no "
                "longer contains -- drop the entry.".format(
                    pattern, paths.relative(directory), asset["name"]))

        text = texts / file_name(asset["license"])
        if not text.exists():
            raise Refusal(
                "{} is under {} and nothing wrote that text. it is shared with what gradle resolved, "
                "so this needs a resolved dependency under the same license or a copy of its "
                "own.".format(asset["name"], asset["license"]))
        say("  {} covers {} file(s) under {}".format(
            asset["name"], len(found), paths.relative(directory)))
        rows.append({"name": asset["name"], "license": asset["license"],
                     "text": "texts/" + text.name})
    return rows


def fold_embedded_works(rows, texts):
    """give a row that carries somebody else's work under other terms both licenses and both texts.

    **the carrier is rewritten rather than joined by a second row**, for the reason the table says: a
    peer row would state a dependency this build does not have.

    a resolved list that no longer contains the carrier is a refusal. the alternative is a declaration
    that quietly stops applying -- which is the failure mode of every hand-written entry beside a
    generated list, and the one thing worth guarding here.
    """
    by_name = {row["name"].lower(): row for row in rows}
    for embedded in EMBEDDED_WORKS:
        row = by_name.get(embedded["carrier"].lower())
        if row is None:
            raise Refusal(
                "{} is declared to carry {} and gradle resolved no such library, so the declaration "
                "describes something this APK does not contain. remove it, or name the library that "
                "replaced it.".format(embedded["carrier"], embedded["work"]))

        text = paths.LICENSE_TEXTS / embedded["text"]
        if not text.exists():
            raise Refusal("{} states the terms of {} and is not there".format(
                paths.relative(text), embedded["work"]))
        carried = texts / file_name(row["name"])
        write_text(carried, "\n".join([
            embedded_preamble(row, embedded),
            (texts / file_name(row["license"])).read_text(encoding="utf-8"),
            "{0}\n{1}\n{0}\n".format("=" * 78, embedded["license"]),
            text.read_text(encoding="utf-8"),
        ]).rstrip("\n") + "\n")

        row["license"] = "{}, and {} for {}".format(row["license"], embedded["license"],
                                                    embedded["work"])
        row["text"] = "texts/" + carried.name
    return rows


def embedded_preamble(row, embedded):
    """what the row's two licenses are and which part of it each one covers."""
    return (
        "{carrier} is under {license}, and carries {work} under {other}.\n"
        "\n"
        "{work_capitalised} is kept at\n"
        "  {source}\n"
        "\n"
        "both texts follow: {license} first, covering {carrier} itself, then {other}.\n".format(
            carrier=row["name"], license=row["license"], work=embedded["work"],
            other=embedded["license"], source=embedded["source"],
            work_capitalised=embedded["work"][0].upper() + embedded["work"][1:]))


# --- gradle -----------------------------------------------------------------------------------------


def gradle_launcher():
    launcher = paths.ROOT / ("gradlew.bat" if tc.IS_WINDOWS else "gradlew")
    if not launcher.exists():
        raise Refusal("missing {}".format(paths.relative(launcher)))
    return launcher


def write_local_properties(toolchain):
    """gradle's own way of being told where the SDK is, written from what the resolver found.

    it is ignored by git: it holds a path that is true on one machine.
    """
    write_text(paths.ROOT / "local.properties", "\n".join([
        "# written by scripts/build-apk.py from what the toolchain resolver found. do not edit:",
        "# it is regenerated on every build, and it is ignored by git because it is true on one",
        "# machine.",
        "sdk.dir=" + str(toolchain.android_sdk).replace("\\", "\\\\"),
    ]) + "\n")


SIGNING_PROPERTIES = ("storeFile", "storePassword", "keyAlias", "keyPassword")


def keystore(toolchain, release):
    """the key this APK is signed with: generated on demand for development, asserted for a release.

    **the two halves are opposites on purpose.** a debug key is disposable and the only thing that
    matters is that it stays the same one on this machine, so a missing one is made on the spot. a
    release key is the opposite in every respect: android refuses an update signed by a different
    key, and the recovery is an uninstall, which takes the save data of everybody who installed the
    last release. generating one on demand would mean a key that quietly reappears as a *new* key,
    with nothing saying so until somebody else's upgrade fails -- so a missing one is refused here.
    """
    if release:
        # already asserted before the staging ran, and there is nothing to generate for a release.
        return

    path = paths.APP / "debug.keystore"
    if path.exists():
        return
    say("generating a debug keystore")
    run([toolchain.jdk / "bin" / ("keytool.exe" if tc.IS_WINDOWS else "keytool"),
         "-genkeypair", "-keystore", str(path), "-alias", "sharpdroid",
         "-storepass", "android", "-keypass", "android",
         "-keyalg", "RSA", "-keysize", "2048", "-validity", "10000",
         "-dname", "CN=sharpdroid"])
    produced(path, "the debug keystore")


def unescape_property(value):
    """read a value the way java's Properties.load reads it, because that is what gradle uses.

    **the backslash is an escape character in this format**, and the file names a windows path. a
    single backslash before an ordinary letter is dropped, so `Y:\\dir\\key.jks` written once loads as
    `Y:dirkey.jks` -- and the two halves of this check would then disagree: reading the value
    literally finds the file and lets the build proceed, and gradle, reading it correctly, fails
    several tasks later on a path nobody typed. a check that passes where the thing it stands in for
    fails is worse than no check.

    the sequences java defines are the four whitespace ones and a unicode escape; anything else keeps
    the character and loses the backslash, which is what turns a doubled one back into a single.
    """
    known = {"t": "\t", "n": "\n", "r": "\r", "f": "\f"}
    out = []
    index = 0
    while index < len(value):
        char = value[index]
        if char != "\\" or index + 1 >= len(value):
            out.append(char)
            index += 1
            continue
        following = value[index + 1]
        if following == "u" and index + 5 < len(value) + 1:
            try:
                out.append(chr(int(value[index + 2:index + 6], 16)))
                index += 6
                continue
            except ValueError:
                pass
        out.append(known.get(following, following))
        index += 2
    return "".join(out)


def release_keystore():
    """assert the release key and the file naming it, before gradle is asked for anything.

    gradle would fail on this too, several tasks in and as a null store file, which names neither the
    file that is missing nor the command that makes one. it is checked here so that the refusal is
    the instruction.

    the passwords are read by the app's build file rather than passed from here: a password handed
    over as a project property is a password in the command line of a process anybody on the machine
    can list.
    """
    properties = paths.APP / "release-signing.properties"
    if not properties.exists():
        raise Refusal(
            "the release identity is signed with a key that is not in this repository and {} is "
            "not there. make one, once, and keep a copy off this machine -- a lost release key "
            "means nobody who installed a release can ever take another one:\n"
            "  keytool -genkeypair -keystore app/release.keystore -alias sharpdroid \\\n"
            "      -keyalg RSA -keysize 4096 -validity 10000 -dname CN=sharpdroid\n"
            "then write {} naming it, with the password you chose:\n"
            "  {}".format(paths.relative(properties), paths.relative(properties),
                          "\n  ".join("{}=".format(key) for key in SIGNING_PROPERTIES)))

    values = {}
    for line in properties.read_text(encoding="utf-8").splitlines():
        line = line.strip()
        if not line or line.startswith("#") or "=" not in line:
            continue
        name, _, value = line.partition("=")
        values[name.strip()] = unescape_property(value.strip())

    missing = [key for key in SIGNING_PROPERTIES if not values.get(key)]
    if missing:
        raise Refusal("{} says nothing about {}".format(
            paths.relative(properties), ", ".join(missing)))

    # the store file is resolved the way gradle's own file() resolves it, which is against the app
    # directory rather than against the repository root or the working directory. an absolute path is
    # left alone, so a key kept outside this tree works and is the better place for one.
    store = Path(values["storeFile"])
    if not store.is_absolute():
        store = paths.APP / store
    if not store.exists():
        raise Refusal("{} names {} and it is not there".format(
            paths.relative(properties), store))

    say("release keystore: {}".format(store))


def gradle_environment(toolchain):
    """the environment gradle needs, including the one variable without which it does not start.

    **the temporary directory is not tidiness.** gradle's client talks to its daemon through a
    selector, and on windows the JDK builds that selector's wakeup pipe out of a unix-domain socket
    placed at an automatically assigned address under the temporary directory. on at least one
    machine this project is developed on, connecting to such a socket under the user's local
    application data fails, and the JDK reports it as being unable to establish a loopback
    connection -- which names TCP loopback and is the wrong component entirely: ordinary loopback
    sockets work fine there.

    it has to be the environment variable. the path is chosen by a native call, so neither of the
    two JVM properties that look like they would move it does. setting it costs nothing on a machine
    that does not have the problem.
    """
    temporary = ensure(paths.BUILD / "gradle" / "tmp")
    return tc.java_home_environment(toolchain, {"TEMP": temporary, "TMP": temporary})


def build_with_gradle(toolchain, package, label, offline, release):
    launcher = gradle_launcher()

    # the C++ runtime, from the resolver rather than from gradle's own idea of which NDK is
    # installed. the host layer links it dynamically and the copy in the APK has to be the one it
    # was linked against.
    stl = toolchain.ndk_sysroot / "usr" / "lib" / "aarch64-linux-android" / "libc++_shared.so"
    if not stl.exists():
        raise Refusal("libc++_shared.so is not where the NDK should have it: {}".format(stl))

    # the build-tools revision, for the same reason as the STL above: it is declared in
    # toolchain.json, and AGP's own default is a different revision that gradle downloads on demand.
    # unstated, the APK is packaged by a revision this repository never asked for and the fetched one
    # sits unused.
    arguments = [str(launcher), ":app:assemble" + build_type(release).capitalize(),
                 "-PsharpdroidStlSo=" + str(stl),
                 "-PsharpdroidBuildTools=" + toolchain.build_tools_version,
                 "-PsharpdroidBundleAssets=" + str(paths.BUILD_BUNDLE),
                 "-PsharpdroidApplicationId=" + package,
                 "-PsharpdroidAppLabel=" + label,
                 "-PsharpdroidCommit=" + repository_commit(),
                 "-PsharpdroidFexVersion=" + fex_version()]
    if offline:
        arguments.append("--offline")

    # **the previous APK is deleted so that a whole new archive is written rather than that one
    # edited.** the packaging step updates the archive in place, and an entry that changes size is
    # appended while the old bytes are left where they were -- so an APK rebuilt all day accumulates
    # holes nothing ever reads. measured once at ten megabytes of dead space in a file whose entries
    # come to twenty-nine. it installs and runs perfectly, which is why it went unnoticed; it costs
    # a third of every install in the deploy loop, and it makes a recorded APK size depend on how
    # many times the APK was built. deleting one file costs a repackage and no recompilation.
    written = gradle_output(release)
    if written.exists():
        written.unlink()

    run(arguments, cwd=paths.ROOT, env=gradle_environment(toolchain))


def repository_commit():
    """the commit this APK is built from, short, with a marker when the tree is not clean.

    the app puts it beside its version on the About screen, and what it is for is a bug report: the
    version alone names a fortnight of commits, so a report against one is a report against whichever
    of them the reporter happened to install.

    **an empty answer is a supported state and not a failure.** a source archive carries no `.git`, and
    a machine may have no `git` on its path at all -- neither is a reason to refuse a build, and the
    screen is written to show the version alone when this says nothing. so every way of not knowing
    lands on the same empty string rather than on an exception or on a word like "unknown", which would
    be a string somebody quotes into a report and then tries to resolve.

    **the dirty marker is the half that earns this.** an APK built from a working tree with edits in it
    is not the commit it names, and it is the ordinary case during development -- so a commit printed
    without one would be the most confident wrong answer this project could put on a screen.
    """
    head = subprocess.run(["git", "-C", str(paths.ROOT), "rev-parse", "--short", "HEAD"],
                          stdout=subprocess.PIPE, stderr=subprocess.DEVNULL,
                          encoding="utf-8", errors="replace")
    if head.returncode != 0:
        return ""
    commit = head.stdout.strip()
    if not commit:
        return ""

    # `status --porcelain` over `diff --quiet`, because the second answers about tracked files that
    # differ and says nothing about a file that was added and never committed. the submodules are
    # excluded: a submodule at a commit other than the recorded one is a real thing to know about and
    # it is not what this line claims, which is what the app's own sources were.
    dirt = subprocess.run(["git", "-C", str(paths.ROOT), "status", "--porcelain",
                           "--ignore-submodules=all"],
                          stdout=subprocess.PIPE, stderr=subprocess.DEVNULL,
                          encoding="utf-8", errors="replace")
    if dirt.returncode == 0 and dirt.stdout.strip():
        commit += "-dirty"
    return commit


def fex_version():
    """the FEXCore the host layer was compiled from, in FEX's own release naming.

    **read out of the host layer's build tree rather than asked of git**, because the value
    describes a binary and git only knows what the checkout says today. this step does not build the
    host layer -- packaging after moving the submodule and before rebuilding is an ordinary thing to
    do, and describing the submodule there labels the archive with a FEXCore it does not contain.
    the About screen is exactly where somebody goes to check that, so it is the one number that must
    not be taken on trust.

    `generated/git_version.h` is FEXCore's own, written when the host layer is configured and read
    by the compiler that builds it. being a compile input is what holds the two together: a version
    that changes recompiles CPUID.cpp and CodeCache.cpp and relinks, so a header naming one FEXCore
    beside a library built from another is not a state an ordinary build reaches. **the string does
    not survive into the library to be read back** -- it is nine bytes of a constexpr array and the
    compiler folds it into immediates -- so the header is what answers, rather than the binary.

    FEX tags a release a month and names it for the month -- `FEX-2607` -- so `describe` against the
    tags is the version a person would quote, and it degrades by itself: a commit past a tag is that
    tag with a count and an abbreviated hash after it, which is still the right answer and still
    readable.

    **the dirty marker is the one this repository should never see.** FEX is a pinned submodule that
    is never modified, so a suffix here is not a stale working tree -- it is that rule having been
    broken, and the About screen is a reasonable place for it to surface.

    empty when the header is missing or names no version, for the reason `repository_commit` is: not
    knowing is a state the screen is written for, and a placeholder is a string somebody would try to
    resolve.
    """
    try:
        header = paths.HOST_FEX_VERSION_HEADER.read_text(encoding="utf-8", errors="replace")
    except OSError:
        return ""
    described = re.search(r'#define\s+GIT_DESCRIBE_STRING\s+"([^"]*)"', header)
    return described.group(1).strip() if described else ""


def build_type(release):
    """which of the app's two build types this APK is, named once for the three places that ask.

    **it tracks the identity rather than being a separate choice.** the release identity is the one
    a stranger installs, and the only difference between the types is that theirs is not debuggable
    -- so a release-identity APK that was debuggable, or a development one that could not be attached
    to, would each be the wrong half of the pair. a third identity named with --package is a
    development one and gets the development type.
    """
    return "release" if release else "debug"


def gradle_output(release):
    kind = build_type(release)
    return paths.BUILD / "gradle" / "app" / "outputs" / "apk" / kind / "app-{}.apk".format(kind)


def collect(apk, release):
    """copy the APK to the path every other script predicts.

    one rule for where an APK is, shared, rather than eight scripts each taught the build system's
    own output layout.
    """
    written = gradle_output(release)
    if not written.exists():
        raise Refusal("gradle reported success and {} is not there".format(
            paths.relative(written)))
    ensure(apk.parent)
    shutil.copyfile(str(written), str(apk))
    produced(apk, "the APK", quiet=True)


# --- what is actually in it ---------------------------------------------------------------------------


def verify(apk, bundled, guest_libraries, notices, release):
    """open the APK and assert what it holds, while the artefact is still on this machine.

    an APK missing its dex installs and then dies at a missing class; one missing the host layer
    installs and dies at a missing library. a missing driver hook is worse than either, because
    nothing fails: the driver loader falls back to the platform's own quietly, so a comparison
    between two drivers would measure one of them twice and report a difference of zero.
    """
    step("what is in the APK")
    with zipfile.ZipFile(str(apk)) as archive:
        sizes = {entry.filename: entry.file_size for entry in archive.infolist()}

    required = ["classes.dex",
                "lib/arm64-v8a/libsharpdroid-host-layer.so",
                "lib/arm64-v8a/libc++_shared.so",
                "lib/arm64-v8a/libmain_hook.so",
                "lib/arm64-v8a/libhook_impl.so"]
    for name in required:
        if name not in sizes:
            raise Refusal("packaging failed: {} is not in the APK".format(name))
        if sizes[name] <= 0:
            raise Refusal("packaging failed: {} is empty".format(name))
        say("  {:>12,}  {}".format(sizes[name], name))

    # **the native libraries have to be stored rather than deflated**, and that is a hard requirement
    # of the driver path rather than a size preference: the driver loader opens its hooks by soname
    # out of the app's native library directory, which only holds real files when the installer
    # extracts them. getting it wrong fails by quietly falling back to the platform's driver.
    for name, count in sizes.items():
        if name.startswith("lib/arm64-v8a/") and count <= 0:
            raise Refusal("packaging failed: {} is empty".format(name))

    # **the guest libraries, before the build.** an APK without them installs, lists games and starts
    # nothing -- the guest's own interpreter is missing, so the failure is several layers below
    # anything the app prints, which is exactly how this shipped unnoticed for as long as it did.
    for name in ("assets/guest-libs/contents", "assets/guest-libs/identity",
                 "assets/guest-libs/ld-linux-x86-64.so.2", "assets/guest-libs/libc.so.6",
                 "assets/guest-libs/libvulkan.so.1", "assets/guest-libs/libaaudio.so",
                 # and the terms they are redistributed under, asserted in the archive rather than
                 # only in the tree it was assembled from.
                 "assets/guest-libs/licenses.txt",
                 "assets/guest-libs/licenses/glibc.copyright",
                 "assets/guest-libs/licenses/gcc-12.copyright",
                 "assets/guest-libs/licenses/openssl.copyright",
                 "assets/guest-libs/licenses/LGPL-2.1", "assets/guest-libs/licenses/GPL-2",
                 "assets/guest-libs/licenses/GPL-3", "assets/guest-libs/licenses/Apache-2.0"):
        if name not in sizes or sizes[name] <= 0:
            raise Refusal("packaging failed: {} is missing or empty in the APK".format(name))
    check_listing(sizes, "guest-libs")
    say("  {:>12,}  the guest libraries, {} files".format(
        sum(count for name, count in sizes.items() if name.startswith("assets/guest-libs/")),
        guest_libraries))

    # **the notices, in the archive rather than only in the tree it was assembled from.** the index
    # is checked line by line rather than as a filename, because an index naming a document the APK
    # does not carry is a row in the list that opens onto nothing -- and that failure looks like a
    # broken screen rather than like a missing notice, which is what it is.
    host_notices, dex_notices, vendored_notices = notices
    if sizes.get("assets/licenses/notices.json", 0) <= 0:
        raise Refusal("packaging failed: assets/licenses/notices.json is missing or empty in the APK")
    with zipfile.ZipFile(str(apk)) as archive:
        index = json.loads(archive.read("assets/licenses/notices.json").decode("utf-8"))
    listed = index.get("libraries", [])
    staged = host_notices + dex_notices + vendored_notices
    if len(listed) != staged:
        raise Refusal("packaging failed: {} notices were staged and the APK's index names {}".format(
            staged, len(listed)))
    for library in listed:
        entry = "assets/licenses/" + library["text"]
        if entry not in sizes or sizes[entry] <= 0:
            raise Refusal(
                "packaging failed: the notice index names {} for {} and the APK does not carry "
                "it".format(library["text"], library["name"]))
    say("  {:>12,}  the license notices, {} native, {} in the dex, {} committed here".format(
        sum(count for name, count in sizes.items() if name.startswith("assets/licenses/")),
        host_notices, dex_notices, vendored_notices))
    check_embedded_works(sizes)
    check_bundled_build(sizes, bundled)
    if release:
        check_release(apk)


def check_embedded_works(sizes):
    """**every notice a dependency ships inside itself is one this build has accounted for.**

    a library that carries somebody else's work under other terms says so in a `NOTICE` beside it, and
    the packer puts that file in the APK whether or not anybody looked at it. so the APK knows the
    answer, and the only way this stays true as dependencies move is to ask it rather than to
    remember: an undeclared one is a refusal here, on this machine, rather than a license found by
    accident a third time.

    the declared ones are asserted in the other direction too. an `evidence` entry that is no longer
    in the APK means the carrier stopped shipping it -- which is the moment the claim in the table
    stops being the artefact's and starts being ours alone.
    """
    declared = {work["evidence"]: work for work in EMBEDDED_WORKS}
    for entry, work in declared.items():
        if entry not in sizes:
            raise Refusal(
                "packaging failed: {} is declared to carry {}, evidenced by {}, and the APK does not "
                "contain it. the carrier no longer says what this build says it says.".format(
                    work["carrier"], work["work"], entry))

    # signature manifests are the packer's own and are not a dependency saying anything.
    found = [name for name in sizes
             if name.rsplit("/", 1)[-1] in ("NOTICE", "NOTICE.txt")
             and not name.startswith(("META-INF/MANIFEST", "assets/"))]
    undeclared = sorted(set(found) - set(declared))
    if undeclared:
        raise Refusal(
            "packaging failed: {} in the APK, shipped by a dependency to state terms of its own, and "
            "nothing accounts for it. read it, and either add it to the embedded-works table or say "
            "why it needs no entry.".format(", ".join(undeclared)))
    say("  {:>12}  {} embedded work(s), each evidenced in the archive".format("", len(declared)))


def check_release(apk):
    """**the release APK proves it is one, rather than being trusted to be.**

    both of these have been wrong in an artefact that looked finished. a debuggable release lets
    anything on the device attach to the process and read the app's private directory, and it is one
    attribute set by a build type -- so the way it goes wrong is that the wrong type was assembled,
    which nothing else here would notice. and an unsigned or debug-signed release is an APK that
    either does not install or can never be updated, and both are found by whoever installed it
    rather than here.

    the manifest is read with aapt2 rather than parsed: the copy in an APK is binary XML, and the
    tool that wrote it is already resolved.
    """
    toolchain = tc.resolve().require("build_tools", "jdk")
    aapt2 = toolchain.build_tools / ("aapt2.exe" if tc.IS_WINDOWS else "aapt2")
    manifest = capture([aapt2, "dump", "xmltree", "--file", "AndroidManifest.xml", str(apk)])
    for line in manifest.splitlines():
        if "android:debuggable" in line and line.rstrip().endswith("=true"):
            raise Refusal(
                "packaging failed: the release APK is debuggable. it was assembled as the wrong "
                "build type -- only the development one sets that attribute")

    apksigner = toolchain.build_tools / ("apksigner.bat" if tc.IS_WINDOWS else "apksigner")
    signature = capture([apksigner, "verify", "--print-certs", str(apk)])
    signer = re.search(r"Signer #1 certificate SHA-256 digest:\s*([0-9a-f]+)", signature)
    if not signer:
        raise Refusal("packaging failed: the release APK carries no signature apksigner can read")

    # **compared against the debug certificate rather than recognised by its shape.** a release key
    # is free to carry any name and any size, so anything that guessed from those would refuse a
    # perfectly good key one day. the debug one is the specific certificate that must not be here,
    # and its password is public and hardcoded above, so reading it needs no secret.
    debug_digest = debug_certificate(toolchain)
    if debug_digest and debug_digest == signer.group(1):
        raise Refusal(
            "packaging failed: the release APK is signed with the generated debug key. it is "
            "throwaway and its password is in a public file, so anybody could publish an APK that "
            "upgrades over this one")
    say("  signed by {}..., and not debuggable".format(signer.group(1)[:16]))


def debug_certificate(toolchain):
    """the SHA-256 of the generated debug key's certificate, or nothing if there is no debug key.

    nothing is a supported answer: a machine that has only ever built the release identity has no
    debug keystore, and the certificate that must not be used is then one that does not exist here.
    """
    path = paths.APP / "debug.keystore"
    if not path.exists():
        return None
    keytool = toolchain.jdk / "bin" / ("keytool.exe" if tc.IS_WINDOWS else "keytool")
    listing = capture([keytool, "-list", "-v", "-keystore", str(path),
                       "-alias", "sharpdroid", "-storepass", "android"])
    found = re.search(r"SHA256:\s*((?:[0-9A-F]{2}:)+[0-9A-F]{2})", listing)
    return found.group(1).replace(":", "").lower() if found else None


def check_bundled_build(sizes, bundled):
    """the build inside the APK, when one was asked for."""
    assets = [name for name in sizes if name.startswith("assets/sharpemu/")]
    if bundled is None:
        if assets:
            raise Refusal("packaging failed: no build was named and the APK carries one anyway")
        say("  no bundled build, as asked")
        return

    # **the identity is required rather than packed if it happens to be there.** without it the app
    # cannot tell the tree on disk from the one in this APK, and the launch that would have noticed
    # runs the previous payload instead of saying anything.
    for name in ("assets/sharpemu/meta.json", "assets/sharpemu/contents",
                 "assets/sharpemu/identity",
                 "assets/sharpemu/" + bundled.payload.name):
        if name not in sizes or sizes[name] <= 0:
            raise Refusal("packaging failed: {} is missing or empty in the APK".format(name))
    if not any(name.startswith("assets/sharpemu/plugins/") for name in sizes):
        raise Refusal("packaging failed: the bundled build has no plugins/ in the APK")

    check_listing(sizes, "sharpemu")
    say("  {:>12,}  the bundled build, {} entries".format(
        sum(count for name, count in sizes.items() if name.startswith("assets/sharpemu/")),
        len(assets)))


def check_listing(sizes, tree):
    """**every line of one tree's listing, against what the archive actually holds.**

    the listing is what the app walks on the first launch that needs the tree, so a name in it that
    was never packaged is an unpack that aborts after writing most of the files -- a failure the
    device would find and this machine can.
    """
    listing = (paths.BUILD_BUNDLE / tree / "contents").read_text(encoding="utf-8")
    for line in listing.splitlines():
        if not line.strip():
            continue
        name = "assets/{}/{}".format(tree, line.split("\t", 1)[1])
        if name not in sizes:
            raise Refusal(
                "packaging failed: {}'s listing names {} and the APK has no such entry".format(
                    tree, name))


if __name__ == "__main__":
    main(entry)
