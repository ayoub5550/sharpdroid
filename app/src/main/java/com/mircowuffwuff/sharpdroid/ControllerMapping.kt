package com.mircowuffwuff.sharpdroid

import android.util.SparseArray
import android.view.InputDevice
import android.view.KeyEvent
import android.view.MotionEvent
import org.json.JSONObject
import java.io.File
import java.util.Locale

/**
 * the controller mapping: which input of which device drives which control of which port, for a run
 * with automatic controller mapping off. [PadState] runs it; this is what it is read from.
 *
 * **one JSON file for all four ports**, the shape Dolphin's `GCPadNew.ini` and Eden's `player_<n>_`
 * keys both have, read once when the process that runs a guest starts. a port holds a binding per
 * target it uses and two motors:
 *
 * ```
 * { "version": 1,
 *   "ports": [
 *     { "bindings": {
 *         "cross":         { "device": "2020:0112 #1", "name": "Xbox Wireless Controller",
 *                            "key": "KEYCODE_BUTTON_A" },
 *         "left-stick-up": { "device": "2020:0112 #1", "name": "Xbox Wireless Controller",
 *                            "source": "JOYSTICK", "axis": "AXIS_Y", "direction": "-" } },
 *       "large-motor": { "device": "045e:0b13 #1", "name": "Xbox Wireless Controller", "motor": 0 },
 *       "small-motor": "handheld" } ] }
 * ```
 *
 * **an input is spelled the way android spells it**, `KeyEvent.keyCodeToString` and
 * `MotionEvent.axisToString`, with the source as `dumpsys input` prints it. so a file can be written
 * from what the platform reports without a table of names of our own, and those names are constants
 * that never change meaning. an axis carries its source and a direction, as Dolphin's `Axis 1-` does:
 * a device may have the same axis on two sources, and each half of an axis is an input of its own.
 *
 * **`name` is for display only.** what is matched is the identity in `device`, which [identityBase]
 * and [DeviceNumbers] define; a name is stored beside it so that a binding to a device that is not
 * connected can still say what it was.
 *
 * **a file this app cannot read is never rewritten or removed.** an unknown version or a file that
 * is not JSON gives a run with no bindings and a line saying why, and an entry that cannot be read is
 * refused on its own while the rest of the file still applies -- a mapping is a person's work, and a
 * reader that discarded what it did not understand would destroy it on the first older app to open it.
 */
