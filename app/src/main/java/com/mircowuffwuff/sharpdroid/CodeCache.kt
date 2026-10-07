package com.mircowuffwuff.sharpdroid

import java.io.File
import java.security.MessageDigest

/**
 * FEXCore's disk cache of translated code -- `--fex DiskCache=1` -- kept per build and held under a
 * size limit, which is what lets a launch turn it on without being asked.
 *
 * **what it buys is measured.** on a Snapdragon 8 Gen 3 a ReadyToRun payload reaches `Calling guest
 * entry` in 2.4-2.5 s cold and 1.50-1.56 s with a warm cache, and the run that writes the cache was
 * no slower than one without it. the same translations are what a game hits the first time it walks
 * into new code, so a title played before also re-translates less of itself mid-run: that half is
 * the argument rather than a measurement, since no game was run where the figure was taken.
 *
 * **what kept it opt-in was that nothing bounded it**, and the two halves of the bound are here:
 *
 * - **a directory per build.** FEX keys an entry by the guest bytes and its own configuration and
 *   checks its version in every file it loads, so mixing builds in one directory is never *wrong* --
 *   but every new payload adds a set of entries beside the old ones rather than replacing them, and
 *   an IL-only payload grew its cache from 55 to 83 MB over three runs. a payload's directory is
 *   named for the payload file and for the install of this app that runs it, so an update of either
 *   starts a fresh one and the old one becomes the first thing [prepare] removes.
 * - **a limit over all of them**, [LIMIT_BYTES], settled before each run. directories other than the
 *   one about to be used go first, least recently used first; the one about to be used is started
 *   over only if it alone is over the limit. one run can still carry it past the limit while it
 *   writes, and the next launch takes it back.
 *
 * **the cache directory, because every byte here is derived.** losing it costs one cold start, so it
 * is exactly what the platform is allowed to reclaim under storage pressure, and *Clear cache* in the
 * system's app info screen is a way to drop it that needs nothing of ours. it is never exported.
 */
object CodeCache {

    /**
     * the most this may hold across every build, before a run writes more.
     *
     * **half a gigabyte is several warm titles**: a trivial guest on a ReadyToRun payload settled at
     * 44-46 MB, and most of that is the emulator rather than the game.
     */
    const val LIMIT_BYTES = 512L shl 20

    /** where every build's directory sits. */
    @JvmStatic
    fun root(cacheDir: File): File = File(cacheDir, "fex-code-cache")

    /**
     * the directory one payload, run by one install of this app, keeps its translations in.
     *
     * **the payload's path, length and modification time, rather than a hash of its bytes.** it is
     * hundreds of megabytes read on the way to every launch, and each of these moves when the file is
     * re-staged, re-unpacked or replaced. the install time stands in for the host layer, since an
     * update of the APK is the only way that changes -- and with it, possibly, FEX.
     */
    @JvmStatic
    fun directoryFor(cacheDir: File, installedAt: Long, payload: File): File {
        val identity = listOf(
            installedAt.toString(),
            payload.absolutePath,
            payload.length().toString(),
            payload.lastModified().toString(),
        ).joinToString("\n")
        val digest = MessageDigest.getInstance("SHA-256").digest(identity.toByteArray())
        return File(root(cacheDir), digest.take(8).joinToString("") { "%02x".format(it) })
    }

    /**
     * makes room for a run that is about to use [current], and creates it.
     *
     * **called on the launch thread before the host layer starts**, so nothing is writing to any of
     * these while they are measured or deleted: this process runs one guest and is killed after it.
     *
     * @return the bytes removed, for the launch log.
     */
    @JvmStatic
    @JvmOverloads
    fun prepare(cacheDir: File, current: File, limit: Long = LIMIT_BYTES): Long {
        val existing = root(cacheDir).listFiles { file -> file.isDirectory }.orEmpty()
        val sizes = existing.associateWith(::size)
        var total = sizes.values.sum()
        var removed = 0L

        fun drop(directory: File) {
            val bytes = sizes[directory] ?: 0
            directory.deleteRecursively()
            total -= bytes
            removed += bytes
        }

        // **this build's own first, and only when it alone is too big.** it is the directory the run
        // is about to read, so it is the last thing worth losing -- unless keeping it would leave the
        // limit unreachable however much else went.
        if ((sizes[current] ?: 0) > limit) {
            drop(current)
        }
        // **then the others, least recently used first.** the order is the time a run last asked for
        // each, which is stamped below rather than read off the files: FEX touches its own files
        // inside a directory, and a directory's own time moves only when an entry in it is added or
        // removed.
        for (directory in existing.filter { it != current }.sortedBy { it.lastModified() }) {
            if (total <= limit) break
            drop(directory)
        }

        current.mkdirs()
        current.setLastModified(System.currentTimeMillis())
        return removed
    }

    /** what a directory holds, all the way down. */
    private fun size(directory: File): Long =
        directory.walkBottomUp().filter { it.isFile }.sumOf { it.length() }
}
