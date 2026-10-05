package com.mircowuffwuff.sharpdroid

import android.content.Context
import android.os.Build
import android.os.CombinedVibration
import android.os.SystemClock
import android.os.VibrationEffect
import android.os.Vibrator
import android.os.VibratorManager
import android.view.InputDevice
import androidx.annotation.RequiresApi

/**
 * the guest's rumble, on whatever this device and its controllers can vibrate with.
 *
 * **this is the only thing the host layer calls up into besides the guest file layer.** everything else
 * about a run goes downward: the surface is handed over as a native window and audio is pure NDK. there
 * is no NDK vibrator API at all, so rumble has to come back through JNI, and the class and its method
 * are resolved once at library load for the reason the file layer's bridge spells out -- a thread the
 * host layer attached itself searches the system class loader, which has never heard of anything in
 * this APK.
 *
 * **the host layer does not call this on a guest thread**, and that is a constraint rather than a
 * convenience. a vibrate is a binder round trip to the system server, and a guest thread waiting on one
 * is a guest thread that cannot acknowledge a garbage-collection suspension. so the native side records
 * the request and delivers it from a thread of its own; what arrives here is already off the guest's
 * critical path, and is also **not** the UI thread. it is always the same thread, which is what lets
 * each motor's pacing below be kept without a lock.
 *
 * **under automatic mapping a game's rumble drives every motor there is**: this device's own and those
 * of every connected controller that has any, which matches input being taken from every controller at
 * once. with automatic mapping off, each port drives the two motors the controller mapping names for
 * it -- see [useMapping] -- and a port naming none vibrates nothing.
 */
object PadRumble {

    private const val TAG = "sharpdroid"

    /**
     * how long one request buzzes for.
     *
     * the guest's seam sets a rumble level and leaves it set; it does not say how long. a vibrator
     * takes a duration and stops on its own. so each request is a short pulse and a game holding a
     * rumble on sends more of them -- which is what every android emulator does with this seam. the
     * length is Dolphin's.
     */
    private const val PULSE_MILLIS = 100L

    /**
     * **how much of a pulse runs before the same level is sent again.**
     *
     * a game holding a level may ask for it on every frame, and each ask sent on would be a binder
     * call per motor per frame -- and under automatic mapping one ask reaches every motor there is. so
     * a motor already buzzing at the level asked for is left alone until most of its pulse has gone:
     * at most twelve or so calls a second per motor while a level is held, and none while nothing is.
     * a level that changes is sent at once.
     */
    private const val REFRESH_MILLIS = 80L

    /**
     * the weakest amplitude worth sending.
     *
     * below about this the actuator does not move and the request is only a wakeup for the vibrator
     * service. zero means stop, and is handled before this.
     */
    private const val FLOOR = 8

    /**
     * something that vibrates, and what was last sent to it.
     *
     * [level] and [sentAt] are touched only on the rumble thread; see the class comment.
     */
    private abstract class Motor(val amplitudeControl: Boolean) {
        var level = 0
        var sentAt = 0L

        abstract fun send(effect: VibrationEffect)
        abstract fun stop()
    }

    /** this device's own motor, or the default one of several. */
    private class Handheld(private val vibrator: Vibrator) : Motor(vibrator.hasAmplitudeControl()) {
        override fun send(effect: VibrationEffect) = vibrator.vibrate(effect)
        override fun stop() = vibrator.cancel()
    }

    /**
     * **every motor of one controller, driven as one.** a controller's motors are numbered and not
     * named, and nothing says which is the strong one, so under automatic mapping each gets the louder
     * of the two levels a game asks for -- the same answer this device's single motor gets. one
     * combined request drives all of them, which is one binder call rather than one per motor.
     */
    @RequiresApi(Build.VERSION_CODES.S)
    private class Controller(private val manager: VibratorManager, amplitude: Boolean) : Motor(amplitude) {
        override fun send(effect: VibrationEffect) =
            manager.vibrate(CombinedVibration.createParallel(effect))
        override fun stop() = manager.cancel()
    }

