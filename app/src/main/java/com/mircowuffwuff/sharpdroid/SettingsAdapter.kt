package com.mircowuffwuff.sharpdroid

import android.content.res.ColorStateList
import android.graphics.drawable.GradientDrawable
import android.text.SpannableString
import android.text.Spanned
import android.text.style.ForegroundColorSpan
import android.view.LayoutInflater
import android.view.View
import android.view.ViewGroup
import androidx.core.view.isVisible
import androidx.recyclerview.widget.RecyclerView
import com.google.android.material.button.MaterialButton
import com.google.android.material.color.MaterialColors
import com.google.android.material.dialog.MaterialAlertDialogBuilder
// the colour roles are Material's own attributes, and this module's R does not carry them: a
// non-transitive R class holds what the module itself declares and nothing a library does.
import com.google.android.material.R as MaterialR
import com.mircowuffwuff.sharpdroid.databinding.DialogMessageBinding
import com.mircowuffwuff.sharpdroid.databinding.ItemSettingColourBinding
import com.mircowuffwuff.sharpdroid.databinding.ItemSettingHeaderBinding
import com.mircowuffwuff.sharpdroid.databinding.ItemSettingPresetBinding
import com.mircowuffwuff.sharpdroid.databinding.ItemSettingSwitchBinding
import com.mircowuffwuff.sharpdroid.databinding.ItemSettingValueBinding

/**
 * draws a section's rows.
 *
 * **a long press on a row the user has set offers to put it back to the default**, and it is the one
 * piece of UI here that exists because of the precedence rule rather than because a screen wanted it
 * -- a row with no way back is a one-way door. it is deliberately the only sign of that rule:
 * a mark distinguishing a set row from an untouched one has to hold a space on every row, which
 * indents the whole list to annotate one of them.
 *
 * **on a per-game list that gesture is replaced rather than joined**, and [useGlobal] is what replaces
 * it: a row overriding the global value draws a button saying so. the reasoning above inverts there --
 * an overridden row is the interesting case rather than the rare one, so the space a mark costs is
 * worth paying, and a hidden gesture doing the same thing in different words beside a visible button
 * would be two ways to say one thing.
 *
 * @param onChanged called after a row writes, so the screen can redraw a row whose subtitle depends
 *   on it and so a theme change can take effect while the user is looking at it.
 */
