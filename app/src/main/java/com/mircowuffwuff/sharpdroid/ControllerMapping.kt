package com.mircowuffwuff.sharpdroid

import android.util.SparseArray
import android.view.InputDevice
import android.view.KeyEvent
import android.view.MotionEvent
import org.json.JSONObject
import java.io.File

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
 *         "cross":         { "device": { "name": "Xbox Wireless Controller", "number": 1 },
 *                            "key": "KEYCODE_BUTTON_A" },
 *         "left-stick-up": { "device": { "name": "Xbox Wireless Controller", "number": 1 },
 *                            "source": "JOYSTICK", "axis": "AXIS_Y", "direction": "-" },
 *         "touchpad":      { "device": { "name": "DualSense Wireless Controller",
 *                                        "role": "touchpad", "number": 1 }, ... } },
 *       "large-motor": { "device": { "name": "Xbox Wireless Controller", "number": 2 }, "motor": 0 },
 *       "small-motor": "handheld" } ] }
 * ```
 *
 * **a device is its name, a role and a number** -- see [identityBase] and [DeviceNumbers] -- and is
 * written as an object rather than one string, because a name can hold any character and a string
 * joining it to the rest would need either escaping or a separator some name contains.
 *
 * **an input is spelled the way android spells it**, `KeyEvent.keyCodeToString` and
 * `MotionEvent.axisToString`, with the source as `dumpsys input` prints it. so a file can be written
 * from what the platform reports without a table of names of our own, and those names are constants
 * that never change meaning. an axis carries its source and a direction, as Dolphin's `Axis 1-` does:
 * a device may have the same axis on two sources, and each half of an axis is an input of its own.
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
     * @param device the identity the input belongs to, as [identity] joins it.
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
            if (device == null) "the handheld's motor" else "${describe(device)} motor $index"
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
            val device = deviceOf(json.optJSONObject("device"))
            if (device == null) {
                AppLog.w(TAG, "[pad] refused $where: its device is not a name, a number and a known role")
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
            val device = deviceOf(json?.optJSONObject("device"))
            val index = json?.optInt("motor", -1) ?: -1
            if (device == null || index < 0) {
                AppLog.w(TAG, "[pad] refused $where: it is neither \"handheld\" nor a device and a motor")
                return null
            }
            return Motor(device, index)
        }

        /** a file's device object as an [identity], or null when it is not one. */
        private fun deviceOf(json: JSONObject?): String? {
            val name = json?.optString("name").orEmpty()
            val role = json?.optString("role").orEmpty()
            val number = json?.optInt("number", 0) ?: 0
            if (name.isEmpty() || number < 1 || (role.isNotEmpty() && role !in ROLES)) {
                return null
            }
            return identity(name, role, number)
        }

        /** the roles a device that is not a gamepad can have, in the order [roleOf] tries them. */
        private val ROLES = listOf("touchpad", "mouse", "keyboard", "other")

        /**
         * a device's identity as one string, which is what bindings are matched by.
         *
         * **joined with a character no name contains** rather than written out as a person reads it, so
         * that no name, however it is spelled, can make two identities equal. [describe] is the readable
         * form.
         */
        @JvmStatic
        fun identity(name: String, role: String, number: Int): String = "$name\u0000$role\u0000$number"

        /** an identity as a person reads it: `"Xbox Wireless Controller" #2`, `"…" touchpad #1`. */
        @JvmStatic
        fun describe(identity: String): String {
            val parts = identity.split('\u0000')
            if (parts.size != 3) return identity
            val role = if (parts[1].isEmpty()) "" else " ${parts[1]}"
            return "\"${parts[0]}\"$role #${parts[2]}"
        }

        /**
         * a device's identity, short of its number: its name, and a role when it is not a gamepad.
         *
         * **the name, as Dolphin identifies a device**, rather than vendor and product as Eden does
         * or android's descriptor. a handheld can take a connected controller over and put a virtual
         * copy of its own in its place -- the AYN Odin 3 does, for a Bluetooth Xbox or DualSense --
         * and the copy keeps the controller's name but not its vendor, product or Bluetooth address.
         * every external controller there shares one vendor and product and one descriptor, so only
         * the name still tells an Xbox from a DualSense, and only the name still matches the same
         * controller when nothing has replaced it. the descriptor would also make a controller
         * switched to another mode, or a mapping taken to another phone, a new device.
         *
         * what it costs is that two models sharing a name share bindings, and that a controller named
         * differently over USB and over Bluetooth is two devices.
         *
         * **the role is ours.** one controller can be several android devices with one name -- the
         * DualSense's touchpad is a device of its own, named as its gamepad is -- and numbering them
         * together would make `#2` mean another part of the same controller on one day and a second
         * controller on another.
         */
        @JvmStatic
        fun identityBase(device: InputDevice): String =
            "${device.name}\u0000${roleOf(device.sources).orEmpty()}"

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
 * the identity of every connected device, number included: `"Xbox Wireless Controller" #2`.
 *
 * **a device takes the lowest number no connected device of the same name and role holds, and keeps
 * it for as long as it stays connected** -- Dolphin's rule in `ControllerInterface::AddDevice` for a
 * device with no controller number. so when #1 of two identical controllers drops out, #2 is still #2
 * and goes on driving what #2 is bound to, and the next one to connect takes #1. Dolphin numbers a
 * gamepad by android's controller number instead, which counts every connected gamepad, and Eden by
 * position among them all: either moves a binding whenever a different controller connects first.
 * the devices already present when this is first asked are numbered in the order android gave them
 * ids, which is the order they connected.
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
            while (indexOf("$base\u0000$number") >= 0) number++
            bases.put(id, base)
            identities.put(id, "$base\u0000$number")
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
