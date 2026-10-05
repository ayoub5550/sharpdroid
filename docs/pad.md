# the pad bridge

android gamepad state and rumble, across the guest boundary, in the same process and the same address space.

it rides in on the syscall boundary [`host-layer.md`](host-layer.md) describes, in a magic number range of its own. that document owns the mechanism and the two invariants; this one starts where a magic number in the pad range has been recognised and owns everything outward from there — the formats, the commands, rumble delivery, and the counters that say whether any of it happened.

**it is not a thunk, and the difference is worth stating first.** the vulkan and audio thunks forward a guest call to a real NDK library. there is no NDK library here: android has no input API an app may read a gamepad from, so the host layer answers these calls itself out of what the app has pushed into it.

## why the direction is inverted

everything else in the host layer flows one way. the app calls down, the guest calls down, and **nothing ever calls up into guest code** — that is the invariant both thunks are built around, and it is why the audio thunk refuses AAudio's three callback setters outright.

input does not naturally fit that. it originates in java, as a `KeyEvent` or a `MotionEvent` delivered to the activity, and it has to reach C# running as guest x86-64. that is host to guest.

so it is inverted into a pull:

```
a KeyEvent or MotionEvent arrives at the activity
  -> PadState edits the one live snapshot and pushes the whole of it down through JNI
    -> the host layer holds the latest, and only the latest
      -> the guest polls for it through a syscall with a magic number
```

no host thread ever enters guest code, and the guest's own thread makes every crossing itself. the two invariants hold unchanged.

**rumble goes the other way and is therefore the easy direction** — a guest call the host answers, exactly like a thunked one.

## why a call and not shared memory

the guest and the host share one address space 1:1, so the obvious cheaper design is a page of host memory whose address the guest is handed at startup: a load instead of a trap.

**measured on the device, a trap here costs 34.3 ns and a load 0.79** — forty-three times, and both nothing. the emulator samples the pad at most once a millisecond per polling guest thread, so a trap costs about **34 µs per second** of one core: at 60 fps that is 0.57 µs of a frame against the 282 µs it would take to lose a single frame per second, or roughly two thousandths of one fps.

what the trap buys for that is three things the page cannot:

- **no structure layout shared between two repositories.** the formats below are checked by the call that carries them — a format and a byte count go in, and a disagreement is refused and named. a mirrored struct is checked by nobody, and the failure is plausible wrong values with nothing erroring.
- **rumble.** it is guest to host, which a call already is. a page needs a second mechanism invented for it and something polling to notice a request.
- **the guest never receives a host address to dereference.** a stale one is a segfault inside the emulator at a moment nothing is watching.

**and it needs no new artefact at all.** the payload reaches the trap by P/Invoking the C library's own `syscall` wrapper, and `libc.so.6` is already among the staged x86-64 shared objects and already how the emulator reaches a dozen other things. there is no generated stub, no library to build and nothing added to `guest-libs/`.

## the commands

`0x50440000` is the magic — one range along from audio's, deliberately distinct so all three stay decodable apart in a trace and in a crash. real linux x86-64 syscall numbers are all below 1000 and this FEXCore has no table indexed by syscall number, so the whole upper range is free and an unrecognised number can never be mistaken for one of ours. the range is tested before the syscall switch, in `LinuxSyscallHandler::Dispatch`.

| command | signature, as the payload calls it | answer |
| --- | --- | --- |
| `0` read | `(format, out, size, port)` | 1 when the port has a pad and 0 when it has none, the buffer written either way; negative on refusal |
| `1` rumble | `(large, small)` | 0, as soon as the request is recorded. port 1's rumble, for the payloads that read a single pad |
| `2` rumble on a port | `(port, large, small)` | 0, as soon as the request is recorded; negative for a port that does not exist |

**there are four ports, numbered 0 to 3 across the boundary**: port index 0 is what the app calls Controller port 1. **the read is a poll and writes into the guest's own buffer in place**, there being no pointer translation anywhere. a rumble returns before anything has buzzed; see below.

**the two rumbles are one request with and without a port.** `1` is answered by passing port index 0 to exactly what `2` does, so neither has behaviour of its own.

## the formats, and the check that replaces a shared layout

**a format is named after the `hostContract` generation that introduced it**, and the number a read sends is that generation — with one exception, in the table. a payload declaring a later generation that leaves the pad alone keeps reading the newest format. [`build-format.md`](build-format.md) owns the generations.

| format | a read sends | size | what it carries |
| --- | --- | --- | --- |
| **the contract 3 format** | `1` | 12 bytes | port 1 only, whatever the port argument says: a `uint32` of button bits, six axis bytes — left X and Y, right X and Y, left trigger, right trigger — a connected flag and one reserved byte |
| **the contract 4 format** | `4` | 64 bytes | the named port, field for field the emulator's own pad state: the first eleven bytes above, then the pad type, connection, battery, a motion flag and a reserved byte, then acceleration and angular velocity as three floats each, then two touch points of an active flag, an id and X and Y as floats |

