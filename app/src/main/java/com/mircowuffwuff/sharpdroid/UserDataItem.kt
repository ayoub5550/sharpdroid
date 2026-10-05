package com.mircowuffwuff.sharpdroid

import java.io.File

/**
 * one card on the User data screen.
 *
 * **[Kind.EVERYTHING] leads, and it contains the cards under it.** nesting is what a first slot is
 * for: the whole thing is what most people came to export, and burying it in a toolbar made the one
 * common action the least visible thing on the screen. the parts below it are for somebody who wants
 * one of them, and each says its own size so the containment is legible rather than guessed at.
 *
 * **the settings card is here even though it is not a file under the user directory.** it is
 * `SharedPreferences` lines and the controller mapping's file beside `user/`, and it is still
 * something the person chose and something an export has to carry: a backup that restores a library and its saves but drops the theme, the driver and the preset
 * leaves setup work behind, which is exactly what a backup is for avoiding.
 */
data class UserDataItem(
    val kind: Kind,
    /** bytes on disk, or zero where there is nothing yet. */
    val bytes: Long,
    /**
     * what the card counts beside its size -- titles, or settings that differ from their default --
     * or null where the kind has nothing countable and the size stands alone.
     */
    val count: Int?,
    /**
     * the card's second line.
     *
     * **a card's description belongs to the screen and its title does not**, which is why only this
     * one is overridable. Save data is Save data on both screens; what it *is* differs -- "your
     * progress across titles" on the app's own screen and this game's progress on a game's -- and a
     * line naming titles in the plural under one game's artwork would be describing the wrong thing.
     */
    val description: Int = kind.description,
) {

    enum class Kind(val title: Int, val description: Int) {
        EVERYTHING(R.string.user_data_everything, R.string.user_data_everything_description),
        SAVE_DATA(R.string.user_data_saves, R.string.user_data_saves_description),
        SHADER_CACHE(R.string.user_data_shaders, R.string.user_data_shaders_description),
        SETTINGS(R.string.user_data_settings, R.string.user_data_settings_description),
    }

    companion object {

        /**
         * measures each part, walking the tree.
         *
         * **on a worker.** it is a recursive walk of a directory a long session fills, and the number
         * it produces is the only thing on a card that has to be read from disk at all.
         *
         * @param settingsChanged how many changes the card reports: the app's rows that differ from
         *   their default, every row a game overrides, and each controller port mapping anything. it is
         *   not a measurement and is not taken here.
         */
        @JvmStatic
        fun measure(filesDir: File, settingsChanged: Int): List<UserDataItem> {
            val data = filesDir.parentFile!!
            val saves = AppStorage.saveData(filesDir)
            val shaders = AppStorage.pipelineCacheRoot(filesDir)
            return listOf(
                // **exactly what the Export button packs, measured the same way it packs it.** that is
                // `files/` and `shared_prefs/` less UserDataArchive.IGNORED - not the sum of the cards
                // below, which would be short by everything no card names and would drift every time
                // one was added or taken away. the imported builds and drivers are the bulk of it, so
                // a figure measuring only the emulator's own output would understate this button by
                // two orders of magnitude.
                UserDataItem(Kind.EVERYTHING, exportable(data), null),
                // a game's saves are one directory per title id, so the count is that level's width
                // rather than a walk.
                UserDataItem(Kind.SAVE_DATA, size(saves), children(saves)),
                UserDataItem(Kind.SHADER_CACHE, size(shaders), children(shaders)),
                UserDataItem(Kind.SETTINGS, 0, settingsChanged),
            )
        }

        /**
         * the two parts of one title, for the per-game screen.
         *
         * **no count on either card.** on the app's screen the figure counts titles, which is the
         * thing that varies there; here it is one by construction, and a card reading "4.1 MB · 1
         * title" would be spending a clause to say what the screen is already about.
         *
         * **Save data is measured even when [titleId] is shared**, and the caller is what decides
         * whether to draw the card at all -- see [Game.sharesSaveDirectory]. measuring is honest
         * either way; offering to act on it is not.
         */
        @JvmStatic
        fun measureGame(filesDir: File, titleId: String): List<UserDataItem> {
            val saves = File(AppStorage.saveData(filesDir), titleId)
            val shaders = AppStorage.pipelineCacheOf(filesDir, titleId)
            return listOf(
                UserDataItem(
                    Kind.SAVE_DATA,
                    size(saves),
                    null,
                    R.string.game_user_data_saves_description,
                ),
                // **the shader card keeps the app's own line.** "compiled as you play and rebuilt
                // if deleted" is true of one game in the same words it is true of all of them, and
                // a version naming the game spent its whole width saying so and was ellipsized.
                UserDataItem(Kind.SHADER_CACHE, size(shaders), null),
            )
        }

        /** how many entries sit directly under [dir], or null where it does not exist yet. */
        private fun children(dir: File): Int? = dir.listFiles()?.count { it.isDirectory }

        /** bytes an Everything export would carry: the two roots it packs, less what it skips. */
        private fun exportable(data: File): Long =
            listOf("files", "shared_prefs").sumOf { top -> exportable(data, File(data, top)) }

        private fun exportable(data: File, file: File): Long {
            val name = file.absolutePath
                .removePrefix(data.absolutePath + File.separator)
                .replace(File.separatorChar, '/')
            if (name in UserDataArchive.IGNORED) return 0L
            if (file.isDirectory) return file.listFiles()?.sumOf { exportable(data, it) } ?: 0L
            return if (file.isFile) file.length() else 0L
        }

        /**
         * bytes under [file], following the tree.
         *
         * `listFiles` answers null for a path that is not a directory and for one that cannot be
         * read; both are zero here, which is what a card with nothing in it should say.
         */
        private fun size(file: File): Long = when {
            file.isFile -> file.length()
            else -> file.listFiles()?.sumOf { size(it) } ?: 0L
        }
    }
}
