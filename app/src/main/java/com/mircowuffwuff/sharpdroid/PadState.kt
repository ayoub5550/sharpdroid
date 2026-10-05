package com.mircowuffwuff.sharpdroid

import android.os.Build
import android.util.SparseArray
import android.view.InputDevice
import android.view.KeyEvent
import android.view.MotionEvent
import kotlin.math.abs
import kotlin.math.roundToInt

/**
 * one gamepad, as the emulator's input seam wants it, assembled from android's key and motion events.
 *
 * the button numbering is the seam's own and not the guest's: the emulator translates these to
 * `SCE_PAD_BUTTON` bits on its side of the boundary, so no PlayStation ABI value appears here. sticks
 * are 0..255 with 128 centred and **Y growing downward**, which matches both android's axis sign and
 * the seam's convention, so nothing is flipped anywhere.
 */
object PadButton {
    const val UP = 1 shl 0
    const val DOWN = 1 shl 1
    const val LEFT = 1 shl 2
    const val RIGHT = 1 shl 3
    const val CROSS = 1 shl 4
    const val CIRCLE = 1 shl 5
    const val SQUARE = 1 shl 6
    const val TRIANGLE = 1 shl 7
    const val L1 = 1 shl 8
    const val R1 = 1 shl 9
    const val L2 = 1 shl 10
    const val R2 = 1 shl 11
    const val L3 = 1 shl 12
    const val R3 = 1 shl 13
    const val OPTIONS = 1 shl 14
    const val TOUCHPAD = 1 shl 15
    const val CREATE = 1 shl 16
    const val PS = 1 shl 17
    const val MIC = 1 shl 18
}

/**
 * what a binding in a controller mapping can drive on a port: 17 buttons, four directions on each
 * stick, and the two triggers.
 *
 * **a trigger is one target and gives both the L2 or R2 press and its depth**, as a real DualSense
 * does, so L2 and R2 are not among the buttons. Dolphin binds a trigger's press and depth separately,
 * which a GameCube trigger's click at the end of its travel needs and a DualSense's does not.
 *
 * the names are the controller mapping file's, and a target's index is its position in [NAMES].
 */
object PadTarget {
    const val COUNT = 27

    /** the targets below this are buttons, each pressing [BUTTON_BITS] at its own index. */
    const val BUTTONS = 17

    /** each stick's four directions start here, in the order up, down, left, right. */
    const val LEFT_STICK = 17
    const val RIGHT_STICK = 21

    const val L2 = 25
    const val R2 = 26

    @JvmField
    val NAMES = arrayOf(
        "up", "down", "left", "right", "cross", "circle", "square", "triangle",
        "l1", "r1", "l3", "r3", "options", "touchpad", "create", "ps", "mic",
        "left-stick-up", "left-stick-down", "left-stick-left", "left-stick-right",
        "right-stick-up", "right-stick-down", "right-stick-left", "right-stick-right",
        "l2", "r2",
    )

    @JvmField
    val BUTTON_BITS = intArrayOf(
        PadButton.UP, PadButton.DOWN, PadButton.LEFT, PadButton.RIGHT,
        PadButton.CROSS, PadButton.CIRCLE, PadButton.SQUARE, PadButton.TRIANGLE,
        PadButton.L1, PadButton.R1, PadButton.L3, PadButton.R3,
        PadButton.OPTIONS, PadButton.TOUCHPAD, PadButton.CREATE, PadButton.PS, PadButton.MIC,
    )
}

/**
 * the live state of the pads, and the only thing that talks to the host layer about input.
 *
 * **two ways to fill it, and a run uses one.** automatic mapping merges every controller into
 * Controller port 1 by button position. a controller mapping -- [useMapping], with automatic mapping
 * off -- drives all four ports from whatever inputs [ControllerMapping] binds, and nothing else.
 *
 * **held here rather than rebuilt per event**, because android delivers one event per changed control:
 * a key event says a button went down and says nothing about the sticks, so a snapshot assembled from
 * one event alone would report every other control as released. so each event edits this and the whole
 * of a port is pushed down afterwards.
 *
 * not thread-safe and deliberately not synchronised: every caller is the input dispatch on the UI
 * thread. the crossing into other threads happens on the native side, which takes a lock for it.
 */
