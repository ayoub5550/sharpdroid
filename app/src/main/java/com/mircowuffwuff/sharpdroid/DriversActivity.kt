package com.mircowuffwuff.sharpdroid

import android.net.Uri
import android.os.Bundle
import android.util.Log
import android.view.View
import android.widget.Toast
import androidx.activity.result.contract.ActivityResultContracts
import androidx.appcompat.app.AppCompatActivity
import androidx.recyclerview.widget.GridLayoutManager
import com.google.android.material.dialog.MaterialAlertDialogBuilder
import com.mircowuffwuff.sharpdroid.databinding.ActivityManagerBinding
import java.io.File
import java.util.concurrent.Executors

/**
 * the GPU driver manager: which Vulkan drivers are on the device, which one a game loads, and how one
 * arrives.
 *
 * reached from **Settings → Graphics → Graphics driver**, and from nowhere else.
 *
 * **the system driver is pinned at the top with no delete button**, and it is what a launch loads
 * when nothing is chosen -- Eden's shape, and the same shape the build manager pins the bundled build
 * with. everything below it was either staged from a PC or imported from a zip.
 *
 * **an import is on the worker**: it reads and writes megabytes through a content provider, and the
 * scan behind it parses a `meta.json` per package across FUSE-backed external storage. either on the
 * main thread is a frozen screen and, past five seconds, a dialog offering to kill the app.
 */
class DriversActivity : AppCompatActivity() {

    private lateinit var binding: ActivityManagerBinding
    private lateinit var adapter: DriverAdapter
    private lateinit var settings: Settings
    private lateinit var drawnWith: String
    private val worker = Executors.newSingleThreadExecutor()

    private val internalRoot: File get() = AppStorage.installedDrivers(filesDir)
    private val staged: File get() = AppStorage.stagedDrivers(getExternalFilesDir(null)!!)

    /**
     * the zip picker.
     *
     * registered at construction, which the contract requires -- the activity can be recreated while
     * the picker is in front of it, and a launcher registered later than `onCreate` has nothing to
     * deliver the result to.
     *
     * **`OpenDocument` rather than `OpenDocumentTree`**: this takes one file and wants no persisted
     * grant, since the package is read once and lives in the app's own storage afterwards.
     */
    private val picker = registerForActivityResult(ActivityResultContracts.OpenDocument()) { zip ->
        // null is the back button, which is not an error and not worth a word.
        if (zip != null) importZip(zip)
    }

