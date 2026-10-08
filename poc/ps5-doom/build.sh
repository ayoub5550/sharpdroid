#!/bin/bash
# DOOM (shareware) as a native PS5 app: a free, legal title to run SharpEmu against.
#
#   bash poc/ps5-doom/build.sh           -> build/ps5-doom/{PPSA96660, doom-td, native/}
#
# what it fetches, all pinned: the PS5 native-app boilerplate (GPL-3.0; its clean-room libc.prx
# runtime and FSELF tooling, and through it the public ps5-payload-sdk), doomgeneric (GPL-2.0),
# and the DOOM v1.9 shareware IWAD (id Software's shareware licence allows passing the unmodified
# file on), taken from the crispy-doom-ps5 v1.0 release and checked by sha256. IWAD=freedoom uses
# Freedoom phase 1 (BSD-3) instead.
#
# what it builds:
#   PPSA96660/   the app folder (eboot.bin, sce_module/libc.prx, sce_sys/, assets/doom1.wad):
#                plays the attract-mode demos, which is real gameplay with no input
#   doom-td/     the same app with assets/args.txt = "-timedemo demo1": renders demo1 as fast as it
#                can and prints "timed N gametics in M realtics (F fps)" -- the benchmark
#   native/      doomgeneric_bench.c built for the host (x86-64, clang) and for android arm64: the
#                same frame conversion, no emulator -- the native baseline for the timedemo
#
# needs: the NDK (SHARPDROID_NDK, for clang/lld and the arm64 twin), g++, make, python3, wget, unzip.
set -euo pipefail
HERE=$(cd "$(dirname "$0")" && pwd)
ROOT=$(cd "$HERE/../.." && pwd)
NDK=${SHARPDROID_NDK:?set SHARPDROID_NDK to an r29 NDK}
NDKB=$NDK/toolchains/llvm/prebuilt/linux-x86_64/bin
OUT=${OUT:-$ROOT/build/ps5-doom}
WORK=${WORK:-$OUT/work}
BOILERPLATE_REPO=https://github.com/blackbearreloaded/ps5-native-app-boilerplate.git
BOILERPLATE_COMMIT=3ada439fa044f60c8b488678577a740761385711
DOOMGENERIC_REPO=https://github.com/ozkl/doomgeneric.git
DOOMGENERIC_COMMIT=dcb7a8dbc7a16ce3dda29382ac9aae9d77d21284
WAD_ZIP=https://github.com/alex-free/crispy-doom-ps5/releases/download/v1.0/crispy-doom-ps5-v1.0-7.1.0.zip
WAD_SHA256=1d7d43be501e67d927e415e0b8f3e29c3bf33075e859721816f652a526cac771
FREEDOOM_ZIP=https://github.com/freedoom/freedoom/releases/download/v0.13.0/freedoom-0.13.0.zip

mkdir -p "$WORK" "$OUT"
fetch() { # repo commit dir
  [ -d "$3/.git" ] || git clone -q "$1" "$3"
  git -C "$3" fetch -q --depth 1 origin "$2" 2>/dev/null || true
  git -C "$3" checkout -q "$2"
}
fetch $BOILERPLATE_REPO $BOILERPLATE_COMMIT "$WORK/boilerplate"
fetch $DOOMGENERIC_REPO $DOOMGENERIC_COMMIT "$WORK/doomgeneric"

# the IWAD
WAD=$WORK/doom1.wad
if [ "${IWAD:-shareware}" = freedoom ]; then
  [ -f "$WORK/freedoom.zip" ] || wget -q -O "$WORK/freedoom.zip" $FREEDOOM_ZIP
  unzip -q -o -j "$WORK/freedoom.zip" '*/freedoom1.wad' -d "$WORK"
  cp "$WORK/freedoom1.wad" "$WAD"
else
  if [ ! -f "$WAD" ] || ! echo "$WAD_SHA256  $WAD" | sha256sum -c --status; then
    [ -f "$WORK/crispy.zip" ] || wget -q -O "$WORK/crispy.zip" $WAD_ZIP
    unzip -q -o -j "$WORK/crispy.zip" 'CrispyDoom/doom/DOOM1.WAD' -d "$WORK"
    mv "$WORK/DOOM1.WAD" "$WAD"
    echo "$WAD_SHA256  $WAD" | sha256sum -c --strict
  fi