    /** a controller's single motor, below android 12, where that is all a controller exposes. */
    private class LegacyController(private val vibrator: Vibrator) : Motor(vibrator.hasAmplitudeControl()) {
        override fun send(effect: VibrationEffect) = vibrator.vibrate(effect)
        override fun stop() = vibrator.cancel()
    }

    @Volatile
    private var handheld: Motor? = null

    /** this device's own vibrator, which a port can name as well as automatic mapping driving it. */
    @Volatile
    private var handheldVibrator: Vibrator? = null

    /**
     * the connected controllers that have a motor, by device id.
     *
     * **replaced whole rather than edited**, by the UI thread on a device arriving or leaving, so the
     * rumble thread always iterates a list nobody is changing. a controller that stays connected keeps
     * its entry, and with it the pacing of what was last sent to it.
     */
    @Volatile
    private var controllers: Map<Int, Motor> = emptyMap()

    /**
     * whether a game may vibrate anything -- Settings, Controls, Controller vibration.
     *
     * **gated here rather than in the host layer, and that is deliberate.** the guest's request still
     * crosses the boundary and is still counted as asked for; what stops is the platform call. so a
     * run with this off stays distinguishable in the log from a run where the game never asked, which
     * is the distinction the two counters exist to preserve.
     *
     * volatile because the host layer's delivery thread reads it and the UI thread writes it.
     */
    @Volatile
    @JvmStatic
    var enabled: Boolean = true

    /**
     * whether automatic mapping is on, which decides which motors a game's rumble reaches. see the
     * class comment.
     */
    @Volatile
    @JvmStatic
    var automatic: Boolean = true

    /**
     * one device's motors as the ports drive them: a level for each, sent together.
     *
     * **one call carries every motor of a device**, because a new vibration on an input device
     * replaces the one before it, so a call per motor would leave only the last one running. [levels]
     * is what the ports ask for now and [sent] what was last sent, and both are the rumble thread's
     * alone, like [Motor]'s pacing.
     */
    private abstract class Outputs(motors: Int) {
        val levels = IntArray(motors)
        val sent = IntArray(motors)
        var sentAt = 0L

        /** every motor at its level in [levels], those under [FLOOR] stopped. */
        abstract fun send(levels: IntArray)
        abstract fun stop()
    }

    /** this device's own motor, as a port names it. */
    private class HandheldOutputs(private val vibrator: Vibrator) : Outputs(1) {
        private val amplitude = vibrator.hasAmplitudeControl()
        override fun send(levels: IntArray) = vibrator.vibrate(pulse(levels[0], amplitude))
        override fun stop() = vibrator.cancel()
    }

    /**
     * a controller's motors, each named by its vibrator id in one combined vibration.
     *
     * **that is the call that addresses one motor**, and it is chosen over the other on purpose: the
     * vibrator a controller's manager hands out for one id is believed to drive every channel of the
     * device at once on the platform's input path. Dolphin drives a motor through that per-id vibrator
     * and Eden drives all of a controller's motors together, so neither settles it; a large motor that
     * can be told from the small one is what does.
     */
    @RequiresApi(Build.VERSION_CODES.S)
    private class ControllerOutputs(
        private val manager: VibratorManager,
        private val ids: IntArray,
    ) : Outputs(ids.size) {
        private val amplitude =
            BooleanArray(ids.size) { manager.getVibrator(ids[it]).hasAmplitudeControl() }

        override fun send(levels: IntArray) {
            val combined = CombinedVibration.startParallel()
            for (i in ids.indices) {
                if (levels[i] >= FLOOR) combined.addVibrator(ids[i], pulse(levels[i], amplitude[i]))
            }
            manager.vibrate(combined.combine())
        }

        override fun stop() = manager.cancel()
    }

