package com.mircowuffwuff.sharpdroid

import android.content.Intent
import android.graphics.drawable.GradientDrawable
import android.os.Bundle
import android.view.LayoutInflater
import android.widget.Toast
import androidx.appcompat.app.AppCompatActivity
import androidx.recyclerview.widget.LinearLayoutManager
import com.google.android.material.dialog.MaterialAlertDialogBuilder
import com.mircowuffwuff.sharpdroid.databinding.ActivitySettingsSectionBinding
import com.mircowuffwuff.sharpdroid.databinding.DialogColourPickerBinding
import java.io.File

/**
 * one section's rows.
 *
 * one activity for every section rather than one per section, because the rows differ and nothing
 * else does -- the toolbar, the list, the store and the "Use default" long press are the same screen
 * four times over.
 *
 * **and one activity for the global scene and for a game's own**, which is the stronger half of the
 * same argument: named a game, this screen writes that game's store instead of the app's and draws
 * the *Use global value* button on any row overriding it, and nothing else about it moves. so a row
 * added to Emulation, Graphics or Controls appears in both the day it is written, rather than the day
 * somebody remembers a second copy exists.
 *
 * **a subsection is a label above a run of rows, not another button press.** Eden's *Advanced
 * settings → Graphics* is the shape.
 *
 * **a section reached from a row rather than from the grid is not an exception to that**, and
 * [SettingsActivity.Section.JIT_ACCURACY] is the one: what the rule forbids is a run of two or
 * three rows put behind a press, and what that row opens is a page of ten with subsections of its
 * own, reading out what is chosen inside it the way the build and driver rows read out theirs.
 */
class SettingsSectionActivity : AppCompatActivity() {

    private lateinit var binding: ActivitySettingsSectionBinding
    private lateinit var settings: Settings
    private lateinit var adapter: SettingsAdapter
    private lateinit var section: SettingsActivity.Section
    private lateinit var drawnWith: String

    /**
     * the game this screen answers for, as [Game.configKey] names it, or null for the app's own.
     *
     * it is carried rather than looked up: the scene that opened this screen already read the dump,
     * and reading it again here would be a second answer to a question with one right answer.
     */
    private var game: String? = null

    /**
     * whether a Custom theme was chosen when this screen was built, and therefore whether the seed
     * colour row was in the list.
     *
     * **the store cannot answer this after the fact**: choosing a theme writes it and then restarts
     * the screen, so by the time anything asks, the new choice is the only one recorded. this is read
     * once, on the way in, while it is still true.
     */
    private var builtWithColourRow = false

    /**
     * what the seed colour row is doing on this screen's first layout, carried across the restart a
     * theme change causes.
     *
     * **the restart is why this has to be carried at all.** a palette is resolved when views are
     * inflated, so a theme cannot be swapped underneath a screen that is already drawn and the
     * activity is rebuilt instead -- and the instance that comes back has no idea a theme just
     * changed, let alone whether the row it is about to draw was on the screen a moment ago. one
     * value in the saved state is the whole of what it needs.
     */
    private var colourRowArrival = COLOUR_ROW_STILL

