#!/system/bin/sh
# DOOM on a phone (Firebase Test Lab's BenchTest, or adb): SharpEmu running a real PS5 app.
# layout in /data/local/tmp/sharpdroid: sharpdroid-host-layer libc++_shared.so guest-libs/
#   payload-doom/SharpEmu (a NativeAOT x64 payload, scripts/package-build.py --nativeaot)
#   games/PPSA96660 games/doom-td doom-native/{doom-arm64,doom-x64,doom1.wad} (poc/ps5-doom/build.sh)
# writes results/doom-summary.txt (and results/doom-*.log, results/doom-frames/).
# the decision rule for the timedemo numbers is docs/dry-lab.md ("DOOM rule"), written before the run.
# knobs (env): DOOM_REPEATS (2 interleaved rounds), DOOM_PLAY_SECONDS (60).
D=${BENCH_DIR:-/data/local/tmp/sharpdroid}
cd $D || exit 1
R=$D/results
REPEATS=${DOOM_REPEATS:-2}; PLAY=${DOOM_PLAY_SECONDS:-60}
mkdir -p $R/doom-frames ./bench-tmp ./bench-home
export LD_LIBRARY_PATH=.
S=$R/doom-summary.txt
: > $S
{
  echo "model=$(getprop ro.product.model) device=$(getprop ro.product.device) sdk=$(getprop ro.build.version.sdk)"
  echo "doom repeats=$REPEATS play=${PLAY}s"
} >> $S
chmod 755 ./sharpdroid-host-layer ./payload-doom/SharpEmu ./doom-native/doom-arm64 ./doom-native/doom-x64 2>/dev/null
# SharpEmu keeps settings and caches under HOME / XDG dirs; the shell uid's HOME is not writable
HL="./sharpdroid-host-layer --libs ./guest-libs --tmp ./bench-tmp --env HOME=$D/bench-home \
 --env XDG_DATA_HOME=$D/bench-home/.local/share --env XDG_CONFIG_HOME=$D/bench-home/.config \
 --env XDG_CACHE_HOME=$D/bench-home/.cache --env DOTNET_EnableWriteXorExecute=0"
W=$D/doom-native
step() { # label timeout command...
  l=$1; t=$2; shift 2
  log=$R/doom-$l.log
  start=$(cut -d' ' -f1 /proc/uptime)
  timeout $t "$@" > $log 2>&1 </dev/null
  rc=$?
  end=$(cut -d' ' -f1 /proc/uptime)
  echo "RUN $l rc=$rc wall=$start..$end" >> $S
  grep -e '^timed' -e 'NOTIFY' -e '\[FPS\]' -e 'CRITICAL' -e 'Unhandled' $log | head -40 | sed "s/^/  $l /" >> $S
}
# S: SharpEmu + FEX running the PS5 eboot; F: the x86-64 twin under FEX alone; A: the arm64 twin.
# no presenter in the timed rows (no host window: SDL finds no video device, flips are not
# presented), exactly as in the sandbox run that put SharpEmu at 97-99 % of native.
i=1; while [ $i -le $REPEATS ]; do
  step td-sharpemu-$i 420 $HL --env SHARPEMU_NO_FLIP_PACING=1 ./payload-doom/SharpEmu $D/games/doom-td/eboot.bin
  step td-x64fex-$i 420 $HL $W/doom-x64 -iwad $W/doom1.wad -timedemo demo1
  step td-arm64-$i 240 $W/doom-arm64 -iwad $W/doom1.wad -timedemo demo1
  i=$((i + 1))
done
# the attract mode with the Android presenter (VK_EXT_headless_surface through the host layer's
# Vulkan) and 60 Hz flip pacing: does it hold 60 fps, and what is on screen.
step play $((PLAY + 30)) timeout $PLAY $HL --env SHARPEMU_HOST_WINDOW=android --env SHARPEMU_LOG_FPS=1 \
  --env SHARPEMU_DUMP_VIDEOOUT=1 --env SHARPEMU_DUMP_VIDEOOUT_DIR=$R/doom-frames \
  --env SHARPEMU_DUMP_VIDEOOUT_EVERY=300 --env SHARPEMU_DUMP_VIDEOOUT_MAX=4 \
  ./payload-doom/SharpEmu $D/games/PPSA96660/eboot.bin
ls -la $R/doom-frames >> $S 2>&1
echo DOOM-DONE >> $S
