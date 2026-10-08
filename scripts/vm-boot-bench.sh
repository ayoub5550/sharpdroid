# the payload's boot, timed inside the VM, one line per run. run by scripts/vm.py, never by hand:
#
#   py scripts/vm.py run --timeout 3600 --payload build/builds/<id> -- sh ./vm-boot-bench.sh
#
# what it times is SharpEmu from exec to exit with an eboot that is not one: the runtime is created
# (every HLE module registered, the loader and the CPU backend constructed, CoreCLR having jitted or
# R2R-loaded all of it) and then refuses the file. that is the part of a boot that is the emulator
# rather than the game, which is what the .NET and FEX knobs below can move.
#
# the VM is TCG on x86-64, so the seconds are not a phone's. compare rows with each other, never with
# a device -- docs/vm.md says why.

RUNS=${BENCH_RUNS:-3}
# BENCH_ONLY=baseline,diskcache-ram runs just those rows; unset runs them all.
ONLY=$(echo "${BENCH_ONLY:-}" | tr , ' ')
mkdir -p ./bench-tmp ./bench-cache ./bench-out
head -c 4096 /dev/zero > ./bench-tmp/eboot.bin

now() { cut -d' ' -f1 /proc/uptime; }

# the base environment is the launcher's own, minus what needs a window, a pad or a speaker.
base() {
  ./sharpdroid-host-layer --libs ./guest-libs --tmp ./bench-tmp \
    --env DOTNET_EnableWriteXorExecute=0 --env SHARPEMU_HOST_WINDOW=android \
    --env SHARPEMU_HOST_AUDIO=android --env SHARPEMU_HOST_INPUT=android \
    "$@"
}

one() {
  label=$1; shift
  if [ -n "$ONLY" ]; then
    case " $ONLY " in *" $label "*) ;; *) return 0 ;; esac
  fi
  i=1
  while [ "$i" -le "$RUNS" ]; do
    start=$(now)
    base "$@" > ./bench-tmp/last.log 2>&1
    rc=$?
    end=$(now)
    echo "BENCH $label run=$i rc=$rc seconds=$(awk "BEGIN{print $end-$start}")"
    # every run's log is kept on the run share, so a row can be read after the VM exits
    cp ./bench-tmp/last.log "./bench-out/$label-$i.log"
    # the outside clock, so a payload's own [BOOT] marks (SHARPEMU_BOOT_TRACE=1) line up with it
    echo "[BENCH] start=$start end=$end" >> "./bench-out/$label-$i.log"
    i=$((i + 1))
  done
}

P=./payload/SharpEmu
E=./bench-tmp/eboot.bin

one baseline            $P $E
# every method the runtime jits, with its tier: what R2R did not cover, and why (docs/performance-roadmap.md)
# where the seconds go: SharpEmu's own phase marks, against the uptime the row started at
one boot-trace          --env SHARPEMU_BOOT_TRACE=1 $P $E
# the HLE warm-up's own knobs (perf/android/lean-warmup); a payload without them ignores them
one warmup-legacy       --env SHARPEMU_BOOT_TRACE=1 --env SHARPEMU_WARMUP=legacy --env SHARPEMU_AEROLIB_PRELOAD=0 $P $E
one warmup-1thread      --env SHARPEMU_BOOT_TRACE=1 --env SHARPEMU_WARMUP_THREADS=1 $P $E
one jit-summary         --env DOTNET_JitStdOutFile=$PWD/bench-out/jit-summary.txt --env DOTNET_JitDisasmSummary=1 $P $E
one tieredpgo-off       --env DOTNET_TieredPGO=0 $P $E
one gen0-64m            --env DOTNET_GCgen0size=0x4000000 $P $E
one pgo-off+gen0-64m    --env DOTNET_TieredPGO=0 --env DOTNET_GCgen0size=0x4000000 $P $E
one r2r-off             --env DOTNET_ReadyToRun=0 $P $E
# the code cache: the first run fills it, the rest read it. the launcher's own two flags.
# diskcache keeps it on the run share, which is uncached 9p: every read of the cache is a round
# trip to the host, under TCG, and the first bench measured that rather than the cache (docs/vm.md).
# diskcache-ram keeps it in the initramfs's own rootfs, which is RAM, as a phone's flash nearly is.
rm -rf ./bench-cache/*
one diskcache           --fex DiskCache=1 --fex DiskCachePath=$PWD/bench-cache/ $P $E
rm -rf /bench-cache-ram; mkdir -p /bench-cache-ram
one diskcache-ram       --fex DiskCache=1 --fex DiskCachePath=/bench-cache-ram/ $P $E

echo "--- the last run's tail"
tail -n 15 ./bench-tmp/last.log
