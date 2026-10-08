#!/bin/sh
# FexPoc inside scripts/vm.py (TCG): correctness first, then same-path timings. never read a
# native-vs-translated ratio off this VM (docs/dry-lab.md, rule 5).
# STAGES picks and orders: t1 t4 una full x64 (default all). N, ROUNDS size the benches.
N=${N:-20000}; ROUNDS=${ROUNDS:-3}; STAGES=${STAGES:-"t1 t4 una full x64"}
P=$PWD/payload
mkdir -p ./bench-tmp
F="$P/arm64/FexPoc --blob $P/arm64/guest.bin --lib $P/arm64/libsharpfex.so"
for s in $STAGES; do
  case $s in
    t1) echo "=== fex: 1 guest thread under GC"
        ( cd $P/arm64 && timeout 240 $F --tests-only --threads 1 --thread-n 2000 --thread-timeout 200 ); echo "rc=$?" ;;
    t4) echo "=== fex: 4 guest threads x 1e6 imports under a GC storm"
        ( cd $P/arm64 && timeout 300 $F --tests-only --threads 4 --thread-n 1000000 --thread-timeout 240 ); echo "rc=$?" ;;
    una) echo "=== fex: unaligned locked add"
        ( cd $P/arm64 && timeout 120 $F --tests-only --threads 1 --thread-n 100 --unaligned ); echo "rc=$?" ;;
    full) echo "=== fex model"
        ( cd $P/arm64 && timeout 600 $F --n $N --rounds $ROUNDS $EXTRA ); echo "rc=$?" ;;
    x64) echo "=== x64 model (host layer)"
        timeout 600 ./sharpdroid-host-layer --libs ./guest-libs --tmp ./bench-tmp $P/x64/FexPoc --n $N --rounds $ROUNDS \
          --blob $P/x64/guest.bin $EXTRA; echo "rc=$?" ;;
  esac
done
