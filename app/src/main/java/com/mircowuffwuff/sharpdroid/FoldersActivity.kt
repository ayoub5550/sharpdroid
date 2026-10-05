package com.mircowuffwuff.sharpdroid

import android.net.Uri
import android.os.Bundle
import android.view.View
import android.widget.Toast
import androidx.activity.result.contract.ActivityResultContracts
import androidx.appcompat.app.AppCompatActivity
import androidx.recyclerview.widget.LinearLayoutManager
import com.google.android.material.dialog.MaterialAlertDialogBuilder
import com.mircowuffwuff.sharpdroid.databinding.ActivityManagerBinding
import java.util.concurrent.Executors

/**
 * the game folder manager: which folders the user granted, how one arrives, and how one goes.
 *
 * reached from **Settings → Game files → Game folders**, and from nowhere else -- the game list's toolbar
 * carries only the cog, because adding a folder is done a handful of times and reading the list is
 * done every launch. its empty state offers the picker directly rather than this screen: with no
 * folders yet there is nothing here to manage.
 *
 * **the same screen the build and driver managers are**, down to the layout -- a toolbar over a list
 * with the add button floating at the bottom right. a folder is not a package and none of the
 * importing applies, but a list of things with one destructive action each is the shape those two
 * already are, and a third one that looked different would be a third thing to learn.
 *
 * **a row is a granted folder rather than a game.** what is listed is what the user picked; how many
 * games are inside one is the game list's question and it answers it by scanning.
 *
 * **taking a grant queries a provider and so does dropping one**, so both are on the worker. the
 * store behind them is [GameLibrary], which is also what the game list scans.
 */
class FoldersActivity : AppCompatActivity() {

    private lateinit var binding: ActivityManagerBinding
    private lateinit var adapter: FolderAdapter
    private lateinit var drawnWith: String
    private val worker = Executors.newSingleThreadExecutor()

    /**
     * the directory picker.
     *
     * registered at construction, which the contract requires -- the activity can be recreated while
     * the picker is in front of it, and a launcher registered later than `onCreate` has nothing to
     * deliver the result to.
     *
     * **`OpenDocumentTree` rather than `OpenDocument`**, and the grant it returns is persisted: the
     * app reads these files for as long as the folder is in the library, which is the opposite of the
     * one-shot read a driver or build zip gets.
     */
    private val picker = registerForActivityResult(ActivityResultContracts.OpenDocumentTree()) { tree ->
        // null is the back button, which is not an error and not worth a word.
        if (tree != null) add(tree)
    }

    override fun onCreate(state: Bundle?) {
        Theme.apply(this)
        drawnWith = Theme.signature(this)
        super.onCreate(state)
        binding = ActivityManagerBinding.inflate(layoutInflater)
        setContentView(binding.root)
        SystemBars.apply(this, binding.root)

        binding.toolbar.setTitle(R.string.setting_folders)
        binding.toolbar.setNavigationOnClickListener { finish() }

        adapter = FolderAdapter(emptyList(), this::confirmRemove)
        // a column rather than the managers' grid: a row is one line of text with one button on it,
        // and there is nothing to read across.
        binding.list.layoutManager = LinearLayoutManager(this)
        binding.list.adapter = adapter

        binding.empty.setText(R.string.folders_none)
        binding.importZip.setText(R.string.folders_add)
        // no initial uri: the picker then opens wherever it was left, which is the right answer for
        // somebody adding a second folder beside the first.
        binding.importZip.setOnClickListener { picker.launch(null) }
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
     * rereads the store and redraws.
     *
     * on the worker because [GameLibrary.trees] cross-checks the stored list against the grants
     * android says the app still holds, which is a binder round trip.
     */
    private fun refresh() {
        worker.execute {
            val trees = GameLibrary.trees(this)
            runOnUiThread {
                if (isFinishing || isDestroyed) return@runOnUiThread
                show(trees)
            }
        }
    }

    private fun show(trees: List<Uri>) {
        binding.progress.visibility = View.GONE
        adapter.submit(trees.map { FolderAdapter.Item(it, GameLibrary.label(it)) })
        binding.empty.visibility = if (trees.isEmpty()) View.VISIBLE else View.GONE
    }

    /**
     * puts a picked folder in the library.
     *
     * what it says about each outcome is [GameLibrary.message]'s, since the game list's empty state
     * offers the same picker and the two may not word a refusal differently.
     */
    private fun add(tree: Uri) {
        binding.progress.visibility = View.VISIBLE
        worker.execute {
            val added = GameLibrary.add(this, tree)
            val label = GameLibrary.label(tree)
            runOnUiThread {
                if (isFinishing || isDestroyed) return@runOnUiThread
                Toast.makeText(this, GameLibrary.message(this, added, label), Toast.LENGTH_LONG)
                    .show()
                refresh()
            }
        }
    }

    /**
     * **the confirmation says the files are not touched**, because removing a folder from a library is
     * the kind of thing that reads as deleting it. what actually goes is this app's access.
     */
    private fun confirmRemove(item: FolderAdapter.Item) {
        MaterialAlertDialogBuilder(this)
            .setTitle(R.string.folders_remove_title)
            .setMessage(getString(R.string.folders_remove_message, item.label))
            .setPositiveButton(R.string.folders_remove) { _, _ -> remove(item.tree) }
            .setNegativeButton(R.string.cancel, null)
            .show()
    }

    private fun remove(tree: Uri) {
        binding.progress.visibility = View.VISIBLE
        worker.execute {
            GameLibrary.remove(this, tree)
            runOnUiThread {
                if (isFinishing || isDestroyed) return@runOnUiThread
                refresh()
            }
        }
    }
}