    override fun onCreate(state: Bundle?) {
        Theme.apply(this)
        drawnWith = Theme.signature(this)
        super.onCreate(state)
        game = intent.getStringExtra(EXTRA_GAME)?.takeIf { it.isNotEmpty() }
        settings = game?.let { Settings.forGame(this, it) } ?: Settings.of(this)
        section = runCatching {
            SettingsActivity.Section.valueOf(intent.getStringExtra(EXTRA_SECTION).orEmpty())
        }.getOrElse {
            // nothing but a hand-written intent reaches this, and finishing is a better answer than
            // an empty screen that looks like a section with no settings in it.
            finish()
            return
        }

        binding = ActivitySettingsSectionBinding.inflate(layoutInflater)
        setContentView(binding.root)
        SystemBars.apply(this, binding.root)
        binding.toolbar.setTitle(section.title)
        binding.toolbar.setNavigationOnClickListener { finish() }

        builtWithColourRow = Theme.chosen(this) == Settings.THEME_CUSTOM
        colourRowArrival = state?.getInt(STATE_COLOUR_ROW) ?: COLOUR_ROW_STILL

        // **the list is built as it was a moment ago, not as it is**, when a theme change is what
        // brought this screen back: the row that is arriving is left out so there is something for
        // it to arrive into, and the row that is leaving is drawn one more time so there is
        // something to take away. a screen that opened any other way builds the list it means.
        val arriving = colourRowArrival == COLOUR_ROW_ARRIVING
        val before =
            if (colourRowArrival == COLOUR_ROW_STILL) rows() else rows(colourRow = !arriving)
        adapter = SettingsAdapter(
            settings,
            before,
            onChanged = this::changed,
        )
        binding.settings.layoutManager = LinearLayoutManager(this)
        binding.settings.adapter = adapter

        // **one layout later, because a row cannot animate into a list that has not been drawn
        // yet.** the adapter is set during onCreate and nothing is measured until after it returns,
        // so an insert asked for here would be part of the first layout rather than a change to it,
        // and would simply appear -- which is the thing being fixed.
        if (colourRowArrival != COLOUR_ROW_STILL) {
            val at = rows(colourRow = true).indexOfFirst { it.key == Settings.KEY_CUSTOM_COLOUR }
            binding.settings.post {
                // **the flag is cleared here rather than above, because [onResume] reads it.** it
                // runs between this method and this runnable, and its own redraw would settle the
                // list into its final shape before the row had anywhere to move from.
                colourRowArrival = COLOUR_ROW_STILL
                if (at >= 0) adapter.replaceRow(rows(), at, arriving) else adapter.submit(rows())
            }
        }
    }

    /**
     * **the one value that has to outlive the restart a theme change causes**, and it is written
     * here rather than when the theme is chosen because that is when the restart takes it.
     */
    override fun onSaveInstanceState(out: Bundle) {
        super.onSaveInstanceState(out)
        out.putInt(STATE_COLOUR_ROW, colourRowArrival)
    }

    /**
     * redraws after a write.
     *
     * **the theme is the one row that changes the screen it is on**, so it restarts the activity
     * rather than redrawing a list -- a palette applied at inflation cannot be swapped underneath
     * views that have already been inflated with the old one.
     */
    private fun changed(row: SettingRow) {
        if (row.key == Settings.KEY_THEME || row.key == Settings.KEY_CUSTOM_COLOUR) {
            // **the seed colour row comes and goes with the Custom theme, and the screen it is on is
            // about to be rebuilt** -- so what it is doing is worked out here, while both answers are
            // still knowable, and handed to the instance that comes back. a seed colour that changed
            // leaves the row exactly where it was, which is the common case and moves nothing.
            val nowCustom = Theme.chosen(this) == Settings.THEME_CUSTOM
            colourRowArrival = when {
                nowCustom && !builtWithColourRow -> COLOUR_ROW_ARRIVING
                !nowCustom && builtWithColourRow -> COLOUR_ROW_LEAVING
                else -> COLOUR_ROW_STILL
            }
            Theme.reapply(this)
            return
        }
        // the bars, if that is what moved. it takes effect on the screen the toggle is on and needs
        // no restart, because the behaviour is read inside the inset listener rather than baked in
        // when the screen was built.
        if (row.key == Settings.KEY_FULLSCREEN) {
            SystemBars.apply(this, binding.root)
        }
        // **the preset is the one write here that changes every row under it**, because a knob
        // row's default is the rung's own value and choosing a rung also drops every override on
        // it. so the narrow notification below would leave nine switches drawn against the rung
        // before this one.
        //
        // **it redraws every row and says so, rather than saying nothing about what moved.** the
        // same key names the row that opens this screen from the section above, where the only
        // thing that moves is the button under it -- and a redraw that assumes nothing takes that
        // button away in one frame instead of fading it.
        if (row.key == Settings.KEY_FEX_PRESET) {
            adapter.refresh(rows())
            return
        }
        // **the row that changed is named, so the rows under it slide rather than jump.** on a
        // per-game list a write can add or take away the Use global value button, which changes how
        // tall that row is; telling the adapter only that something changed is what makes the rest
        // of the list snap into its new place.
        adapter.submit(rows(), row)
        // **and the ladder reads out what the rows under it say**, so overriding one of them is a
        // second row to redraw. it is named rather than folded into the call above because it did
        // not move: the list is the same length and the same shape, and a full redraw would throw
        // away the movement that call exists to produce.
        if (section == SettingsActivity.Section.JIT_ACCURACY) {
            adapter.rebind(Settings.KEY_FEX_PRESET)
        }
    }