class ControllerMapping private constructor(
    /** every binding that could be read. */
    val bindings: List<Binding>,
    /** each port's two motors, the large one at `port * 2` and the small one after it. null is none. */
    val motors: Array<Motor?>,
) {

    /**
     * one input driving one target.
     *
     * @param slot the port and the target as one index, `port * PadTarget.COUNT + target`, which is
     *   how [PadState] holds a port's state.
     * @param device the identity the input belongs to, as [DeviceNumbers] numbers it.
     * @param keyCode the key, or [NO_KEY] for an axis.
     * @param source the axis's source, one of `InputDevice.SOURCE_*`. 0 for a key.
     * @param axis the axis, one of `MotionEvent.AXIS_*`. [NO_AXIS] for a key.
     * @param negative whether the binding is the axis's negative half.
     */
    class Binding(
        val slot: Int,
        val device: String,
        val keyCode: Int,
        val source: Int,
        val axis: Int,
        val negative: Boolean,
    )

    /**
     * a motor a port names: [index] among one device's vibrators, Dolphin's `Motor 0`, `Motor 1` --
     * or, with [device] null, the handheld's own.
     */
    class Motor(val device: String?, val index: Int) {
        override fun toString(): String =
            if (device == null) "the handheld's motor" else "$device motor $index"
    }

    companion object {

        private const val TAG = "sharpdroid"

        /** the version this app writes and the newest it reads. */
        const val VERSION = 1

        const val PORTS = 4
        const val NO_KEY = -1
        const val NO_AXIS = -1

        /** what a run with no file, or with one that could not be read, maps: nothing. */
        @JvmField
        val NONE = ControllerMapping(emptyList(), arrayOfNulls(PORTS * 2))

        /**
         * **never bindable, and refused if a file names them.** `BACK` always opens the panel over a
         * running game, and the volume keys are the device's. both reach android whatever a mapping
         * says, because a binding is the only thing that stops an event going on.
         */
        private val NEVER_BOUND = intArrayOf(
            KeyEvent.KEYCODE_BACK,
            KeyEvent.KEYCODE_VOLUME_UP,
            KeyEvent.KEYCODE_VOLUME_DOWN,
            KeyEvent.KEYCODE_VOLUME_MUTE,
        )

        /** an axis's source, by the name `dumpsys input` prints for it. */
        private val SOURCES = mapOf(
            "JOYSTICK" to InputDevice.SOURCE_JOYSTICK,
            "GAMEPAD" to InputDevice.SOURCE_GAMEPAD,
            "DPAD" to InputDevice.SOURCE_DPAD,
            "KEYBOARD" to InputDevice.SOURCE_KEYBOARD,
            "TOUCHSCREEN" to InputDevice.SOURCE_TOUCHSCREEN,
            "TOUCHPAD" to InputDevice.SOURCE_TOUCHPAD,
            "TOUCH_NAVIGATION" to InputDevice.SOURCE_TOUCH_NAVIGATION,
            "MOUSE" to InputDevice.SOURCE_MOUSE,
            "MOUSE_RELATIVE" to InputDevice.SOURCE_MOUSE_RELATIVE,
            "TRACKBALL" to InputDevice.SOURCE_TRACKBALL,
            "STYLUS" to InputDevice.SOURCE_STYLUS,
            "BLUETOOTH_STYLUS" to InputDevice.SOURCE_BLUETOOTH_STYLUS,
            "ROTARY_ENCODER" to InputDevice.SOURCE_ROTARY_ENCODER,
            "HDMI" to InputDevice.SOURCE_HDMI,
        )

        /**
         * reads [file], saying what it found.
         *
         * **a missing file is not an error**: it is what an install that has never mapped anything
         * has, and the run then has no controller on any port, which is what automatic mapping off
         * with nothing mapped means.
         */
        @JvmStatic
        fun read(file: File): ControllerMapping {
            if (!file.isFile) {
                AppLog.i(TAG, "[pad] no controller mapping at ${file.path}, so no port has a controller")
                return NONE
            }
            val root = try {
                JSONObject(file.readText())
            } catch (e: Exception) {
                AppLog.w(TAG, "[pad] the controller mapping at ${file.path} could not be read, " +
                        "so no port has a controller", e)
                return NONE
            }
            val version = root.optInt("version", 0)
            if (version > VERSION) {
                AppLog.w(TAG, "[pad] the controller mapping at ${file.path} is version $version and " +
                        "this app reads up to $VERSION, so no port has a controller. the file is left " +
                        "as it is")
                return NONE
            }
            if (version < 1) {
                AppLog.w(TAG, "[pad] the controller mapping at ${file.path} names no version, " +
                        "so no port has a controller")
                return NONE
            }
            val ports = root.optJSONArray("ports")
            if (ports == null) {
                AppLog.w(TAG, "[pad] the controller mapping at ${file.path} has no ports, " +
                        "so no port has a controller")
                return NONE
            }
            if (ports.length() > PORTS) {
                AppLog.w(TAG, "[pad] the controller mapping names ${ports.length()} ports and " +
                        "there are $PORTS. the rest are ignored")
            }

            val bindings = ArrayList<Binding>()
            val motors = arrayOfNulls<Motor>(PORTS * 2)
            var refused = 0
            for (port in 0 until minOf(ports.length(), PORTS)) {
                val where = "port ${port + 1}"
                val entry = ports.optJSONObject(port)
                if (entry == null) {
                    AppLog.w(TAG, "[pad] refused $where: it is not an object")
                    refused++
                    continue
                }
                val map = entry.optJSONObject("bindings")
                if (map != null) {
                    for (name in map.keys()) {
                        val target = PadTarget.NAMES.indexOf(name)
                        val binding = if (target < 0) {
                            AppLog.w(TAG, "[pad] refused $where's $name: there is no target by that name")
                            null
                        } else {
                            bindingOf(port * PadTarget.COUNT + target, map.optJSONObject(name),
                                "$where's $name")
                        }
                        if (binding == null) refused++ else bindings.add(binding)
                    }
                }
                for ((row, key) in arrayOf("large-motor", "small-motor").withIndex()) {
                    val value = entry.opt(key)
                    if (value == null || value == JSONObject.NULL) continue
                    val motor = motorOf(value, "$where's $key")
                    if (motor == null) refused++ else motors[port * 2 + row] = motor
                }
            }
            AppLog.i(TAG, "[pad] controller mapping version $version from ${file.path}: " +
                    "${bindings.size} bindings, ${motors.count { it != null }} motors, $refused refused")
            return ControllerMapping(bindings, motors)
        }

        /** one binding, or null after saying why it is refused. */
        private fun bindingOf(slot: Int, json: JSONObject?, where: String): Binding? {
            if (json == null) {
                AppLog.w(TAG, "[pad] refused $where: it is not an object")
                return null
            }
            val device = json.optString("device")
            if (device.isEmpty()) {
                AppLog.w(TAG, "[pad] refused $where: it names no device")
                return null
            }
            val key = json.optString("key")
            if (key.isNotEmpty()) {
                val code = KeyEvent.keyCodeFromString(key)
                if (code == KeyEvent.KEYCODE_UNKNOWN) {
                    AppLog.w(TAG, "[pad] refused $where: android has no key called $key")
                    return null
                }
                if (code in NEVER_BOUND) {
                    AppLog.w(TAG, "[pad] refused $where: $key is never bound, it is always android's")
                    return null
                }
                return Binding(slot, device, code, 0, NO_AXIS, false)
            }
            val axisName = json.optString("axis")
            val axis = MotionEvent.axisFromString(axisName)
            if (axis < 0) {
                AppLog.w(TAG, "[pad] refused $where: it names neither a key nor an axis android has")
                return null
            }
            val source = SOURCES[json.optString("source")]
            if (source == null) {
                AppLog.w(TAG, "[pad] refused $where: its source is not one of ${SOURCES.keys}")
                return null
            }
            val negative = when (json.optString("direction")) {
                "+" -> false
                "-" -> true
                else -> {
                    AppLog.w(TAG, "[pad] refused $where: its direction is neither + nor -")
                    return null
                }
            }
            return Binding(slot, device, NO_KEY, source, axis, negative)
        }

        /** one motor, or null after saying why it is refused. */
        private fun motorOf(value: Any, where: String): Motor? {
            if (value == "handheld") {
                return Motor(null, 0)
            }
            val json = value as? JSONObject
            val device = json?.optString("device").orEmpty()
            val index = json?.optInt("motor", -1) ?: -1
            if (device.isEmpty() || index < 0) {
                AppLog.w(TAG, "[pad] refused $where: it is neither \"handheld\" nor a device and a motor")
                return null
            }
            return Motor(device, index)
        }

        /**
         * a device's identity, short of its number: `vvvv:pppp` from its vendor and product.
         *
         * **vendor and product rather than android's descriptor or the name**, as Eden does it: the
         * descriptor is one physical unit in one mode, so a binding would not survive another phone
         * or a controller switched to another mode, and a name is shared by different models -- two
         * DualSense generations with different layouts carry the same one.
         *
         * **two refinements of ours.** a device that is not a gamepad gets a word for what it is,
         * `054c:0ce6 touchpad`, because one controller can be several android devices sharing a
         * vendor and product -- the DualSense is a gamepad, a touchpad and a third for its battery
         * and lights -- and numbering them together would make `#2` mean another part of the same
         * controller on one day and a second controller on another. and a device reporting neither
         * a vendor nor a product, which the built-in keys and jacks commonly do, has nothing to
         * number among but its name, so the name stands in, quoted, which is Dolphin's identity for
         * every device.
         */
        @JvmStatic
        fun identityBase(device: InputDevice): String {
            if (device.vendorId == 0 && device.productId == 0) {
                return "\"${device.name}\""
            }
            val ids = String.format(Locale.ROOT, "%04x:%04x", device.vendorId, device.productId)
            val role = roleOf(device.sources) ?: return ids
            return "$ids $role"
        }

        /** what a device is when it is not a gamepad, or null when it is one. the first match wins. */
        private fun roleOf(sources: Int): String? = when {
            has(sources, InputDevice.SOURCE_GAMEPAD) || has(sources, InputDevice.SOURCE_JOYSTICK) -> null
            has(sources, InputDevice.SOURCE_TOUCHPAD) -> "touchpad"
            has(sources, InputDevice.SOURCE_MOUSE) || has(sources, InputDevice.SOURCE_MOUSE_RELATIVE) -> "mouse"
            has(sources, InputDevice.SOURCE_KEYBOARD) -> "keyboard"
            else -> "other"
        }

        /** a source carries a class bit as well as its own, so it is tested whole -- see [PadState.isGamepad]. */
        private fun has(sources: Int, source: Int): Boolean = (sources and source) == source
    }
}