    override fun onCreate(state: Bundle?) {
        Theme.apply(this)
        drawnWith = Theme.signature(this)
        super.onCreate(state)
        // **which store this manager is choosing for**, forwarded by the row that opened it. a
        // per-game section sends its game, the cog's own sections send nothing, and everything below
        // -- the list, the radio, the import and the delete -- is the same screen either way: what
        // moves is only which file the selection is written to.
        settings = intent.getStringExtra(SettingsSectionActivity.EXTRA_GAME)
            ?.takeIf { it.isNotEmpty() }
            ?.let { Settings.forGame(this, it) }
            ?: Settings.of(this)
        binding = ActivityManagerBinding.inflate(layoutInflater)
        setContentView(binding.root)
        SystemBars.apply(this, binding.root)

        binding.toolbar.setTitle(R.string.setting_driver)
        binding.toolbar.setNavigationOnClickListener { finish() }

        adapter = DriverAdapter(emptyList(), this::selectSystem, this::select, this::confirmDelete)
        // **the settings scene's grid, and its column count**, because these are that scene's cards:
        // a driver is chosen by reading a handful of alternatives against each other, which is what
        // a grid is for, where a build list is scanned down a column for the newest of an id.
        // **every card is the same width, the system one included.** it spanned the row while it was
        // the only card that could not be confused for another, and that turned out to buy less than
        // it cost: a card of a different size in a grid of identical ones reads as a different *kind*
        // of thing rather than as the pinned one. it is first and it wears its own badge, which is
        // what says it is pinned; the shape says nothing the badge does not.
        binding.list.layoutManager =
            GridLayoutManager(this, resources.getInteger(R.integer.settings_section_columns))
        binding.list.adapter = adapter

        // **the mime types are a filter and never the check.** a provider is free to report a zip as
        // application/octet-stream, so narrowing to application/zip alone hides files the user can
        // see in every other app. DriverImport decides what a file actually is.
        binding.importZip.setOnClickListener {
            picker.launch(
                arrayOf(
                    "application/zip",
                    "application/x-zip-compressed",
                    "application/octet-stream",
                )
            )
        }
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
     * rescans and redraws.
     *
     * on the worker because it opens and parses a `meta.json` per package across two volumes, one of
     * which is FUSE-backed external storage -- reads there can be slow for reasons that have nothing
     * to do with how many drivers there are.
     */
    private fun refresh() {
        worker.execute {
            val listing = DriverLibrary.of(internalRoot, staged, settings.driver)
            runOnUiThread {
                if (isFinishing || isDestroyed) return@runOnUiThread
                show(listing)
            }
        }
    }

    private fun show(listing: DriverLibrary.Listing) {
        binding.progress.visibility = View.GONE
        val items = mutableListOf<DriverAdapter.Item>(
            // **the system driver first, and no sort reaches it.** it is the one row that is always
            // in the same place and always there, which is what "pinned" has to mean for it to be
            // worth anything.
            DriverAdapter.Item.System(listing.systemSelected)
        )
        listing.entries.forEach { items += DriverAdapter.Item.Driver(it) }
        adapter.submit(items)
    }

    /**
     * back to the driver the device shipped with.
     *
     * **it is stored rather than cleared**, so the row the user tapped is a choice like any other. an
     * absent setting means the same thing, which is what makes the system driver the default without
     * anything having to say so -- but a user who deliberately went back to it should not have that
     * read as never having decided.
     */
    private fun selectSystem() {
        // per-game, the row already marked is still worth a write -- see BuildsActivity.select, which
        // states the reason once for both managers.
        if (GpuDriver.isSystem(settings.driver) && !settings.perGame) return
        settings.driver = GpuDriver.SYSTEM
        Log.i(TAG, "[app] selected the system GPU driver")
        refresh()
    }

    /**
     * chooses a package. **nothing is copied here and nothing waits.**
     *
     * a staged package's library is copied onto internal storage at launch, because the linker will
     * not `dlopen` one from external storage -- [MainActivity] does that, and it is the same copy the
     * `--es driver` path has always done. an imported one is already where it has to be.
     *
     * **what is stored is the folder name**, so the choice names one package rather than a family.
     */
    private fun select(entry: DriverLibrary.Entry) {
        if (entry.selected && !settings.perGame) return
        settings.driver = entry.driver.folder
        Log.i(TAG, "[app] selected " + entry.driver.identity() + " at " + entry.driver.dir)
        refresh()
    }

    /**
     * imports a picked zip, and selects it -- which is the only reading that makes sense: importing a
     * driver is how somebody says they want to run on it.
     */
    private fun importZip(zip: Uri) {
        binding.progress.visibility = View.VISIBLE
        worker.execute {
            val result = DriverImport.import(this, zip, internalRoot)
            runOnUiThread {
                if (isFinishing || isDestroyed) return@runOnUiThread
                binding.progress.visibility = View.GONE
                when (result) {
                    is DriverImport.Result.Refused ->
                        Toast.makeText(this, result.reason, Toast.LENGTH_LONG).show()

                    is DriverImport.Result.Imported -> {
                        settings.driver = result.driver.folder
                        Toast.makeText(
                            this,
                            getString(R.string.driver_imported, result.driver.name),
                            Toast.LENGTH_LONG,
                        ).show()
                    }
                }
                refresh()
            }
        }
    }

    /**
     * **the warning is about the staged copy, not about which copy the row is showing.**
     *
     * a deletion removes both copies of a name, because the list shows one entry per folder and
     * leaving half of it behind would look like a deletion that did nothing. so the sentence about a
     * PC has to be chosen by whether a staged copy exists.
     */
    private fun confirmDelete(entry: DriverLibrary.Entry) {
        val alsoStaged = File(staged, entry.driver.folder).isDirectory
        MaterialAlertDialogBuilder(this)
            .setTitle(R.string.driver_delete_title)
            .setMessage(
                getString(
                    if (alsoStaged) R.string.driver_delete_message_staged
                    else R.string.driver_delete_message,
                    entry.driver.name,
                )
            )
            .setPositiveButton(R.string.manager_delete) { _, _ -> delete(entry) }
            .setNegativeButton(R.string.cancel, null)
            .show()
    }

    /**
     * removes a package, and moves the selection back to the system driver if it was the chosen one.
     *
     * **the replacement is the system driver rather than the next package in the list**, which is
     * where this differs from the build manager and it is not a shortcut. builds are ours and
     * interchangeable, so the newest remaining one is a reasonable substitute; driver packages come
     * from different people and target different Adreno generations, so promoting an unrelated one
     * would be the app choosing a driver nobody asked for. the system driver always works.
     */
    private fun delete(entry: DriverLibrary.Entry) {
        binding.progress.visibility = View.VISIBLE
        val folder = entry.driver.folder
        val wasChosen = settings.driver == folder
        worker.execute {
            val gone = GpuDriver.delete(entry.driver.dir)
            // both copies, when there are two: the list shows one entry per folder, so leaving the
            // staged half behind would look like a deletion that did nothing.
            GpuDriver.delete(File(internalRoot, folder))
            GpuDriver.delete(File(staged, folder))
            // and the library that was copied out of it, or the next launch loads a driver whose
            // package is gone -- a run attributed to something the manager no longer lists.
            GpuDriver.delete(AppStorage.installedDriver(cacheDir, folder))
            runOnUiThread {
                if (isFinishing || isDestroyed) return@runOnUiThread
                if (!gone) {
                    Toast.makeText(this, R.string.driver_delete_failed, Toast.LENGTH_LONG).show()
                }
                if (wasChosen) {
                    settings.driver = GpuDriver.SYSTEM
                    Toast.makeText(this, R.string.driver_now_system, Toast.LENGTH_LONG).show()
                }
                refresh()
            }
        }
    }

    private companion object {
        const val TAG = "sharpdroid"
    }
}