object PadState {

    private const val TAG = "sharpdroid"

    /** centre for a stick axis. the seam's convention, and android's 0.0 maps onto it. */
    private const val CENTRE = 128

    private var buttons = 0
    private var leftX = CENTRE
    private var leftY = CENTRE
    private var rightX = CENTRE
    private var rightY = CENTRE
    private var leftTrigger = 0
    private var rightTrigger = 0

    /**
     * which device ids are gamepads automatic mapping is tracking, in the order they arrived.
     *
     * every one of them is merged into port 1, so what this set is for is knowing whether *any* pad
     * is present, so that unplugging one of two does not report the pad as gone.
     */
    private val devices = LinkedHashSet<Int>()

    /** true once anything has been seen, which is what the guest is told as `connected`. */
    val connected: Boolean get() = devices.isNotEmpty()

    /**
     * whether automatic mapping runs -- Settings, Controls, Automatic controller mapping.
     *
     * **turning it off releases everything and says so, rather than simply going quiet.** a button
     * held at the moment it is switched off would otherwise stay held for the rest of the run, since
     * nothing after this point will process its release.
     *
     * **events are then not consumed by it either**, which matters more than it looks: an unconsumed
     * key goes on to the view hierarchy, so the panel drawn over a running guest stays reachable with
     * the d-pad. under a controller mapping only a bound event is consumed, for the same reason.
     */
    @JvmStatic
    var enabled: Boolean = true
        set(value) {
            if (field == value) return
            field = value
            if (!value) {
                devices.clear()
                release()
            }
            push()
        }

    /**
     * whether this event came from something with a stick or a gamepad button on it.
     *
     * the source is a bit mask and a device is commonly several things at once -- a gamepad that also
     * reports itself as a keyboard is the normal case, which is why this tests for the bits rather
     * than comparing equality.
     */
    @JvmStatic
    fun isGamepad(device: InputDevice?): Boolean {
        if (device == null) {
            return false
        }
        val sources = device.sources
        return (sources and InputDevice.SOURCE_GAMEPAD) == InputDevice.SOURCE_GAMEPAD ||
                (sources and InputDevice.SOURCE_JOYSTICK) == InputDevice.SOURCE_JOYSTICK
    }

    /**
     * the button mapping, or 0 for a key that is not one of ours.
     *
     * **positional rather than by letter.** Android names the face buttons A, B, X and Y after the
     * layout most controllers are printed with, and the mapping is by where the button physically is:
     * a is the bottom button and becomes Cross, B is the right one and becomes Circle, X is the left
     * one and becomes Square, Y is the top one and becomes Triangle. that is what makes a controller
     * with PlayStation glyphs on it behave the way its glyphs say, and it is what every other android
     * emulator of a PlayStation does.
     *
     * `KEYCODE_BACK` is deliberately absent. a gamepad that reports its own back button would
     * otherwise open the in-game panel, and on this device the built-in controls do exactly that.
     */
    private fun buttonFor(keyCode: Int): Int = when (keyCode) {
        KeyEvent.KEYCODE_BUTTON_A -> PadButton.CROSS
        KeyEvent.KEYCODE_BUTTON_B -> PadButton.CIRCLE
        KeyEvent.KEYCODE_BUTTON_X -> PadButton.SQUARE
        KeyEvent.KEYCODE_BUTTON_Y -> PadButton.TRIANGLE
        KeyEvent.KEYCODE_DPAD_UP -> PadButton.UP
        KeyEvent.KEYCODE_DPAD_DOWN -> PadButton.DOWN
        KeyEvent.KEYCODE_DPAD_LEFT -> PadButton.LEFT
        KeyEvent.KEYCODE_DPAD_RIGHT -> PadButton.RIGHT
        KeyEvent.KEYCODE_BUTTON_L1 -> PadButton.L1
        KeyEvent.KEYCODE_BUTTON_R1 -> PadButton.R1
        // the triggers arrive as keys on a pad with digital ones and as axes on a pad with analogue
        // ones. both are handled: this sets the button bit, and the axis path below sets the depth.
        KeyEvent.KEYCODE_BUTTON_L2 -> PadButton.L2
        KeyEvent.KEYCODE_BUTTON_R2 -> PadButton.R2
        KeyEvent.KEYCODE_BUTTON_THUMBL -> PadButton.L3
        KeyEvent.KEYCODE_BUTTON_THUMBR -> PadButton.R3
        KeyEvent.KEYCODE_BUTTON_START -> PadButton.OPTIONS
        KeyEvent.KEYCODE_BUTTON_SELECT -> PadButton.CREATE
        else -> 0
    }

