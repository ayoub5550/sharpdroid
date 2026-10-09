#!/bin/sh
# poc/fexpoc/build.sh [out-dir] -- everything the FexPoc needs, into out-dir (build/fexpoc):
#   guest.bin            the x86-64 guest blob (NDK clang -target x86_64, linked at 0x8_0000_0000)
#   arm64/FexPoc         NativeAOT linux-bionic-arm64 (model "fex"), with libsharpfex.so (stripped)
#   x64/FexPoc           NativeAOT linux-x64 (model "x64": native on x86, or under the host layer)
#   vm/                  a vm.py payload: `scripts/vm.py run --payload build/fexpoc/vm -- "sh ./payload/poc.sh"`
#   device/bundle.tar    for poc/fexpoc/run-device.sh on a phone (needs build/host/sharpdroid-host-layer)
# env: SHARPDROID_NDK (r29), SHARPDROID_CMAKE (cmake 3.22 + ninja), SHARPDROID_DOTNET (sdk 10),
#      FEXPOC_HOST_BUILD (a configured cmake dir of host/, default build/host)
set -e
HERE=$(cd "$(dirname "$0")" && pwd)
ROOT=$(cd "$HERE/../.." && pwd)
OUT=$(mkdir -p "${1:-$ROOT/build/fexpoc}" && cd "${1:-$ROOT/build/fexpoc}" && pwd)
NDK=${SHARPDROID_NDK:?set SHARPDROID_NDK to an r29 NDK}
NDKB=$NDK/toolchains/llvm/prebuilt/linux-x86_64/bin
[ -n "$SHARPDROID_CMAKE" ] && PATH=$SHARPDROID_CMAKE:$PATH
[ -n "$SHARPDROID_DOTNET" ] && PATH=$SHARPDROID_DOTNET:$PATH
export PATH DOTNET_CLI_TELEMETRY_OPTOUT=1
HB=${FEXPOC_HOST_BUILD:-$ROOT/build/host}
OBJ=$OUT/obj; mkdir -p $OBJ $OUT/arm64 $OUT/x64

echo "== guest blob"
$NDKB/clang --target=x86_64-linux-gnu -c $HERE/guest.S -o $OBJ/guest.o
$NDKB/ld.lld -static -nostdlib --image-base=0x800000000 -Ttext=0x800000000 -e _start -o $OBJ/guest.elf $OBJ/guest.o
$NDKB/llvm-objcopy -O binary $OBJ/guest.elf $OUT/guest.bin
cp $OUT/guest.bin $OUT/arm64/; cp $OUT/guest.bin $OUT/x64/

echo "== libsharpfex.so ($HB)"
cmake --build $HB --target sharpfex -j "$(nproc)"
$NDKB/llvm-strip -o $OUT/arm64/libsharpfex.so $HB/libsharpfex.so

echo "== FexPoc arm64 (model fex)"
dotnet publish $HERE/FexPoc.csproj -c Release -r linux-bionic-arm64 -o $OUT/arm64 \
  -p:PublishAotUsingRuntimePack=true -p:DisableUnsupportedError=true \
  -p:CppCompilerAndLinker=$NDKB/aarch64-linux-android30-clang -p:ObjCopyName=$NDKB/llvm-objcopy \
  -p:BaseIntermediateOutputPath=$OBJ/arm64/ -p:BaseOutputPath=$OBJ/bin-arm64/ -nologo -v:q

echo "== FexPoc x64 (model x64)"
dotnet publish $HERE/FexPoc.csproj -c Release -r linux-x64 -o $OUT/x64 -p:CppCompilerAndLinker=gcc \
  -p:BaseIntermediateOutputPath=$OBJ/x64/ -p:BaseOutputPath=$OBJ/bin-x64/ -nologo -v:q
rm -f $OUT/arm64/*.dbg $OUT/x64/*.dbg

echo "== vm payload"
rm -rf $OUT/vm; mkdir -p $OUT/vm/arm64 $OUT/vm/x64
cp $OUT/arm64/FexPoc $OUT/arm64/libsharpfex.so $OUT/guest.bin $OUT/vm/arm64/
cp $OUT/x64/FexPoc $OUT/guest.bin $OUT/vm/x64/
cp $HERE/poc-vm.sh $OUT/vm/poc.sh
: > $OUT/vm/SharpEmu # vm.py stages a payload only if it looks like one

if [ -x $ROOT/build/host/sharpdroid-host-layer ]; then
  echo "== device bundle"
  S=$OBJ/device; rm -rf $S; mkdir -p $S/guest-libs $S/fexpoc/arm64 $S/fexpoc/x64 $OUT/device
  cp $ROOT/build/host/sharpdroid-host-layer $S/
  cp "$(find $NDK -path '*aarch64-linux-android/libc++_shared.so' | head -1)" $S/
  find $ROOT/guest-libs/x86_64 -maxdepth 1 -type f -exec cp {} $S/guest-libs/ \;
  [ -d $ROOT/guest-libs/bin ] && cp $ROOT/guest-libs/bin/* $S/
  cp $OUT/vm/arm64/* $S/fexpoc/arm64/; cp $OUT/vm/x64/* $S/fexpoc/x64/
  chmod -R a+rX $S; chmod 755 $S/sharpdroid-host-layer $S/fexpoc/arm64/FexPoc $S/fexpoc/x64/FexPoc
  tar -cf $OUT/device/bundle.tar -C $S .
  cp $HERE/run-device.sh $OUT/device/run.sh
fi
ls -l $OUT/guest.bin $OUT/arm64/FexPoc $OUT/arm64/libsharpfex.so $OUT/x64/FexPoc
[ -f $OUT/device/bundle.tar ] && ls -l $OUT/device/bundle.tar
exit 0
