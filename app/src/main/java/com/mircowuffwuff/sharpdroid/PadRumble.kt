package com.mircowuffwuff.sharpdroid

import android.content.Context
import android.os.Build
import android.os.CombinedVibration
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
 * critical path, and is also **not** the UI thread.
 *
 * **under automatic mapping a game's rumble drives every motor there is**: this device's own and those
 * of every connected controller that has any, which matches input being taken from every controller at
 * once. with automatic mapping off, it drives the motors the ports name, and there are no port rows to
 * name one -- so nothing vibrates.
 */
object PadRumble {

    private const val TAG = "sharpdroid"

    /**
     * how long one request buzzes for.
     *
     * the guest's seam sets a rumble level and leaves it set; it does not say how long. a vibrator
     * takes a duration and stops on its own. so each request is a short pulse and a game holding a
     * rumble on sends more of them -- which is what every android emulator does with this seam, and it
     * is why the number is small enough that two in a row read as continuous.
     */
    private const val PULSE_MILLIS = 80L

    /**
     * the weakest amplitude worth sending.
     *
     * below about this the actuator does not move and the request is only a wakeup for the vibrator
     * service. zero means stop, and is handled before this.
     */
    private const val FLOOR = 8

    /** something that vibrates. */
    private abstract class Motor(val amplitudeControl: Boolean) {
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

    /**
     * the connected controllers that have a motor, by device id.
     *
     * **replaced whole rather than edited**, by the UI thread on a device arriving or leaving, so the
     * rumble thread always iterates a list nobody is changing. a controller that stays connected keeps
     * its entry.
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
        } else {
            val motor = Handheld(device)
            handheld = motor
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
    }

    /**
     * the host layer's entry point. resolved as `rumble(III)Z` at library load, so **the name and
     * signature are part of an interface** and cannot be changed on this side alone.
     *
     * **it returns whether the platform took the request, and the host counts only the trues.** a
     * void version reported success for anything that did not crash, so a rumble refused for want of
     * the `VIBRATE` permission -- which throws here rather than at any earlier check -- was counted as
     * delivered. a counter that cannot distinguish those is worse than none.
     *
     * @param port the port the guest asked about, 0 to 3. under automatic mapping every port drives
     *   every motor, since every controller is merged into port 1.
     * @param large the strong motor, 0..255.
     * @param small the weak motor, 0..255.
     * @return true when at least one motor has the request; false when there is nothing to vibrate,
     *   nothing was asked for, vibration is off, or every motor refused.
     */
    @JvmStatic
    @Suppress("UNUSED_PARAMETER")
    fun rumble(port: Int, large: Int, small: Int): Boolean {
        if (!enabled || !automatic) {
            return false
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

    private fun drive(motor: Motor, strength: Int): Boolean {
        try {
            if (strength < FLOOR) {
                motor.stop()
                return false
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
