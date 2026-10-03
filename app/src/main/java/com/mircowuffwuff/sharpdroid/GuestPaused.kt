package com.mircowuffwuff.sharpdroid

import android.content.Context
import android.graphics.Bitmap
import android.os.Handler
import android.os.Looper
import android.os.SystemClock
import android.view.Choreographer
import android.view.LayoutInflater
import android.view.PixelCopy
import android.view.SurfaceView
import android.view.View
import android.widget.ImageView
import com.google.android.material.button.MaterialButton

/**
 * what a paused game shows: the frame it stopped on, and a button to carry on.
 *
 * **the frame is copied out of the surface at the moment of pausing, because the surface does not
 * keep it.** android takes a `SurfaceView`'s surface away whenever the app is left, and hands back an
 * empty one on the way in -- empty until the guest draws into it, which a paused guest does not do.
 * so without a copy, a game left and returned to is a black screen with a button on it. Eden keeps
 * the frame for the same reason and in the same place: in front of the surface, until the game draws
 * again.
 *
 * **and it stays up after the button is pressed, until the guest has drawn.** a guest returning to a
 * surface it has not seen yet has a swapchain to make before its first frame, and taking the copy
 * away the moment the button is pressed would show the empty surface for that long. so on the way
 * out the button goes at once and the picture waits for [HostLayer.nativePresentedFrames] to move,
 * up to [LONGEST_WAIT].
 */
class GuestPaused(context: Context, private val onResume: Runnable) {

    private val root: View = LayoutInflater.from(context).inflate(R.layout.view_guest_paused, null, false)
    private val still: ImageView = root.findViewById(R.id.paused_frame)
    private val resume: MaterialButton = root.findViewById(R.id.paused_resume)
    private val main = Handler(Looper.getMainLooper())

    /** the copy of the last frame, or null before it arrives or when there was none to copy. */
    private var frame: Bitmap? = null

    /**
     * which pause a copy belongs to. **a copy arrives asynchronously**, and one that lands after the
     * game it was taken for has already been resumed -- or paused again -- is a picture of a moment
     * nobody is looking at any more.
     */
    private var pauses = 0

    /**
     * whether the back panel is open over this, as [GuestOverlay] last said. **the button is faded
     * out while it is**: the panel carries a Resume of its own, and two on screen at once is one too
     * many.
     */
    private var covered = false

    /** the presented-frame count at the moment of resuming, or -1 while nothing is being waited for. */
    private var resumedAt = -1L
    private var waitingSince = 0L

    private val watch = object : Choreographer.FrameCallback {
        override fun doFrame(frameTimeNanos: Long) {
            if (resumedAt < 0) {
                return
            }
            if (HostLayer.nativePresentedFrames() > resumedAt ||
                SystemClock.uptimeMillis() - waitingSince > LONGEST_WAIT
            ) {
                gone()
                return
            }
            Choreographer.getInstance().postFrameCallback(this)
        }
    }

    init {
        resume.setOnClickListener { onResume.run() }
    }

    /** added over the surface by [MainActivity], above the loading screen and under the back panel. */
    fun view(): View = root

    /**
     * the game has just paused: show the button, and copy the frame out of [surface] while it is
     * still there to copy.
     */
    fun show(surface: SurfaceView) {
        stopWaiting()
        pauses++
        root.visibility = View.VISIBLE
        // a pause taken from the back panel happens under it, so the button arrives faded out.
        resume.animate().cancel()
        resume.alpha = if (covered) 0f else 1f
        resume.visibility = View.VISIBLE
        // **a frame already held is kept.** pausing again before the last picture went away -- a tap
        // on Resume and then straight back out of the app -- would otherwise copy the surface in the
        // middle of being replaced.
        if (frame == null) {
            copy(surface, pauses)
        }
    }

    /** the game has just resumed: the button goes now, and the picture once the game has drawn. */
    fun hide() {
        resume.visibility = View.GONE
        if (frame == null) {
            gone()
            return
        }
        resumedAt = HostLayer.nativePresentedFrames()
        waitingSince = SystemClock.uptimeMillis()
        Choreographer.getInstance().postFrameCallback(watch)
    }

    /**
     * the back panel has started to open over this, or to close: the button fades out or back in
     * over [duration], which is the panel's own, so the two read as one motion.
     *
     * **its opacity is all that changes.** a view over the surface that goes from `INVISIBLE` to
     * `VISIBLE` does not reach the display until something asks for a layout -- see
     * [OverGuestSurface] -- and a button at no opacity cannot be pressed anyway, since the panel's dim
     * takes every touch on the screen while it is open.
     */
    fun cover(covered: Boolean, duration: Long) {
        this.covered = covered
        resume.animate().alpha(if (covered) 0f else 1f).setDuration(duration).start()
    }

    /**
     * **PixelCopy rather than anything of the host layer's**, because the picture wanted is the one on
     * the panel, which is exactly what a copy of the surface is. a surface with nothing in it yet --
     * a pause during the boot -- has no picture to give, and the screen then shows whatever is under
     * it, which is the loading card.
     */
    private fun copy(surface: SurfaceView, pause: Int) {
        val width = surface.width
        val height = surface.height
        if (width <= 0 || height <= 0 || !surface.holder.surface.isValid) {
            return
        }
        val bitmap = Bitmap.createBitmap(width, height, Bitmap.Config.ARGB_8888)
        PixelCopy.request(surface, bitmap, { result ->
            if (result == PixelCopy.SUCCESS && pause == pauses && resume.visibility == View.VISIBLE && frame == null) {
                frame = bitmap
                still.setImageBitmap(bitmap)
            } else {
                bitmap.recycle()
            }
        }, main)
    }

    private fun stopWaiting() {
        resumedAt = -1
        Choreographer.getInstance().removeFrameCallback(watch)
    }

    private fun gone() {
        stopWaiting()
        root.visibility = View.GONE
        still.setImageDrawable(null)
        frame?.recycle()
        frame = null
    }

    private companion object {
        /**
         * how long the picture waits for the game to draw, in milliseconds.
         *
         * a guest that comes back to a new surface makes a swapchain and draws in a small fraction of
         * this. one that draws nothing at all for a second -- a game between scenes -- is shown as it
         * is rather than as it was.
         */
        const val LONGEST_WAIT = 1000L
    }
}
