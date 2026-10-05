package com.mircowuffwuff.sharpdroid

import android.app.ActivityManager
import android.content.Context
import android.content.Intent
import android.net.Uri
import android.os.Bundle
import android.text.format.DateUtils
import android.text.format.Formatter
import android.view.LayoutInflater
import android.view.View
import android.widget.Toast
import androidx.activity.result.contract.ActivityResultContracts
import androidx.appcompat.app.AppCompatActivity
import androidx.recyclerview.widget.GridLayoutManager
import com.google.android.material.dialog.MaterialAlertDialogBuilder
import com.mircowuffwuff.sharpdroid.databinding.ActivityManagerBinding
import com.mircowuffwuff.sharpdroid.databinding.DialogMessageBinding
import java.io.File
import java.text.SimpleDateFormat
import java.util.Date
import java.util.Locale
import java.util.concurrent.Executors

/**
 * the User data screen: what the emulator has written that belongs to the person using it.
 *
 * reached from **Settings → User data**, whose card opens this directly rather than a list of rows
 * holding one row that opens it.
 *
 * **the manager screen again** -- the same toolbar over the same grid, as the build, driver and folder
 * managers are. what is listed is not a package and none of the importing applies, but a grid of
 * things with actions on each is the shape those three already are, and a fourth that looked
 * different would be a fourth thing to learn.
 *
 * **the whole of it is the first card rather than a pair of toolbar actions**, which is what makes the
 * most common thing anybody comes here for the most visible thing on the screen. it is also what lets
 * it state a size: a toolbar action is a word, where a card carries the figure that says how much is
 * about to move.
 *
 * **neither is the floating button the other managers carry.** that slot would sit on top of the last
 * card's own buttons -- this screen's list is four cards that never scroll, so the space a manager's
 * list reserves under itself is space this one does not have.
 *
 * **every button that destroys something confirms first, and the two exports do not.** an export
 * writes one file into a place the user picked and touches nothing else; a confirmation there would
 * be a question with no stake in it, and a screen that asks about everything teaches people to stop
 * reading. both imports confirm, because both replace something already on the device.
 */
class UserDataActivity : AppCompatActivity() {

    private lateinit var binding: ActivityManagerBinding
    private lateinit var adapter: UserDataAdapter
    private lateinit var drawnWith: String
    private val worker = Executors.newSingleThreadExecutor()

    /** which card's button opened the picker. read when it answers, and only then. */
    private var pending: UserDataItem.Kind? = null

    /**
     * the two pickers, registered at construction as the contract requires -- this activity can be
     * recreated while a picker is in front of it, and a launcher registered later than `onCreate` has
     * nothing to deliver a result to.
     */
    private val exportTo =
        registerForActivityResult(ActivityResultContracts.CreateDocument(ZIP)) { out ->
            val kind = pending
            pending = null
            if (out != null && kind != null) export(kind, out)
        }

    private val importFrom = registerForActivityResult(ActivityResultContracts.OpenDocument()) { zip ->
        val kind = pending
        pending = null
        if (zip != null && kind != null) offerImport(kind, zip)
    }

    override fun onCreate(state: Bundle?) {
        Theme.apply(this)
        drawnWith = Theme.signature(this)
        super.onCreate(state)
        binding = ActivityManagerBinding.inflate(layoutInflater)
        setContentView(binding.root)
        SystemBars.apply(this, binding.root)

        binding.toolbar.setTitle(R.string.settings_user_data)
        binding.toolbar.setNavigationOnClickListener { finish() }

        adapter = UserDataAdapter(emptyList(), this::act)
        binding.list.layoutManager =
            GridLayoutManager(this, resources.getInteger(R.integer.settings_section_columns))
        binding.list.adapter = adapter

        // **no empty state.** the four cards are always there: a device with nothing saved yet still
        // has save data as a thing that exists and is empty, and each card says so on its own line.
        binding.importZip.visibility = View.GONE
    }

    override fun onResume() {
        super.onResume()
        if (Theme.recreateIfStale(this, drawnWith)) return
        SystemBars.apply(this, binding.root)
        refresh()
    }

    override fun onDestroy() {
        worker.shutdown()
        super.onDestroy()
    }

