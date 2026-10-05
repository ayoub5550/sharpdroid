package com.mircowuffwuff.sharpdroid

import android.app.Activity
import android.content.Intent
import android.net.Uri
import android.os.Build
import android.os.Environment
import android.provider.Settings
import android.util.Log
import java.io.File

/**
 * the all-files permission, and the one thing the app does differently while it is held.
 *
 * **it is an opt-in and it is never the mechanism.** a folder the user granted is reached through a
 * content provider, file by file, and that is how the app works with nothing switched on -- every
 * reference emulator this one was modelled on does the same, and what it costs was measured rather
 * than argued: [GuestFiles] and `docs/guest-files.md`. turning this on changes nothing about which
 * folders are in the library or how they are listed. it changes exactly one thing, and only for a
 * game inside one of them: the directory becomes an ordinary path, so the guest opens its files with
 * ordinary syscalls and the file layer is never registered at all.
 *
 * **that is today's staged code path rather than a third implementation of anything** -- the same one
 * every measurement in this project was taken on -- which is why the whole feature is a branch at
 * launch. [GameListActivity] is where the branch is.
 *
 * **the permission is read at each launch and never cached.** it can be revoked from the platform's
 * own settings while the app is running, and a stale yes would be a path the app can no longer open.
 *
 * the row that turns it on is in Settings → Game files, beside the folder manager -- the two are
 * one question, which is where a library comes from and how it is reached.
 *
 * **`android.provider.Settings` is imported here and this app has a [Settings] of its own.** the
 * explicit import wins inside this file, which is correct and worth a sentence, because the two are
 * one letter apart and one of them opens a platform screen.
 */
object AllFiles {

    private const val TAG = "sharpdroid"

    /**
     * whether this device has the permission at all.
     *
     * it arrived in API 30 and `minSdk` is 28, so below that there is nothing to ask for and nothing
     * to show -- a granted game is reached through the provider, which is what it is for.
     */
    fun supported(): Boolean = Build.VERSION.SDK_INT >= Build.VERSION_CODES.R

    /** whether it is held, right now. never remembered -- see the note on the class. */
    fun granted(): Boolean = supported() && Environment.isExternalStorageManager()

    /**
     * opens the platform's own toggle for this app, and says whether anything opened.
     *
     * there is no dialog to raise: this permission is granted in Settings and nowhere else. the
     * per-app screen is the one to land on; a device without it falls back to the list of every app,
     * which is worse but is not nothing.
     */
    fun request(activity: Activity): Boolean {
        if (!supported()) return false
        val direct = Intent(
            Settings.ACTION_MANAGE_APP_ALL_FILES_ACCESS_PERMISSION,
            Uri.fromParts("package", activity.packageName, null),
        )
        if (start(activity, direct)) return true
        return start(activity, Intent(Settings.ACTION_MANAGE_ALL_FILES_ACCESS_PERMISSION))
    }

    private fun start(activity: Activity, intent: Intent): Boolean =
        try {
            activity.startActivity(intent)
            true
        } catch (e: Exception) {
            Log.w(TAG, "[app] could not open " + intent.action, e)
            false
        }

    /**
     * where a granted game's directory is on disk, or null to reach it through the provider instead.
     *
     * null is an ordinary answer and every one of its causes is: the permission is not held, the
     * device is too old to have it, the volume is not mounted, or the id does not map the way
     * [TreeDocument.path] assumes. each of those means the app does what it does by default.
     *
     * **the `eboot.bin` is what is checked rather than the directory**, and it is the same test the
     * scan applies. a derived path that exists but is not this game would otherwise be a run that
     * boots something else; a derived path that is right but empty is a dump that would fail much
     * further on, with the file layer named for it.
     */
    fun pathTo(documentId: String): File? {
        if (!granted()) return null
        val directory = TreeDocument.path(documentId) ?: return null
        if (!File(directory, Game.EBOOT).isFile) {
            Log.w(TAG, "[app] all-files access is on, but " + documentId + " does not resolve to a"
                + " readable game directory -- " + directory + " has no " + Game.EBOOT + " in it."
                + " reaching it through the grant instead")
            return null
        }
        return directory
    }
}