    /** a controller's one vibrator below android 12, which is all a controller exposes there. */
    private class LegacyOutputs(private val vibrator: Vibrator) : Outputs(1) {
        private val amplitude = vibrator.hasAmplitudeControl()
        override fun send(levels: IntArray) = vibrator.vibrate(pulse(levels[0], amplitude))
        override fun stop() = vibrator.cancel()
    }

    /**
     * where each port's motors are among the devices connected now: for each slot -- a port's large
     * motor at `port * 2`, its small one after it -- the outputs holding that motor and which of their
     * motors it is. replaced whole on a device change, like [controllers], and a device still
     * connected keeps its outputs and with them its pacing.
     */
    private class Routing(
        val outputs: Array<Outputs?>,
        val motors: IntArray,
        val byDevice: Map<Int, Outputs>,
        val handheld: Outputs?,
    )

    /** the motors the controller mapping names, slot by slot, or null under automatic mapping. */
    @Volatile
    private var mapped: Array<ControllerMapping.Motor?>? = null

    @Volatile
    private var routing: Routing? = null

    /** each slot's latest request, 0..255. touched only on the rumble thread. */
    private val requested = IntArray(ControllerMapping.PORTS * 2)

    /**
     * drives each port's own two motors from now on, rather than every motor there is. called once,
     * before [attach], by the process that runs a guest with automatic mapping off.
     */
    @JvmStatic
    fun useMapping(motors: Array<ControllerMapping.Motor?>) {
        mapped = motors.copyOf()
    }

