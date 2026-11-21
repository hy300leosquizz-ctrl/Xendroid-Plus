package xendroid.compose.ui.compress

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
import xendroid.compose.R

/**
 * Foreground service (`foregroundServiceType="dataSync"`) keeping the main process alive while
 * multi-GB `.iso`, extracted `.xex` folder, or `GOD/SVOD` containers are compressed into a
 * multi-threaded Zstandard `.zar` archive in the background.
 */
class ZarCompressService : Service() {

    override fun onBind(intent: Intent?): IBinder? = null

    override fun onStartCommand(intent: Intent?, flags: Int, startId: Int): Int {
        when (intent?.action) {
            ACTION_STOP -> {
                stopForeground(STOP_FOREGROUND_REMOVE)
                stopSelf()
                return START_NOT_STICKY
            }
            ACTION_PROGRESS, ACTION_START -> {
                val gameTitle = intent.getStringExtra(EXTRA_TITLE).orEmpty()
                    .ifBlank { getString(R.string.lib_compress_title) }
                val percent = intent.getIntExtra(EXTRA_PERCENT, -1)
                ensureChannel()
                val notification = buildNotification(gameTitle, percent)
                if (intent.action == ACTION_START) {
                    runCatching {
                        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.Q) {
                            startForeground(NOTIFICATION_ID, notification, ServiceInfo.FOREGROUND_SERVICE_TYPE_DATA_SYNC)
                        } else {
                            startForeground(NOTIFICATION_ID, notification)
                        }
                    }
                } else {
                    getSystemService(NotificationManager::class.java)?.notify(NOTIFICATION_ID, notification)
                }
            }
        }
        return START_NOT_STICKY
    }

    private fun ensureChannel() {
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.O) {
            val mgr = getSystemService(NotificationManager::class.java) ?: return
            if (mgr.getNotificationChannel(CHANNEL_ID) == null) {
                mgr.createNotificationChannel(
                    NotificationChannel(
                        CHANNEL_ID,
                        getString(R.string.lib_compress_title),
                        NotificationManager.IMPORTANCE_LOW,
                    ).apply { setShowBadge(false) }
                )
            }
        }
    }

    private fun buildNotification(title: String, percent: Int): Notification {
        val sub = if (percent in 0..100) {
            getString(R.string.lib_compress_progress, percent)
        } else {
            getString(R.string.cz_compressing)
        }
        return NotificationCompat.Builder(this, CHANNEL_ID)
            .setSmallIcon(xendroid.compose.core.R.drawable.app_icon)
            .setContentTitle(title)
            .setContentText(sub)
            .setOngoing(true)
            .setOnlyAlertOnce(true)
            .setProgress(100, percent.coerceIn(0, 100), percent < 0)
            .build()
    }

    companion object {
        private const val CHANNEL_ID = "zar_compression"
        private const val NOTIFICATION_ID = 3604
        private const val ACTION_START = "xendroid.compose.action.ZAR_COMPRESS_START"
        private const val ACTION_PROGRESS = "xendroid.compose.action.ZAR_COMPRESS_PROGRESS"
        private const val ACTION_STOP = "xendroid.compose.action.ZAR_COMPRESS_STOP"
        private const val EXTRA_TITLE = "title"
        private const val EXTRA_PERCENT = "percent"

        fun notifyStart(context: Context, title: String) {
            runCatching {
                val intent = Intent(context, ZarCompressService::class.java)
                    .setAction(ACTION_START)
                    .putExtra(EXTRA_TITLE, title)
                    .putExtra(EXTRA_PERCENT, 0)
                if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.O) {
                    context.startForegroundService(intent)
                } else {
                    context.startService(intent)
                }
            }
        }

        fun notifyProgress(context: Context, title: String, percent: Int) {
            runCatching {
                val intent = Intent(context, ZarCompressService::class.java)
                    .setAction(ACTION_PROGRESS)
                    .putExtra(EXTRA_TITLE, title)
                    .putExtra(EXTRA_PERCENT, percent)
                context.startService(intent)
            }
        }

        fun notifyStop(context: Context) {
            runCatching {
                context.startService(Intent(context, ZarCompressService::class.java).setAction(ACTION_STOP))
            }
        }
    }
}