    /**
     * takes a key event. returns true when it was the pad's, in which case it goes no further.
     *
     * **consuming it is what stops a d-pad walking the app's focus.** an unconsumed `DPAD_DOWN`
     * reaching the view hierarchy moves focus to whatever is focusable, and the in-game panel has a
     * button on it.
     */
    @JvmStatic
    fun onKey(event: KeyEvent): Boolean {
        if (mapping != null) {
            return onMappedKey(event)
        }
        if (!enabled || !isGamepad(event.device)) {
            return false
        }
        val bit = buttonFor(event.keyCode)
        if (bit == 0) {
            return false
        }
        devices.add(event.deviceId)

        // a repeat is android's auto-repeat on a key that is already down, and it says nothing new.
        // acting on it would be a press per repeat interval for a finger that never moved.
        if (event.action == KeyEvent.ACTION_DOWN && event.repeatCount > 0) {
            return true
        }

        buttons = when (event.action) {
            KeyEvent.ACTION_DOWN -> buttons or bit
            KeyEvent.ACTION_UP -> buttons and bit.inv()
            else -> return false
        }
        // a digital trigger has no depth to report, so the button bit is given one: fully down or not
        // at all. a pad with analogue triggers sends the axis instead and the axis wins, since it
        // arrives as its own event and overwrites this.
        when (bit) {
            PadButton.L2 -> leftTrigger = if (event.action == KeyEvent.ACTION_DOWN) 255 else 0
            PadButton.R2 -> rightTrigger = if (event.action == KeyEvent.ACTION_DOWN) 255 else 0
        }
        push()
        return true
    }

    /**
     * takes a joystick motion event. returns true when it was the pad's.
     *
     * one event carries every axis the device has, so all of them are read rather than looking for
     * which changed -- android does not say, and reading them all is what makes the snapshot whole.
     */
    @JvmStatic
    fun onMotion(event: MotionEvent): Boolean {
        if (mapping != null) {
            return onMappedMotion(event)
        }
        if (!enabled || !isGamepad(event.device) ||
            event.action != MotionEvent.ACTION_MOVE) {
            return false
        }
        devices.add(event.deviceId)

        leftX = axisToByte(event, MotionEvent.AXIS_X)
        leftY = axisToByte(event, MotionEvent.AXIS_Y)
        // Z and RZ are where the right stick is on essentially every android gamepad, the HID usage
        // tables having no second stick of their own.
        rightX = axisToByte(event, MotionEvent.AXIS_Z)
        rightY = axisToByte(event, MotionEvent.AXIS_RZ)

        // two spellings for the same pair of controls, and a device uses one or the other. LTRIGGER
        // and RTRIGGER are the gamepad names; BRAKE and GAS are what a device described as a wheel
        // reports, and several handheld pads describe themselves that way. the larger of the two is
        // taken so that a device sending only one is unaffected by the other reading zero.
        leftTrigger = maxOf(
            triggerToByte(event, MotionEvent.AXIS_LTRIGGER),
            triggerToByte(event, MotionEvent.AXIS_BRAKE))
        rightTrigger = maxOf(
            triggerToByte(event, MotionEvent.AXIS_RTRIGGER),
            triggerToByte(event, MotionEvent.AXIS_GAS))

        // a d-pad that reports as a hat rather than as four keys. the bits are rewritten from the hat
        // whenever the device has one, so a hat returning to centre releases them.
        if (hasAxis(event.device, MotionEvent.AXIS_HAT_X) ||
            hasAxis(event.device, MotionEvent.AXIS_HAT_Y)) {
            val hatX = event.getAxisValue(MotionEvent.AXIS_HAT_X)
            val hatY = event.getAxisValue(MotionEvent.AXIS_HAT_Y)
            buttons = buttons and
                    (PadButton.LEFT or PadButton.RIGHT or PadButton.UP or PadButton.DOWN).inv()
            if (hatX < -0.5f) buttons = buttons or PadButton.LEFT
            if (hatX > 0.5f) buttons = buttons or PadButton.RIGHT
            if (hatY < -0.5f) buttons = buttons or PadButton.UP
            if (hatY > 0.5f) buttons = buttons or PadButton.DOWN
        }

        // the analogue triggers also press the digital bits, because a game reads one or the other and
        // a pad that only ever sent axes would never press L2 as far as the guest could tell.
        buttons = if (leftTrigger > 32) buttons or PadButton.L2 else buttons and PadButton.L2.inv()
        buttons = if (rightTrigger > 32) buttons or PadButton.R2 else buttons and PadButton.R2.inv()

        push()
        return true
    }

