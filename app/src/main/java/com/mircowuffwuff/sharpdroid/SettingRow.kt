package com.mircowuffwuff.sharpdroid

import android.text.SpannableString
import android.text.Spanned

/**
 * one row in a settings section.
 *
 * **typed rows in a list rather than a `PreferenceScreen`, which is the yuzu lineage's shape** -- see
 * Eden's `SettingsAdapter` and the item classes beside it. it is why their settings screens look the
 * way they do, and following it is the whole reason this app took the AndroidX dependency graph:
 * being able to read one of Eden's screens and carry the pattern across is worth more than being
 * able to build offline.
 *
 * **a row knows its key, and the key is what makes "unset" reachable.** [Settings.isSet] answers
 * whether the user has ever touched this row, which decides whether a long press offers *Use
 * default* at all. a row with no key -- an action, a header, a permission this app does not own -- is
 * never in either state, unless it stores outside the preferences and carries a [Screen.reset] for
 * its way back.
 *
 * **nothing on screen distinguishes a set row from an untouched one**, and that is a trade rather
 * than an omission: a mark saying so has to hold its width on every row, which indents the whole
 * list to annotate one of them. the distinction is real -- only a set row reaches a launch -- and the
 * cost of not drawing it is that the long press is undiscoverable. the per-game scene needs a
 * visible affordance for exactly this, and that is the place to reconsider it.
 */
sealed class SettingRow {

    /** the key this row reads and writes, or null for a row that is not a stored value. */
    open val key: String? = null

    /**
     * whether this row is drawn when its section is opened for one game.
     *
     * **a row left off the per-game screen is the app's rather than a title's**, so whatever reads
     * it reads the app's own store: a setting no game can override has nothing to fall back from.
     * true for every row that does not say otherwise, which is what lets a section be shared by the
     * two screens with nothing else telling them apart.
     */
    open val perGame: Boolean = true

    /**
     * whether this row takes a tap, or is drawn greyed out and ignores one.
     *
     * **a row is disabled by another row's value**, and the controller port rows are the case: while
     * Automatic controller mapping is on, every controller plays on port 1 whatever a port says, so a
     * port row that opened would be a screen of choices that change nothing. greyed rather than left
     * out, because the mapping stays stored and comes back the moment the switch is turned off -- a row
     * that vanished would read as the mapping being gone.
     */
    open val enabled: Boolean = true

    /**
     * what the adapter finds this row by across rebuilds of its list. the key, for a row that stores a
     * preference; a row that stores something else names its own, so a write to it still redraws the
     * one row rather than the whole list -- see [SettingsAdapter.submit].
     */
    open val id: String? get() = key

    /**
     * a divider with a label, for a subsection inside a section.
     *
     * **a label above a run of rows, never another button press.** a subsection is a grouping and
     * not a destination, so hiding one behind a tap adds a screen without adding a choice.
     *
     * **a page of its own is a different thing and this rule does not forbid it.** what it forbids
     * is a run of two or three rows put behind a press for tidiness; a page long enough to want
     * headers of its own, reached from a [Screen] row that reads out what is chosen inside it, is a
     * destination that answers a question rather than a grouping that hides one.
     *
     * [perGame] false for a label whose every row is left off a game's screen, which would otherwise
     * be drawn there over nothing.
     *
     * **it is greyed while every row under it is**, which the adapter reads off the list rather than
     * being told -- see [SettingsAdapter]. a label over a run of rows nobody can tap names a choice
     * nobody can make.
     */
    data class Header(val title: Int, override val perGame: Boolean = true) : SettingRow()

    /** a boolean, drawn as a Material switch. */
    data class Switch(
        override val key: String,
        val title: Int,
        /** a line explaining the row, or null for a row whose title already says what it is. */
        val summary: Int?,
        val default: Boolean,
        override val perGame: Boolean = true,
    ) : SettingRow()

    /**
     * a choice from a fixed list, drawn as a row that opens a single-choice dialog.
     *
     * [values] is what is stored and [entries] is what is shown, one to one. they are separate
     * because what the payload parses -- `0.75` -- is not what a person should have to read.
     */
    data class Dropdown(
        override val key: String,
        val title: Int,
        val summary: Int,
        val entries: Array<String>,
        val values: Array<String>,
        val default: String,
    ) : SettingRow() {
        // an Array in a data class gives identity equals/hashCode, which would make two rows built
        // from the same arrays unequal. nothing here compares rows, and saying so is cheaper than an
        // override nothing calls.
        override fun equals(other: Any?) = this === other
        override fun hashCode() = System.identityHashCode(this)
    }

