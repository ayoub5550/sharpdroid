#!/system/bin/sh
# FexPoc on a phone (Firebase Test Lab's BenchTest pushes this as run.sh; adb works the same).
# layout in /data/local/tmp/sharpdroid (bundle-device.sh): sharpdroid-host-layer libc++_shared.so
# guest-libs/ fexpoc/{arm64,x64}/. writes results/summary.txt; BenchTest copies results/ off the device.
# knobs (env): POC_N (1000000), POC_ROUNDS (5 per process), POC_REPEATS (3 interleaved processes per model).
D=${BENCH_DIR:-/data/local/tmp/sharpdroid}
cd $D || exit 1
N=${POC_N:-1000000}; ROUNDS=${POC_ROUNDS:-5}; REPEATS=${POC_REPEATS:-3}
R=$D/results
mkdir -p $R ./bench-tmp
exec 3>&1 > $R/run.log 2>&1 </dev/null
export LD_LIBRARY_PATH=.
S=$R/summary.txt
: > $S
{
  echo "model=$(getprop ro.product.model) device=$(getprop ro.product.device) sdk=$(getprop ro.build.version.sdk)"
  echo "uname=$(uname -a)"
  echo "n=$N rounds=$ROUNDS repeats=$REPEATS"
} >> $S
cat /sys/class/thermal/thermal_zone*/temp > $R/thermal-start.txt 2>/dev/null
P=$D/fexpoc
F="$P/arm64/FexPoc --blob $P/arm64/guest.bin --lib $P/arm64/libsharpfex.so"
X="./sharpdroid-host-layer --libs ./guest-libs --tmp ./bench-tmp $P/x64/FexPoc --blob $P/x64/guest.bin"
chmod 755 $P/arm64/FexPoc $P/x64/FexPoc ./sharpdroid-host-layer 2>/dev/null
step() { # label timeout command...
  l=$1; t=$2; shift 2
  log=$R/$l.log
  timeout $t "$@" > $log 2>&1 </dev/null
  rc=$?
  echo "RUN $l rc=$rc" >> $S
  grep -e '^\[TEST\]' -e '^\[BENCH\]' -e '^\[POC\]' -e '^\[THREADS\]' $log | sed "s/^/  $l /" >> $S
}
# interleaved, so heat drifts across both models alike
# fex: sfx_call saves the import-boundary state (default). fexfull: it saves every vector register too.
i=1; while [ $i -le $REPEATS ]; do
  step fex-$i 300 $F --n $N --rounds $ROUNDS
  step x64-$i 300 $X --n $N --rounds $ROUNDS
  step fexfull-$i 300 $F --n $N --rounds $ROUNDS --skip-threads --fex sfx.CallSave=full
  i=$((i + 1))
done
step threads4 300 $F --tests-only --threads 4 --thread-n 5000000 --thread-timeout 240
step threads8 300 $F --tests-only --threads 8 --thread-n 2000000 --thread-timeout 240
step unaligned 120 $F --tests-only --threads 1 --thread-n 1000 --unaligned
cat /sys/class/thermal/thermal_zone*/temp > $R/thermal-end.txt 2>/dev/null
echo DONE >> $S
cat $S >&3