**the contract 3 format sends 1 rather than 3** because the payloads that read it are already in people's hands and send 1, so 1 is what identifies it.

**the host layer holds the contract 4 state for each port and nothing else, and every format is written from it.** each format is one row of a table in `pad_bridge.cpp` — its generation, the number it sends, its size, which port it is answered from and the function that writes it — so a format is supported by a row and by nothing else, and **the contract 3 row and the port-less rumble are needed only while the app's contract range includes 3**. raising the range's floor past 3 means deleting that row and that rumble.

**sticks are 0..255 with 128 centred and Y growing downward, and triggers are 0..255**, which are the conventions the emulator's own gamepad snapshot already uses, so nothing between an android axis and the guest's pad data rescales anything. motion is in the emulator's units, m/s² and rad/s, and touch points run 0..1 from the top left corner. the pad type and connection are the emulator's own enumerations, and a battery of 0 means nothing knows it.

**the format and the byte count are arguments to every read, and a mismatch on either is refused rather than read**, as is a port that does not exist. the two sides of this live in different repositories that release independently and no compiler ever sees both, so the check is the whole reason a call was chosen over shared memory. the refusal names what the guest asked for and every format the host layer reads, and says the payload and the host layer are out of step; the payload stops asking after one, since it is not a condition that repairs itself mid-run. **a refused read writes nothing**, which the regression set checks along with every format and every refusal.

the button numbering is the **emulator's own seam values**, not the guest's. the translation to `SCE_PAD_BUTTON` bits happens on the payload's side, so no PlayStation ABI value appears anywhere in this repository.

## the mapping

**positional, not by letter.** android names the face buttons after the layout most controllers are printed with, and each maps to where it physically is: A is the bottom button and becomes Cross, B the right and Circle, X the left and Square, Y the top and Triangle. that is what makes a controller with PlayStation glyphs behave the way its glyphs say.

a d-pad arrives either as four keys or as a hat axis, and both are handled — the hat rewrites the four bits whenever the device has one, so returning to centre releases them. a trigger arrives either as a key or as an axis, and again both: a key gives the bit and a full-depth value, an axis gives the depth and also presses the bit, because a game reads one or the other and a pad that only ever sent axes would never appear to press L2.

**events are taken at `dispatchKeyEvent` and `dispatchGenericMotionEvent`, before the view hierarchy**, and that is what makes the d-pad work rather than a stylistic choice: an unconsumed direction key moves focus to whatever is focusable, and the panel drawn over a running guest has a button on it. `KEYCODE_BACK` is deliberately not one of the pad's keys, so a controller's own back button opens that panel like the software one.

**the host layer carries four ports, and one of them reaches a game.** the emulator's pad exports read at most two states, take the type, motion and touch of the first and merge the rest into one pad, so its input source reads port 1 alone — handing it a second port would have two players steering one character. a payload that addresses players separately reads the contract 4 format on each port.

## the two switches

Settings → Controls, both on by default.

| | |
| --- | --- |
| **Automatic controller mapping** | every connected controller, by button position, merged into port 1. off hands input to the port rows, and with none drawn a run has no controller. **the app's rather than a game's**: it is not drawn on the per-game screen, and the launch reads it from the app's own store, so a per-game store holding it is never consulted |
| **Controller vibration** | whether a game may vibrate anything at all. with automatic mapping on, a game's rumble drives every connected controller's motors and the device's own; with it off, the motors the ports name, and with none drawn nothing vibrates. **overridable per game**, so a mapping set up once for the whole install can still be silenced for one title |

**turning rumble off leaves the pad working**, and the mapping decides where rumble goes rather than whether it does — that is Controller vibration's alone.

**neither becomes a launch argument.** they are read once by the process that runs the guest — which is given to one run and ended with it — and applied to what the app does with events it receives and with a request it is handed. so neither of them can move the vector a launch is made with, and nothing about the host layer's flags changes.

**turning the mapping off releases everything first**, rather than simply going quiet: a button held at that moment would otherwise stay held for the rest of the run, since nothing afterwards processes its release. it also stops *consuming* events, so a pad still reaches the app's own screens and the panel over a running guest stays reachable with a d-pad.

**rumble is gated in the app rather than in the bridge**, so the guest's request still crosses and is still counted as asked for and only the platform call stops. that keeps a run with vibration switched off distinguishable in the log from a run where the game never asked — which is the distinction the two counters exist to preserve.

## rumble, and the thread that delivers it

there is no NDK vibrator, so rumble is a JNI call up into the app — the only thing besides the guest file layer that calls upward at all.

