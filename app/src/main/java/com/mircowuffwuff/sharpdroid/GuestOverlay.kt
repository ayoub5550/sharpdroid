package com.mircowuffwuff.sharpdroid

import android.content.ClipData
import android.content.ClipboardManager
import android.content.Context
import android.os.Build
import android.os.Handler
import android.os.Looper
import android.view.LayoutInflater
import android.view.View
import android.view.ViewGroup
import android.widget.LinearLayout
import android.widget.TextView
import android.widget.Toast
import androidx.core.view.ViewCompat
import androidx.core.view.WindowInsetsCompat
import androidx.core.view.updatePadding
import androidx.recyclerview.widget.LinearLayoutManager
import androidx.recyclerview.widget.RecyclerView
import com.google.android.material.button.MaterialButton

/**
 * the panel the back button opens over a running game.
 *
 * **it is the only thing the back button does during a run, and that is the point.** a back press that
 * finished the activity would end a game silently, at the depth of one accidental gesture -- the game
 * survives the app being left, and does not survive the activity ending. here back opens this, back
 * again closes it, and leaving is a labelled button inside it -- two deliberate acts, of which the
 * second says what it does. pausing is the button beside it.
 *
 * **most of it is the log**, which is what the panel is for now: everything this process has printed,
 * in the order it printed it -- the emulator's own logger, its raw console writes, the host layer's
 * lines and the app's. those first three share one pipe already and the fourth is put beside them by
 * [AppLog]. **it does not name the running game**: the cover that started it and the screen it booted
 * behind both say which game this is, and a third statement of it would cost the width this panel has
 * to spend.
 *
 * **it wears the scheme the settings scene names, and that arrives as a [Context].** [MainActivity]
 * keeps the framework fullscreen theme, because its window is a surface a guest renders into -- so
 * everything here is built from [Theme.overlayContext] instead, which resolves the same colour roles
 * the app's own screens do. what is drawn over a game and what is drawn on the game list are one
 * palette; the window under them is not involved. **that is also what makes the panel a layout rather
 * than views assembled in code**: a themed context has the Material attributes an inflation asks for.
 *
 * **the panel is 44% of the width and it is a weight rather than a measurement.** dividing the
 * screen in pixels would be the same answer on this device and a wrong one on a panel of another
 * shape, and the layout already has a mechanism for a proportion.
 */