/**
 * the identity of every connected device, number included: `2020:0112 #1`.
 *
 * **Dolphin's numbering** (`ControllerInterface::AddDevice`): a device takes the lowest number no
 * connected device of the same identity holds, and keeps it for as long as it stays connected. so when
 * #1 of two identical controllers drops out, #2 is still #2 and goes on driving what #2 is bound to,
 * and the next one to connect takes #1. Eden's position among every controller would instead move a
 * binding whenever a different controller connected first. the devices already present when this is
 * first asked are numbered in the order android gave them ids, which is the order they connected.
 *
 * virtual devices are left out: they are what `adb shell input` and the on-screen keyboard inject
 * from, and nothing a person holds.
 *
 * not thread-safe: asked only on the UI thread, when a device arrives or leaves.
 */
class DeviceNumbers {

    private val bases = SparseArray<String>()
    private val identities = SparseArray<String>()

    /** re-reads the connected devices. true when any arrived, left or changed identity. */
    fun update(): Boolean {
        val present = InputDevice.getDeviceIds()
        present.sort()
        var changed = false
        // the departed first, so that a number one of them held is free for anything arriving now.
        for (i in identities.size() - 1 downTo 0) {
            val id = identities.keyAt(i)
            if (present.binarySearch(id) < 0) {
                identities.removeAt(i)
                bases.remove(id)
                changed = true
            }
        }
        for (id in present) {
            val device = InputDevice.getDevice(id) ?: continue
            if (device.isVirtual) continue
            val base = ControllerMapping.identityBase(device)
            if (bases.get(id) == base) continue
            identities.remove(id)
            var number = 1
            while (indexOf("$base #$number") >= 0) number++
            bases.put(id, base)
            identities.put(id, "$base #$number")
            changed = true
        }
        return changed
    }

    /** the identity of a connected device, or null for one that has none. */
    fun identityOf(deviceId: Int): String? = identities.get(deviceId)

    /** the android device id carrying [identity], or [NO_DEVICE] when nothing connected does. */
    fun deviceIdOf(identity: String): Int {
        val index = indexOf(identity)
        return if (index < 0) NO_DEVICE else identities.keyAt(index)
    }

    /** every connected device's id, in id order. */
    fun deviceIds(): IntArray = IntArray(identities.size()) { identities.keyAt(it) }

    private fun indexOf(identity: String): Int {
        for (i in 0 until identities.size()) {
            if (identities.valueAt(i) == identity) return i
        }
        return -1
    }

    companion object {
        const val NO_DEVICE = Int.MIN_VALUE
    }
}