    override fun onResume() {
        super.onResume()
        if (!::adapter.isInitialized) return
        if (Theme.recreateIfStale(this, drawnWith)) return
        // the bars may have been toggled on another section, and the all-files switch is changed on
        // the platform's own screen -- both come back through here.
        SystemBars.apply(this, binding.root)
        // **a row that is part way in or out is holding the list one row away from what the store
        // says, and it is doing that deliberately.** this method runs before the pass that moves it,
        // so redrawing here would put the list straight into its final shape and leave the animation
        // describing a change that had already happened -- which RecyclerView reports as an
        // inconsistency rather than ignoring.
        if (colourRowArrival == COLOUR_ROW_STILL) adapter.refresh(rows())
    }

    // ----------------------------------------------------------------------------------------------
    // the rows

    /**
     * the rows, for this section and for whichever store this screen was opened against.
     *
     * **two sections have nothing to say about one game and answer with nothing when named one.** App
     * is the look and behaviour of the app itself, and Game files is which folders it may read -- both
     * belong to the install rather than to a title, so a per-game copy would be a screen offering to
     * set something that could only ever be set once. nothing in the app opens either that way; a
     * hand-written intent still can, and an empty list is the same answer the section guard in
     * [onCreate] gives a name that is not a section at all.
     *
     * **a section a game does have can still hold a row that is the app's**, and that row says so
     * itself through [SettingRow.perGame]: a game's screen draws the section without it, so one
     * section serves both screens and nothing else here tells them apart.
     *
     * [colourRow] is whether the seed colour row is wanted, and defaults to what the stored theme
     * says -- which is the right answer everywhere except the one layout a theme change is being
     * animated across, where the list has to be built as it stood before the change so that the
     * difference is something a viewer can watch happen.
     */
    private fun rows(
        colourRow: Boolean = Theme.chosen(this) == Settings.THEME_CUSTOM,
    ): List<SettingRow> {
        val all = when (section) {
            SettingsActivity.Section.APP -> if (game == null) appRows(colourRow) else emptyList()
            SettingsActivity.Section.EMULATION -> emulationRows()
            SettingsActivity.Section.JIT_ACCURACY -> jitAccuracyRows()
            SettingsActivity.Section.GRAPHICS -> graphicsRows()
            SettingsActivity.Section.CONTROLS -> controlsRows()
            SettingsActivity.Section.GAME_FILES -> if (game == null) gameFilesRows() else emptyList()
            // User data is a screen of its own, so its card never opens this activity. a
            // hand-written intent still can, and an empty list is what it gets - the same answer the
            // section guard in onCreate gives a name that is not a section at all.
            SettingsActivity.Section.USER_DATA -> emptyList()
        }
        return if (game == null) all else all.filter { it.perGame }
    }