    /**
     * a device arriving or leaving. neither is an event on its own, so both come from the activity.
     *
     * a pad going away pushes a released state rather than only clearing the flag: a stick held over
     * when the cable came out would otherwise be the last thing the guest was told, and it would hold
     * that position forever. under a controller mapping the same holds per target -- see [resolve].
     */
    @JvmStatic
    fun onDeviceChanged() {
        val mapping = mapping
        if (mapping != null) {
            resolve(mapping)
            return
        }
        if (!enabled) return
        val present = LinkedHashSet<Int>()
        for (id in InputDevice.getDeviceIds()) {
            val device = InputDevice.getDevice(id)
            if (isGamepad(device)) {
                present.add(id)
            }
        }
        if (present == devices) {
            return
        }
        devices.clear()
        devices.addAll(present)
        if (devices.isEmpty()) {
            release()
        }
        push()
    }

    /** everything up and centred. what a pad that has gone away reports. */
    @JvmStatic
    fun release() {
        buttons = 0
        leftX = CENTRE
        leftY = CENTRE
        rightX = CENTRE
        rightY = CENTRE
        leftTrigger = 0
        rightTrigger = 0
    }

    /** releases everything and tells the host, for a run being left rather than a pad being unplugged. */
    @JvmStatic
    fun clear() {
        if (mapping != null) {
            levels.fill(0)
            pushPorts(ALL_PORTS)
            return
        }
        release()
        push()
    }

    private fun hasAxis(device: InputDevice?, axis: Int): Boolean =
        device?.getMotionRange(axis, InputDevice.SOURCE_JOYSTICK) != null

    /**
     * a stick axis, -1.0..1.0 from android, onto 0..255 with 128 centred.
     *
     * the device's own flat and fuzz are not applied. Android reports them per axis and the emulator
     * applies a deadzone of its own when it merges the stick, so subtracting one here would be a
     * deadzone inside a deadzone -- and the second one would be invisible to anybody tuning the first.
     */
    private fun axisToByte(event: MotionEvent, axis: Int): Int {
        if (!hasAxis(event.device, axis)) {
            return CENTRE
        }
        val value = event.getAxisValue(axis).coerceIn(-1f, 1f)
        if (abs(value) < 0.001f) {
            return CENTRE
        }
        // 128 + v*127 rather than 127.5, so that centre is exactly 128 and both ends are reachable:
        // -1.0 gives 1 and 1.0 gives 255. losing 0 costs nothing and an off-centre centre costs a
        // permanent drift.
        return (CENTRE + value * 127f).roundToInt().coerceIn(0, 255)
    }

    /** a trigger axis, 0.0..1.0 from android, onto 0..255. */
    private fun triggerToByte(event: MotionEvent, axis: Int): Int {
        if (!hasAxis(event.device, axis)) {
            return 0
        }
        val value = event.getAxisValue(axis).coerceIn(0f, 1f)
        return (value * 255f).roundToInt().coerceIn(0, 255)
    }

    /**
     * hands the whole state down to the host layer.
     *
     * **every event, with no coalescing.** the native side takes one uncontended lock and copies
     * twelve bytes, and the guest reads whatever is latest -- so a push that arrives between two polls
     * costs nothing and a push that is skipped is a control the guest never learns about.
     */
    private fun push() {
        // every controller is merged into one pad, and that pad is Controller port 1.
        HostLayer.nativeSetPadState(
            0, buttons, leftX, leftY, rightX, rightY, leftTrigger, rightTrigger, connected)
    }

