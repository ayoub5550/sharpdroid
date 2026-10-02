#!/system/bin/sh
# the host layer's regression set.
#
# pushed to /data/local/tmp/sharpdroid/ and run there, because that is where the guests, the staged
# x86-64 glibc set and the host's own libc++_shared.so live.
#
# **every mode is one lowercase token** -- `smc-full`, `asyncsig-safepoint`, `vulkan-off` -- so a
# result can be grepped for on a word boundary, and a mode named in a commit message or a bug report
# is the same string the run printed. the column is aligned by `report` rather than by spaces inside
# the name, because padding baked into a name stops it being a name.
#
# **a mode that must fail is `run_fails`, not a hand-written pair of echoes.** the negative controls
# are what say the positive ones are testing anything at all: if `vulkan` passed because something
# other than the thunk provided libvulkan.so.1, `vulkan-off` failing to fail is the only thing that
# would notice. they are modes in their own right and are counted like the rest.
cd /data/local/tmp/sharpdroid || exit 1
export LD_LIBRARY_PATH=.

FAILED=0

# %-19s is the width of the longest mode name, `asyncsig-safepoint`. widen it if a longer one
# arrives; nothing breaks if it is not widened, the column just stops lining up.
report() {
  printf '%s  %-19s %s\n' "$1" "$2" "$3"
}

run() {
  NAME=$1
  shift
  ./sharpdroid-host-layer "$@" >./last-run.log 2>&1
  STATUS=$?
  if [ "$STATUS" -eq 0 ]; then
    report PASS "$NAME" ""
  else
    report FAIL "$NAME" "(exit $STATUS)"
    tail -n 25 ./last-run.log
    FAILED=1
  fi
}

# a mode whose whole point is that it does not work. the second argument is what to say when it
# *does*, because "it passed" is the confusing outcome here and a reader needs to be told why that
# is bad news rather than good.
run_fails() {
  NAME=$1
  WHY=$2
  shift 2
  ./sharpdroid-host-layer "$@" >./last-run.log 2>&1
  if [ $? -ne 0 ]; then
    report PASS "$NAME" "fails as it should"
  else
    report FAIL "$NAME" "$WHY"
    FAILED=1
  fi
}

run spike --spike
run hello-nostdlib ./hello-nostdlib
run hello-libc ./hello-libc
run signals ./signals
run getent --libs ./guest-libs ./getent --version
run smc ./smc
# the default asynchronous-signal site is `syscall`, which is what boots the game. it does
# not reach the guest's spinning worker, so case 1 of this guest is expected to fail there -- which
# is a real gap, recorded rather than papered over. `safepoint` is the mode that
# covers all three routes, so that is the one this asserts, and running it here is what keeps the
# interrupt-fault-page machinery from rotting while it waits to be trusted on the real workload.
run asyncsig-safepoint --asyncsig safepoint ./asyncsig

# the pause. the host layer stops the guest for half a second and fails the run unless a thread was
# caught inside translated code; the guest fails it unless its monotonic clock left the half second
# out and its spinning thread stopped for it. run without the self-test, the guest sees no pause and
# must fail -- which is what says the first mode is the pause working rather than a guest that
# passes whatever happens.
run pause --pause-selftest ./pause
run_fails pause-off "the guest passed with nothing pausing it, so it cannot tell a pause from none" ./pause

# the vulkan thunk. these need a working GPU and are the only guests here that depend on
# anything outside the host layer, so a failure is worth reading before assuming a regression:
# `vulkan` checks the marshalling and the stub table, `vkrender` checks that the driver actually
# executes what the guest submits.
run vulkan --vulkan --libs ./guest-libs ./vulkan
run vkrender --vulkan --libs ./guest-libs ./vkrender
run vkswap --vulkan --libs ./guest-libs ./vkswap

# the negative control for the thunk: without --vulkan there is no libvulkan.so.1 to be had, so the
# guest's own ld.so must fail to start it. this is what says the passes above are the thunk working
# rather than something else providing vulkan.
run_fails vulkan-off "vulkan ran without the thunk enabled" --libs ./guest-libs ./vulkan

# the audio thunk. this one makes a noise -- three seconds of a 440 Hz tone on whatever the device
# is currently routing to -- and that is deliberate: the check is that AAudioStream_getFramesRead
# advanced at the stream's own sample rate, which a stream that opened and played nothing cannot do.
run aaudio --audio --libs ./guest-libs ./aaudio

# its negative control, the same shape as vulkan's: without --audio every entry point answers
# AAUDIO_ERROR_UNAVAILABLE, so AAudio_createStreamBuilder must fail and the guest must exit 1.
run_fails aaudio-off "aaudio played without the thunk enabled" --libs ./guest-libs ./aaudio

# and the same guest under the other two SMC modes. `full` is the fallback configuration -- it
# needs no page protection at all, only InvalidateGuestCodeRange -- so it has to keep working or
# there is nothing to fall back to. `none` is expected to *fail*, and is checked for failing:
# it is what says the smc guest is testing something rather than passing by accident.
run smc-full --smc full ./smc
run_fails smc-none "smc passed without SMC detection -- the test is not testing anything" --smc none ./smc

rm -f ./last-run.log
exit $FAILED