    private fun appRows(colourRow: Boolean): List<SettingRow> {
        val rows = mutableListOf<SettingRow>()
        // **Material You and Custom are dropped from the list where dynamic colour does not exist**,
        // rather than shown and refused. both are the same generator - one seeded by the wallpaper
        // and one by a colour - so both need API 31, and minSdk is 28. this is the version guard
        // that keeping the floor where it is implies.
        val entries = resources.getStringArray(R.array.theme_entries)
        val dynamic = setOf(Settings.THEME_MATERIAL_YOU, Settings.THEME_CUSTOM)
        val keep = Settings.THEMES.indices.filter {
            Settings.THEMES[it] !in dynamic || Theme.dynamicColourAvailable()
        }
        rows += SettingRow.Dropdown(
            key = Settings.KEY_THEME,
            title = R.string.setting_theme,
            summary = R.string.setting_theme_summary,
            entries = keep.map { entries[it] }.toTypedArray(),
            values = keep.map { Settings.THEMES[it] }.toTypedArray(),
            default = Settings.THEME_DEFAULT,
        )
        // **only while a Custom theme is chosen.** a seed colour with no scheme to generate would be
        // a control that changes nothing, and it sits directly under the row that put it there so
        // the two read as one choice rather than as two.
        if (colourRow) {
            rows += SettingRow.Colour(
                key = Settings.KEY_CUSTOM_COLOUR,
                title = R.string.setting_custom_colour,
                summary = R.string.setting_custom_colour_summary,
                // **the accent rather than the seed, and taken from the theme rather than computed.**
                // this screen is already wearing the scheme the seed generated, so its colorPrimary
                // *is* the accent -- no generator call, no chance of the row and the theme disagreeing.
                // it needs no live updating either: a colour change restarts the screen.
                colour = themeColour(com.google.android.material.R.attr.colorPrimary),
            ) { pickColour() }
        }
        rows += SettingRow.Switch(
            key = Settings.KEY_FULLSCREEN,
            title = R.string.setting_fullscreen,
            summary = R.string.setting_fullscreen_summary,
            default = false,
        )
        // **on by default, which is what makes this the only App row whose default is not the empty
        // one.** an untouched row and an empty store both leave the estimate on, so the behaviour a
        // fresh install has is the behaviour this row describes.
        rows += SettingRow.Switch(
            key = Settings.KEY_LOADING_ESTIMATE,
            title = R.string.setting_loading_estimate,
            summary = R.string.setting_loading_estimate_summary,
            default = true,
        )
        rows += SettingRow.Switch(
            key = Settings.KEY_AUTO_RESUME,
            title = R.string.setting_auto_resume,
            summary = R.string.setting_auto_resume_summary,
            default = false,
        )
        return rows
    }

    /**
     * a manager screen, told which store it is choosing for.
     *
     * **the extras are forwarded rather than rebuilt**, so a build or a driver picked from a per-game
     * section is written to that game's store. without this the manager would write the app's row
     * while the row that opened it showed a game's -- a screen saying one thing while a launch does
     * another, which is the failure the whole precedence design exists to avoid.
     */
    private fun manager(screen: Class<out AppCompatActivity>): Intent =
        Intent(this, screen).putExtra(EXTRA_GAME, game)

    /**
     * another section of rows, told the same store this one is writing.
     *
     * **it is this activity again**, which is what makes a section reached from a row cost nothing
     * over one reached from the grid: the toolbar, the list, the long press and the per-game
     * behaviour are already here, and the section decides only which rows are built.
     */
    private fun section(to: SettingsActivity.Section): Intent =
        Intent(this, SettingsSectionActivity::class.java)
            .putExtra(EXTRA_SECTION, to.name)
            .putExtra(EXTRA_GAME, game)

    /** a colour out of the theme this screen was inflated with. */
    private fun themeColour(attr: Int): Int {
        val typed = android.util.TypedValue()
        theme.resolveAttribute(attr, typed, true)
        return typed.data
    }

    /**
     * the Emulation section.
     *
     * **the build row has no toggle above it.** exactly one build ships per APK, so there is no
     * recommendation to follow and nothing for a switch to govern -- the row simply names what a
     * launch will run, and tapping it opens the manager. a toggle there would need a constant, a
     * stored key and a badge behind it, and every one of those is a way for the screen to say one
     * thing while the launch does another.
     */
    private fun emulationRows(): List<SettingRow> = listOf(
        // **no strict dynlib resolution row.** the payload parses `--strict` and carries it as far as
        // the options the dispatcher is handed, and nothing reads it from there -- so a switch here
        // would promise that a launch fails on an unresolved import when a launch does no such thing.
        // it is also a diagnostic rather than a setting: what it offers a user is a game that refuses
        // to start. the flag stays reachable, and `--ez strict true` on a launch still passes it,
        // because the merge reads the intent before it reads the store.
        SettingRow.Header(R.string.settings_group_sharpemu),
        SettingRow.Screen(
            key = Settings.KEY_BUILD,
            title = R.string.setting_build,
            summary = R.string.setting_build_summary,
            value = chosenBuildLabel(),
        ) {
            startActivity(manager(BuildsActivity::class.java))
        },
        // no FEXCore version row beside this one. exactly one FEXCore is linked into the host layer
        // and there is nothing for a choice to select between, so a dropdown with one entry would
        // offer a capability the app does not have.
        SettingRow.Header(R.string.settings_group_fexcore),
        // **the ladder and the knobs it sets are one screen, and this row is the way in.** ten rows
        // about how guest code is translated would be most of this section and would bury the build
        // above them; behind one row that reads out what is chosen, they are a page somebody opens
        // when that is the question they came with.
        //
        // **both ways back live here rather than on the screen behind it**, because what either of
        // them puts back is the whole configuration: a rung and every knob overriding it, which is
        // the one thing this row reads out. the key it carries is the rung's, and the store reads
        // that key as standing for all of it -- see Settings.answers, and Settings.clear, which
        // settles the same set.
        SettingRow.Screen(
            key = Settings.KEY_FEX_PRESET,
            title = R.string.setting_jit_accuracy,
            summary = R.string.setting_jit_accuracy_summary,
            value = jitAccuracyLabel(),
        ) {
            startActivity(section(SettingsActivity.Section.JIT_ACCURACY))
        },
        // **below the ladder rather than a rung on it.** every rung above the middle spends
        // faithfulness for speed, and describing the host truthfully spends nothing -- so a rung that
        // turned this off would be one that ran slower and translated no more faithfully. it is a
        // switch for the case the ladder cannot express: a device this probe reads wrongly.
        SettingRow.Switch(
            key = Settings.KEY_HOST_FEATURE_PROBE,
            title = R.string.setting_host_feature_probe,
            summary = R.string.setting_host_feature_probe_summary,
            default = true,
        ),
    )

