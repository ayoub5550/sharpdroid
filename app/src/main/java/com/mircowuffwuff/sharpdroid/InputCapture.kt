package com.mircowuffwuff.sharpdroid

import android.app.Activity
import android.os.Handler
import android.os.Looper
import android.util.SparseArray
import android.view.InputDevice
import android.view.KeyEvent
import android.view.MotionEvent
import android.view.Window
import androidx.appcompat.app.AlertDialog
import com.google.android.material.dialog.MaterialAlertDialogBuilder
import kotlin.math.abs

/**
 * the dialog that binds one target to the next input pressed: Dolphin's `MotionAlertDialog` and the
 * `InputDetector` behind it (`InputCommon/ControllerInterface/CoreDevice.cpp`), on android's events.
 *
 * **Dolphin's rules.** an input is pressed when it passes [THRESHOLD] and released when it falls
 * under `1 - THRESHOLD`; capture ends when the pressed input is released, gives up after
 * [INITIAL_WAIT_MS] with nothing pressed, and ends after [MAXIMUM_WAIT_MS] whatever is held. every key
 * is swallowed while it is open, and a pointer's motion is not, so a touch still reaches Cancel. a key
 * pressed within [SPURIOUS_MS] of an axis is that axis's own echo -- a trigger reporting a button as
 * well as its travel -- and the axis is taken.
 *
 * **where it differs, and why:**
 *
 * - **giving up changes nothing.** Dolphin clears the binding when capture times out; here a person
 *   who put the controller down keeps what they had, and Clear is a button of its own.
 * - **BACK cancels on a short press.** Dolphin can bind BACK, so it needs a long press to mean cancel;
 *   here BACK is never bound, which leaves the ordinary press free. the volume keys pass to android,
 *   being the device's.
 * - **events rather than a poll.** an input only changes with an event, so the one thing Dolphin's
 *   10 ms poll adds is noticing the timeouts, which two delayed posts do.
 * - **an input's resting point is not read, because this process cannot read it.** Dolphin's
 *   interface has been fed every event since the app started and knows where each input was when
 *   capture began; android sends a stick nothing while it rests. so an input is taken as resting at
 *   zero, which a hat and a trigger reporting 0 to 1 always do -- and an axis centred on zero that
 *   arrives already past the threshold in its device's first event, which could be a trigger resting
 *   at one end of a centred range, is not taken until it has come back. a hat pressed as the first
 *   thing its device sends is a press, and is taken. what this costs is a stick flicked so hard that
 *   its first report is already past half way: it has to be let go and pushed again.
 * - **one input per binding.** Dolphin joins inputs pressed together into an expression; a binding
 *   here is one input, so the first pressed wins, and among inputs pressed in the same event the one
 *   furthest past the threshold, then the lowest axis -- the Odin's own L2 moves `LTRIGGER` and `BRAKE`
 *   together.
 * - **axes come only from a joystick.** a touchpad, a mouse or the screen is never an axis, so a touch
 *   on the DualSense's touchpad cannot be mistaken for a stick. Dolphin leaves out only the pointer
 *   class.
 *
 * nothing here runs outside the dialog, and nothing runs in a game.
 *
 * @param slot the port and target the result binds, as [ControllerMapping.Binding.slot] counts them.
 * @param numbers the identity of every connected device, as a launch made now would number them.
 */
