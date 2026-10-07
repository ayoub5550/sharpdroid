package com.mircowuffwuff.sharpdroid

import android.app.GameManager
import android.app.GameState
import android.content.Context
import android.os.Build

/**
 * tells the platform what a run is doing -- loading, playing or paused -- through
 * `GameManager.setGameState`, android 13 and later.
 *
 * **what it is for is the boot.** a power HAL that reads this can hold the CPU up while a game says
 * it is loading, and a boot here is seconds of nothing but CPU: the emulator starting under
 * translation, then the game's own start-up. after the first frame it says the game is playing,
 * which is when the platform should stop treating it as a burst.
 *
 * **it asks for nothing and promises nothing.** what a device does with these is up to its vendor --
 * some boost, some do nothing at all -- and a call that is ignored costs one binder transaction. the
 * manifest's `appCategory="game"` is what makes the platform listen to it at all, since the game
 * manager answers only for packages that say they are games.
 *
 * every call is best-effort: a platform that refuses one is a run that goes on without the hint.
 */
object GameStateReport {

    /** the tap until the first frame: the emulator starting and the game loading behind it. */
    @JvmStatic
    fun loading(context: Context) = report(context, true, GameState.MODE_GAMEPLAY_INTERRUPTIBLE)

    /** from the first frame on, and after a resume that comes back to a running game. */
    @JvmStatic
    fun playing(context: Context) = report(context, false, GameState.MODE_GAMEPLAY_UNINTERRUPTIBLE)

    /**
     * stopped where it stands. **content rather than none**: the activity is still up and drawing
     * the paused screen, it is just not running the game.
     */
    @JvmStatic
    fun paused(context: Context) = report(context, false, GameState.MODE_CONTENT)

    private fun report(context: Context, loading: Boolean, mode: Int) {
        if (Build.VERSION.SDK_INT < Build.VERSION_CODES.TIRAMISU) return
        try {
            context.getSystemService(GameManager::class.java)?.setGameState(GameState(loading, mode))
        } catch (e: RuntimeException) {
            AppLog.w("GameStateReport", "[app] game state not reported: $e")
        }
    }
}