    /**
     * what the build row shows underneath itself.
     *
     * **it always names a concrete build**, which is what shipping exactly one buys: nothing stored
     * means the bundled build, so the row reads its name rather than describing a rule. a subtitle
     * along the lines of "none chosen, the most recent is used" would explain the launcher instead
     * of answering the question the row is there to answer.
     *
     * **the name alone, without the SharpEmu version beside it.** every row on this screen answers
     * with one thing, and a version is a second: it is what tells two builds of one name apart, which
     * is a question the manager behind this row exists for and this row cannot answer anyway -- it has
     * room for one line and there may be several.
     *
     * **a build that is not there says so rather than showing its name.** a folder can go --
     * deleted from a PC, or the external volume wiped -- and a row that went on naming it would leave
     * the game failing to start as the only place a user could find out.
     *
     * it reads a `meta.json` on the main thread, which is one small file: the alternative is a row
     * that draws empty and fills in a frame later, on a screen that is otherwise synchronous.
     */
    private fun chosenBuildLabel(): String {
        val internalRoot = AppStorage.installedBuilds(filesDir)
        val staged = AppStorage.stagedBuilds(getExternalFilesDir(null)!!)
        // **the bundled build is named from the APK's own asset**, which is what lets this row be
        // right on a fresh install: nothing extracts it until a game is launched with it selected,
        // so the directory it will live in does not exist yet, and reading the disk would report the
        // one build that is certainly there as missing.
        val bundled = BundledBuild.identity(this, internalRoot)
        val folder = settings.build
            ?: bundled?.folder
            // a debug app bundles none, so it falls back to what a launch would run: the most
            // recently staged build. naming it beats describing the rule that found it.
            ?: return SharpEmuBuild.mostRecent(staged, internalRoot)?.name
                ?: getString(R.string.setting_build_none)
        val build = bundled?.takeIf { it.folder == folder }
            ?: SharpEmuBuild.read(File(internalRoot, folder))
            ?: SharpEmuBuild.read(File(staged, folder))
            ?: return getString(R.string.setting_build_missing, folder)
        return build.name
    }

    /**
     * what the JIT accuracy row shows underneath itself.
     *
     * **it names the rung, or it names none.** once any setting below it is overridden the
     * configuration is no longer one of the rungs, and a row that went on naming the rung somebody
     * started from would describe what they have now by what they had then -- which for anybody who
     * has moved several may be nearer a different rung altogether.
     *
     * **every knob counts, because every rung names every knob.** there is no group a preset stays
     * out of, so there is none whose override a rung could still honestly describe.
     *
     * it reads what a launch would resolve rather than this store alone, so a game inheriting an
     * override from the app says Custom on the game's own screen.
     */
    private fun jitAccuracyLabel(): String {
        if (settings.fexOverrides().isNotEmpty()) {
            return getString(R.string.setting_fex_custom)
        }
        val entries = resources.getStringArray(R.array.fex_preset_entries)
        // a store naming a rung this build does not have reads as the default, which is the same
        // answer the list opens on and the same one a launch runs.
        val at = FexPreset.ALL.indexOf(settings.fexPreset ?: FexPreset.DEFAULT)
        return entries[if (at < 0) FexPreset.ALL.indexOf(FexPreset.DEFAULT) else at]
    }