class InputCapture(
    private val activity: Activity,
    private val title: String,
    private val slot: Int,
    private val numbers: DeviceNumbers,
    private val onBound: (ControllerMapping.Binding) -> Unit,
    private val onClear: () -> Unit,
) {

    private val handler = Handler(Looper.getMainLooper())
    private lateinit var dialog: AlertDialog

    /** true once a result is taken or the dialog is going, after which every event is only swallowed. */
    private var done = false

    /**
     * one device's joystick axes and what capture knows of each half: [ready] false from a press until
     * it is released, or from arriving past the threshold until it comes back.
     */
    private class Axes(
        val axes: IntArray,
        /** whether each axis is centred on zero and not a hat, so may rest away from it. */
        val mayRestOff: BooleanArray,
        /** which halves exist, positive at `2i` and negative at `2i + 1`. */
        val halves: BooleanArray,
    ) {
        val ready = BooleanArray(halves.size) { true }
        var seen = false
    }

    private val devices = SparseArray<Axes>()

    private class Detection(
        val deviceId: Int,
        val binding: ControllerMapping.Binding,
        val time: Long,
        val score: Float,
    ) {
        var released = false
        val isKey: Boolean get() = binding.keyCode != ControllerMapping.NO_KEY
    }

    private val detections = ArrayList<Detection>()

    fun show() {
        dialog = MaterialAlertDialogBuilder(activity)
            .setTitle(title)
            .setMessage(R.string.capture_message)
            .setNegativeButton(R.string.cancel, null)
            .setNeutralButton(R.string.capture_clear) { _, _ -> onClear() }
            .create()
        // Dolphin's, and for the same reason: a tap beside the dialog is too easy to make by accident
        // with a controller in both hands.
        dialog.setCanceledOnTouchOutside(false)
        dialog.setOnDismissListener {
            done = true
            handler.removeCallbacksAndMessages(null)
        }
        // **the window's callback is wrapped rather than the dialog subclassed**, which keeps the
        // builder's own Material styling -- Dolphin subclasses AlertDialog and repaints its background
        // by hand to get it back. created by now, so the callback being wrapped is AppCompat's own.
        val window = dialog.window!!
        val inner = window.callback
        window.callback = object : Window.Callback by inner {
            override fun dispatchKeyEvent(event: KeyEvent): Boolean =
                key(event) ?: inner.dispatchKeyEvent(event)

            override fun dispatchGenericMotionEvent(event: MotionEvent): Boolean =
                motion(event) ?: inner.dispatchGenericMotionEvent(event)
        }
        dialog.show()
        handler.postDelayed({ if (detections.isEmpty()) dialog.cancel() }, INITIAL_WAIT_MS)
        handler.postDelayed({ finish() }, MAXIMUM_WAIT_MS)
    }

    /** a key: true when swallowed, null when it goes on to the dialog. */
    private fun key(event: KeyEvent): Boolean? {
        when (event.keyCode) {
            KeyEvent.KEYCODE_VOLUME_UP, KeyEvent.KEYCODE_VOLUME_DOWN, KeyEvent.KEYCODE_VOLUME_MUTE ->
                return null
            KeyEvent.KEYCODE_BACK -> {
                if (event.action == KeyEvent.ACTION_UP && !event.isCanceled) dialog.cancel()
                return true
            }
        }
        if (done) return true
        // a virtual device -- adb's input, the on-screen keyboard -- has no identity to bind to.
        val identity = numbers.identityOf(event.deviceId) ?: return true
        val code = event.keyCode
        when (event.action) {
            KeyEvent.ACTION_DOWN -> {
                // a repeat is android's auto-repeat on a key already down, and a key already down
                // when the dialog opened has no press here to have been.
                if (event.repeatCount > 0 || detections.any { it.matches(event.deviceId, code) }) {
                    return true
                }
                detections.add(Detection(event.deviceId,
                    ControllerMapping.Binding(slot, identity, code, 0, ControllerMapping.NO_AXIS, false),
                    event.eventTime, 1f))
            }
            KeyEvent.ACTION_UP -> {
                val pressed = detections.firstOrNull { it.matches(event.deviceId, code) && !it.released }
                if (pressed != null) {
                    pressed.released = true
                    finish()
                }
            }
        }
        return true
    }

    private fun Detection.matches(deviceId: Int, keyCode: Int) =
        isKey && this.deviceId == deviceId && binding.keyCode == keyCode

    /** a motion event: true when swallowed, null when it goes on to the dialog. */
    private fun motion(event: MotionEvent): Boolean? {
        if ((event.source and InputDevice.SOURCE_CLASS_JOYSTICK) == 0) return null
        if (done || event.action != MotionEvent.ACTION_MOVE) return true
        val identity = numbers.identityOf(event.deviceId) ?: return true
        var axes = devices.get(event.deviceId)
        if (axes == null) {
            axes = axesOf(event.device) ?: return true
            devices.put(event.deviceId, axes)
        }
        val first = !axes.seen
        axes.seen = true
        var released = false
        for (i in axes.axes.indices) {
            val value = event.getAxisValue(axes.axes[i])
            for (negative in BOOLEANS) {
                val half = 2 * i + if (negative) 1 else 0
                if (!axes.halves[half]) continue
                val state = if (negative) -value else value
                if (first && axes.mayRestOff[i] && state > THRESHOLD) {
                    axes.ready[half] = false
                    continue
                }
                if (!axes.ready[half]) {
                    if (state < RELEASE) {
                        axes.ready[half] = true
                        detections.firstOrNull { it.matches(event.deviceId, axes.axes[i], negative) }
                            ?.let { it.released = true; released = true }
                    }
                    continue
                }
                if (state > THRESHOLD) {
                    axes.ready[half] = false
                    detections.add(Detection(event.deviceId,
                        ControllerMapping.Binding(slot, identity, ControllerMapping.NO_KEY,
                            InputDevice.SOURCE_JOYSTICK, axes.axes[i], negative),
                        event.eventTime, state))
                }
            }
        }
        if (released) finish()
        return true
    }

    private fun Detection.matches(deviceId: Int, axis: Int, negative: Boolean) =
        !isKey && this.deviceId == deviceId && binding.axis == axis && binding.negative == negative

    /** [device]'s joystick axes, read once per device per dialog, or null for a device with none. */
    private fun axesOf(device: InputDevice?): Axes? {
        val ranges = device?.motionRanges?.filter {
            (it.source and InputDevice.SOURCE_CLASS_JOYSTICK) != 0
        } ?: return null
        if (ranges.isEmpty()) return null
        val axes = ranges.map { it.axis }.distinct().toIntArray()
        val halves = BooleanArray(axes.size * 2)
        val mayRestOff = BooleanArray(axes.size)
        for (range in ranges) {
            val i = axes.indexOf(range.axis)
            // Dolphin's: a half exists where the range reaches, so a trigger running 0 to 1 has no
            // negative half to be pressed.
            if (range.max > 0) halves[2 * i] = true
            if (range.min < 0) halves[2 * i + 1] = true
            mayRestOff[i] = range.min < 0 &&
                range.axis != MotionEvent.AXIS_HAT_X && range.axis != MotionEvent.AXIS_HAT_Y
        }
        return Axes(axes, mayRestOff, halves)
    }

    /**
     * takes the result, if there is one, and closes. what is taken is in the class comment: an axis
     * over a key pressed with it, then the earliest, then the furthest past the threshold, then the
     * lowest axis.
     */
    private fun finish() {
        if (done) return
        done = true
        val chosen = detections
            .filterNot { key ->
                key.isKey && detections.any { !it.isKey && abs(it.time - key.time) < SPURIOUS_MS }
            }
            .sortedWith(compareBy<Detection>({ it.time }, { -it.score }, { it.binding.axis }))
            .firstOrNull()
        if (chosen != null) onBound(chosen.binding)
        // posted rather than called, as Dolphin does: this runs inside the dialog's own dispatch.
        handler.post { dialog.dismiss() }
    }

    private companion object {
        /** Dolphin's `INPUT_DETECT_THRESHOLD`. */
        const val THRESHOLD = 0.55f
        const val RELEASE = 1 - THRESHOLD

        /** Dolphin's three seconds with nothing pressed, and five in all. */
        const val INITIAL_WAIT_MS = 3000L
        const val MAXIMUM_WAIT_MS = 5000L

        /** Dolphin's `SPURIOUS_TRIGGER_COMBO_THRESHOLD`. */
        const val SPURIOUS_MS = 150L

        val BOOLEANS = booleanArrayOf(false, true)
    }
}
