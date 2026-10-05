package com.mircowuffwuff.sharpdroid

import android.content.Context
import android.content.SharedPreferences
import java.io.File

/**
 * what the user chose, and -- just as importantly -- what they did not.
 *
 * **"unset" is a value here, distinct from "set to the default", and that distinction is the whole
 * design.** a launch merges three sources in this order, last winning:
 *
 * ```
 * the build's own env  <  these settings  <  the launch intent's extras
 * ```
 *
 * which is the order `docs/build-format.md` already documents for the environment. the trap it
 * exists to avoid is precise: if a row that has never been touched reported its default as a
 * *choice*, the app would start sending that value on every launch -- and `am start` omitting the
 * matching extra would no longer reach the default, because a default it can no longer get to is
 * not a default. so a row that has never been touched contributes nothing to the argument vector,
 * and [MainActivity] sees an absence this file had no hand in.
 *
 * that is what keeps `scripts/run.py` uncompromised. it is also why every accessor here answers a
 * nullable, and why there is a [clear] rather than only a setter: "use the default" has to be
 * reachable from the UI, or a row is a one-way door.
 *
 * **[fexPreset] is the one row that contributes while untouched, and it is an exception on purpose.**
 * every rung of that ladder names every knob it covers, so that two installs reading the same word
 * run the same translation whatever FEXCore defaults to -- and a launch therefore spells the whole
 * JIT configuration out rather than leaving any of it unsaid. an untouched row resolves to the
 * default rung, which is what an install used to get by silence, so the run is the same and the
 * vector is not. `--es fexpreset none` is the way back to a launch that names none of it, and it is
 * a launch extra rather than a row for exactly the reason above: a stored value that meant "say
 * nothing" would be a configuration nobody can read off the screen.
 *
 * **a `SharedPreferences` line, like [GameLibrary]'s folder list.** nothing here is big enough to
 * want a file of its own, and the platform's own store is what survives an app update without a
 * migration of ours.
 *
 * **one game may answer differently, and that is [fallback].** [forGame] opens a store of that game's
 * own with the global one behind it, so the precedence a launch merges becomes four deep:
 *
 * ```
 * the build's own env  <  global settings  <  this game's settings  <  the launch intent's extras
 * ```
 *
 * every property a per-game scene offers asks its own store first and the one behind it second, so a
 * game that overrides nothing is the global configuration exactly. **the FEXCore configuration is
 * the one that is more than a per-key answer** -- a rung and the knobs overriding it are one thing
 * between them, and [fexOverrides] carries the rule that joins the two levels. what does **not** fall back is the
 * App section and [automaticControllerMapping] -- a theme, a fullscreen mode or a controller mapping is
 * the app's rather than a title's, and a property that fell back would imply a per-game one exists.
 *
 * @param fallback the store consulted when this one holds no answer, or null for the global store.
 */