    /**
     * the JIT accuracy screen: the ladder, then every knob a rung sets, then the ones no rung does.
     *
     * **the list is written out rather than generated from FEXCore's option table**, and that is not
     * laziness in the other direction: several options in that table are read only by machinery a
     * library host does not build, and the host layer refuses three more outright. generating rows
     * would ship seven controls that change nothing, which is the exact failure `--fex` refusing an
     * unknown name exists to prevent.
     *
     * **each knob row's default is the chosen rung's own value**, so moving the ladder moves every
     * row nobody has touched -- and a long press on one of them puts it back to the rung rather than
     * to a constant. see [Settings.fexKnobDefault].
     */
    private fun jitAccuracyRows(): List<SettingRow> {
        val rows = mutableListOf<SettingRow>(
            // **cards rather than a control with a position per rung**, because the configuration
            // can be none of them: the moment a knob below is overridden, no rung describes what is
            // set, and a control that must land on one of its positions cannot say so. nothing
            // chosen is that state, drawn.
            SettingRow.Cards(
                key = Settings.KEY_FEX_PRESET,
                entries = resources.getStringArray(R.array.fex_preset_entries),
                values = FexPreset.ALL,
                chosen = (settings.fexPreset ?: FexPreset.DEFAULT)
                    .takeIf { settings.fexOverrides().isEmpty() },
            ),
        )
        rows += SettingRow.Header(R.string.settings_group_memory_ordering)
        rows += FexPreset.ORDERING.map(::knobRow)
        rows += SettingRow.Header(R.string.settings_group_code_generation)
        rows += FexPreset.CODEGEN.map(::knobRow)
        rows += SettingRow.Header(R.string.settings_group_block_lookup)
        rows += FexPreset.LOOKUP.map(::knobRow)
        return rows
    }

    /** one FEXCore knob, drawn against whatever the rung above it settles that knob on. */
    private fun knobRow(knob: FexPreset.Knob): SettingRow = SettingRow.Switch(
        key = Settings.fexKnobKey(knob.option),
        title = knob.title,
        summary = knob.summary,
        default = knob.checked(settings.fexKnobDefault(knob.option)),
    )

    private fun graphicsRows(): List<SettingRow> = listOf(
        SettingRow.Dropdown(
            key = Settings.KEY_RENDER_SCALE,
            title = R.string.setting_internal_resolution,
            summary = R.string.setting_internal_resolution_summary,
            entries = resources.getStringArray(R.array.render_scale_entries),
            values = Settings.RENDER_SCALES,
            default = Settings.RENDER_SCALES[0],
        ),
        SettingRow.Header(R.string.settings_group_vulkan),
        SettingRow.Screen(
            key = Settings.KEY_DRIVER,
            title = R.string.setting_driver,
            summary = R.string.setting_driver_summary,
            value = chosenDriverLabel(),
        ) {
            startActivity(manager(DriversActivity::class.java))
        },
        // under the driver rather than above it, because a driver change invalidates a cache: the
        // blob carries the implementation's compatibility header and is rejected and rebuilt when
        // the driver moves. reading them in this order is reading cause before consequence.
        SettingRow.Switch(
            key = Settings.KEY_DISK_SHADER_CACHE,
            title = R.string.setting_disk_shader_cache,
            summary = R.string.setting_disk_shader_cache_summary,
            default = false,
        ),
    )

    /**
     * what the driver row shows underneath itself.
     *
     * **it always names something concrete**, because the system driver is always there -- so the row
     * reads a name rather than describing what happens when nothing is chosen.
     *
     * **a package that is not there says so rather than showing its name.** a staged directory can go
     * -- deleted from a PC, or the external volume wiped -- and a row that went on naming it would
     * leave a launch quietly falling back to the system driver as the only place to find out.
     *
     * it reads a `meta.json`, which is one small file, on the main thread: the alternative is a row
     * that draws empty and fills in a frame later on a screen that is otherwise synchronous.
     */
    private fun chosenDriverLabel(): String {
        val folder = settings.driver
        if (GpuDriver.isSystem(folder)) return getString(R.string.driver_system)
        val internalRoot = AppStorage.installedDrivers(filesDir)
        val staged = AppStorage.stagedDrivers(getExternalFilesDir(null)!!)
        val driver = GpuDriver.resolve(folder!!, internalRoot, staged)
            ?: return getString(R.string.setting_driver_missing, folder)
        return driver.name
    }

