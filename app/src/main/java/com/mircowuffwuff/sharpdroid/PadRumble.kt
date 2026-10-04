package com.mircowuffwuff.sharpdroid

import android.content.Context
import android.os.Build
import android.os.VibrationEffect
import android.os.Vibrator
import android.os.VibratorManager

/**
 * the guest's rumble, on whatever this device can vibrate with.
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

    @Volatile
    private var vibrator: Vibrator? = null

    @Volatile
    private var amplitudeControl = false

    /**
     * whether a game may drive the motor -- Settings, Controls, the vibrate switch.
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
            AppLog.i(TAG, "[pad] this device has no vibrator, so rumble goes nowhere")
            vibrator = null
            return
        }
        vibrator = device
        amplitudeControl = device.hasAmplitudeControl()
        // **"an actuator exists" and "we may drive it" are different questions, and only the first is
        // answered here.** neither hasVibrator nor hasAmplitudeControl consults the VIBRATE
        // permission, so both answer truthfully to an app that has not been granted it and the vibrate
        // call alone throws. so this line says what it actually knows rather than "ready".
        AppLog.i(TAG, "[pad] a vibrator is present, amplitude control " +
                (if (amplitudeControl) "available" else "absent") +
                ". whether a buzz arrives is only known when one is asked for")
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
        vibrator?.cancel()
        vibrator = null
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
     * @param port the port the guest asked about, 0 to 3. every port drives this device's own motor.
     * @param large the strong motor, 0..255. a single-actuator device is driven by this.
     * @param small the weak motor, 0..255, used only when there is nothing stronger asked for.
     * @return true when the platform accepted it; false when there is no vibrator, nothing was asked
     *   for, or the request was refused.
     */
    @JvmStatic
    @Suppress("UNUSED_PARAMETER")
    fun rumble(port: Int, large: Int, small: Int): Boolean {
        if (!enabled) {
            return false
        }
        val device = vibrator ?: return false
        // android has one actuator to aim at and the seam names two motors, so the louder wins. taking
        // the larger rather than a sum or an average is what keeps a request for one strong motor from
        // arriving weaker than it was asked for.
        val strength = maxOf(large, small).coerceIn(0, 255)
        try {
            if (strength < FLOOR) {
                device.cancel()
                return false
            }
            val effect = if (amplitudeControl) {
                VibrationEffect.createOneShot(PULSE_MILLIS, strength)
            } else {
                // no amplitude control, so the only choice is whether it buzzes at all. DEFAULT_AMPLITUDE
                // is the platform's own idea of a reasonable strength, which is a better answer than
                // picking one here.
                VibrationEffect.createOneShot(PULSE_MILLIS, VibrationEffect.DEFAULT_AMPLITUDE)
            }
            device.vibrate(effect)
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
