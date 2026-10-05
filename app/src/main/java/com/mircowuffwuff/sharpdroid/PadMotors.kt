package com.mircowuffwuff.sharpdroid

import android.content.Context
import android.os.Build
import android.os.CombinedVibration
import android.os.VibrationEffect
import android.os.Vibrator
import android.os.VibratorManager
import android.view.InputDevice

/**
 * the motors a controller port can name: this device's own, and each connected device's.
 *
 * **a motor is Dolphin's: an index among one device's vibrators**, `Motor 0`, `Motor 1`, in the order
 * android lists their ids. nothing says which is the strong one, which is why a port names its large
 * and its small motor itself and why picking one buzzes it -- that buzz is this app's, Dolphin having
 * none.
 *
 * the port screen lists and buzzes them, in the app's own process.
 */
object PadMotors {

    private const val TAG = "sharpdroid"

    /** how long the buzz on picking a motor lasts: long enough to tell a large motor from a small. */
    private const val BUZZ_MILLIS = 250L

    /**
     * the vibrator ids of [device], in the order a motor's index counts them, or none.
     *
     * **below android 12 a controller exposes one vibrator**, driving every motor it has, so it is one
     * motor here -- the only one a port could name there.
     */
    @JvmStatic
    fun vibratorIds(device: InputDevice): IntArray {
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.S) {
            return device.vibratorManager.vibratorIds
        }
        @Suppress("DEPRECATION")
        return if (device.vibrator.hasVibrator()) intArrayOf(0) else IntArray(0)
    }

    /** this device's own vibrator, or null where it has none. */
    @JvmStatic
    fun handheld(context: Context): Vibrator? {
        val vibrator = if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.S) {
            context.getSystemService(VibratorManager::class.java)?.defaultVibrator
        } else {
            @Suppress("DEPRECATION")
            context.getSystemService(Vibrator::class.java)
        }
        return vibrator?.takeIf { it.hasVibrator() }
    }

    /**
     * every motor there is right now, in the order a list offers them: this device's, then each
     * connected device's in [numbers]'s order.
     */
    fun present(context: Context, numbers: DeviceNumbers): List<ControllerMapping.Motor> {
        val found = ArrayList<ControllerMapping.Motor>()
        if (handheld(context) != null) found.add(ControllerMapping.Motor(null, 0))
        for (id in numbers.deviceIds()) {
            val identity = numbers.identityOf(id) ?: continue
            val device = InputDevice.getDevice(id) ?: continue
            for (index in vibratorIds(device).indices) found.add(ControllerMapping.Motor(identity, index))
        }
        return found
    }

    /**
     * a short full-strength buzz on [motor] alone. false when it is not here or the platform refused.
     *
     * **one vibrator of a controller is named by id in a combined vibration**, which is the call that
     * addresses a single motor.
     */
    fun buzz(context: Context, motor: ControllerMapping.Motor, numbers: DeviceNumbers): Boolean {
        try {
            val identity = motor.device
            if (identity == null) {
                val vibrator = handheld(context) ?: return false
                vibrator.vibrate(oneShot(vibrator))
                return true
            }
            val id = numbers.deviceIdOf(identity)
            if (id == DeviceNumbers.NO_DEVICE) return false
            val device = InputDevice.getDevice(id) ?: return false
            val ids = vibratorIds(device)
            if (motor.index !in ids.indices) return false
            if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.S) {
                val manager = device.vibratorManager
                val vibratorId = ids[motor.index]
                manager.vibrate(CombinedVibration.startParallel()
                    .addVibrator(vibratorId, oneShot(manager.getVibrator(vibratorId)))
                    .combine())
            } else {
                @Suppress("DEPRECATION")
                val vibrator = device.vibrator
                vibrator.vibrate(oneShot(vibrator))
            }
            return true
        } catch (e: Exception) {
            AppLog.w(TAG, "[pad] could not buzz $motor", e)
            return false
        }
    }

    private fun oneShot(vibrator: Vibrator): VibrationEffect = VibrationEffect.createOneShot(
        BUZZ_MILLIS,
        if (vibrator.hasAmplitudeControl()) 255 else VibrationEffect.DEFAULT_AMPLITUDE,
    )
}