class Settings private constructor(
    private val prefs: SharedPreferences,
    private val fallback: Settings? = null,
) {

    /**
     * true once the user has touched this row, whatever they set it to.
     *
     * **strictly this store and never [fallback]**, which is what gives one method two useful
     * meanings: on the global store it is "set", and on a per-game store it is "overridden" -- the
     * question *Use global value* is drawn from and the question *Use default* is offered from.
     */
    fun isSet(key: String): Boolean = prefs.contains(key)

    /**
     * whether this row shadows a value the store **behind** this one answers for.
     *
     * **[isSet] is not the same question, and the difference is one group of rows.** a row that is
     * set on a per-game store is usually shadowing the app's answer, which is what *Use global
     * value* offers to hand back -- but a FEXCore knob on a game that names its own rung is
     * shadowing that rung instead, because a level setting a preset owns the whole configuration
     * and drops what the level behind it said. see [fexOverrides].
     *
     * so on such a game there is no global value for a knob row to go back to, and a button
     * offering one would name a store that has been taken out of the answer. the row is left with
     * the rung above it as the only way back, which is a gesture the whole screen shares rather
     * than one that row carries.
     */
    fun overridesGlobal(key: String): Boolean {
        if (!perGame) return false
        if (key.startsWith(KEY_FEX_KNOB_PREFIX)) return isSet(key) && !isSet(KEY_FEX_PRESET)
        return answers(key)
    }

    /**
     * whether this store holds an answer for the row named by [key] -- which for one row is not the
     * same as holding that key.
     *
     * **[KEY_FEX_PRESET] names a screen rather than a value.** the row carrying it reads out the
     * whole JIT configuration and both ways back reset the whole of it, so a store that overrides
     * only a knob and no rung is still answering that row: a button or a long press that stayed
     * away would leave a configuration nobody can get out of from the screen that reports it.
     * [clear] settles the same set, which is what keeps the offer and what it does in step.
     *
     * every other row answers exactly when it is set.
     */
    fun answers(key: String): Boolean =
        if (key == KEY_FEX_PRESET) isSet(key) || ownFexOverrides().isNotEmpty() else isSet(key)

    /** whether this store answers for one game rather than for the app. */
    val perGame: Boolean get() = fallback != null

    /**
     * back to "the app decides", which is not the same as writing the default in.
     *
     * **the JIT preset takes the rows it governs with it**, and that is the same rule that makes
     * choosing a rung settle all of them: a level setting a preset owns the whole configuration, so
     * a level that stops setting one stops owning it. an override left behind would not merely be
     * untidy -- it would **change what it modifies**, having been a change to the rung that was
     * chosen and becoming a change to whatever answers next, which is a configuration nobody asked
     * for. see [fexPreset].
     */
    fun clear(key: String) {
        val edit = prefs.edit().remove(key)
        if (key == KEY_FEX_PRESET) edit.withoutFexKnobs()
        edit.apply()
    }

    /**
     * every FEXCore knob this store overrides, removed from an edit already in progress.
     *
     * **it takes the edit rather than making one**, so that a preset and the rows it settles are
     * written in a single commit: a store part way through, holding a new rung and the previous
     * one's overrides, is a configuration nobody asked for and every listener on the way would see
     * it.
     *
     * **the offered knobs rather than a scan for the prefix**, matching [fexOverrides] -- a key
     * written by a later version of this app is not one this build can reason about, and removing
     * it here would quietly discard a setting on the way past.
     */
    private fun SharedPreferences.Editor.withoutFexKnobs(): SharedPreferences.Editor {
        for (knob in FexPreset.KNOBS) remove(fexKnobKey(knob.option))
        return this
    }

    /**
     * every row back to untouched, at once.
     *
     * **through the store rather than by deleting its file**, and that is not a style preference:
     * `SharedPreferences` is cached per process, so a file removed underneath the framework is one
     * the framework rewrites from memory the moment anything else is set. `clear()` is the only
     * removal that both the disk and the copy in memory agree about.
     *
     * **it does not touch the all-files permission, and there is nothing here that could.** that row
     * stores no key -- [AllFiles] reads the platform live on every look -- so it is an exception to
     * this by construction rather than by an exclusion somebody has to remember.
     *
     * **this store only.** a per-game store is its own file, so resetting the app's settings means
     * this and then [forgetEveryGame] -- see there for why they are two calls rather than one.
     */
    fun clearAll() = prefs.edit().clear().apply()

    /**
     * how many rows are on something other than their default.
     *
     * **it compares values rather than counting what is set, and the two are not the same number.**
     * a row the user opened and put back on the value it already had is set -- it reaches a launch,
     * an export carries it, and *Reset* clears it -- but it is not a row that differs from the
     * default, and a card reporting it would tell somebody three things had changed when the app
     * behaves exactly as it shipped. what the User data screen states is the *state*, so this
     * answers about the state.
     *
     * **the driver has two spellings of one answer** -- absent and [GpuDriver.SYSTEM] both mean the
     * driver the device shipped with -- so it is asked rather than compared. everything else has one
     * default and one way to say it.
     */
    fun changedFromDefault(): Int {
        var count = 0
        if (theme?.let { it != THEME_DEFAULT } == true) count++
        if (customColour?.let { it != CUSTOM_COLOUR_DEFAULT } == true) count++
        if (fullscreen == true) count++
        // **compared against on rather than counted as set**, because this row's default is on: a
        // person who opened it and put it back would otherwise be reported as having changed
        // something the app does exactly as it shipped.
        if (loadingEstimate == false) count++
        if (autoResume == true) count++
        if (strictDynlib == true) count++
        if (fexPreset?.let { it != FexPreset.DEFAULT } == true) count++
        // **each knob moved off what the rung settles on, rather than each knob that is set.** a row
        // opened and put back on the value it already had is an override -- it reaches a launch and
        // Reset clears it -- and is not a change from the default, which is the distinction this
        // whole method draws.
        for (knob in FexPreset.KNOBS) {
            val override = fexKnob(knob.option) ?: continue
            if (override != fexKnobDefault(knob.option)) count++
        }
        // compared against on rather than counted as set, for the reason the loading estimate is.
        if (hostFeatureProbe == false) count++
        if (renderScale?.let { it != RENDER_SCALES[0] } == true) count++
        if (diskShaderCache == true) count++
        // **absence is the default and the bundled build is a choice, even though a launch resolves
        // the two to the same payload.** what separates them is that nothing writes this key by
        // itself: a fresh install stores nothing, and BuildsActivity.select returns early on the row
        // that is already selected - so the only way to hold [SharpEmuBuild.BUNDLED] here is to have
        // been on another build and come back, which is a choice like any other.
        if (build != null) count++
        if (!GpuDriver.isSystem(driver)) count++
        return count
    }

    /**
     * how many rows this store overrides, whatever they are set to.
     *
     * **it counts what is set rather than what differs**, which is the opposite of
     * [changedFromDefault] and right for the opposite reason: an override *is* the state here. a row
     * a game overrides with the same value the app's own row holds still reaches a launch from this
     * file, still survives the app's row moving, and is still what the *Use global value* button
     * takes away -- so reporting it as no change would describe a store that behaves differently as
     * one that behaves the same.
     */
    fun overridden(): Int = prefs.all.size

    // ------------------------------------------------------------------------------------------
    // App

    /**
     * which palette the app's own screens wear. never reaches the guest and never reaches the
     * argument vector -- it is the one row here that is purely the frontend's.
     */
    var theme: String?
        get() = prefs.getString(KEY_THEME, null)
        set(value) = prefs.edit().putString(KEY_THEME, value).apply()

    /**
     * the seed colour a *Custom* theme is generated from.
     *
     * **one colour, and the scheme is derived from it** -- Material's own generator produces the
     * accent, the surfaces, the outlines and every text colour that goes with them, which is why
     * this is a single `Int` rather than the four a scheme would otherwise need. see [Theme].
     */
    var customColour: Int?
        get() = if (prefs.contains(KEY_CUSTOM_COLOUR)) {
            prefs.getInt(KEY_CUSTOM_COLOUR, CUSTOM_COLOUR_DEFAULT)
        } else {
            null
        }
        set(value) = prefs.edit().putInt(KEY_CUSTOM_COLOUR, value!!).apply()

    /**
     * whether the app's own screens hide the system bars.
     *
     * off -- the default -- is the ordinary android screen: the layout pads itself by whatever the
     * status and navigation bars occupy. on is the same edge-to-edge window with the bars hidden and
     * a swipe bringing them back transiently.
     *
     * **this is the app's screens and never a guest's.** [MainActivity] hides its bars whatever this
     * says, because the surface is what decides the extent the guest presents at and a visible bar
     * would shrink it.
     */
    var fullscreen: Boolean?
        get() = if (prefs.contains(KEY_FULLSCREEN)) prefs.getBoolean(KEY_FULLSCREEN, false) else null
        set(value) = prefs.edit().putBoolean(KEY_FULLSCREEN, value!!).apply()

    // ------------------------------------------------------------------------------------------
    // Emulation

    /**
     * the build the user chose, as its on-device folder name.
     *
     * **a concrete identity and never a pointer.** the folder name is derived from `meta.json` --
     * `<id>-<sharpemuVersion>-<packagedAt>` -- so it names one build rather than a family, and a
     * choice survives a newer build of the same id arriving. a stored id would quietly become a
     * different payload the day one did.
     *
     * **null means the bundled build**, the one that ships inside this APK. that is a real answer
     * rather than an absence, which is why no row here has to describe a rule instead of naming a
     * build. a debug app bundles none, so there it falls back to the most recently staged build --
     * the behaviour the deploy loop has always had.
     */
    var build: String?
        get() = prefs.getString(KEY_BUILD, null) ?: fallback?.build
        set(value) = prefs.edit().putString(KEY_BUILD, value).apply()

    /**
     * `--strict` on the payload's own command line, which fails a launch when an imported symbol
     * cannot be resolved rather than continuing without it.
     *
     * **this is a guest argument and not a host-layer flag.** the host layer takes
     * `<elf> [guest args...]` and has always passed the rest through; nothing below the payload
     * knows or cares what this means.
     */
    var strictDynlib: Boolean?
        get() = if (prefs.contains(KEY_STRICT)) {
            prefs.getBoolean(KEY_STRICT, false)
        } else {
            fallback?.strictDynlib
        }
        set(value) = prefs.edit().putBoolean(KEY_STRICT, value!!).apply()

    /**
     * the FEXCore JIT preset, as one of [FexPreset.ALL].
     *
     * **it is not in [guestEnvironment] and must not become so.** a preset reaches FEXCore as
     * `--fex` flags on the host layer's own command line, beside `--smc`. see [FexPreset] for why
     * the environment route does not exist here at all, and [MainActivity] for the rule that keeps
     * a build's `env` out of anything governing the host layer's correctness.
     *
     * stored as the id rather than as an index, so a store survives a rung being inserted into the
     * ladder.
     *
     * **it is a rung and not the whole configuration**, which is what [fexOverrides] is beside it.
     */
    var fexPreset: String?
        get() = prefs.getString(KEY_FEX_PRESET, null) ?: fallback?.fexPreset
        // **writing a rung settles every row it names, in the same edit.** every rung names every
        // knob, so a rung chosen here is a complete answer and an override surviving it would be a
        // row the word on screen does not describe. [clear] is the same rule going the other way.
        set(value) = prefs.edit().putString(KEY_FEX_PRESET, value).withoutFexKnobs().apply()

    /**
     * the individual FEXCore knobs a launch overrides on top of [fexPreset], as option name to
     * value, and empty where nobody has opened one.
     *
     * **a rung plus a sparse map, rather than either alone**, and the alternative that is wrong is
     * worth stating: storing the knob values and working the rung out by comparing them would mean
     * this app could **never correct a preset**. somebody who chose a rung would be frozen at
     * whatever it meant the day they chose it, so a rung later found to reintroduce a fault would
     * reach nobody, ever -- and an install would flip to reading Custom through no action of its
     * own the first time a rung moved. under this shape an updated rung reaches every row that was
     * left alone.
     *
     * **this is [forGame]'s own mechanic one storey up**: a row is unset, meaning take what is
     * behind it, or explicitly set. same predicate, same reason.
     *
     * **and one rule joins the two levels: a level that sets a preset owns the whole configuration,
     * while a level that sets only knobs modifies what it inherited.** without it a game that chose
     * a rung of its own would still be carrying the app's overrides, and its own screen would name
     * one rung while its launch ran another -- which is the failure the whole precedence design
     * exists to avoid.
     */
    fun fexOverrides(): Map<String, String> = inheritedFexOverrides() + ownFexOverrides()

    /**
     * the overrides in force **behind** this store, which is what a row this store leaves alone
     * falls back to. empty on the global store, and empty on a game that names its own rung.
     */
    private fun inheritedFexOverrides(): Map<String, String> =
        if (fallback == null || prefs.contains(KEY_FEX_PRESET)) {
            emptyMap()
        } else {
            fallback.fexOverrides()
        }

    /**
     * this store's own overrides.
     *
     * **read through the offered knobs rather than by scanning the store for the prefix**, so that
     * a key written by a later version of this app and restored into this one contributes nothing
     * rather than reaching a launch. the host layer refuses a FEXCore option it cannot resolve and
     * ends the run saying so, which as a way to find out that a settings import came from a newer
     * build is a game that no longer starts.
     */
    private fun ownFexOverrides(): Map<String, String> {
        val out = LinkedHashMap<String, String>()
        for (knob in FexPreset.KNOBS) {
            prefs.getString(fexKnobKey(knob.option), null)?.let { out[knob.option] = it }
        }
        return out
    }

    /** one knob as **this** store overrides it, or null where this store leaves it alone. */
    fun fexKnob(option: String): String? = prefs.getString(fexKnobKey(option), null)

    fun setFexKnob(option: String, value: String) =
        prefs.edit().putString(fexKnobKey(option), value).apply()

    /**
     * what a knob settles on when this store does not override it.
     *
     * **it is what the row draws as its own default, and what the long press puts it back to** --
     * so it is a function of the stored rung rather than a constant, which is the one place this
     * shape costs anything. an updated rung moves every row nobody has touched, on the next draw.
     */
    fun fexKnobDefault(option: String): String =
        FexPreset.resolve(option, fexPreset, inheritedFexOverrides())

    // ------------------------------------------------------------------------------------------
    // Graphics

    /**
     * `SHARPEMU_RENDER_SCALE`, the guest environment variable `VulkanVideoPresenter` reads: render
     * offscreen targets below native resolution and upscale on present.
     *
     * stored as the string the payload parses rather than as an index, so the store stays readable
     * and a value the desktop UI grows later does not need a table here to be expressible. the four
     * offered are the four the desktop UI offers.
     */
    var renderScale: String?
        get() = prefs.getString(KEY_RENDER_SCALE, null) ?: fallback?.renderScale
        set(value) = prefs.edit().putString(KEY_RENDER_SCALE, value).apply()

    /**
     * the GPU driver the user chose, as its on-device folder name.
     *
     * **[GpuDriver.SYSTEM] is a value here and so is absence**, and they mean the same thing: the
     * driver the device shipped with. storing it is what lets the manager's radio mark a choice
     * rather than an empty store, and it costs nothing to a launch -- [MainActivity] reads both as
     * "no custom driver" and passes no flags at all, which is the run every measurement in this
     * project was taken on.
     *
     * a folder name rather than a display name, because two packages can call themselves the same
     * thing and only one directory can be loaded.
     */
    var driver: String?
        get() = prefs.getString(KEY_DRIVER, null) ?: fallback?.driver
        set(value) = prefs.edit().putString(KEY_DRIVER, value).apply()

    /**
     * whether compiled graphics pipelines are kept between runs.
     *
     * **off is the default, and it is the one row here whose default is not the emulator's own.**
     * the cache is derived state written throughout a run and kept per title, so holding it is a
     * thing to opt into rather than something an install starts doing unasked -- and what it buys is
     * time rather than capability, which is the kind of trade that belongs to the person paying it.
     *
     * **it travels as `SHARPEMU_VK_PIPELINE_CACHE=0`, which is the emulator's own opt-out**, and the
     * cache then lives for the run and is never written. leaving `SHARPEMU_VK_PIPELINE_CACHE_PATH`
     * out instead would not disable anything: unset means the emulator resolves its portable
     * default, a directory inside the build, so the cache would still persist and would sit where a
     * re-stage destroys it.
     *
     * nothing deletes a cache already on the device. the User data screen is where bytes are
     * removed, and a file nothing reads costs only space.
     */
    var diskShaderCache: Boolean?
        get() = if (prefs.contains(KEY_DISK_SHADER_CACHE)) {
            prefs.getBoolean(KEY_DISK_SHADER_CACHE, false)
        } else {
            fallback?.diskShaderCache
        }
        set(value) = prefs.edit().putBoolean(KEY_DISK_SHADER_CACHE, value!!).apply()

    // ------------------------------------------------------------------------------------------
    // Controls

    /**
     * whether controllers reach the guest through the app's own automatic mapping: every connected
     * controller, by button position, merged into port 1.
     *
     * **off hands controller input to the port rows**, which is to say to nothing while there are none
     * to hand it to: a run with this off has no controller. the row's own summary says what on does.
     *
     * **the app's rather than a game's, so it does not fall back.** a mapping is set up for the
     * controllers a person owns rather than for a title, and the launch reads this out of the app's
     * own store the way it reads the App section -- so a per-game store holding this key is never
     * consulted, and the per-game screen does not draw the row.
     *
     * **the key says *automatic*** because the unqualified name belongs to the per-port mappings
     * rather than to the switch above them.
     *
     * **off does not disable rumble**, which is [vibrateHandheld]'s to decide. the two are separate because a
     * person who wants to play by touch on a device that has a pad in it should still feel a game's
     * haptics, and because a controller that misbehaves is a different complaint from a motor that is
     * distracting.
     *
     * **neither of these becomes a launch argument.** they are read by the process that runs the
     * guest and applied to what this app does with events it receives and with a request it is
     * handed, so neither of them can move the vector a launch is made with.
     */
    var automaticControllerMapping: Boolean?
        get() = if (prefs.contains(KEY_AUTOMATIC_CONTROLLER_MAPPING)) {
            prefs.getBoolean(KEY_AUTOMATIC_CONTROLLER_MAPPING, true)
        } else {
            null
        }
        set(value) = prefs.edit().putBoolean(KEY_AUTOMATIC_CONTROLLER_MAPPING, value!!).apply()

    /**
     * whether the loading screen estimates how far along a boot is, or simply says one is happening.
     *
     * **off is the indeterminate bar on every launch of every game**, with the cover, the name and the
     * phase line unchanged and the screen still coming down at the first frame. what goes is the
     * determinate sweep and the figure under it -- the half of that screen that needs a record.
     *
     * **the record is still written while this is off, and that is not a compromise.** the host layer's
     * checkpoint tap cannot be turned off by this: `Reached()` arriving at the end of the table is the
     * only signal the app has that the guest has presented a frame, and it is what takes the loading
     * screen down -- a launch that dropped `--boot-progress` would leave that screen over a running
     * game forever. so the timings are stamped either way, and declining to record them would throw an
     * answer away rather than save any work. it also keeps switching this back on immediate, where
     * discarding would cost one more indeterminate boot per title afterwards.
     *
     * **the row says so**, because a switch that reads as "stop doing this" while the app quietly keeps
     * timing every boot is a small lie, and one clause in a summary is the whole of the fix.
     *
     * **the app's rather than a title's**, like the theme and the fullscreen mode above it: a person
     * who does not want an estimate does not want one per game. so it is not offered per game and does
     * not fall back -- see [forGame].
     *
     * gated where the screen is drawn and not in the argument vector, so this row cannot move what a
     * launch is made with.
     */
    var loadingEstimate: Boolean?
        get() = if (prefs.contains(KEY_LOADING_ESTIMATE)) {
            prefs.getBoolean(KEY_LOADING_ESTIMATE, true)
        } else {
            null
        }
        set(value) = prefs.edit().putBoolean(KEY_LOADING_ESTIMATE, value!!).apply()

    /**
     * whether coming back to the app resumes a game that leaving it paused.
     *
     * **off -- the default -- is Eden's rule**: leaving pauses, and only a person resumes. on undoes
     * exactly the pause that leaving made and no other, so a game somebody paused from the back panel
     * before leaving is still paused when they return. and a pause that coming back will undo draws
     * no play button, since one that appeared and went in the same moment would only be a flicker.
     *
     * **the app's rather than a title's**, like the loading estimate: it is about how a person uses
     * the app rather than about a game. so it is not offered per game and does not fall back -- see
     * [forGame]. read by the process that runs the guest and never a launch argument.
     */
    var autoResume: Boolean?
        get() = if (prefs.contains(KEY_AUTO_RESUME)) prefs.getBoolean(KEY_AUTO_RESUME, false) else null
        set(value) = prefs.edit().putBoolean(KEY_AUTO_RESUME, value!!).apply()

    /**
     * whether FEXCore is told what this processor can do, out of the processor's own ID registers.
     *
     * **on is the honest answer and off is the fallback**, which is the opposite way round from most
     * switches here. understating a host's instruction set never changes what translated code
     * computes, only how many instructions it takes to compute it -- so there is no correctness
     * argument for the conservative set, and this row exists because a probe is a thing that can be
     * wrong about a device nobody here has.
     *
     * **not a preset rung, and it must not become one.** the preset ladder trades faithfulness for
     * speed, and describing the host truthfully is not a trade -- a rung that turned this off would
     * be a rung that was slower for no fidelity.
     *
     * it is a launch argument: off travels as `--host-features minimal` and on says nothing, so this
     * row adds to the vector only when it is turned off.
     */
    var hostFeatureProbe: Boolean?
        get() = if (prefs.contains(KEY_HOST_FEATURE_PROBE)) {
            prefs.getBoolean(KEY_HOST_FEATURE_PROBE, true)
        } else {
            fallback?.hostFeatureProbe
        }
        set(value) = prefs.edit().putBoolean(KEY_HOST_FEATURE_PROBE, value!!).apply()

    /**
     * whether a game may drive **this device's** vibration motor.
     *
     * **named for the handheld and not for vibration in general**, because a controller with a motor
     * of its own is a second answer to "should this rumble" rather than the same one -- and the day
     * that exists, a setting called simply *vibrate* would have to mean both or be renamed.
     *
     * gated in the app rather than in the host layer, because the vibrator is the app's: the guest's
     * request still crosses and is still counted, and what changes is whether the platform is asked.
     * that keeps a run with this off distinguishable in the log from a run where the game never asked.
     */
    var vibrateHandheld: Boolean?
        get() = if (prefs.contains(KEY_VIBRATE_HANDHELD)) {
            prefs.getBoolean(KEY_VIBRATE_HANDHELD, true)
        } else {
            fallback?.vibrateHandheld
        }
        set(value) = prefs.edit().putBoolean(KEY_VIBRATE_HANDHELD, value!!).apply()

    /**
     * the guest environment these settings contribute, in the order it should be applied.
     *
     * **only what was actually chosen.** an untouched row puts nothing in the map, so the guest's
     * environment is byte-for-byte one this file had no hand in.
     *
     * the launcher's own four -- the host window, its size, the host audio selector and the .NET one
     * -- are written by [MainActivity] *after* this map and are deliberately not expressible here.
     * they are the contract a payload is run under rather than a preference, which is the same rule
     * `docs/build-format.md` states for a build's `env`.
     *
     * **[fexPreset] is absent for a stronger reason than that**, and adding it here would be a
     * defect rather than a style choice: this map has a build's own `env` merged underneath it, so
     * anything expressible here is expressible by a payload -- and a JIT preset is not a payload's to
     * choose. it travels as host-layer flags instead.
     */
    fun guestEnvironment(): Map<String, String> {
        val env = LinkedHashMap<String, String>()
        renderScale?.let { env["SHARPEMU_RENDER_SCALE"] = it }
        return env
    }

    companion object {
        const val KEY_THEME = "theme"
        const val KEY_CUSTOM_COLOUR = "custom_colour"
        const val KEY_FULLSCREEN = "fullscreen"
        const val KEY_LOADING_ESTIMATE = "loading_estimate"
        const val KEY_AUTO_RESUME = "auto_resume"
        const val KEY_BUILD = "build"
        const val KEY_STRICT = "strict_dynlib"
        const val KEY_FEX_PRESET = "fex_preset"

        /**
         * the prefix one FEXCore knob's override is stored under, followed by the option's own
         * FEXCore name.
         *
         * **the option's name rather than an index or a short id**, because that is the string the
         * host layer resolves and the only spelling that cannot drift: a key built from a position
         * in a list would name a different knob the day the list gains one, and every store already
         * written would then be describing the wrong row.
         */
        private const val KEY_FEX_KNOB_PREFIX = "fex_knob_"

        /** the key one knob's override is stored under. */
        @JvmStatic
        fun fexKnobKey(option: String) = KEY_FEX_KNOB_PREFIX + option
        const val KEY_HOST_FEATURE_PROBE = "host_feature_probe"
        const val KEY_RENDER_SCALE = "render_scale"
        const val KEY_DRIVER = "driver"
        const val KEY_DISK_SHADER_CACHE = "disk_shader_cache"
        // **both are named for the narrow thing they govern rather than for their subject**, because
        // the broad name is the one a later setting will want. per-pad mappings would make a plain
        // `controller_mapping` the wrong name for the switch that turns automatic mapping on, and a
        // physical pad's own motor would make a plain `vibrate` ambiguous against this device's.
        // a stored key cannot be renamed later without either abandoning what people have chosen or
        // carrying a migration forever, so the cost of getting this wrong is paid once and kept.
        const val KEY_AUTOMATIC_CONTROLLER_MAPPING = "automatic_controller_mapping"
        const val KEY_VIBRATE_HANDHELD = "vibrate_handheld"

        /** the four the desktop UI offers, as the payload parses them. */
        val RENDER_SCALES = arrayOf("1.0", "0.75", "0.5", "0.25")

        /** Theme ids, matching the order of `R.array.theme_entries`. */
        const val THEME_SHARPEMU = "sharpemu"
        const val THEME_LIGHT = "light"

        const val THEME_DARK = "dark"
        const val THEME_MATERIAL_YOU = "materialyou"
        const val THEME_CUSTOM = "custom"

        /**
         * **the order the dropdown shows, and the values the store holds.**
         *
         * `Theme.SharpDroid` -- the application style, Material3 following the platform -- is not among
         * them: an explicit Light and an explicit Dark cover it. it stays as a style because the
         * manifest names it and because it is what an activity wears for the moment before [Theme]
         * sets the chosen one.
         */
        val THEMES =
            arrayOf(THEME_SHARPEMU, THEME_LIGHT, THEME_DARK, THEME_MATERIAL_YOU, THEME_CUSTOM)

        /**
         * **every custom seed is at this tone and the picker has no control for it.**
         *
         * tone is HCT's perceptual lightness -- CIE L\* -- and pinning it is what locks a seed's
         * brightness. locking HSV's *value* does not: measured on four seeds picked at value 0.75,
         * the tone runs from **37 for a violet to 71 for a green**.
         *
         * **it is not free.** the generator does read a seed's tone -- `SchemeContent` is a content
         * scheme and its accent tracks it -- but the
         * tracking is clamped: measured on this device, seed tones of 40 and 60 both produce an
         * accent at tone 80, and only above about 60 does the accent drift lighter and lose chroma.
         * so this is the top of the useful range rather than an arbitrary middle, and [HctColour]
         * slides *below* it per hue where that buys chroma.
         */
        const val CUSTOM_TONE = 60f

        /**
         * the seed a Custom theme starts at, before the user has picked one.
         *
         * **the maintainer's own** -- so that selecting *Custom* and changing nothing lands somewhere
         * deliberate rather than on whatever a null would generate.
         *
         * it is a literal rather than a call into [HctColour] for two reasons: the picker produced
         * it, so it is already at that hue's tone and round-trips through the field unchanged, and a
         * constant that needs a gamut search to evaluate would drag the whole per-hue table into the
         * first screen that reads any setting at all.
         */
        const val CUSTOM_COLOUR_DEFAULT: Int = 0xFFA446D8.toInt()

        /**
         * **SharpEmu is the default**: a fresh install should look like this emulator rather than
         * like a stock android app, and the palette is the desktop build's own. it is also the fallback for an id this build does not know, so a store
         * written by a later version degrades to the app's own look rather than to a theme that is no
         * longer offered.
         */
        const val THEME_DEFAULT = THEME_SHARPEMU

        /** the global store's file, and the prefix every per-game store's file carries. */
        private const val STORE = "settings"
        private const val GAME_STORE_PREFIX = "settings-game-"

        @JvmStatic
        fun of(context: Context): Settings =
            Settings(context.getSharedPreferences(STORE, Context.MODE_PRIVATE))

        /**
         * one game's store, with the global one behind it.
         *
         * [key] is the game's identity as [Game.configKey] answers it -- the title id the emulator
         * itself resolves, so that a game's settings, its save data directory and its pipeline cache
         * are all filed under one string rather than under three spellings of one idea.
         *
         * **a file per game rather than prefixed keys in the global store**, which is what makes
         * "forget this game" a deletion and what keeps the global store readable. it costs nothing to
         * an export: the Everything archive packs the whole of `shared_prefs/`, so these travel with
         * it and are restored by an import without either side naming them.
         */
        @JvmStatic
        fun forGame(context: Context, key: String): Settings = Settings(
            context.getSharedPreferences(GAME_STORE_PREFIX + key, Context.MODE_PRIVATE),
            of(context),
        )

        /**
         * every per-game store, emptied.
         *
         * **through each store's own API rather than by deleting files**, for the reason [clearAll]
         * gives: `SharedPreferences` is cached per process, and a file removed underneath the
         * framework is one the framework rewrites from memory the next time anything is set. a store
         * this process has never opened is opened to be cleared, which is the same call either way.
         *
         * **the files are found by listing `shared_prefs/` rather than by remembering which games
         * have one.** an index would be a second thing to keep in step with the stores themselves,
         * and it would be wrong in exactly the case that matters -- a store restored by an import,
         * which arrives as a file and tells nothing.
         */
        @JvmStatic
        fun forgetEveryGame(context: Context) {
            gameStoreKeys(context).forEach {
                context.getSharedPreferences(GAME_STORE_PREFIX + it, Context.MODE_PRIVATE)
                    .edit().clear().apply()
            }
        }

        /** the identity of every game that has a store, whether or not anything is in it. */
        @JvmStatic
        fun gameStoreKeys(context: Context): List<String> {
            val sharedPrefs = File(context.filesDir.parentFile, "shared_prefs")
            return sharedPrefs.listFiles().orEmpty()
                .map { it.name }
                .filter { it.startsWith(GAME_STORE_PREFIX) && it.endsWith(".xml") }
                .map { it.removePrefix(GAME_STORE_PREFIX).removeSuffix(".xml") }
        }
    }
}
