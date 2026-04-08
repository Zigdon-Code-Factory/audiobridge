package com.audiobridge.audiobridge

import android.app.Notification
import android.app.NotificationChannel
import android.app.NotificationManager
import android.app.PendingIntent
import android.app.Service
import android.content.BroadcastReceiver
import android.content.Context
import android.content.Intent
import android.content.IntentFilter
import android.content.pm.ServiceInfo
import android.os.Build
import android.os.Handler
import android.os.IBinder
import android.os.Looper
import android.net.wifi.WifiManager
import android.os.PowerManager
import androidx.core.app.NotificationCompat

class AudioForegroundService : Service() {

    private val CHANNEL_ID = "AudioBridgeChannel"
    private val NOTIFICATION_ID = 1

    private var wakeLock: PowerManager.WakeLock? = null
    private var wifiLock: WifiManager.WifiLock? = null
    private var serverName: String = "AudioBridge"
    private var isMicMuted: Boolean = false
    private var isPaused: Boolean = false

    companion object {
        const val ACTION_TOGGLE_MIC = "com.audiobridge.ACTION_TOGGLE_MIC"
        const val ACTION_TOGGLE_PAUSE = "com.audiobridge.ACTION_TOGGLE_PAUSE"
        const val ACTION_DISCONNECT = "com.audiobridge.ACTION_DISCONNECT"
        const val ACTION_UPDATE_STATE = "com.audiobridge.ACTION_UPDATE_STATE"
    }

    private val actionReceiver = object : BroadcastReceiver() {
        override fun onReceive(context: Context, intent: Intent) {
            when (intent.action) {
                ACTION_TOGGLE_MIC -> {
                    isMicMuted = !isMicMuted
                    updateNotification()
                    Handler(Looper.getMainLooper()).post {
                        MainActivity.methodChannel?.invokeMethod("onMediaAction", "toggleMic")
                    }
                }
                ACTION_TOGGLE_PAUSE -> {
                    isPaused = !isPaused
                    updateNotification()
                    Handler(Looper.getMainLooper()).post {
                        MainActivity.methodChannel?.invokeMethod("onMediaAction", "togglePause")
                    }
                }
                ACTION_DISCONNECT -> {
                    Handler(Looper.getMainLooper()).post {
                        MainActivity.methodChannel?.invokeMethod("onMediaAction", "disconnect")
                    }
                }
            }
        }
    }

