package com.mircowuffwuff.sharpdroid

import android.app.Notification
import android.app.NotificationChannel
import android.app.NotificationManager
import android.app.Service
import android.content.Context
import android.content.Intent
import android.content.pm.ServiceInfo
import android.os.Build
import android.os.IBinder
import androidx.core.app.NotificationCompat
import androidx.core.app.ServiceCompat

/**
 * the foreground service a game holds for as long as it is up, which is what keeps it in memory while
 * the app is left.
 *
 * **it lives in `:guest`, the process the game is in, and that is the whole of why it works.** a
 * process with nothing visible in it is a cached one, and a cached process holding several gigabytes
 * is the first thing the low-memory killer takes. a foreground service is android's answer to that,
 * and it protects the process it runs in -- so it has to be the guest's process, rather than the game
 * list's one, which holds nothing worth keeping.
 *
 * **it shows nothing, and that decides where it runs at all: android 13 and later only.** android
 * starts no foreground service without a notification, and from 13 an app that does not declare
 * `POST_NOTIFICATIONS` -- which this one does not -- can never have one shown. what remains visible is
 * the app's line among the active apps in quick settings, which android keeps for every foreground
 * service and which no app can opt out of. on 12 and earlier the notification would always be in the
 * shade, so the service is not started there and a game left in the background is a cached process
 * like any other.
 *
 * **it must not outlive the run.** the process ends with the run, and a service restarted into a new
 * process would be a line in active apps for a game that is not there. [START_NOT_STICKY] alone does
 * not promise that, since android restarts a service that died holding a start it had not delivered,
 * so the activity stops the service before it ends the process, and a restart that happens anyway
 * finds no game and stops.
 */
class GuestService : Service() {

    override fun onBind(intent: Intent?): IBinder? = null

    override fun onStartCommand(intent: Intent?, flags: Int, startId: Int): Int {
        ServiceCompat.startForeground(
            this,
            NOTIFICATION,
            notification(),
            if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.UPSIDE_DOWN_CAKE) {
                ServiceInfo.FOREGROUND_SERVICE_TYPE_SPECIAL_USE
            } else {
                0
            },
        )
        // **a process android started for this service by itself holds no game, and the service goes
        // at once.** that is a restart: android restarts a service whose process died with a start it
        // had not yet delivered, whatever the service returned. the foreground call above still has to
        // come first, because a service started as a foreground one that does not make it is one
        // android treats as a crash.
        if (!hostsGame) {
            AppLog.i(TAG, "[app] the game's service was started where there is no game, and stops")
            ServiceCompat.stopForeground(this, ServiceCompat.STOP_FOREGROUND_REMOVE)
            stopSelf()
        }
        return START_NOT_STICKY
    }

    /**
     * the notification android requires before it will run the service, and which it never shows --
     * see the class comment. it carries what a notification must and nothing a person would read.
     */
    private fun notification(): Notification {
        getSystemService(NotificationManager::class.java)?.createNotificationChannel(
            NotificationChannel(CHANNEL, getString(R.string.guest_channel), NotificationManager.IMPORTANCE_LOW)
        )
        return NotificationCompat.Builder(this, CHANNEL)
            .setSmallIcon(R.drawable.ic_notification)
            .setOngoing(true)
            .setSilent(true)
            .build()
    }

    companion object {
        private const val CHANNEL = "game"
        private const val NOTIFICATION = 1

        /**
         * whether this process holds a game, which is to say whether [start] was called in it. the
         * service runs in the game's own process, so a process where this is false is one android
         * started for the service alone.
         */
        @Volatile
        private var hostsGame = false

        /**
         * the game is up. called while the activity is on screen, which is when android allows a
         * foreground service to be started at all. does nothing below android 13 -- see the class
         * comment.
         */
        @JvmStatic
        fun start(context: Context) {
            hostsGame = true
            if (Build.VERSION.SDK_INT < Build.VERSION_CODES.TIRAMISU) {
                return
            }
            try {
                context.startForegroundService(Intent(context, GuestService::class.java))
            } catch (e: RuntimeException) {
                // a refusal costs the protection and nothing else: the game runs exactly as it would
                // have without the service, and is as exposed as it always was while the app is left.
                AppLog.e(TAG, "[app] the game's foreground service could not start", e)
            }
        }

        /**
         * the run is over. called before the process is ended, so that android has no started
         * service left to restart into a new one.
         */
        @JvmStatic
        fun stop(context: Context) {
            context.stopService(Intent(context, GuestService::class.java))
        }

        private const val TAG = "sharpdroid"
    }
}