    /**
     * measures on the worker and draws on the main thread.
     *
     * **the settings count is taken on the worker too**, beside the walk rather than before it: the
     * app's own store is a handful of lookups against something the framework already holds in
     * memory, but a game's store is a file the framework has to find and parse the first time it is
     * asked for, and there is one per game that has ever been configured.
     */
    private fun refresh() {
        val files = filesDir
        worker.execute {
            // **the app's own rows that differ, plus every row any game overrides.** the card is one
            // figure over one Reset button, and that button clears both stores -- so a number counting
            // only the first would report "nothing changed" over a button that is about to change
            // something. the two are added rather than shown apart because the card has one line.
            val changed = Settings.of(this).changedFromDefault() +
                Settings.gameStoreKeys(this).sumOf { Settings.forGame(this, it).overridden() }
            val items = UserDataItem.measure(files, changed)
            runOnUiThread { if (!isFinishing) adapter.submit(items) }
        }
    }

    // ---------------------------------------------------------------------------------------------
    // what a button does

    private fun act(kind: UserDataItem.Kind, action: UserDataAdapter.Action) = when (action) {
        UserDataAdapter.Action.EXPORT -> {
            pending = kind
            exportTo.launch(suggestedName(kind))
        }
        UserDataAdapter.Action.IMPORT -> {
            pending = kind
            // the zip mime type rather than everything, so the picker greys out what cannot be one.
            importFrom.launch(arrayOf(ZIP, "application/octet-stream"))
        }
        UserDataAdapter.Action.DELETE -> confirmDelete(kind)
        UserDataAdapter.Action.RESET -> confirmReset()
    }

    private fun suggestedName(kind: UserDataItem.Kind): String {
        val stamp = SimpleDateFormat("yyyy-MM-dd", Locale.US).format(Date())
        val what = if (kind == UserDataItem.Kind.EVERYTHING) "everything" else "savedata"
        return "sharpdroid-$what-$stamp.zip"
    }

    // ---------------------------------------------------------------------------------------------
    // export

    private fun export(kind: UserDataItem.Kind, out: Uri) = busy {
        val bytes = when (kind) {
            UserDataItem.Kind.EVERYTHING -> UserDataArchive.exportEverything(this, out)
            else -> UserDataArchive.exportSaveData(this, out)
        }
        runOnUiThread {
            done()
            if (bytes == null) {
                toast(getString(R.string.user_data_export_failed))
            } else {
                toast(getString(R.string.user_data_exported, formatted(bytes)))
            }
        }
    }

    // ---------------------------------------------------------------------------------------------
    // import

    /**
     * reads the archive's manifest, then asks.
     *
     * **the manifest is what refuses a mismatch**, and the refusal names the archive rather than the
     * button: somebody who picked the wrong file wants to know what they picked.
     */
    private fun offerImport(kind: UserDataItem.Kind, zip: Uri) = busy {
        val manifest = UserDataArchive.read(this, zip)
        runOnUiThread {
            done()
            val wanted = if (kind == UserDataItem.Kind.EVERYTHING) {
                UserDataArchive.Kind.EVERYTHING
            } else {
                UserDataArchive.Kind.SAVE_DATA
            }
            if (manifest == null) {
                toast(getString(R.string.user_data_import_not_ours))
                return@runOnUiThread
            }
            if (manifest.kind != wanted) {
                toast(getString(R.string.user_data_import_wrong_kind, describe(manifest.kind)))
                return@runOnUiThread
            }
            val message = getString(
                if (kind == UserDataItem.Kind.EVERYTHING) {
                    R.string.user_data_import_everything_message
                } else {
                    R.string.user_data_import_saves_message
                },
                provenance(manifest),
            )
            ask(R.string.user_data_import_title, message, R.string.action_import) {
                runImport(kind, zip)
            }
        }
    }

    /** *"exported 5 days ago"*, or a date where the clock disagrees. */
    private fun provenance(manifest: UserDataArchive.Manifest): String {
        val now = System.currentTimeMillis()
        // a device whose clock is behind the one that wrote the archive would otherwise be told the
        // file was exported in three days' time, which reads as a bug in the archive rather than in
        // the clock. a plain date is the honest answer there.
        val when_ = if (manifest.exportedAt in 1 until now) {
            DateUtils.getRelativeTimeSpanString(
                manifest.exportedAt, now, DateUtils.MINUTE_IN_MILLIS
            ).toString()
        } else {
            SimpleDateFormat("yyyy-MM-dd", Locale.US).format(Date(manifest.exportedAt))
        }
        // the version the archive was written by is read and kept, and it is deliberately not shown:
        // what somebody deciding whether to import can act on is how old the file is.
        return getString(R.string.user_data_provenance, when_)
    }