fi

# the boilerplate wants clang-18/llvm-config on PATH: point them at the NDK's clang (21), which
# builds a byte-identical libc.prx (the boilerplate checks its sha256)
TOOLS=$WORK/tools; mkdir -p "$TOOLS"
for t in clang clang++ ld.lld llvm-ar llvm-ranlib llvm-objcopy llvm-strip llvm-nm; do ln -sf "$NDKB/$t" "$TOOLS/$t"; done
ln -sf "$NDKB/clang" "$TOOLS/clang-18"
command -v ninja >/dev/null || { [ -n "${SHARPDROID_CMAKE:-}" ] && ln -sf "$SHARPDROID_CMAKE/ninja" "$TOOLS/ninja"; }
cat > "$TOOLS/llvm-config" <<EOF
#!/bin/sh
case "\$1" in
  --bindir) echo $NDKB;;
  --version) $NDKB/clang --version | head -1 | sed 's/.*version \([0-9.]*\).*/\1/';;
  --prefix) echo $(dirname "$NDKB");;
  --libdir) echo $(dirname "$NDKB")/lib;;
esac
EOF
chmod +x "$TOOLS/llvm-config"
export PATH=$TOOLS:$PATH LLVM_CONFIG=$TOOLS/llvm-config USE_CCACHE=0 CXX=${CXX:-g++}

# the app tree: the boilerplate with src/ replaced by doomgeneric and the PS5 platform layer
B=$WORK/boilerplate
rm -rf "$B/src" "$B/assets" "$B/dist" "$B/build" && mkdir -p "$B/src/doom" "$B/assets"
DG=$WORK/doomgeneric/doomgeneric
# every engine file, minus the other ports' platform layers, their sound/music backends and the
# build files (doomgeneric_ps5.c is this port's platform layer)
cp "$DG"/*.c "$DG"/*.h "$B/src/doom/"
(cd "$B/src/doom" && rm -f doomgeneric_allegro.c doomgeneric_emscripten.c doomgeneric_linuxvt.c \
  doomgeneric_sdl.c doomgeneric_soso.c doomgeneric_sosox.c doomgeneric_win.c doomgeneric_xlib.c \
  gusconf.c i_allegromusic.c i_allegrosound.c i_sdlmusic.c i_sdlsound.c icon.c mus2mid.c)
cp "$WORK/doomgeneric/LICENSE" "$B/src/doom/LICENSE-doomgeneric.txt"
cp "$HERE/doomgeneric_ps5.c" "$B/src/"
cp "$HERE/param.json" "$B/sce_sys/param.json"
cp "$WAD" "$B/assets/doom1.wad"
(cd "$B" && bash tools/setup-native-dependencies.sh && APP_INCLUDE_PATHS=src/doom bash tools/build.sh Folder)
rm -rf "$OUT/PPSA96660" "$OUT/doom-td"
cp -a "$B/dist/PPSA96660" "$OUT/PPSA96660"
cp -a "$B/dist/PPSA96660" "$OUT/doom-td"
echo "-timedemo demo1" > "$OUT/doom-td/assets/args.txt"

# the native twins
mkdir -p "$OUT/native"
SRCS=$(ls "$B"/src/doom/*.c)
"$NDKB/clang" --target=x86_64-pc-linux-gnu --gcc-toolchain=/usr -fuse-ld=lld -O2 -w -DNORMALUNIX -DLINUX \
  -I"$B/src/doom" $SRCS "$HERE/doomgeneric_bench.c" -o "$OUT/native/doom-x64" -lm
"$NDKB/aarch64-linux-android30-clang" -O2 -w -DNORMALUNIX -DLINUX \
  -I"$B/src/doom" $SRCS "$HERE/doomgeneric_bench.c" -o "$OUT/native/doom-arm64" -lm
cp "$WAD" "$OUT/native/doom1.wad"
ls -la "$OUT" "$OUT/PPSA96660" "$OUT/native"