    /**
     * the Game files section: where games are read from, and how.
     *
     * **all-files access is here rather than in App**: App is the look and behaviour of the app, and
     * how it reaches a game's files is a data concern. the folder manager is the other half of that
     * same question -- which folders the app may read -- so the two belong together.
     *
     * **everything here is about files somebody else already owns**, living elsewhere on the device
     * and reached by a grant that can be revoked. what the emulator itself writes is User data, which
     * is a section of its own for that reason.
     *
     * **no subsection label.** every row in the section is the section's own subject now, and a
     * heading repeating the screen's title above the only group on it labels nothing.
     *
     * **the folder row is unconditional and the all-files row is not, and that ordering is the point.**
     * `MANAGE_EXTERNAL_STORAGE` needs API 30 and `minSdk` is 28, so this section builds a list rather
     * than returning one: an early return on the permission would take the folder manager with it, and
     * on a device below 30 that is a device with no way to add a game at all.
     *
     * the all-files row is an [SettingRow.External] rather than a [SettingRow.Switch] because this app
     * cannot set it: it is granted in android's own settings and nowhere else, so the row shows the
     * state and a tap opens the screen that changes it.
     */
    /**
     * the Controls section.
     *
     * **both rows are on by default, and both are read by the process that runs the guest rather than
     * turned into a launch argument.** that is the difference between these and every row in Emulation:
     * a build, a preset or a resolution has to reach the payload's command line, while these two govern
     * what this app does with events it receives and with a request it is handed -- so nothing is
     * passed, and neither of them can move the vector a launch is made with.
     *
     * **the mapping row is the app's, and a game's screen leaves it out.** a controller mapping is set
     * up for the controllers a person owns rather than for a title, and the launch reads it from the
     * app's own store. what a mapping set up once may still want for one title is silence, which is
     * the vibration row.
     *
     * **there are no port rows under the mapping switch**, which is why turning it off leaves a run
     * with no controller at all. the row's own summary says what on does; a switch that silently meant
     * something other than its label is what a settings screen must never be.
     */
    private fun controlsRows(): List<SettingRow> = listOf(
        SettingRow.Switch(
            key = Settings.KEY_AUTOMATIC_CONTROLLER_MAPPING,
            title = R.string.setting_automatic_controller_mapping,
            summary = R.string.setting_automatic_controller_mapping_summary,
            default = true,
            perGame = false,
        ),
        SettingRow.Switch(
            key = Settings.KEY_CONTROLLER_VIBRATION,
            title = R.string.setting_controller_vibration,
            summary = R.string.setting_controller_vibration_summary,
            default = true,
        ),
    )

    private fun gameFilesRows(): List<SettingRow> {
        // **a count rather than the folders themselves.** the build row names one build and the
        // driver row one package, because there is one of each; there is any number of folders, and
        // a row that listed them would be the screen behind it drawn badly on one line.
        //
        // it reads the store on the main thread, which is a `SharedPreferences` line cross-checked
        // against the grants android holds - the same read FoldersActivity does on a worker, where
        // it is followed by drawing a list rather than a number.
        val folders = GameLibrary.trees(this).size
        val rows = mutableListOf<SettingRow>(
            SettingRow.Screen(
                // nothing stored: this is a place to go rather than a value that was chosen.
                key = null,
                title = R.string.setting_folders,
                summary = R.string.setting_folders_summary,
                value = if (folders == 0) {
                    getString(R.string.setting_folders_empty)
                } else {
                    resources.getQuantityString(R.plurals.setting_folders_count, folders, folders)
                },
                chosen = folders > 0,
            ) {
                startActivity(Intent(this, FoldersActivity::class.java))
            },
        )
        if (AllFiles.supported()) {
            rows += SettingRow.External(
                title = R.string.setting_all_files,
                summary = R.string.setting_all_files_summary,
                checked = AllFiles.granted(),
            ) {
                if (!AllFiles.request(this)) {
                    Toast.makeText(this, R.string.all_files_unavailable, Toast.LENGTH_LONG).show()
                }
            }
        }
        return rows
    }