    /**
     * called by the activity that owns a run, before the guest starts.
     *
     * the application context is held rather than the activity, because this outlives any one screen
     * and holding an activity here would keep it from being collected.
     */
    @JvmStatic
    fun attach(context: Context) {
        val device = if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.S) {
            val manager = context.getSystemService(VibratorManager::class.java)
            manager?.defaultVibrator
        } else {
            @Suppress("DEPRECATION")
            context.getSystemService(Vibrator::class.java)
        }
        if (device == null || !device.hasVibrator()) {
            // said once, and not an error: a device with no actuator is a perfectly ordinary thing to
            // run this on, and the alternative is a line per rumble for the rest of the run.
            AppLog.i(TAG, "[pad] this device has no vibrator of its own")
            handheld = null
            handheldVibrator = null
        } else {
            val motor = Handheld(device)
            handheld = motor
            handheldVibrator = device
            // **"an actuator exists" and "we may drive it" are different questions, and only the
            // first is answered here.** neither hasVibrator nor hasAmplitudeControl consults the
            // VIBRATE permission, so both answer truthfully to an app that has not been granted it and
            // the vibrate call alone throws. so this line says what it actually knows rather than
            // "ready".
            AppLog.i(TAG, "[pad] a vibrator is present, amplitude control " +
                    (if (motor.amplitudeControl) "available" else "absent") +
                    ". whether a buzz arrives is only known when one is asked for")
        }
        onDeviceChanged()
    }

    /**
     * a device arriving, leaving or changing. called from the same listener the pad's own state
     * hears, on the UI thread.
     *
     * **the motors are found here rather than on each rumble**, because asking a device what it can
     * vibrate with is a call into the input service, and a request arriving at a frame's pace must
     * not make one.
     */
    @JvmStatic
    fun onDeviceChanged() {
        val previous = controllers
        val found = LinkedHashMap<Int, Motor>()
        for (id in InputDevice.getDeviceIds()) {
            val device = InputDevice.getDevice(id) ?: continue
            if (!PadState.isGamepad(device)) {
                continue
            }
            val kept = previous[id]
            if (kept != null) {
                found[id] = kept
                continue
            }
            val motor = motorsOf(device) ?: continue
            found[id] = motor
            AppLog.i(TAG, "[pad] ${device.name} can vibrate, amplitude control " +
                    if (motor.amplitudeControl) "available" else "absent")
        }
        controllers = found
        mapped?.let { routing = route(it, routing) }
    }

    /**
     * each slot's motor found among the devices connected now, through the identities [PadState]
     * numbered for this run -- so this is called after it has numbered them, which is the order the
     * activity's listener calls the two in.
     *
     * a slot naming a device that is not here, or a motor that device does not have -- the Odin's
     * copy of a controller has none -- drives nothing until a change brings it.
     */
    private fun route(mapped: Array<ControllerMapping.Motor?>, previous: Routing?): Routing {
        val outputs = arrayOfNulls<Outputs>(mapped.size)
        val motors = IntArray(mapped.size)
        val byDevice = HashMap<Int, Outputs>()
        var own: Outputs? = null
        for (slot in mapped.indices) {
            val motor = mapped[slot] ?: continue
            val identity = motor.device
            val output = if (identity == null) {
                own = own ?: previous?.handheld ?: handheldVibrator?.let { HandheldOutputs(it) }
                own
            } else {
                val id = PadState.deviceIdOf(identity)
                if (id == DeviceNumbers.NO_DEVICE) {
                    null
                } else {
                    byDevice[id]
                        ?: (previous?.byDevice?.get(id) ?: outputsOf(id))?.also { byDevice[id] = it }
                }
            }
            if (output == null || motor.index >= output.levels.size) continue
            outputs[slot] = output
            motors[slot] = motor.index
        }
        return Routing(outputs, motors, byDevice, own)
    }

    private fun outputsOf(deviceId: Int): Outputs? {
        val device = InputDevice.getDevice(deviceId) ?: return null
        val ids = PadMotors.vibratorIds(device)
        if (ids.isEmpty()) return null
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.S) {
            return ControllerOutputs(device.vibratorManager, ids)
        }
        @Suppress("DEPRECATION")
        return LegacyOutputs(device.vibrator)
    }

    private fun motorsOf(device: InputDevice): Motor? {
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.S) {
            val manager = device.vibratorManager
            val ids = manager.vibratorIds
            if (ids.isEmpty()) {
                return null
            }
            return Controller(manager, ids.all { manager.getVibrator(it).hasAmplitudeControl() })
        }
        @Suppress("DEPRECATION")
        val vibrator = device.vibrator
        return if (vibrator.hasVibrator()) LegacyController(vibrator) else null
    }

    /**
     * detaches, so a run that has ended cannot buzz.
     *
     * the native delivery thread is not stopped by this and does not need to be -- it waits forever by
     * design and the process ends with the run. what this prevents is a request already in flight
     * arriving after the guest is gone.
     */
    @JvmStatic
    fun detach() {
        handheld?.stop()
        handheld = null
        for (motor in controllers.values) {
            motor.stop()
        }
        controllers = emptyMap()
        routing?.let { route ->
            route.handheld?.stop()
            for (output in route.byDevice.values) output.stop()
        }
        routing = null
    }

    /**
     * the host layer's entry point. resolved as `rumble(III)Z` at library load, so **the name and
     * signature are part of an interface** and cannot be changed on this side alone.
     *
     * **it returns whether the platform took the request, and the host counts only the trues.** a
     * void version reported success for anything that did not crash, so a rumble refused for want of
     * the `VIBRATE` permission -- which throws here rather than at any earlier check -- was counted as
     * delivered. a counter that cannot distinguish those is worse than none. a motor already buzzing
     * at the level asked for counts as taken: the platform has it.
     *
     * @param port the port the guest asked about, 0 to 3. under automatic mapping every port drives
     *   every motor, since every controller is merged into port 1; under a controller mapping it
     *   drives that port's own two.
     * @param large the strong motor, 0..255.
     * @param small the weak motor, 0..255.
     * @return true when at least one motor has the request; false when there is nothing to vibrate,
     *   nothing was asked for, vibration is off, or every motor refused.
     */
    @JvmStatic
    fun rumble(port: Int, large: Int, small: Int): Boolean {
        if (!enabled) {
            return false
        }
        if (!automatic) {
            return rumbleMapped(port, large, small)
        }
        // a motor is driven by the louder of the two levels a game asks for. taking the larger
        // rather than a sum or an average is what keeps a request for one strong motor from arriving
        // weaker than it was asked for.
        val strength = maxOf(large, small).coerceIn(0, 255)
        var took = false
        handheld?.let { took = drive(it, strength) or took }
        for (motor in controllers.values) {
            took = drive(motor, strength) or took
        }
        return took
    }

    /**
     * a request under a controller mapping: the port's large motor at [large] and its small one at
     * [small], each motor at the strongest request any slot naming it makes -- so a motor named by
     * both rows, or by two ports, takes the stronger. only the outputs this port names are touched.
     */
    private fun rumbleMapped(port: Int, large: Int, small: Int): Boolean {
        if (port !in 0 until ControllerMapping.PORTS) return false
        val routing = routing ?: return false
        requested[port * 2] = large.coerceIn(0, 255)
        requested[port * 2 + 1] = small.coerceIn(0, 255)
        val first = routing.outputs[port * 2]
        val second = routing.outputs[port * 2 + 1]
        var took = false
        if (first != null) took = drive(first, routing)
        if (second != null && second !== first) took = drive(second, routing) or took
        return took
    }

    /** [output] at the levels its slots ask for, paced as [drive] paces one motor. */
    private fun drive(output: Outputs, routing: Routing): Boolean {
        val levels = output.levels
        levels.fill(0)
        for (slot in routing.outputs.indices) {
            if (routing.outputs[slot] !== output) continue
            val motor = routing.motors[slot]
            if (requested[slot] > levels[motor]) levels[motor] = requested[slot]
        }
        try {
            if (levels.all { it < FLOOR }) {
                // a stop is sent once, as for a single motor.
                if (output.sent.any { it != 0 }) {
                    output.stop()
                    output.sent.fill(0)
                }
                return false
            }
            val now = SystemClock.uptimeMillis()
            if (levels.contentEquals(output.sent) && now - output.sentAt < REFRESH_MILLIS) {
                return true
            }
            output.send(levels)
            levels.copyInto(output.sent)
            output.sentAt = now
            return true
        } catch (e: Exception) {
            AppLog.w(TAG, "[pad] a rumble request failed", e)
            return false
        }
    }

    /**
     * one pulse at [strength]. with no amplitude control the only choice is whether it buzzes at all,
     * and the platform's own idea of a reasonable strength is a better answer than picking one here.
     */
    private fun pulse(strength: Int, amplitudeControl: Boolean): VibrationEffect =
        VibrationEffect.createOneShot(
            PULSE_MILLIS,
            if (amplitudeControl) strength else VibrationEffect.DEFAULT_AMPLITUDE,
        )

    private fun drive(motor: Motor, strength: Int): Boolean {
        try {
            if (strength < FLOOR) {
                // a stop is sent once: a game holding nothing may say so on every frame.
                if (motor.level != 0) {
                    motor.stop()
                    motor.level = 0
                }
                return false
            }
            val now = SystemClock.uptimeMillis()
            if (strength == motor.level && now - motor.sentAt < REFRESH_MILLIS) {
                return true
            }
            val effect = if (motor.amplitudeControl) {
                VibrationEffect.createOneShot(PULSE_MILLIS, strength)
            } else {
                // no amplitude control, so the only choice is whether it buzzes at all.
                // DEFAULT_AMPLITUDE is the platform's own idea of a reasonable strength, which is a
                // better answer than picking one here.
                VibrationEffect.createOneShot(PULSE_MILLIS, VibrationEffect.DEFAULT_AMPLITUDE)
            }
            motor.send(effect)
            motor.level = strength
            motor.sentAt = now
            return true
        } catch (e: Exception) {
            // the vibrator service can go away, and a throw crossing back into JNI would be delivered
            // at the delivery thread's next call -- a different request entirely. this is not worth
            // ending a run over.
            AppLog.w(TAG, "[pad] a rumble request failed", e)
            return false
        }
    }
}