    /**
     * a fixed set of named alternatives, drawn as cards laid out across the row and picked by
     * tapping one.
     *
     * **it is not a [Dropdown] and the difference is that the value may be none of them.** a
     * dropdown answers with the entry that is chosen; this answers with the entry that is chosen
     * *or with nothing at all*, which is the state a configuration is in once something below it
     * has been changed away from what the choice here implies. a control with a position for every
     * alternative and none for that state cannot say it.
     *
     * **[chosen] is therefore nullable and is the whole of that.** null draws every card
     * unselected, which reads as a control whose value is not on the list rather than as one
     * nothing has been done to yet -- the rows underneath say what the configuration actually is.
     *
     * **it carries no title and no summary.** it is the first thing on the screen it belongs to,
     * under a toolbar that already names it, and a label repeating that name would be the screen's
     * title written twice.
     *
     * **and it carries neither way back.** the row that opens that screen is where both live, since
     * what they put back is the whole configuration rather than this one choice -- see
     * [Settings.answers].
     */
    data class Cards(
        override val key: String,
        val entries: Array<String>,
        val values: Array<String>,
        /** the value whose card is drawn as chosen, or null where none of them is what is set. */
        val chosen: String?,
    ) : SettingRow() {
        // as Dropdown: arrays in a data class give identity equals, and nothing compares rows.
        override fun equals(other: Any?) = this === other
        override fun hashCode() = System.identityHashCode(this)
    }

    /**
     * a row that opens a screen or a dialog of its own, showing what is currently chosen underneath it.
     *
     * **[value] is text rather than a resource**, which is the difference between this and
     * [Dropdown]: what it shows is the name of something on the device -- a build, a controller --
     * rather than one of a fixed set of labels this app shipped. a part of it may be marked [Absent],
     * for a name whose thing is not there: a build or a driver that is no longer on the device, a
     * controller that is not connected, a motor that is not available.
     *
     * **[key] is null for a row that stores no preference.** the build and driver rows each name a
     * stored choice, and the long press puts it back; the folder manager is a place to go rather than
     * a value that was picked, so there is no default for it to go back to and no gesture on it. the
     * controller mapping's rows store into a file of their own rather than a preference, so they carry
     * an [id] instead and their way back is [reset].
     */
    data class Screen(
        override val key: String?,
        val title: Int,
        /** a line explaining the row, or null for a row whose title already says what it is. */
        val summary: Int?,
        val value: CharSequence,
        /**
         * whether [value] names something, or reports that there is nothing.
         *
         * the value line is drawn in the accent, which is what marks it as the answer to the row.
         * "None" is not an answer of that kind -- it is the absence of one -- so it is drawn in the
         * body colour instead, and reads as a state rather than as a choice somebody made. for a row
         * with a [reset], it is also whether the long press has anything to put back.
         */
        val chosen: Boolean = true,
        /** filled into [title] for a title with a number in it -- the four controller ports. */
        val titleArg: Int? = null,
        /**
         * a glyph drawn before the row's text, or null for none. the four controller ports carry one,
         * as Eden draws a controller beside each of its players.
         */
        val icon: Int? = null,
        /**
         * whether [icon] is drawn in the accent rather than the body colour: the split [chosen] makes
         * for [value], between an answer and the absence of one.
         */
        val iconAccented: Boolean = false,
        override val enabled: Boolean = true,
        override val perGame: Boolean = true,
        override val id: String? = key,
        /**
         * the long press's way back, for a row whose value is not a preference. it is offered with the
         * same Use default question a stored row's long press asks, and only while [chosen].
         */
        val reset: (() -> Unit)? = null,
        val onClick: () -> Unit,
    ) : SettingRow()

    /**
     * a colour, drawn as a swatch and picked in a dialog.
     *
     * **it appears under the Theme row only while a Custom theme is chosen**, which is why it is a
     * row type rather than a section of its own: a colour with no scheme to seed would be a control
     * that does nothing, and a scheme with no colour would be a theme nobody can change.
     */
    data class Colour(
        override val key: String,
        val title: Int,
        val summary: Int,
        val colour: Int,
        val onClick: () -> Unit,
    ) : SettingRow()

    /**
     * a switch showing state this app does not own -- today, the all-files permission.
     *
     * **it shows rather than sets, and the difference is the platform's rather than a design
     * choice.** `MANAGE_EXTERNAL_STORAGE` is granted in android's own settings and nowhere else, so
     * the switch cannot be flipped from here: tapping the row opens that screen, and the state is
     * read back when the user comes back. it is drawn as a switch because a switch is what the thing
     * *is*, and a row whose description had to begin with "Off." was saying in a sentence what a
     * widget says at a glance.
     *
     * it carries no key. there is nothing stored, so there is no default to go back to and no long
     * press.
     */
    data class External(
        val title: Int,
        val summary: Int,
        val checked: Boolean,
        val onClick: () -> Unit,
    ) : SettingRow()

    /**
     * a span marking part of a [Screen.value] as naming something that is not there. the adapter
     * draws what it covers in the body colour, where the rest of the line takes the accent.
     *
     * **a mark rather than a colour**, because a row's colours are the adapter's: [Screen.chosen] and
     * [Screen.iconAccented] are answered there too, from the theme the list is drawn in. a row says
     * what is absent and never which colour that is.
     */
    class Absent

    companion object {
        /** [text] marked [Absent] from end to end. */
        fun absent(text: CharSequence): CharSequence = SpannableString(text).apply {
            setSpan(Absent(), 0, length, Spanned.SPAN_EXCLUSIVE_EXCLUSIVE)
        }
    }

}