class GuestOverlay(
    private val context: Context,
    private val onPause: Runnable,
    private val onResume: Runnable,
    private val onExit: Runnable,
) {

    /**
     * the whole-screen dim, which is also what swallows a touch aimed past the panel.
     *
     * an [OverGuestSurface] rather than a plain layout, and **that class is the one thing here worth
     * reading before changing anything**: it is `INVISIBLE` rather than `GONE` while closed, so the
     * panel has a width to slide in from on the first open of a run, and the price of `INVISIBLE` is
     * that becoming visible has to ask for a layout or it never reaches the display.
     */
    private val root: OverGuestSurface = OverGuestSurface(context).apply {
        orientation = LinearLayout.HORIZONTAL
        setBackgroundColor(SCRIM)
    }

    /**
     * inflated against [root] without attaching, which is what keeps its own `layout_` attributes;
     * the weight below is set here because it is this class that decides how wide the panel is.
     */
    private val panel: View =
        LayoutInflater.from(context).inflate(R.layout.guest_overlay, root, false)

    private val log: RecyclerView = panel.findViewById(R.id.log)
    private val empty: TextView = panel.findViewById(R.id.empty)
    private val lines = LogAdapter(context)
    private val pause: MaterialButton = panel.findViewById(R.id.pause)

    /** whether the game is paused, as the activity last said through [setPaused]. */
    private var paused = false

    /** whether a back press closes this or opens it. flipped before the animation, not after it. */
    var isOpen: Boolean = false
        private set

    /**
     * the sequence after the last line taken from the host layer's ring.
     *
     * reset on every open rather than kept across one, because the panel drops what it holds when it
     * closes: a run being played holds nothing of its own log, and a panel being opened reads the
     * window from wherever it now begins.
     */
    private var cursor: Long = 0

    private val ticker = Handler(Looper.getMainLooper())

    /**
     * the panel's own padding, in pixels, since the weight is applied to the panel rather than to
     * anything inside it.
     *
     * **it is the settings list's padding**, which is the number every list in the app is inset by,
     * and the layout inside the panel spaces itself against it.
     */
    private val pad = (PAD * context.resources.displayMetrics.density).toInt()

    /**
     * **only while the panel is open.** the cost of the ring is paid by the log pump whatever happens;
     * the cost of reading it is paid only by a person looking at it, which is what keeps an ordinary
     * run exactly the run it was.
     */
    private val poll = object : Runnable {
        override fun run() {
            pump()
            ticker.postDelayed(this, POLL)
        }
    }

    init {
        log.layoutManager = LinearLayoutManager(context).apply {
            // a log reads from the bottom: the newest line is the one somebody opened this for, and a
            // short one should sit at the foot of the frame rather than hang from its head.
            stackFromEnd = true
        }
        log.adapter = lines
        // the lines arrive in blocks and are never edited in place, so there is nothing for an item
        // animator to animate and its 250 ms of change animation would only delay the newest line.
        log.itemAnimator = null

        panel.findViewById<MaterialButton>(R.id.copy).setOnClickListener { copy() }
        pause.setOnClickListener {
            // **neither pausing nor resuming closes the panel.** both are tapped from inside it, by
            // somebody who is using it, and it closes the way it does at any other time: back, or a
            // tap past it.
            if (paused) {
                onResume.run()
            } else {
                onPause.run()
            }
        }
        panel.findViewById<MaterialButton>(R.id.exit).setOnClickListener {
            // the run ends here and the process ends with it, which is the same ending every launch
            // that is not exit_group already takes. the guest is not asked to stop first because
            // there is nothing to ask with: its threads are inside translated code, which is the
            // very reason the host layer answers exit_group with _exit.
            close()
            onExit.run()
        }

        // a cutout sits over the panel in landscape and over nothing else, since the surface below
        // is meant to reach every edge. so the padding is the panel's rather than the window's, and
        // MainActivity stays what SystemBars documents it as: a screen that pads for nothing.
        ViewCompat.setOnApplyWindowInsetsListener(panel) { view, windowInsets ->
            val insets = windowInsets.getInsets(WindowInsetsCompat.Type.displayCutout())
            view.updatePadding(pad + insets.left, pad + insets.top, pad, pad + insets.bottom)
            windowInsets
        }
        panel.updatePadding(pad, pad, pad, pad)

        val past = View(context)
        past.setOnClickListener { close() }

        root.addView(panel, LinearLayout.LayoutParams(0, ViewGroup.LayoutParams.MATCH_PARENT, PANEL))
        root.addView(past, LinearLayout.LayoutParams(0, ViewGroup.LayoutParams.MATCH_PARENT, BESIDE))
        // the panel eats what lands on it, so a touch beside a button does not read as a tap past
        // the overlay and close it.
        panel.isClickable = true
    }

    /** Added over the surface by [MainActivity], above the unpacking bar. */
    fun view(): View = root

    /**
     * whether the game is paused, which decides what the pause button says and does.
     *
     * **told rather than asked**, because the game pauses from places this panel never sees -- leaving
     * the app, the button drawn over a paused game, a controller -- and the activity is the one place
     * all of them go through.
     */
    fun setPaused(paused: Boolean) {
        this.paused = paused
        pause.setText(if (paused) R.string.overlay_resume else R.string.overlay_pause)
        pause.setIconResource(if (paused) R.drawable.ic_play else R.drawable.ic_pause)
    }

    fun open() {
        if (isOpen) {
            return
        }
        isOpen = true
        root.show()
        root.alpha = 0f
        root.animate().alpha(1f).setDuration(SLIDE).start()
        panel.translationX = -panel.width.toFloat()
        panel.animate().translationX(0f).setDuration(SLIDE).start()

        // everything still held, however long ago this run started. a panel opened three minutes in
        // shows the three minutes, which is the whole reason the ring is filled from the start of the
        // process rather than from the first time somebody looks.
        cursor = HostLayer.nativeLogOldest()
        pump()
        ticker.postDelayed(poll, POLL)
    }

    fun close() {
        if (!isOpen) {
            return
        }
        isOpen = false
        ticker.removeCallbacks(poll)
        root.animate().alpha(0f).setDuration(SLIDE)
            .withEndAction {
                root.hide()
                // emptied after it is off screen rather than as it leaves, so that what slides out is
                // the panel that was being read rather than an empty one.
                lines.clear()
                empty.visibility = View.VISIBLE
            }.start()
        panel.animate().translationX(-panel.width.toFloat()).setDuration(SLIDE).start()
    }

    /**
     * takes whatever arrived since the last look.
     *
     * **the end is read before the range is asked for, and never after**, so a line printed between
     * the two calls is simply next time's rather than a line skipped: the range asked for is one this
     * side has already decided the width of.
     *
     * a range the ring has passed by is clamped to what it still holds, so what is shown may begin
     * later than what was asked for. that is the same silent dropping the desktop viewer does at its
     * own cap, and it is why the panel says how far back it goes by starting where the ring starts.
     */
    private fun pump() {
        val next = HostLayer.nativeLogNext()
        if (next <= cursor) {
            return
        }
        val arrived = HostLayer.nativeLogRange(cursor, next)
        cursor = next
        if (arrived.isEmpty()) {
            return
        }

        // asked before the insert, because after it the last item is always the new one and the
        // question is whether the person was reading the foot of the log or somewhere above it.
        val manager = log.layoutManager as LinearLayoutManager
        val pinned = manager.findLastVisibleItemPosition() >= lines.itemCount - 1

        lines.append(arrived)
        empty.visibility = View.GONE
        if (pinned) {
            log.scrollToPosition(lines.itemCount - 1)
        }
    }

    /**
     * puts the tail of the log on the clipboard.
     *
     * **a budget rather than everything**, because the clipboard crosses a Binder transaction and a
     * transaction that is too large is an exception rather than a truncation. the newest lines are the
     * ones worth keeping when something has to be dropped, which is why the tail is what is taken.
     */
    private fun copy() {
        if (lines.isEmpty()) {
            return
        }
        val clipboard = context.getSystemService(ClipboardManager::class.java) ?: return
        clipboard.setPrimaryClip(
            ClipData.newPlainText(context.getString(R.string.overlay_log), lines.tail(COPY_BUDGET))
        )
        // android 13 and later show a confirmation of their own for every copy, so saying it again
        // here would be the same message twice.
        if (Build.VERSION.SDK_INT < Build.VERSION_CODES.TIRAMISU) {
            Toast.makeText(context, R.string.overlay_log_copied, Toast.LENGTH_SHORT).show()
        }
    }

    private companion object {
        /**
         * the dim over the game, and **the one colour here that is not the scheme's**.
         *
         * it is a shade cast on somebody else's picture rather than a surface of ours, so it is black
         * in every scheme: a light scheme's own surface used as a dim would wash the game out and
         * leave the panel with nothing to stand against, which is the opposite of what the dim is
         * for. the panel over it is the scheme's, at whatever lightness the scheme is.
         */
        const val SCRIM = 0x99000000.toInt()

        /**
         * 44 against 56, **and the pair is what says it rather than either number**. a log line
         * carries a level, a category and a source position before its message begins, so the panel
         * is the width of the thing on it rather than a third of the screen -- while the game
         * behind stays the larger share, since the dim is what makes this legible and it is cast over
         * somebody's picture.
         *
         * they are written as the percentage rather than reduced to 11 against 14, because what is
         * being chosen here is how much of the screen the panel takes and the reduced pair does not
         * say that to anybody reading it.
         */
        const val PANEL = 44f
        const val BESIDE = 56f

        /** the settings list's own padding, which is what the panel is inset by. */
        const val PAD = 10
        const val SLIDE = 160L

        /**
         * how often the ring is asked for what is new.
         *
         * **slower than a frame, deliberately.** a log is read rather than watched, four updates a
         * second is faster than anybody scrolls, and each one is a lock the log pump could be waiting
         * on -- so the interval is chosen against what the guest is doing rather than against what
         * looks smooth.
         */
        const val POLL = 250L

        /**
         * how much of the log a copy takes, in characters.
         *
         * a Binder transaction fails somewhere around a megabyte and the failure is an exception on
         * the way to the clipboard, so this is well under it: enough to carry a boot and the minutes
         * around whatever went wrong, and not enough to reach the ceiling.
         */
        const val COPY_BUDGET = 256 * 1024
    }
}