**it is not delivered on the guest's thread, and that is the hardest constraint in this part.** a vibrate is a binder round trip to the system server, and the host layer delivers asynchronous signals at syscall exits only while the runtime suspends every thread with `SIGRTMIN` to collect — so a guest thread parked in a platform call is one that cannot acknowledge a collection. that is the same mistake that stopped audio dead partway into runs. so the guest's call records the request and returns, and one host thread of ours does the waiting: attached to the runtime once for its whole life, idle on a condition variable, and holding **a generation per port rather than a flag** so that two requests on one port arriving between deliveries collapse to the newer instead of the older winning. the app is handed the port with every request, `rumble(port, large, small)`, and decides which motor it drives.

the seam sets a level and never says for how long, while a vibrator takes a duration and stops by itself, so each request is a 100 ms pulse — Dolphin's length — and a game holding rumble on sends more of them.

**a motor is sent a level only when the level changes, or when 80 ms of its pulse have gone.** a game holding a level may ask for it on every frame, and under automatic mapping one ask reaches every motor there is, each a binder call — and for a controller, a packet over its link. paced this way a held level costs at most about twelve calls a second per motor, a level that changes is sent at once, and a stop is sent once however often a game repeats it. Eden sends a 50 ms pulse per request; Dolphin sends 100 ms only when a motor crosses half strength.

**the motors are found when a device arrives or leaves**, from the same listener the pad's own state hears, never on a request: asking a device what it can vibrate with is a call into the input service. a controller's motors come from its `VibratorManager` on android 12 and later and its single `Vibrator` below that, and the Odin 3's built-in controls have none, so on that device its own motor is the one that buzzes.

**the seam names two motors and a controller's are numbered rather than named, so under automatic mapping the louder level drives every motor**, the device's single one included. per-trigger vibration and the DualSense adaptive triggers are not forwarded at all: there is no actuator behind either, and an approximation would be indistinguishable from an ordinary rumble at the louder of the two levels. the lightbar is not forwarded for the same reason.

### the permission, and what its absence looks like

rumble needs `android.permission.VIBRATE`, a normal permission granted at install.

**its absence does not look like a missing permission.** `hasVibrator()` and `hasAmplitudeControl()` are both answered truthfully without it, so every capability check reports a healthy actuator and the `vibrate` call alone throws a `SecurityException` naming the permission. a rumble path can therefore look completely ready right up to the first buzz that does not happen.

that is also why the java side **returns whether the platform took the request** and the host counts only the trues. a void call reported success for anything that did not crash, so a refused request counted as delivered — and nothing earlier would have contradicted it.

## saying whether any of it happened

the failure this part is most likely to have is silence, and silence has three causes that look identical from outside: the payload never polled, the payload polled and no pad was connected, or a pad was connected and the game ignored it.

so three lines print once each, whether or not anything is being traced:

- the first read in each format, naming the format
- the first read that finds a pad connected
- the first rumble the guest asks for

and the run summary counts reads, reads that found a pad, **rumbles asked for and rumbles delivered separately** — a gap between those two is what a broken delivery path looks like, and one number could not tell them apart. the summary prints whenever the bridge is enabled, including its zeroes, because a zero is the reading that matters.

| | |
| --- | --- |
| `--pad` | enables the bridge. **off by default**, in the shape `--vulkan` and `--audio` have: without it a poll is refused, the payload reports no pad, and the run is the one it was before this part existed |
| `--trace-pad` | every poll, every rumble asked for, and every rumble delivered with whether the app took it. chatty — up to a thousand lines a second per polling thread, so it is for one question at a time |
| `--pad-selftest` | **one fabricated rumble at full strength on each port in turn when the guest first polls**, 700 ms apart. it exists because the two directions fail independently and an ordinary run exercises only one: a game that polls proves the read path continuously, while rumble is proven by nothing at all unless the title happens to vibrate. every port rather than only those with a pad, because at the first poll the app may not have reported its pads yet. each one announces itself and its port in the log, so a buzz can never be mistaken for a game's own |

the app exposes all three as launch extras — `--ez tracepad`, `--ez padselftest` — and passes `--pad` on every launch.

## what the payload has to do

the launcher sets `SHARPEMU_HOST_INPUT=android` and the payload is expected to register a host input source that polls this bridge: in the contract 3 format at **contract generation 3**, and in the contract 4 format at **generation 4**. [`build-format.md`](build-format.md) owns the numbers.

the range admits generation 2 as well, so a generation-2 build still launches. it registers no input source, and its pad exports then report a controller that is permanently connected and permanently neutral — a game that ignores every button. that is admitted where a missing audio backend is refused, and the difference is what a person can tell: silent audio is indistinguishable from a scene with no music, while a controller that does nothing is obvious within seconds, and the launch log names the generation that ran.
