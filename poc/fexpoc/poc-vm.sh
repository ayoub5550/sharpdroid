#!/bin/sh
# FexPoc inside scripts/vm.py (TCG): correctness first, then same-path timings. never read a
# native-vs-translated ratio off this VM (docs/dry-lab.md, rule 5).
# STAGES picks and orders: t1 t4 una full x64, and the same fex stages under the callback fast path
# and with AFP hidden: t1fast t4fast unafast fullfast fullnoafp, and the fast entry's fallback exit
# forced on every return: fullfallback (default: everything but x64).
# N, ROUNDS size the benches. vm.py forwards no env, so edit STAGES below for ad-hoc runs.
N=${N:-20000}; ROUNDS=${ROUNDS:-3}
STAGES=${STAGES:-"t1 t4 una full t1fast t4fast unafast fullfast fullnoafp fullfallback"}
P=$PWD/payload
mkdir -p ./bench-tmp
F="$P/arm64/FexPoc --blob $P/arm64/guest.bin --lib $P/arm64/libsharpfex.so"
FAST="--fex sfx.Callback=fast"
for s in $STAGES; do
  case $s in
    t1|t1fast) o=; [ $s = t1fast ] && o=$FAST
        echo "=== fex: 1 guest thread under GC ($s)"
        ( cd $P/arm64 && timeout 240 $F --tests-only --threads 1 --thread-n 2000 --thread-timeout 200 $o ); echo "rc=$?" ;;
    t4|t4fast) o=; [ $s = t4fast ] && o=$FAST
        echo "=== fex: 4 guest threads x 1e6 imports under a GC storm ($s)"
        ( cd $P/arm64 && timeout 300 $F --tests-only --threads 4 --thread-n 1000000 --thread-timeout 240 $o ); echo "rc=$?" ;;
    una|unafast) o=; [ $s = unafast ] && o=$FAST
        echo "=== fex: unaligned locked add ($s)"
        ( cd $P/arm64 && timeout 120 $F --tests-only --threads 1 --thread-n 100 --unaligned $o ); echo "rc=$?" ;;
    full) echo "=== fex model (callback legacy)"
        ( cd $P/arm64 && timeout 600 $F --n $N --rounds $ROUNDS --skip-threads $EXTRA ); echo "rc=$?" ;;
    fullfast) echo "=== fex model (callback fast, and fast + CallSave=full)"
        ( cd $P/arm64 && timeout 600 $F --n $N --rounds $ROUNDS --skip-threads $FAST $EXTRA ); echo "rc=$?"
        ( cd $P/arm64 && timeout 600 $F --n $N --rounds $ROUNDS --skip-threads --fex "sfx.Callback=fast;sfx.CallSave=full" $EXTRA ); echo "rc=$?" ;;
    fullnoafp) echo "=== fex model (callback fast, AFP hidden from FEX)"
        ( cd $P/arm64 && timeout 600 $F --n $N --rounds $ROUNDS --skip-threads --fex "sfx.Callback=fast;sfx.AFP=0" $EXTRA ); echo "rc=$?" ;;
    fullfallback) echo "=== fex model (fast entry, every return through CALLBACKRET: the fallback exit)"
        ( cd $P/arm64 && timeout 600 $F --n $N --rounds $ROUNDS --threads 2 --thread-n 20000 --fex sfx.Callback=fast-fallback $EXTRA ); echo "rc=$?" ;;
    x64) echo "=== x64 model (host layer)"
        timeout 600 ./sharpdroid-host-layer --libs ./guest-libs --tmp ./bench-tmp $P/x64/FexPoc --n $N --rounds $ROUNDS \
          --blob $P/x64/guest.bin $EXTRA; echo "rc=$?" ;;
  esac
done