    /**
     * the colour picker behind the Custom theme's swatch.
     *
     * **one rectangle: hue across, chroma down, at a pinned tone.** three sliders were the first
     * version, a shade square with a hue bar the second, hue and *saturation* the third. these are
     * the generator's own axes rather than a conversion into them -- [HctColour] has why that matters
     * and what it cost to make the vertical one behave.
     *
     * **the preview beside it is the generated scheme, live** -- [SchemePreview] asks the same
     * generator the theme is built with, so it is the theme drawn small rather than an impression of
     * it. the screens *around* the dialog cannot repaint as the knob moves: a scheme is resolved
     * while a hierarchy is inflated, so repainting them means recreating the activity, which would
     * close the dialog doing the picking. saving does exactly that, which is the same path a theme
     * change already takes.
     */
    private fun pickColour() {
        val picker = DialogColourPickerBinding.inflate(LayoutInflater.from(this))
        val stored = settings.customColour ?: Settings.CUSTOM_COLOUR_DEFAULT
        val hue = HctColour.hueOf(stored)
        picker.field.hue = hue
        // **the stored colour's tone is read and discarded**, and its chroma is expressed as a
        // fraction of what its hue can reach. a seed saved by an earlier build opens where it belongs
        // and is normalised the moment it is saved again.
        picker.field.chromaFraction =
            (HctColour.chromaOf(stored) / HctColour.ceiling(hue).coerceAtLeast(1f)).coerceIn(0f, 1f)

        fun chosen(): Int = HctColour.colour(picker.field.hue, picker.field.chroma())

        /** redraws the swatch, the hex and the whole preview from whatever is picked right now. */
        fun redraw() {
            val colour = chosen()
            // **the preview is the generated scheme rather than the seed painted about.** the roles
            // come from the same generator the theme is built with, so what is drawn here is what
            // arrives on Save. it is also the only readout the dialog needs: a swatch and a hex under
            // the field would say the same thing with less of the theme around it.
            val scheme = SchemePreview.of(colour)
            picker.previewBackground.background = GradientDrawable().apply {
                cornerRadius = 16f * resources.displayMetrics.density
                setColor(scheme.background)
            }
            picker.previewCard.background = GradientDrawable().apply {
                cornerRadius = 16f * resources.displayMetrics.density
                setColor(scheme.surfaceContainer)
                setStroke((resources.displayMetrics.density).toInt().coerceAtLeast(1), scheme.outline)
            }
            picker.previewHeading.setTextColor(scheme.onSurfaceVariant)
            picker.previewTitle.setTextColor(scheme.onSurface)
            picker.previewSummary.setTextColor(scheme.onSurfaceVariant)
            picker.previewAccent.setTextColor(scheme.primary)
        }
        redraw()

        picker.field.onPicked = { _, _ -> redraw() }

        MaterialAlertDialogBuilder(this)
            .setTitle(R.string.setting_custom_colour)
            .setView(picker.root)
            .setPositiveButton(R.string.save) { _, _ ->
                settings.customColour = chosen()
                // the scheme is generated from this at inflation, so the screen has to be built
                // again to wear it. same path the theme row itself takes.
                Theme.reapply(this)
            }
            .setNegativeButton(R.string.cancel, null)
            .show()
    }

    companion object {
        const val EXTRA_SECTION = "section"

        /**
         * the game whose store this screen writes, as [Game.configKey] names it. absent for the app's
         * own settings, which is what every screen reached from the cog sends.
         */
        const val EXTRA_GAME = "game"

        /** where [colourRowArrival] is kept while a theme change rebuilds this screen. */
        private const val STATE_COLOUR_ROW = "colourRow"

        /** the seed colour row is where it was: a first visit, or a change that did not move it. */
        private const val COLOUR_ROW_STILL = 0

        /** a Custom theme was just chosen and the row is coming in under the one that chose it. */
        private const val COLOUR_ROW_ARRIVING = 1

        /** a Custom theme was just left and the row is going out. */
        private const val COLOUR_ROW_LEAVING = 2
    }
}