    private const val PORTS = ControllerMapping.PORTS
    private const val ALL_PORTS = (1 shl PORTS) - 1

    /** a target's level runs 0..255: a key gives all of it, and an axis's half is scaled onto it. */
    private const val FULL = 255

    /** a button bound to an axis is pressed past half way, which is Dolphin's threshold for one. */
    private const val PRESSED = 127

    /** how far a trigger travels before it also presses L2 or R2, the figure automatic mapping uses. */
    private const val TRIGGER_PRESSED = 32

    /** what a port was last pushed as: buttons, four stick bytes, two triggers, connected. */
    private const val SENT_FIELDS = 8

    /** the run's controller mapping, or null under automatic mapping. set once, by [useMapping]. */
    private var mapping: ControllerMapping? = null

    private val numbers = DeviceNumbers()

    /** whether the mapping has been resolved against the connected devices at least once. */
    private var resolved = false

    /**
     * the bindings of each connected device, keyed by android's device id.
     *
     * **this is what keeps an event to a lookup.** a device's identity and number are worked out when
     * it arrives, never per event, and what an event finds here is already the list of targets its
     * input drives. replaced whole when a device arrives or leaves, never edited.
     */
    private var tables = SparseArray<DeviceTable>()

    /**
     * every target of every port, 0..255, at `port * PadTarget.COUNT + target` -- a slot.
     *
     * **one binding per target**, so a slot has exactly one input writing it and never needs combining
     * with another. two devices on one port drive different slots, which is why unplugging one leaves
     * what the other holds alone.
     */
    private val levels = IntArray(PORTS * PadTarget.COUNT)

    /** the android device id each slot's binding resolved to, or [DeviceNumbers.NO_DEVICE]. */
    private val owners = IntArray(PORTS * PadTarget.COUNT) { DeviceNumbers.NO_DEVICE }

    /** whether each port has a binding on a connected device, which is what the guest is told. */
    private val portConnected = BooleanArray(PORTS)

    /** each port as it was last pushed, [SENT_FIELDS] apiece. -1 until the first push. */
    private val sent = IntArray(PORTS * SENT_FIELDS) { -1 }

    /**
     * one connected device's bindings: a key's targets by key code, and its bound axes as parallel
     * arrays sorted by axis.
     */
    private class DeviceTable(
        val keys: SparseArray<IntArray>,
        val axes: IntArray,
        val axisSources: IntArray,
        val axisNegative: BooleanArray,
        val axisSlots: IntArray,
    )

    /**
     * runs this launch from [mapping] rather than automatically. called once, before the first sweep
     * of the connected devices, by the process that runs a guest.
     */
    @JvmStatic
    fun useMapping(mapping: ControllerMapping) {
        this.mapping = mapping
    }


    /**
     * a key, under a controller mapping. consumed only when it is bound: an unbound press goes on to
     * android, as Dolphin leaves it, and that is what keeps `BACK` and the volume keys android's.
     */
    private fun onMappedKey(event: KeyEvent): Boolean {
        val slots = tables.get(event.deviceId)?.keys?.get(event.keyCode) ?: return false
        if (event.action == KeyEvent.ACTION_DOWN && event.repeatCount > 0) {
            return true
        }
        val level = when (event.action) {
            KeyEvent.ACTION_DOWN -> FULL
            KeyEvent.ACTION_UP -> 0
            else -> return false
        }
        var touched = 0
        for (slot in slots) {
            if (levels[slot] != level) {
                levels[slot] = level
                touched = touched or (1 shl (slot / PadTarget.COUNT))
            }
        }
        if (touched != 0) pushPorts(touched)
        return true
    }

