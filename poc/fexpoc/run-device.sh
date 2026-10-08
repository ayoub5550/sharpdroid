#!/system/bin/sh
# FexPoc on a phone (Firebase Test Lab's BenchTest pushes this as run.sh; adb works the same).
# layout in /data/local/tmp/sharpdroid (bundle-device.sh): sharpdroid-host-layer libc++_shared.so
# guest-libs/ fexpoc/{arm64,x64}/. writes results/summary.txt; BenchTest copies results/ off the device.
# knobs (env): POC_N (1000000), POC_ROUNDS (5 per process), POC_REPEATS (3 interleaved processes per variant).
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
# interleaved, so heat drifts across every variant alike. the decision rule for this run is in
# docs/dry-lab.md ("callback fast path rule"), written before it.
#   fex          sfx_call through FEX's HandleCallback (legacy, the default), import-boundary save
#   fexfast      sfx.Callback=fast: the shim's own guest entry, a matched call-return entry
#   x64          today's model under the host layer
#   fexfastfull  fast entry, and sfx_call saves every vector register too (CallSave=full)
#   fexnoafp     fast entry with FEAT_AFP hidden from FEX: no FPCR write at any JIT<->host
#                transition. diagnostic only (how much of a boundary is the FPCR write?)
#   fexfallback  fast entry, but every return through FEX's CALLBACKRET block (the fast path's
#                fallback exit): its correctness on silicon, and the entry/exit split of the gain
# every fex process also runs the thunk-slot import rows (hle_add_thunk*) and the call_* splits.
i=1; while [ $i -le $REPEATS ]; do
  step fex-$i 300 $F --n $N --rounds $ROUNDS
  step fexfast-$i 300 $F --n $N --rounds $ROUNDS --fex sfx.Callback=fast
  step x64-$i 300 $X --n $N --rounds $ROUNDS
  step fexfastfull-$i 300 $F --n $N --rounds $ROUNDS --skip-threads --fex "sfx.Callback=fast;sfx.CallSave=full"
  step fexnoafp-$i 300 $F --n $N --rounds $ROUNDS --skip-threads --fex "sfx.Callback=fast;sfx.AFP=0"
  step fexfallback-$i 300 $F --n $N --rounds $ROUNDS --skip-threads --fex sfx.Callback=fast-fallback
  i=$((i + 1))
done
step threads4 300 $F --tests-only --threads 4 --thread-n 5000000 --thread-timeout 240
step threads8 300 $F --tests-only --threads 8 --thread-n 2000000 --thread-timeout 240
step threads4fast 300 $F --tests-only --threads 4 --thread-n 5000000 --thread-timeout 240 --fex sfx.Callback=fast
step threads8fast 300 $F --tests-only --threads 8 --thread-n 2000000 --thread-timeout 240 --fex sfx.Callback=fast
step threads4fallback 300 $F --tests-only --threads 4 --thread-n 1000000 --thread-timeout 240 --fex sfx.Callback=fast-fallback
step unaligned 120 $F --tests-only --threads 1 --thread-n 1000 --unaligned
step unalignedfast 120 $F --tests-only --threads 1 --thread-n 1000 --unaligned --fex sfx.Callback=fast
cat /sys/class/thermal/thermal_zone*/temp > $R/thermal-end.txt 2>/dev/null
echo DONE >> $S
cat $S >&3
