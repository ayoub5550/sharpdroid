package com.mircowuffwuff.sharpdroid

import android.content.Intent
import android.os.Bundle
import androidx.appcompat.app.AppCompatActivity
import androidx.recyclerview.widget.GridLayoutManager
import com.mircowuffwuff.sharpdroid.databinding.ActivitySettingsBinding

/**
 * the global settings scene: large section buttons, each with a line saying what is behind it, and
 * About in the toolbar's corner.
 *
 * the shape is Eden's settings menu -- buttons with a brief explanation, and a scrolling list of rows
 * behind each one.
 *
 * **About is a button and not a section, because nothing behind it is a setting.** it is the one
 * thing reachable from here that does not change what a launch does, and a card for it in a grid of
 * cards that do is a card read past. the corner is where the game list's cog is, one press earlier,
 * which is the position it was given deliberately rather than the position that was free.
 *
 * **only the sections that have something in them are here.** Controls and Logging are sections this
 * app will grow, and every row in both is a later piece of work; a button that opens an empty screen
 * is worse than a button that is not there yet, because the empty screen looks like a bug in the one
 * that is. a section arrives with its rows.
 */
class SettingsActivity : AppCompatActivity() {

    private lateinit var binding: ActivitySettingsBinding

    /** the theme this screen was drawn with, so a change made behind it is noticed on the way back. */
    private lateinit var drawnWith: String

    override fun onCreate(state: Bundle?) {
        // before setContentView, or the theme is resolved after the views are already inflated.
        Theme.apply(this)
        drawnWith = Theme.signature(this)
        super.onCreate(state)
        binding = ActivitySettingsBinding.inflate(layoutInflater)
        setContentView(binding.root)
        SystemBars.apply(this, binding.root)

        binding.toolbar.setNavigationOnClickListener { finish() }
        binding.about.setOnClickListener { startActivity(Intent(this, AboutActivity::class.java)) }
        // **one column upright, two on a wide screen** -- Eden's shape, and the reason is that a
        // section button is a title and one line, so a single column in landscape wastes two thirds
        // of the width. the count is a resource with a `-land` qualifier rather than a measurement:
        // android already resolves it, and an activity is recreated across a rotation, so it is
        // re-read without anything here watching for one.
        binding.sections.layoutManager =
            GridLayoutManager(this, resources.getInteger(R.integer.settings_section_columns))
        binding.sections.adapter = SectionAdapter(Section.shown, perGame = false) { section ->
            // **a section is usually a list of rows and does not have to be.** User data is a manager
            // screen, the shape the build, driver and folder managers already use, so its card opens
            // that directly rather than a list holding one row that opens it.
            val screen = section.screen
            startActivity(
                if (screen == null) {
                    Intent(this, SettingsSectionActivity::class.java)
                        .putExtra(SettingsSectionActivity.EXTRA_SECTION, section.name)
                } else {
                    Intent(this, screen)
                }
            )
        }
    }

    /**
     * **the theme is changed on the screen this one opens**, so coming back has to notice. without
     * it, backing out of a theme change lands here on the old palette, and only leaving the settings
     * scene entirely and re-entering produces the new one.
     */
    override fun onResume() {
        super.onResume()
        if (Theme.recreateIfStale(this, drawnWith)) return
        // the fullscreen toggle is one screen further in, and this screen is what it comes back to.
        SystemBars.apply(this, binding.root)
    }