    private fun runImport(kind: UserDataItem.Kind, zip: Uri) = busy {
        // **the bytes restored, which is the same measurement the export that wrote them reported.**
        // neither counts the manifest, so a round trip says the same number twice.
        val bytes = when (kind) {
            UserDataItem.Kind.EVERYTHING -> UserDataArchive.importEverything(this, zip)
            else -> UserDataArchive.importSaveData(this, zip)
        }
        runOnUiThread {
            done()
            if (bytes == null) {
                toast(getString(R.string.user_data_import_failed))
                return@runOnUiThread
            }
            if (kind == UserDataItem.Kind.EVERYTHING) {
                // **the toast is posted before the process ends, and it is what says the import
                // worked.** the screen this was pressed from is about to disappear, so nothing on it
                // can carry the news - and an app that simply vanishes reads as a crash rather than
                // as a success. a text toast is drawn by the system rather than by this app, so it
                // survives the process that asked for it.
                toast(getString(R.string.user_data_imported, formatted(bytes)))
                // **the process has to end here, and it is not cosmetic.** SharedPreferences is
                // cached per process: the framework is still holding the settings this import just
                // replaced on disk, and the next write of any row would put the old ones back over
                // the imported file. restarting is what makes the imported store the one that is
                // read. the delay is the toast's, so it is enqueued and on screen before the process
                // it was posted from goes away.
                after { restart() }
            } else {
                toast(getString(R.string.user_data_imported, formatted(bytes)))
                refresh()
            }
        }
    }

    // ---------------------------------------------------------------------------------------------
    // delete and reset

    private fun confirmDelete(kind: UserDataItem.Kind) = when (kind) {
        UserDataItem.Kind.EVERYTHING -> confirmWipe()
        UserDataItem.Kind.SAVE_DATA -> ask(
            R.string.user_data_delete_saves_title,
            getString(R.string.user_data_delete_saves_message),
            R.string.action_delete,
        ) { wipe(AppStorage.saveData(filesDir)) }
        else -> ask(
            R.string.user_data_delete_shaders_title,
            getString(R.string.user_data_delete_shaders_message),
            R.string.action_delete,
        ) { wipe(AppStorage.pipelineCacheRoot(filesDir)) }
    }

    private fun wipe(directory: File) = busy {
        val ok = directory.deleteRecursively() || !directory.exists()
        runOnUiThread {
            done()
            toast(getString(if (ok) R.string.user_data_deleted else R.string.user_data_delete_failed))
            refresh()
        }
    }

    /**
     * the whole install, in two questions.
     *
     * **two dialogs doing different jobs, rather than the same one twice.** a repeated *Are you
     * sure?* teaches the rhythm and gets clicked through; what earns a second tap is the second
     * question saying something the first did not. the first names what goes, counted and measured
     * from the card's own figure. the second names what it costs -- that there is no undo, and that
     * **the app closes the moment it is pressed**, which arrives as a crash if nobody said so.
     */
    private fun confirmWipe() {
        val total = adapter.everythingSize()
        ask(
            R.string.user_data_wipe_title,
            getString(R.string.user_data_wipe_message, formatted(total)),
            R.string.action_delete,
        ) {
            ask(
                R.string.user_data_wipe_final_title,
                getString(R.string.user_data_wipe_final_message),
                R.string.user_data_wipe_confirm,
            ) { wipeEverything() }
        }
    }

    /**
     * back to the just-installed state, by the platform's own route.
     *
     * **`clearApplicationUserData` rather than a recursive delete of ours**, and the reason is the
     * `SharedPreferences` cache again: this app cannot remove a file the framework is still holding
     * in memory and have the removal stick. the platform can, because it ends the process as part of
     * the same call. that is also what makes it match the words -- it is uninstall and reinstall,
     * including the external files directory, so anything staged from a PC goes with it.
     *
     * **the all-files permission is the one thing it does not take, which is measured rather than
     * assumed.** `MANAGE_EXTERNAL_STORAGE` is an app op rather than an ordinary runtime permission,
     * and it is still granted after the data directory is gone. so it is an exception here exactly as
     * it is to a settings reset -- in both cases because this app never held it in the first place;
     * [AllFiles] reads the platform live and stores nothing.
     *
     * nothing runs after this. there is no toast and no screen to return to.
     */
    private fun wipeEverything() {
        // said before the call rather than after it, because there is no after: the platform ends
        // this process as part of clearing the data. see [runImport] on why a text toast outlives
        // the app that posted it.
        toast(getString(R.string.user_data_wiped))
        after {
            val manager = getSystemService(Context.ACTIVITY_SERVICE) as ActivityManager
            if (!manager.clearApplicationUserData()) {
                toast(getString(R.string.user_data_delete_failed))
            }
        }
    }