    override fun onCreate() {
        super.onCreate()
        createNotificationChannel()
        val filter = IntentFilter().apply {
            addAction(ACTION_TOGGLE_MIC)
            addAction(ACTION_TOGGLE_PAUSE)
            addAction(ACTION_DISCONNECT)
            addAction(ACTION_UPDATE_STATE)
        }
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.TIRAMISU) {
            registerReceiver(actionReceiver, filter, Context.RECEIVER_NOT_EXPORTED)
        } else {
            registerReceiver(actionReceiver, filter)
        }
    }

    override fun onStartCommand(intent: Intent?, flags: Int, startId: Int): Int {
        // Handle state updates from Flutter
        if (intent?.action == ACTION_UPDATE_STATE) {
            isMicMuted = intent.getBooleanExtra("micMuted", isMicMuted)
            isPaused = intent.getBooleanExtra("paused", isPaused)
            serverName = intent.getStringExtra("serverName") ?: serverName
            updateNotification()
            return START_NOT_STICKY
        }

        serverName = intent?.getStringExtra("serverName") ?: "AudioBridge"
        isMicMuted = false
        isPaused = false

        val notification = buildNotification()

        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.Q) {
            startForeground(NOTIFICATION_ID, notification, ServiceInfo.FOREGROUND_SERVICE_TYPE_MEDIA_PLAYBACK)
        } else {
            startForeground(NOTIFICATION_ID, notification)
        }

        acquireWakeLock()
        return START_NOT_STICKY
    }

    override fun onDestroy() {
        try { unregisterReceiver(actionReceiver) } catch (_: Exception) {}
        releaseWakeLock()
        super.onDestroy()
    }

    override fun onBind(intent: Intent?): IBinder? = null

    private fun buildNotification(): Notification {
        val openIntent = Intent(this, MainActivity::class.java)
        val openPending = PendingIntent.getActivity(
            this, 0, openIntent, PendingIntent.FLAG_IMMUTABLE
        )

        // Mic toggle action
        val micIntent = Intent(ACTION_TOGGLE_MIC).setPackage(packageName)
        val micPending = PendingIntent.getBroadcast(
            this, 1, micIntent, PendingIntent.FLAG_UPDATE_CURRENT or PendingIntent.FLAG_IMMUTABLE
        )

        // Pause toggle action
        val pauseIntent = Intent(ACTION_TOGGLE_PAUSE).setPackage(packageName)
        val pausePending = PendingIntent.getBroadcast(
            this, 2, pauseIntent, PendingIntent.FLAG_UPDATE_CURRENT or PendingIntent.FLAG_IMMUTABLE
        )

        // Disconnect action
        val disconnectIntent = Intent(ACTION_DISCONNECT).setPackage(packageName)
        val disconnectPending = PendingIntent.getBroadcast(
            this, 3, disconnectIntent, PendingIntent.FLAG_UPDATE_CURRENT or PendingIntent.FLAG_IMMUTABLE
        )

        val statusText = when {
            isPaused -> "Paused"
            isMicMuted -> "Streaming (mic off)"
            else -> "Streaming audio"
        }

        val micIcon = if (isMicMuted) android.R.drawable.ic_lock_silent_mode else android.R.drawable.ic_btn_speak_now
        val micLabel = if (isMicMuted) "Mic On" else "Mic Off"

        val pauseIcon = if (isPaused) android.R.drawable.ic_media_play else android.R.drawable.ic_media_pause
        val pauseLabel = if (isPaused) "Resume" else "Pause"

        return NotificationCompat.Builder(this, CHANNEL_ID)
            .setContentTitle(serverName)
            .setContentText(statusText)
            .setSmallIcon(android.R.drawable.ic_media_play)
            .setContentIntent(openPending)
            .setOngoing(true)
            .addAction(micIcon, micLabel, micPending)
            .addAction(pauseIcon, pauseLabel, pausePending)
            .addAction(android.R.drawable.ic_menu_close_clear_cancel, "Stop", disconnectPending)
            .setStyle(androidx.media.app.NotificationCompat.MediaStyle()
                .setShowActionsInCompactView(0, 1, 2))
            .build()
    }

    private fun updateNotification() {
        val manager = getSystemService(NotificationManager::class.java)
        manager?.notify(NOTIFICATION_ID, buildNotification())
    }

    private fun createNotificationChannel() {
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.O) {
            val serviceChannel = NotificationChannel(
                CHANNEL_ID,
                "AudioBridge Streaming",
                NotificationManager.IMPORTANCE_LOW
            )
            serviceChannel.description = "Audio streaming controls"
            getSystemService(NotificationManager::class.java)?.createNotificationChannel(serviceChannel)
        }
    }

    private fun acquireWakeLock() {
        val powerManager = getSystemService(Context.POWER_SERVICE) as PowerManager
        wakeLock = powerManager.newWakeLock(PowerManager.PARTIAL_WAKE_LOCK, "AudioBridge::AudioWaitLock").apply {
            acquire(24 * 60 * 60 * 1000L)
        }

        // Prevent Wi-Fi from entering power save mode — without this, the Wi-Fi chip
        // sleeps between packets and causes 50-200ms blackouts in UDP reception (audible pops).
        // WIFI_MODE_FULL_LOW_LATENCY disables PSM and DTIM coalescing entirely.
        val wifiManager = applicationContext.getSystemService(Context.WIFI_SERVICE) as WifiManager
        val lockMode = if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.Q)
            WifiManager.WIFI_MODE_FULL_LOW_LATENCY
        else
            WifiManager.WIFI_MODE_FULL_HIGH_PERF
        wifiLock = wifiManager.createWifiLock(lockMode, "AudioBridge::WifiLock").apply {
            acquire()
        }
    }

    private fun releaseWakeLock() {
        if (wakeLock?.isHeld == true) {
            wakeLock?.release()
            wakeLock = null
        }
        if (wifiLock?.isHeld == true) {
            wifiLock?.release()
            wifiLock = null
        }
    }
}