class SettingsAdapter(
    private val settings: Settings,
    private var rows: List<SettingRow>,
    private val onChanged: (SettingRow) -> Unit,
) : RecyclerView.Adapter<RecyclerView.ViewHolder>() {

    /**
     * the whole list again, drawn without animation.
     *
     * for arriving at a section and for coming back to one: everything on screen is being drawn for
     * the first time, so there is no movement to describe.
     */
    fun submit(newRows: List<SettingRow>) {
        rows = newRows
        notifyDataSetChanged()
    }

    /**
     * the whole list again, coming back to a section that may have changed while it was away.
     *
     * **[submit] would be wrong here now, and it was not always.** it says "assume nothing about
     * what moved", which RecyclerView answers by laying out again with no animation at all -- fine
     * when nothing on the list could have changed behind the user's back, and no longer true: a row
     * that opens a screen of its own reports what was done on that screen, so returning is exactly
     * when its answer moves. under [submit] the button that appears under it snaps in.
     *
     * **so every row is notified with a payload, and the ones that did not move say so themselves.**
     * [useGlobal] already asks whether a row's answer differs from what is drawn and returns early
     * when it does not, so naming them all animates precisely the one that changed. the payload is
     * also what keeps the holders from being cross-faded against fresh ones, which is what would
     * blink every switch on the list.
     *
     * a rebuild that changed the list's length is not this -- it falls back to [submit], since a row
     * arriving or leaving is [replaceRow]'s to describe.
     */
    fun refresh(newRows: List<SettingRow>) {
        if (newRows.size != rows.size) {
            submit(newRows)
            return
        }
        rows = newRows
        notifyItemRangeChanged(0, rows.size, CHANGED)
    }

    /**
     * the whole list again, but saying which row the user just changed -- so that row alone redraws
     * and the ones below it **slide** to their new places instead of jumping there.
     *
     * **the list is replaced and the notification is narrow, and it needs to be both.** a row like
     * [SettingRow.Screen] carries the text it draws, so a value that changed is a row that has to be
     * rebuilt rather than rebound; but telling the adapter that *everything* changed is what throws
     * the animation away, because `notifyDataSetChanged` means "assume nothing about what moved" and
     * RecyclerView answers by laying out again with no animation at all. this hands it a list it can
     * still reason about, and one index that is different.
     *
     * **the payload is what keeps the row itself from flickering.** a change notification with none
     * makes RecyclerView build a second holder and cross-fade the two, which on a switch row is a
     * visible blink of the toggle; any payload at all means the holder is reused and rebound in
     * place, leaving only the movement below it to animate.
     *
     * **the row is found by its key, and being the same object is only the fast path.** a holder
     * keeps the row it was bound with, and this method rebinds one row and leaves every other holder
     * alone -- so after any write, every row the user did not touch is still holding an object from a
     * list that has since been replaced. looking those up by identity fails, which sends a perfectly
     * ordinary second tap down the fallback below: the list is rebuilt from nothing, the item views
     * are torn down and made again, and the ripple under the finger and the switch's own thumb slide
     * go with them. a key is the same string across every rebuild of a section, so it survives what
     * an object reference cannot.
     *
     * **keys are unique within a section**, which is what makes this a lookup rather than a guess;
     * the identity test stays in front of it for the rows that carry no key at all. it is
     * [SettingRow.id] that is compared, which is the key for every row that has one and a name of
     * the row's own for a row storing outside the preferences.
     *
     * a row that is in no list, or a rebuild that changed the list's length, falls back to [submit]
     * -- the affordance appearing inside a row never changes the length, and the one row set that
     * does grow and shrink belongs to a theme change, which restarts the screen anyway.
     */
    fun submit(newRows: List<SettingRow>, changed: SettingRow) {
        val index =
            rows.indexOfFirst { it === changed || (changed.id != null && it.id == changed.id) }
        if (index < 0 || newRows.size != rows.size) {
            submit(newRows)
            return
        }
        rows = newRows
        notifyItemChanged(index, CHANGED)
    }

    /**
     * one row bound again where it stands, for a row that reads out something another row changed.
     *
     * **the list is already correct when this is called** -- [submit] replaced it -- so this only
     * says that a second row's holder is now describing an older answer than the one it is showing.
     * the payload is the same one [submit] sends, and for the same reason: without it the holder is
     * cross-faded against a second copy of itself.
     */
    fun rebind(id: String) {
        val index = rows.indexOfFirst { it.id == id }
        if (index >= 0) notifyItemChanged(index, CHANGED)
    }

    /**
     * the whole list again, one row longer or one shorter, saying which row arrived or left.
     *
     * **this is the only path that animates a row into or out of the list**, and it exists because a
     * row that comes and goes is otherwise indistinguishable from a list that was replaced: [submit]
     * says "assume nothing", and RecyclerView answers by laying out again with no animation at all.
     * named, an insert fades the new row up while the rows below slide down to make room for it, and
     * a removal does both in reverse -- which is the same pair of movements the Use global value
     * button inside a row already gets, and for the same reason.
     *
     * **it is the caller that knows what moved.** an adapter handed two lists can only diff them,
     * and a diff of a settings section would have to decide whether a row whose value changed is the
     * same row -- a question the caller never has to ask, because it is the one that added or took
     * away the row.
     */
    fun replaceRow(newRows: List<SettingRow>, at: Int, arriving: Boolean) {
        rows = newRows
        if (arriving) notifyItemInserted(at) else notifyItemRemoved(at)
    }

    override fun getItemCount() = rows.size

    override fun getItemViewType(position: Int) = when (rows[position]) {
        is SettingRow.Header -> TYPE_HEADER
        is SettingRow.Switch -> TYPE_SWITCH
        is SettingRow.Dropdown -> TYPE_VALUE
        is SettingRow.Cards -> TYPE_CARDS
        is SettingRow.Colour -> TYPE_COLOUR
        is SettingRow.External -> TYPE_EXTERNAL
        is SettingRow.Screen -> TYPE_SCREEN
    }

    override fun onCreateViewHolder(parent: ViewGroup, viewType: Int): RecyclerView.ViewHolder {
        val inflater = LayoutInflater.from(parent.context)
        return when (viewType) {
            TYPE_HEADER -> HeaderHolder(ItemSettingHeaderBinding.inflate(inflater, parent, false))
            TYPE_SWITCH -> SwitchHolder(ItemSettingSwitchBinding.inflate(inflater, parent, false))
            TYPE_VALUE -> ValueHolder(ItemSettingValueBinding.inflate(inflater, parent, false))
            TYPE_CARDS -> CardsHolder(ItemSettingPresetBinding.inflate(inflater, parent, false))
            TYPE_COLOUR -> ColourHolder(ItemSettingColourBinding.inflate(inflater, parent, false))
            // the same layout a dropdown uses: a title, an explanation and the chosen thing under
            // it. what differs is where the tap goes, which is not something a layout knows.
            TYPE_SCREEN -> ScreenHolder(ItemSettingValueBinding.inflate(inflater, parent, false))
            else -> ExternalHolder(ItemSettingSwitchBinding.inflate(inflater, parent, false))
        }
    }

    /**
     * true only while binding the one row a write just changed.
     *
     * **it is what separates a row changing under the user's finger from a holder being reused**, and
     * without it a list that is scrolled would fade buttons in and out as recycled views arrived on
     * rows that never changed. the payload [submit] sends is the signal: a bind that carries one is
     * the targeted redraw, and a bind that carries none is a view being filled in for the first time
     * or being reused.
     */
    private var changing = false

    /**
     * true while binding the pass that takes a faded-out button's space back -- see [collapse].
     */
    private var collapsing = false

    override fun onBindViewHolder(
        holder: RecyclerView.ViewHolder,
        position: Int,
        payloads: MutableList<Any>,
    ) {
        changing = payloads.isNotEmpty()
        collapsing = payloads.contains(COLLAPSE)
        super.onBindViewHolder(holder, position, payloads)
        changing = false
        collapsing = false
    }

    /**
     * announces that a button which has finished fading is now gone, so the row shrinks and the ones
     * below it slide up.
     *
     * **a second pass, because a height that changes outside one is a height nothing animates.** the
     * framework animates what moved between two layouts of its own; taking the view away inside an
     * animation's end action changes the layout after that comparison has already been made, so the
     * rows below jump to their new places. asking for another change on the same row puts the shrink
     * inside a pass, which is the same thing that makes the button *appearing* slide.
     */
    private fun collapse(row: SettingRow) {
        val index = rows.indexOfFirst { it === row }
        if (index >= 0) notifyItemChanged(index, COLLAPSE)
    }

    override fun onBindViewHolder(holder: RecyclerView.ViewHolder, position: Int) {
        val row = rows[position]
        when (row) {
            is SettingRow.Header -> (holder as HeaderHolder).bind(row)
            is SettingRow.Switch -> (holder as SwitchHolder).bind(row)
            is SettingRow.Dropdown -> (holder as ValueHolder).bind(row)
            is SettingRow.Cards -> (holder as CardsHolder).bind(row)
            is SettingRow.Colour -> (holder as ColourHolder).bind(row)
            is SettingRow.External -> (holder as ExternalHolder).bind(row)
            is SettingRow.Screen -> (holder as ScreenHolder).bind(row)
        }
        drawEnabled(holder.itemView, enabled(position))
    }

    /**
     * whether the row at [position] is drawn enabled.
     *
     * **a header is enabled while any row under it is**, up to the next header, so a subsection whose
     * every row is greyed is greyed with them: the Controller ports label while Automatic controller
     * mapping is on, and every heading on a port screen over a mapping file this app cannot read. it
     * is worked out from the list rather than stored on the header, so that no section has a second
     * value to keep in step with its rows. a header over no rows keeps its colour, there being nothing
     * for it to agree with.
     *
     * **it needs no notification of its own**, because the one write that greys rows, the mapping
     * switch, already rebinds every row on the list -- see [refresh].
     */
    private fun enabled(position: Int): Boolean {
        if (rows[position] !is SettingRow.Header) return rows[position].enabled
        var under = position + 1
        while (under < rows.size && rows[under] !is SettingRow.Header) {
            if (rows[under].enabled) return true
            under++
        }
        return under == position + 1
    }

    /**
     * a row greyed out or not, for every row type at once.
     *
     * **the whole row is what takes a tap** -- a switch's own control is not clickable, see
     * item_setting_switch.xml -- so disabling the row's root is the whole of making it ignore one,
     * the long press included.
     *
     * **the fade is drawn on the root's children rather than on the root**, because the root's alpha
     * is the item animator's: a change that keeps its holder is animated as a move, and that resets
     * the root's animation and its alpha on the way, which would stop a fade there part way through.
     * a row whose state flips under the user's finger -- the four ports and the label over them, as
     * Automatic controller mapping is switched -- fades, and every other bind is simply in the state it
     * belongs in.
     */
    private fun drawEnabled(view: View, enabled: Boolean) {
        view.isEnabled = enabled
        val group = view as? ViewGroup ?: return
        val alpha = if (enabled) 1f else DISABLED_ALPHA
        for (i in 0 until group.childCount) {
            val child = group.getChildAt(i)
            child.animate().cancel()
            if (changing && child.alpha != alpha) {
                child.animate().alpha(alpha).setDuration(FADE_MS).start()
            } else {
                child.alpha = alpha
            }
        }
    }

    /**
     * a row that opens another screen, showing what is currently chosen underneath it.
     *
     * the long press works as on any stored row -- a build that was chosen is a value like any other,
     * and *Use default* puts it back to the app deciding, which for the build row means the one that
     * shipped inside this APK.
     */
    private inner class ScreenHolder(val binding: ItemSettingValueBinding) :
        RecyclerView.ViewHolder(binding.root) {
        fun bind(row: SettingRow.Screen) {
            val arg = row.titleArg
            if (arg == null) {
                binding.title.setText(row.title)
            } else {
                binding.title.text = binding.root.context.getString(row.title, arg)
            }
            // set on both branches, for the reason the value's colour is below: a recycled holder
            // keeps whatever the row it last drew left on it.
            val summary = row.summary
            binding.summary.isVisible = summary != null
            if (summary != null) binding.summary.setText(summary)
            // and the glyph the same way, for the same reason. its tint is the row's, so the layout
            // carries none.
            val icon = row.icon
            binding.icon.isVisible = icon != null
            if (icon != null) {
                binding.icon.setImageResource(icon)
                binding.icon.imageTintList = ColorStateList.valueOf(
                    MaterialColors.getColor(
                        binding.icon,
                        if (row.iconAccented) {
                            MaterialR.attr.colorPrimary
                        } else {
                            MaterialR.attr.colorOnSurfaceVariant
                        },
                    )
                )
            }
            binding.value.text = drawn(row.value)
            // set on both branches rather than only the plain one, because a holder is recycled and
            // the colour of the row it was last bound to would otherwise stay on it.
            binding.value.setTextColor(
                MaterialColors.getColor(
                    binding.value,
                    if (row.chosen) MaterialR.attr.colorPrimary else MaterialR.attr.colorOnSurfaceVariant,
                )
            )

            binding.root.setOnClickListener { row.onClick() }
            // a screen that stores nothing has no default to go back to, so it has no long press.
            // a row storing outside the preferences has its own way back, offered while it is set.
            binding.root.setOnLongClickListener {
                val key = row.key
                val reset = row.reset
                when {
                    key != null -> offerDefault(key, row)
                    reset != null && row.chosen -> offerReset(reset, row)
                    else -> false
                }
            }
            useGlobal(binding.useGlobal, row.key, row)
        }

        /**
         * [value] with each part marked [SettingRow.Absent] in the body colour, over the accent the
         * line is drawn in. a value with no such part is handed over as it is, which is every row that
         * names nothing gone.
         */
        private fun drawn(value: CharSequence): CharSequence {
            if (value !is Spanned) return value
            val marks = value.getSpans(0, value.length, SettingRow.Absent::class.java)
            if (marks.isEmpty()) return value
            val body = MaterialColors.getColor(binding.value, MaterialR.attr.colorOnSurfaceVariant)
            val drawn = SpannableString(value)
            for (mark in marks) {
                drawn.setSpan(
                    ForegroundColorSpan(body), value.getSpanStart(mark), value.getSpanEnd(mark),
                    Spanned.SPAN_EXCLUSIVE_EXCLUSIVE)
            }
            return drawn
        }
    }

    private class HeaderHolder(val binding: ItemSettingHeaderBinding) :
        RecyclerView.ViewHolder(binding.root) {
        fun bind(row: SettingRow.Header) {
            binding.title.setText(row.title)
        }
    }

    private inner class SwitchHolder(val binding: ItemSettingSwitchBinding) :
        RecyclerView.ViewHolder(binding.root) {
        fun bind(row: SettingRow.Switch) {
            binding.title.setText(row.title)
            binding.summary.setText(row.summary)

            // set without the listener attached, or restoring the stored state would read as the
            // user flipping it -- which would write the key and make every untouched row "set" the
            // first time its section is opened.
            binding.toggle.setOnCheckedChangeListener(null)
            binding.toggle.isChecked = stored(row.key, row.default)
            binding.toggle.setOnCheckedChangeListener { _, checked ->
                write(row.key, checked)
                onChanged(row)
            }
            binding.root.setOnClickListener { binding.toggle.toggle() }
            binding.root.setOnLongClickListener { offerDefault(row.key, row) }
            useGlobal(binding.useGlobal, row.key, row)
        }

        /**
         * a FEXCore knob's row, or null for one of the named rows below.
         *
         * **the knobs are looked up rather than listed here**, because every one of them is stored,
         * read and emitted the same way -- an option name and a value -- so a `when` naming them
         * would be a second list to keep in step with [FexPreset.KNOBS] for no answer it does not
         * already give.
         */
        private fun knob(key: String): FexPreset.Knob? =
            FexPreset.KNOBS.firstOrNull { Settings.fexKnobKey(it.option) == key }

        private fun stored(key: String, default: Boolean): Boolean {
            val knob = knob(key)
            // **[default] is the rung's own value here rather than a constant**, which is why an
            // untouched knob row follows the ladder. see Settings.fexKnobDefault.
            if (knob != null) return settings.fexKnob(knob.option)?.let(knob::checked) ?: default
            return named(key, default)
        }

        private fun named(key: String, default: Boolean): Boolean = when (key) {
            Settings.KEY_FULLSCREEN -> settings.fullscreen ?: default
            Settings.KEY_LOADING_ESTIMATE -> settings.loadingEstimate ?: default
            Settings.KEY_AUTO_RESUME -> settings.autoResume ?: default
            Settings.KEY_STRICT -> settings.strictDynlib ?: default
            Settings.KEY_AUTOMATIC_CONTROLLER_MAPPING -> settings.automaticControllerMapping ?: default
            Settings.KEY_CONTROLLER_VIBRATION -> settings.controllerVibration ?: default
            Settings.KEY_DISK_SHADER_CACHE -> settings.diskShaderCache ?: default
            Settings.KEY_HOST_FEATURE_PROBE -> settings.hostFeatureProbe ?: default
            else -> default
        }

        private fun write(key: String, value: Boolean) {
            val knob = knob(key)
            if (knob != null) {
                settings.setFexKnob(knob.option, knob.value(value))
                return
            }
            writeNamed(key, value)
        }

        private fun writeNamed(key: String, value: Boolean) = when (key) {
            Settings.KEY_FULLSCREEN -> settings.fullscreen = value
            Settings.KEY_LOADING_ESTIMATE -> settings.loadingEstimate = value
            Settings.KEY_AUTO_RESUME -> settings.autoResume = value
            Settings.KEY_STRICT -> settings.strictDynlib = value
            Settings.KEY_AUTOMATIC_CONTROLLER_MAPPING -> settings.automaticControllerMapping = value
            Settings.KEY_CONTROLLER_VIBRATION -> settings.controllerVibration = value
            Settings.KEY_DISK_SHADER_CACHE -> settings.diskShaderCache = value
            Settings.KEY_HOST_FEATURE_PROBE -> settings.hostFeatureProbe = value
            else -> Unit
        }
    }

    /**
     * a fixed set of alternatives as cards, one of which may be chosen.
     *
     * **the cards are laid out by a grid whose column count is a resource**, so the same four sit
     * in a row on a wide screen and two by two upright, and this code never asks which it is in.
     *
     * **the chosen card is a wash of the accent over its own surface, outlined and lettered in the
     * accent.** the accent at full strength belongs to a switch track and not to a card ten times
     * its area, where the same colour stops reading as chosen and starts reading as a warning --
     * which is the rule Material itself follows in reserving its container roles for large
     * surfaces. this is that rule one step further, these cards being larger still.
     */
    private inner class CardsHolder(val binding: ItemSettingPresetBinding) :
        RecyclerView.ViewHolder(binding.root) {
        fun bind(row: SettingRow.Cards) {
            val cards = listOf(binding.rung0, binding.rung1, binding.rung2, binding.rung3)
            val labels = listOf(binding.label0, binding.label1, binding.label2, binding.label3)
            val chosen = row.chosen?.let { row.values.indexOf(it) } ?: -1

            val quiet = MaterialColors.getColor(binding.root, MaterialR.attr.colorOutlineVariant)
            val ink = MaterialColors.getColor(binding.root, MaterialR.attr.colorOnSurface)
            val accent = MaterialColors.getColor(binding.root, MaterialR.attr.colorPrimary)
            val plain = MaterialColors.getColor(binding.root, MaterialR.attr.colorSurfaceContainer)
            val fill = MaterialColors.layer(plain, accent, CARD_WASH)
            val density = binding.root.resources.displayMetrics.density

            cards.forEachIndexed { at, card ->
                val selected = at == chosen
                val present = at < row.entries.size
                card.isVisible = present
                if (!present) return@forEachIndexed
                card.strokeWidth = (1f * density).toInt()
                card.setStrokeColor(ColorStateList.valueOf(if (selected) accent else quiet))
                card.setCardBackgroundColor(if (selected) fill else plain)
                // **the state as the widget's own and not only as colours**, so that what is read
                // out matches what is drawn. the tick it would otherwise draw is off: the wash and
                // the outline already say it, and a glyph would push the label off centre.
                card.isCheckable = true
                card.checkedIcon = null
                card.isChecked = selected
                labels[at].text = row.entries[at]
                labels[at].setTextColor(if (selected) accent else ink)
                card.setOnClickListener {
                    write(row.key, row.values[at])
                    onChanged(row)
                }
            }
        }

        private fun write(key: String, value: String) = when (key) {
            Settings.KEY_FEX_PRESET -> settings.fexPreset = value
            else -> Unit
        }
    }

    private inner class ValueHolder(val binding: ItemSettingValueBinding) :
        RecyclerView.ViewHolder(binding.root) {
        fun bind(row: SettingRow.Dropdown) {
            binding.title.setText(row.title)

            val current = stored(row.key) ?: row.default
            val index = row.values.indexOf(current).coerceAtLeast(0)
            // the chosen entry, not the explanation. the explanation is what a row says when it has
            // nothing else to say, and a value the user picked is the thing they came back to check.
            binding.value.text = row.entries[index]
            binding.summary.setText(row.summary)

            binding.root.setOnClickListener {
                // **no Cancel button, which is Eden's shape for a single-choice list and is what
                // stops this one scrolling.** a tap on an entry both chooses and dismisses, so the
                // button was only ever a second way to do what the back gesture and a tap outside
                // already do -- and its row of padding was the difference between five themes fitting
                // and not. the dialogs that *commit* something, the colour picker and the two
                // confirmations, keep theirs.
                MaterialAlertDialogBuilder(binding.root.context)
                    .setTitle(row.title)
                    .setSingleChoiceItems(row.entries, index) { dialog, which ->
                        write(row.key, row.values[which])
                        dialog.dismiss()
                        onChanged(row)
                    }
                    .show()
            }
            binding.root.setOnLongClickListener { offerDefault(row.key, row) }
            useGlobal(binding.useGlobal, row.key, row)
        }

        private fun stored(key: String): String? = when (key) {
            Settings.KEY_THEME -> settings.theme
            Settings.KEY_RENDER_SCALE -> settings.renderScale
            else -> null
        }

        private fun write(key: String, value: String) = when (key) {
            Settings.KEY_THEME -> settings.theme = value
            Settings.KEY_RENDER_SCALE -> settings.renderScale = value
            else -> Unit
        }
    }

    /**
     * a switch the app cannot flip, over state the platform owns.
     *
     * the switch is not interactive -- the layout already makes it so, since the whole row is what
     * takes a tap -- and nothing here writes anything. it is redrawn from [SettingRow.External.checked]
     * each time the section is rebuilt, which is what returning from android's own settings screen
     * causes.
     */
    private inner class ExternalHolder(val binding: ItemSettingSwitchBinding) :
        RecyclerView.ViewHolder(binding.root) {
        fun bind(row: SettingRow.External) {
            binding.title.setText(row.title)
            binding.summary.setText(row.summary)
            binding.toggle.setOnCheckedChangeListener(null)
            binding.toggle.isChecked = row.checked
            binding.root.setOnClickListener { row.onClick() }
            // nothing stored means nothing to go back to, so the long press is not offered.
            binding.root.setOnLongClickListener(null)
            binding.root.isLongClickable = false
            // and nothing to override either. it shares the switch layout, so the button is there to
            // be hidden whether or not this row could ever have one.
            useGlobal(binding.useGlobal, null, row)
        }
    }

    private inner class ColourHolder(val binding: ItemSettingColourBinding) :
        RecyclerView.ViewHolder(binding.root) {
        fun bind(row: SettingRow.Colour) {
            binding.title.setText(row.title)
            binding.summary.setText(row.summary)
            binding.swatch.background = swatch(row.colour)
            binding.root.setOnClickListener { row.onClick() }
            binding.root.setOnLongClickListener { offerDefault(row.key, row) }
        }
    }

    /**
     * the button under a row that overrides the global value, and nothing at all otherwise.
     *
     * **set on both branches**, because a holder is recycled: a button left visible from the row this
     * view last drew would offer to clear an override the row in front of it does not have.
     *
     * a row with no key cannot be overridden -- an action, a header, a permission this app does not
     * own -- and neither can any row on the global list, where the store has nothing behind it.
     */
    private fun useGlobal(button: MaterialButton, key: String?, row: SettingRow) {
        // **asked of the store rather than worked out here**, because whether a row shadows the
        // level behind it is a question about the merge and one group of rows answers it
        // differently -- see Settings.overridesGlobal.
        val overridden = key != null && settings.overridesGlobal(key)
        if (overridden) {
            button.setOnClickListener {
                settings.clear(key!!)
                onChanged(row)
            }
        } else {
            button.setOnClickListener(null)
        }

        // whatever this view was doing for the row it last drew is no longer about this row.
        button.animate().cancel()
        val showing = button.visibility == View.VISIBLE

        // the second half of going away: the fade has run, and this pass is where the space it was
        // holding is given back so that the rows below slide up into it.
        if (collapsing) {
            button.visibility = View.GONE
            button.alpha = 1f
            return
        }
        if (!changing || overridden == showing) {
            // a first draw, a reused holder, or a row whose answer did not move: no transition to
            // describe, so it is simply in the state it belongs in.
            button.visibility = if (overridden) View.VISIBLE else View.GONE
            button.alpha = 1f
            return
        }

        // **the fade is the button's own rather than the row's, and that is the difference from
        // cross-fading the whole row.** a change notification with no payload would fade one copy of
        // the row out against another fading in, which does fade the button -- and takes the switch
        // beside it along, doubling a toggle that is already animating its own thumb.
        if (overridden) {
            // **it appears as the row grows.** the height is the layout's the moment this returns,
            // so the rows below start sliding now and the fade runs over the same stretch.
            button.alpha = 0f
            button.visibility = View.VISIBLE
            button.animate().alpha(1f).setDuration(FADE_MS).start()
        } else {
            // **going the other way the two cannot overlap**, because the row is only as short as
            // the button is absent -- so it fades here and is taken away in a pass of its own, which
            // is what lets the rows below slide up rather than jump. that is also the reason this is
            // quick rather than graceful: the fade is a delay in front of the movement.
            button.animate().alpha(0f).setDuration(FADE_MS).withEndAction {
                // **a cancelled fade ends too, and must not collapse anything.** the end action runs
                // either way, and a cancel is this view being bound to another row or overridden
                // again -- both of which leave it part way up, where a finished fade leaves it at
                // nothing.
                if (button.alpha == 0f) collapse(row)
            }.start()
        }
    }

    /**
     * the way back out of a choice.
     *
     * **offered only for a row that is actually set**, so a long press on an untouched row does
     * nothing rather than showing a dialog whose button would be a no-op -- which would tell the user
     * the row was set when it is not, and on the global list this gesture is the only place that
     * distinction is visible.
     *
     * **and not offered at all on a per-game list**, where [useGlobal] is drawn on the row itself and
     * does the same thing. the wording could not be shared either: on the global list the way back is
     * to the app's own default, and on a per-game one it is to whatever the global list currently
     * says, which is a different sentence about a different value.
     *
     * **a FEXCore knob on a game that names its own rung has neither, and that is a gap.** the button
     * is absent because there is no global value behind such a row -- see [Settings.overridesGlobal]
     * -- and this gesture is absent because the whole list is per-game, so the only way back is to
     * choose the rung again, which settles every row at once rather than the one asked about.
     */
    private fun offerDefault(key: String, row: SettingRow): Boolean {
        if (settings.perGame) return false
        if (!settings.answers(key)) return false
        // any bound holder's context is the activity; the list is what we have a handle on.
        val context = recycler?.context ?: return false
        // **the message is a view of ours rather than setMessage**, and only so that the space below
        // it is the button panel's alone. see dialog_message.xml: a one-line question left a band of
        // empty dialog under it taller than the question.
        val message = DialogMessageBinding.inflate(LayoutInflater.from(context))
        message.message.setText(R.string.setting_use_default_message)
        MaterialAlertDialogBuilder(context)
            .setTitle(R.string.setting_use_default_title)
            .setView(message.root)
            .setPositiveButton(R.string.setting_use_default) { _, _ ->
                settings.clear(key)
                onChanged(row)
            }
            .setNegativeButton(R.string.cancel, null)
            .show()
        return true
    }

    /**
     * the same way back for a row whose value is not a preference -- see [SettingRow.Screen.reset].
     *
     * **the same question in the same words**, because what it does is the same thing to a different
     * store: a binding or a motor put back is the row as it was before anybody chose, which for these
     * rows is nothing bound.
     */
    private fun offerReset(reset: () -> Unit, row: SettingRow): Boolean {
        val context = recycler?.context ?: return false
        val message = DialogMessageBinding.inflate(LayoutInflater.from(context))
        message.message.setText(R.string.setting_use_default_message)
        MaterialAlertDialogBuilder(context)
            .setTitle(R.string.setting_use_default_title)
            .setView(message.root)
            .setPositiveButton(R.string.setting_use_default) { _, _ ->
                reset()
                onChanged(row)
            }
            .setNegativeButton(R.string.cancel, null)
            .show()
        return true
    }

    private var recycler: RecyclerView? = null

    override fun onAttachedToRecyclerView(recyclerView: RecyclerView) {
        super.onAttachedToRecyclerView(recyclerView)
        recycler = recyclerView
    }

    override fun onDetachedFromRecyclerView(recyclerView: RecyclerView) {
        super.onDetachedFromRecyclerView(recyclerView)
        recycler = null
    }

    private companion object {
        /**
         * the circle a colour row draws, built rather than declared -- a drawable resource is
         * compiled and the colour here is a value, so it has to be a `GradientDrawable`.
         *
         * **no ring.** it had one, on the reasoning that a swatch close to the row's own colour would
         * disappear; the switch in the row below has no ring either, and matching it matters more.
         */
        fun swatch(colour: Int): GradientDrawable = GradientDrawable().apply {
            shape = GradientDrawable.OVAL
            setColor(colour)
        }

        /**
         * handed to `notifyItemChanged` so the holder is reused rather than cross-faded against a
         * second one. its value is never read -- that it exists at all is the whole message.
         */
        val CHANGED = Any()

        /**
         * handed to `notifyItemChanged` for the pass that removes a button which has finished fading.
         * it is distinguishable from [CHANGED] because that pass must not start another fade.
         */
        val COLLAPSE = Any()

        /**
         * how long the button takes to fade in or out.
         *
         * **shorter than the movement it accompanies**, which is the framework's own 250 ms for an
         * item change: the fade is what says the button arrived, and the slide is what says the list
         * made room for it, so a fade that outlasted the slide would still be finishing after
         * everything had settled. going away it is also the delay before the row collapses, which is
         * the other reason to keep it short.
         */
        const val FADE_MS = 120L

        /** what a disabled row is drawn at: Material's own opacity for disabled content. */
        const val DISABLED_ALPHA = 0.38f

        const val TYPE_HEADER = 0
        const val TYPE_SWITCH = 1
        const val TYPE_VALUE = 2
        const val TYPE_EXTERNAL = 3
        const val TYPE_COLOUR = 4
        const val TYPE_SCREEN = 5
        const val TYPE_CARDS = 6

        /**
         * how much of the accent is laid over a chosen card's own surface.
         *
         * **the accent at full strength is a switch track's colour and not a card's.** the same
         * value over ten times the area stops reading as a state and starts reading as an alarm,
         * which is why Material keeps its base roles for small controls and its container roles for
         * large ones. this is a shade under the lightest container that scheme would generate.
         */
        const val CARD_WASH = 0.14f
    }
}