    /**
     * a joystick motion event, under a controller mapping. consumed when the device has an axis bound
     * on the event's source.
     *
     * **only the bound axes are read**, each once however many halves and targets it drives, which is
     * why a device's axes are sorted by axis. only `ACTION_MOVE` is taken, since that is what a stick,
     * a trigger and a hat send; a pointer's hover and scroll are not axes a mapping binds.
     */
    private fun onMappedMotion(event: MotionEvent): Boolean {
        if (event.action != MotionEvent.ACTION_MOVE) {
            return false
        }
        val table = tables.get(event.deviceId) ?: return false
        // read once: an event's source is a call into native code each time it is asked.
        val source = event.source
        var matched = false
        var touched = 0
        var axis = ControllerMapping.NO_AXIS
        var value = 0f
        for (i in table.axes.indices) {
            val bound = table.axisSources[i]
            if ((source and bound) != bound) continue
            matched = true
            if (table.axes[i] != axis) {
                axis = table.axes[i]
                value = event.getAxisValue(axis)
            }
            val half = if (table.axisNegative[i]) -value else value
            val level = (half.coerceIn(0f, 1f) * FULL).roundToInt()
            val slot = table.axisSlots[i]
            if (levels[slot] != level) {
                levels[slot] = level
                touched = touched or (1 shl (slot / PadTarget.COUNT))
            }
        }
        if (touched != 0) pushPorts(touched)
        return matched
    }

    /**
     * the mapping against the devices connected now: numbers them, finds each binding's device, and
     * builds the tables an event is looked up in. called on the UI thread when a device arrives,
     * leaves or changes, and once at launch.
     */
    private fun resolve(mapping: ControllerMapping) {
        val before = numbers.deviceIds()
        if (!numbers.update() && resolved) {
            return
        }
        val first = !resolved
        resolved = true

        val found = IntArray(PORTS * PadTarget.COUNT) { DeviceNumbers.NO_DEVICE }
        val byDevice = SparseArray<ArrayList<ControllerMapping.Binding>>()
        for (binding in mapping.bindings) {
            val id = numbers.deviceIdOf(binding.device)
            if (id == DeviceNumbers.NO_DEVICE) continue
            found[binding.slot] = id
            var list = byDevice.get(id)
            if (list == null) {
                list = ArrayList()
                byDevice.put(id, list)
            }
            list.add(binding)
        }
        val built = SparseArray<DeviceTable>(byDevice.size())
        for (i in 0 until byDevice.size()) {
            built.put(byDevice.keyAt(i), tableOf(byDevice.valueAt(i)))
        }
        tables = built

        // a slot whose device left, or that another device now answers, starts again at rest: the
        // release of whatever it held went with the device that held it.
        for (slot in found.indices) {
            if (found[slot] != owners[slot]) {
                owners[slot] = found[slot]
                levels[slot] = 0
            }
        }
        for (port in 0 until PORTS) {
            var any = false
            for (target in 0 until PadTarget.COUNT) {
                if (owners[port * PadTarget.COUNT + target] != DeviceNumbers.NO_DEVICE) {
                    any = true
                    break
                }
            }
            portConnected[port] = any
        }
        pushPorts(ALL_PORTS)
        describe(mapping, if (first) null else before)
    }

    private fun tableOf(bindings: List<ControllerMapping.Binding>): DeviceTable {
        val keys = SparseArray<IntArray>()
        val axes = ArrayList<ControllerMapping.Binding>()
        for (binding in bindings) {
            if (binding.keyCode == ControllerMapping.NO_KEY) {
                axes.add(binding)
                continue
            }
            val slots = keys.get(binding.keyCode)
            keys.put(binding.keyCode, if (slots == null) intArrayOf(binding.slot) else slots + binding.slot)
        }
        axes.sortBy { it.axis }
        return DeviceTable(
            keys,
            IntArray(axes.size) { axes[it].axis },
            IntArray(axes.size) { axes[it].source },
            BooleanArray(axes.size) { axes[it].negative },
            IntArray(axes.size) { axes[it].slot },
        )
    }