    /**
     * runs [work] once the toast just posted is up.
     *
     * **a toast is enqueued rather than drawn**, so a process that ends in the same breath as posting
     * one can go away before the system has shown it. this is the gap between the two, and it is the
     * only thing standing between a message and the process it describes the end of.
     */
    private fun after(work: () -> Unit) =
        binding.root.postDelayed(work, TOAST_LEAD_MS)

    private fun confirmReset() = ask(
        R.string.user_data_reset_title,
        getString(R.string.user_data_reset_message),
        R.string.action_reset,
    ) {
        val settings = Settings.of(this)
        // **through the store's own API rather than by deleting the file**, for the reason the
        // everything import restarts the process: a preferences file removed underneath the framework
        // is a file the framework rewrites from memory the next time anything is set.
        settings.clearAll()
        // **and every game's own store with it**, which is what keeps this button's word good: a
        // per-game setting is a change to what a launch does, so a reset that left them in place
        // would report every row back at its default and still run one game differently. they are a
        // file each, so this is not the call above with a wider reach -- it is a second call.
        Settings.forgetEveryGame(this)
        // **and the controller mapping**, which is a setting kept as a file rather than a preference,
        // so nothing above reaches it.
        AppStorage.controllerMapping(filesDir).delete()
        // **and the grants are released rather than merely forgotten.** android caps how many
        // persisted uri permissions an app may hold, so a folder dropped from the list without
        // releasing its grant is one held against that cap forever, by nothing.
        GameLibrary.trees(this).forEach { GameLibrary.remove(this, it) }
        toast(getString(R.string.user_data_reset_done))
        // the theme is among what just went back to its default, and this is the screen it is drawn
        // on - the same restart the theme row itself takes.
        Theme.reapply(this)
    }

    // ---------------------------------------------------------------------------------------------
    // the plumbing

    /**
     * runs [work] on the worker with the toolbar's progress bar up.
     *
     * the bar is the one in `activity_manager.xml`, drawn inside the toolbar band so that starting it
     * moves nothing: an import is megabytes through a content provider, which is seconds with this
     * screen in front.
     */
    private fun busy(work: () -> Unit) {
        binding.progress.visibility = View.VISIBLE
        worker.execute(work)
    }

    private fun done() {
        binding.progress.visibility = View.GONE
    }

    private fun ask(title: Int, message: String, confirm: Int, then: () -> Unit) {
        // the message is a view rather than setMessage, for dialog_message.xml's reason: a short
        // question otherwise leaves a band of empty dialog under it taller than the question.
        val view = DialogMessageBinding.inflate(LayoutInflater.from(this))
        view.message.text = message
        MaterialAlertDialogBuilder(this)
            .setTitle(title)
            .setView(view.root)
            .setNegativeButton(R.string.cancel, null)
            .setPositiveButton(confirm) { _, _ -> then() }
            .show()
    }

    private fun toast(text: String) = Toast.makeText(this, text, Toast.LENGTH_LONG).show()

    private fun formatted(bytes: Long): String = Formatter.formatShortFileSize(this, bytes)

    private fun describe(kind: UserDataArchive.Kind): String = getString(
        if (kind == UserDataArchive.Kind.EVERYTHING) {
            R.string.user_data_everything
        } else {
            R.string.user_data_saves
        }
    )

    /**
     * ends this process and comes back to this screen, so that nothing cached outlives the import.
     *
     * **the process has to go and where it comes back to is a free choice.** `SharedPreferences` is
     * cached per process and there is no way to make the framework re-read a file that changed
     * underneath it, so the restart is what makes the imported store the one that is read -- but
     * nothing about that decides which screen is on top afterwards, and being dropped on the game
     * list reads as having been thrown out of what you were doing.
     *
     * **a stack rather than one activity**, so the way back out is the way it would have been had the
     * import not happened: this screen, then the settings scene, then the game list. relaunching
     * straight into this activity alone would be worse than the game list, because then back leaves
     * the app.
     *
     * landing here also redraws the figures, since measuring is what `onResume` does -- so the sizes
     * on screen are the imported ones rather than the ones this screen was opened with.
     */
    private fun restart() {
        val list = Intent(this, GameListActivity::class.java)
            .addFlags(Intent.FLAG_ACTIVITY_NEW_TASK or Intent.FLAG_ACTIVITY_CLEAR_TASK)
        startActivities(
            arrayOf(list, Intent(this, SettingsActivity::class.java), Intent(this, UserDataActivity::class.java))
        )
        finishAffinity()
        Runtime.getRuntime().exit(0)
    }

    private companion object {
        const val ZIP = "application/zip"

        /** long enough for a posted toast to reach the screen, short enough not to read as a stall. */
        const val TOAST_LEAD_MS = 400L
    }
}