    /**
     * a section, its button's two lines, and what it is called on the way to the next screen.
     *
     * the label and the summary are string resources, so what appears on screen is the app's own
     * cased text rather than the enum's name.
     */
    enum class Section(
        val title: Int,
        val summary: Int,
        val icon: Int,
        /** the screen this card opens, or null for the ordinary list of rows. */
        val screen: Class<out AppCompatActivity>? = null,
        /**
         * the screen this card opens on a game's scene, where that is a different screen.
         *
         * **null means the same one, which is the ordinary case and the whole design**: a section of
         * rows is served by [SettingsSectionActivity] told which store to write, so a row added to
         * Graphics is offered per game the day it is written. this field is for the section that
         * cannot be -- User data, where what the two screens *mean* differs rather than only what they
         * act on. see [GameUserDataActivity].
         */
        val perGameScreen: Class<out AppCompatActivity>? = null,
        /**
         * the line under this card's title on a game's scene, where it differs.
         *
         * **null means the same line, which is most of them**: "rendering and presentation" describes
         * Graphics whoever is being configured. it is User data that cannot share one, its app-wide
         * summary claiming an install that a game's screen is deliberately not about.
         */
        val perGameSummary: Int? = null,
    ) {
        APP(R.string.settings_app, R.string.settings_app_summary, R.drawable.ic_section_app),
        EMULATION(
            R.string.settings_emulation,
            R.string.settings_emulation_summary,
            R.drawable.ic_section_emulation,
        ),
        GRAPHICS(
            R.string.settings_graphics,
            R.string.settings_graphics_summary,
            R.drawable.ic_section_graphics,
        ),
        CONTROLS(
            R.string.settings_controls,
            R.string.settings_controls_summary,
            R.drawable.ic_section_controls,
        ),
        GAME_FILES(
            R.string.settings_game_files,
            R.string.settings_game_files_summary,
            R.drawable.ic_section_game_files,
        ),
        USER_DATA(
            R.string.settings_user_data,
            R.string.settings_user_data_summary,
            R.drawable.ic_section_user_data,
            UserDataActivity::class.java,
            GameUserDataActivity::class.java,
            R.string.settings_user_data_game_summary,
        ),

        /**
         * the JIT knobs, behind a row in [EMULATION] rather than behind a card of its own.
         *
         * **it is a section that no grid draws, and it must stay one.** the settings grid was
         * already a card too tall at seven, which is what moved About out of it; a screen holding
         * ten rows about one subject belongs under the row naming that subject, where somebody
         * looking for it is already standing.
         *
         * so [icon] and [summary] are here because every constant carries them rather than because
         * anything reads them -- this section appears in neither [shown] nor [perGame]. the icon is
         * the section it is reached from, which is the answer that stays right if a grid ever does
         * draw it.
         */
        JIT_ACCURACY(
            R.string.setting_jit_accuracy,
            R.string.setting_jit_accuracy_summary,
            R.drawable.ic_section_emulation,
        ),

        /**
         * one controller port's bindings, behind its row in [CONTROLS], for the reason
         * [JIT_ACCURACY] is behind its row: 27 rows about one port are a page somebody opens to map
         * that port. which port is [SettingsSectionActivity.EXTRA_PORT], and the toolbar names it.
         *
         * never per game, and drawn by no grid: the mapping is the install's, like the switch above
         * the port rows.
         */
        CONTROLLER_PORT(
            R.string.settings_group_controller_ports,
            R.string.settings_controls_summary,
            R.drawable.ic_section_controls,
        );

        companion object {
            /**
             * the sections that have rows, in the order the grid draws them.
             *
             * **the install's two lead, and [perGame]'s four follow in [perGame]'s own order.** App
             * and Game files are the pair that has nothing to say about one game, so putting them
             * together makes the first row of this grid exactly what a game's scene does not carry --
             * and leaves everything under it as the block the two screens share, arranged the same
             * way on both. somebody who has learned where Controls is on one screen has learned it
             * on the other.
             *
             * **what transfers is the arrangement rather than the position, and that is the whole of
             * what could.** this grid is as wide as the panel and a game's is two columns inside the
             * right two thirds, so no card can land on the same pixel on both; the shape of the
             * block is what an eye carries between them. that is also why the install's pair leads
             * rather than trails: putting it last would line the shared block up by row without
             * lining anything up by width, and would spend the first slot in the app's own settings
             * on neither of the sections the app is mostly opened for.
             *
             * **not called `entries`**, which is kotlin's own name for every constant of an enum
             * since 1.9 -- a member shadowing it compiles with a deprecation warning and then means
             * something different from what it reads as.
             */
            val shown =
                listOf(APP, GAME_FILES, EMULATION, GRAPHICS, CONTROLS, USER_DATA)

            /**
             * the sections a single game answers for, in the order [GameSettingsActivity] draws them.
             *
             * **App and Game files are absent because they belong to the install rather than to a
             * title** -- a theme and a folder grant are set once and apply to everything. what is left
             * is the three that describe how one game is run, and User data, which is what one game
             * has left behind.
             *
             * **User data is last, and it is the one card here that is not a setting.** nothing
             * behind it changes what a launch does -- which is the argument that keeps About out of a
             * card altogether, applied to a card that has a reason to stay: what one game has written
             * is a fact about that game, and this scene is the only place it is asked about.
             */
            val perGame = listOf(EMULATION, GRAPHICS, CONTROLS, USER_DATA)
        }
    }
}