    /**
     * pushes each port in [mask] that differs from what it was last pushed as.
     *
     * **only a change crosses**, unlike automatic mapping's push per event: a stick resting a hair off
     * centre reports the same byte many times over, and none of those is news to the guest.
     */
    private fun pushPorts(mask: Int) {
        for (port in 0 until PORTS) {
            if ((mask and (1 shl port)) == 0) continue
            val base = port * PadTarget.COUNT
            var buttons = 0
            for (target in 0 until PadTarget.BUTTONS) {
                if (levels[base + target] > PRESSED) buttons = buttons or PadTarget.BUTTON_BITS[target]
            }
            val leftTrigger = levels[base + PadTarget.L2]
            val rightTrigger = levels[base + PadTarget.R2]
            if (leftTrigger > TRIGGER_PRESSED) buttons = buttons or PadButton.L2
            if (rightTrigger > TRIGGER_PRESSED) buttons = buttons or PadButton.R2
            // each stick axis is its positive direction less its negative one. Y grows downward, so
            // down is the positive half.
            val left = base + PadTarget.LEFT_STICK
            val right = base + PadTarget.RIGHT_STICK
            val leftX = stick(levels[left + 3], levels[left + 2])
            val leftY = stick(levels[left + 1], levels[left])
            val rightX = stick(levels[right + 3], levels[right + 2])
            val rightY = stick(levels[right + 1], levels[right])
            val connected = if (portConnected[port]) 1 else 0

            val s = port * SENT_FIELDS
            if (sent[s] == buttons && sent[s + 1] == leftX && sent[s + 2] == leftY &&
                sent[s + 3] == rightX && sent[s + 4] == rightY && sent[s + 5] == leftTrigger &&
                sent[s + 6] == rightTrigger && sent[s + 7] == connected) {
                continue
            }
            sent[s] = buttons
            sent[s + 1] = leftX
            sent[s + 2] = leftY
            sent[s + 3] = rightX
            sent[s + 4] = rightY
            sent[s + 5] = leftTrigger
            sent[s + 6] = rightTrigger
            sent[s + 7] = connected
            HostLayer.nativeSetPadState(
                port, buttons, leftX, leftY, rightX, rightY, leftTrigger, rightTrigger, connected == 1)
        }
    }

    /** two opposite directions' levels onto 0..255, 128 centred, by automatic mapping's `128 + v*127`. */
    private fun stick(positive: Int, negative: Int): Int =
        (CENTRE + (positive - negative) * 127f / FULL).roundToInt().coerceIn(0, 255)

    /**
     * says what the mapping resolved to: every device and its identity at launch, the devices that
     * arrived or left since [before] afterwards, and each port that binds anything.
     *
     * **this is the line that tells a run with no input apart.** "no file", "a file whose devices are
     * not connected" and "a file that resolved and a game that ignores it" look identical in play.
     */
    private fun describe(mapping: ControllerMapping, before: IntArray?) {
        val after = numbers.deviceIds()
        for (id in after) {
            if (before != null && before.contains(id)) continue
            val name = InputDevice.getDevice(id)?.name
            AppLog.i(TAG, "[pad] ${numbers.identityOf(id)} is device $id, $name")
        }
        if (before != null) {
            for (id in before) {
                if (!after.contains(id)) AppLog.i(TAG, "[pad] device $id has gone")
            }
        }
        for (port in 0 until PORTS) {
            var bound = 0
            var connected = 0
            for (binding in mapping.bindings) {
                if (binding.slot / PadTarget.COUNT != port) continue
                bound++
                if (owners[binding.slot] != DeviceNumbers.NO_DEVICE) connected++
            }
            val large = mapping.motors[port * 2]
            val small = mapping.motors[port * 2 + 1]
            if (bound == 0 && large == null && small == null) continue
            AppLog.i(TAG, "[pad] port ${port + 1}: $connected of $bound bindings on a connected " +
                    "device. large motor ${describe(large)}, small motor ${describe(small)}")
        }
    }

    /**
     * a port's motor and whether it is there. nothing drives a port's motors yet; this says what the
     * file names so that a mapping can be checked before anything does.
     */
    private fun describe(motor: ControllerMapping.Motor?): String {
        if (motor == null) return "none"
        val device = motor.device ?: return motor.toString()
        val id = numbers.deviceIdOf(device)
        if (id == DeviceNumbers.NO_DEVICE) return "$motor, not connected"
        return "$motor, of ${motorCount(id)} on that device"
    }

    private fun motorCount(deviceId: Int): Int {
        val device = InputDevice.getDevice(deviceId) ?: return 0
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.S) {
            return device.vibratorManager.vibratorIds.size
        }
        @Suppress("DEPRECATION")
        return if (device.vibrator.hasVibrator()) 1 else 0
    }
}
