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
import android.content.pm.PackageManager
import android.content.pm.ServiceInfo
import android.media.AudioAttributes
import android.media.AudioFormat
import android.media.AudioManager
import android.media.AudioTrack
import android.support.v4.media.session.MediaSessionCompat
import android.support.v4.media.session.PlaybackStateCompat
import android.util.Log
import android.view.KeyEvent
import androidx.core.content.ContextCompat
import kotlin.math.PI
import kotlin.math.min
import kotlin.math.sin
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
    private var mediaPlaying: Boolean = false
    private var mediaSession: MediaSessionCompat? = null
    private var lastHeadsetToggle = 0L
    private var pendingHeadsetStartCueUntil = 0L

    companion object {
        const val ACTION_TOGGLE_MIC = "com.audiobridge.ACTION_TOGGLE_MIC"
        const val ACTION_TOGGLE_PAUSE = "com.audiobridge.ACTION_TOGGLE_PAUSE"
        const val ACTION_DISCONNECT = "com.audiobridge.ACTION_DISCONNECT"
        const val ACTION_UPDATE_STATE = "com.audiobridge.ACTION_UPDATE_STATE"
        const val ACTION_ENABLE_MIC = "com.audiobridge.ACTION_ENABLE_MIC"
        private const val TAG = "AudioBridge"

        @Volatile
        private var instance: AudioForegroundService? = null

        fun triggerHeadsetToggle() {
            instance?.onHeadsetToggle()
        }
    }

    private val actionReceiver = object : BroadcastReceiver() {
        override fun onReceive(context: Context, intent: Intent) {
            when (intent.action) {
                ACTION_TOGGLE_MIC -> {
                    if (NativeMic.isRunning()) {
                        toggleSoftMute()
                    } else {
                        Handler(Looper.getMainLooper()).post {
                            MainActivity.methodChannel?.invokeMethod("onMediaAction", "toggleMic")
                        }
                    }
                }
                ACTION_TOGGLE_PAUSE -> {
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
        instance = this
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
        createMediaSession()
    }

    // Headset double-tap (AVRCP next track in A2DP mode; Telecom AT+CHUP in BT SCO mode)
    // mutes or unmutes the microphone while it keeps running.
    private fun createMediaSession() {
        val session = MediaSessionCompat(this, "AudioBridge")
        session.setCallback(object : MediaSessionCompat.Callback() {
            override fun onMediaButtonEvent(mediaButtonEvent: Intent): Boolean {
                @Suppress("DEPRECATION")
                val event = mediaButtonEvent.getParcelableExtra<KeyEvent>(Intent.EXTRA_KEY_EVENT)
                    ?: return super.onMediaButtonEvent(mediaButtonEvent)
                Log.i(TAG, "Media button event: keyCode=${event.keyCode} action=${event.action} repeat=${event.repeatCount}")
                when (event.keyCode) {
                    KeyEvent.KEYCODE_MEDIA_NEXT -> {
                        if (event.action == KeyEvent.ACTION_DOWN && event.repeatCount == 0) onHeadsetToggle()
                        return true
                    }
                    KeyEvent.KEYCODE_MEDIA_PLAY_PAUSE, KeyEvent.KEYCODE_MEDIA_PLAY,
                    KeyEvent.KEYCODE_MEDIA_PAUSE -> {
                        // Consume single-tap without toggling mute so double-tap is standard
                        // across both phone-mic (A2DP) and Bluetooth-mic (SCO) modes.
                        return true
                    }
                }
                return super.onMediaButtonEvent(mediaButtonEvent)
            }
            override fun onSkipToNext() {
                Log.i(TAG, "MediaSession onSkipToNext")
                onHeadsetToggle()
            }
        })
        session.setPlaybackState(PlaybackStateCompat.Builder()
            .setActions(PlaybackStateCompat.ACTION_SKIP_TO_NEXT or
                PlaybackStateCompat.ACTION_PLAY_PAUSE or PlaybackStateCompat.ACTION_PLAY or
                PlaybackStateCompat.ACTION_PAUSE)
            .setState(PlaybackStateCompat.STATE_PLAYING, PlaybackStateCompat.PLAYBACK_POSITION_UNKNOWN, 1f)
            .build())
        session.isActive = true
        mediaSession = session
    }

    private fun onHeadsetToggle() {
        // Some headsets deliver one tap as both a key event and a play/pause command.
        val now = android.os.SystemClock.elapsedRealtime()
        if (now - lastHeadsetToggle < 400) return
        lastHeadsetToggle = now
        val running = NativeMic.isRunning()
        Log.i(TAG, "Headset toggle: micRunning=$running micMuted=${NativeMic.isMuted()}")
        if (running) {
            toggleSoftMute()
        } else if (MainActivity.methodChannel != null) {
            pendingHeadsetStartCueUntil = now + 5000L
            Handler(Looper.getMainLooper()).post {
                MainActivity.methodChannel?.invokeMethod("onMediaAction", "toggleMic")
            }
        } else {
            playCue(muted = true)
        }
    }

    private fun toggleSoftMute() {
        val muted = !NativeMic.isMuted()
        NativeMic.setMuted(muted)
        isMicMuted = muted
        Log.i(TAG, "Soft mute toggled: muted=$muted")
        updateNotification()
        playCue(muted)
        Handler(Looper.getMainLooper()).post {
            MainActivity.methodChannel?.invokeMethod("onMicMuted", muted)
        }
    }

    /** Two short tones: rising when the mic turns on, falling when it is muted. */
    private fun playCue(muted: Boolean) {
        Thread {
            try {
                val rate = 48000
                val freqs = if (muted) doubleArrayOf(880.0, 440.0) else doubleArrayOf(523.0, 1046.0)
                val toneLen = rate * 90 / 1000
                val gapLen = rate * 30 / 1000
                val samples = ShortArray(toneLen * 2 + gapLen)
                val fade = rate * 8 / 1000
                for ((t, f) in freqs.withIndex()) {
                    val offset = t * (toneLen + gapLen)
                    for (i in 0 until toneLen) {
                        val env = min(1.0, min(i, toneLen - 1 - i).toDouble() / fade)
                        samples[offset + i] = (sin(2 * PI * f * i / rate) * env * 0.35 * Short.MAX_VALUE).toInt().toShort()
                    }
                }
                val audioManager = getSystemService(Context.AUDIO_SERVICE) as AudioManager
                val inCall = audioManager.mode == AudioManager.MODE_IN_COMMUNICATION
                val attributes = AudioAttributes.Builder()
                    .setUsage(if (inCall) AudioAttributes.USAGE_VOICE_COMMUNICATION else AudioAttributes.USAGE_MEDIA)
                    .setContentType(AudioAttributes.CONTENT_TYPE_SONIFICATION)
                    .build()
                val track = AudioTrack.Builder()
                    .setAudioAttributes(attributes)
                    .setAudioFormat(AudioFormat.Builder()
                        .setEncoding(AudioFormat.ENCODING_PCM_16BIT)
                        .setSampleRate(rate)
                        .setChannelMask(AudioFormat.CHANNEL_OUT_MONO)
                        .build())
                    .setTransferMode(AudioTrack.MODE_STATIC)
                    .setBufferSizeInBytes(samples.size * 2)
                    .build()
                try {
                    if (track.state == AudioTrack.STATE_UNINITIALIZED) {
                        Log.w(TAG, "Mute cue AudioTrack uninitialized")
                        return@Thread
                    }
                    val written = track.write(samples, 0, samples.size)
                    if (written <= 0 || track.state != AudioTrack.STATE_INITIALIZED) {
                        Log.w(TAG, "Mute cue AudioTrack write failed (written=$written state=${track.state})")
                        return@Thread
                    }
                    track.play()
                    Thread.sleep((samples.size * 1000L / rate) + 300)
                } finally {
                    track.release()
                }
            } catch (e: Exception) {
                Log.w(TAG, "Mute cue failed: ${e.message}")
            }
        }.start()
    }

    // The microphone type keeps mic access while the app is in the background (Android 14+
    // silences it otherwise). It can only be claimed with RECORD_AUDIO granted.
    private fun startForegroundWithTypes(notification: Notification) {
        if (Build.VERSION.SDK_INT < Build.VERSION_CODES.Q) {
            startForeground(NOTIFICATION_ID, notification)
            return
        }
        val micGranted = ContextCompat.checkSelfPermission(this, android.Manifest.permission.RECORD_AUDIO) ==
            PackageManager.PERMISSION_GRANTED
        if (micGranted && Build.VERSION.SDK_INT >= Build.VERSION_CODES.R) {
            try {
                startForeground(NOTIFICATION_ID, notification,
                    ServiceInfo.FOREGROUND_SERVICE_TYPE_MEDIA_PLAYBACK or ServiceInfo.FOREGROUND_SERVICE_TYPE_MICROPHONE)
                return
            } catch (e: Exception) {
                Log.w(TAG, "Microphone foreground type unavailable: ${e.message}")
            }
        }
        try {
            startForeground(NOTIFICATION_ID, notification, ServiceInfo.FOREGROUND_SERVICE_TYPE_MEDIA_PLAYBACK)
        } catch (e: Exception) {
            Log.w(TAG, "Media playback foreground start failed: ${e.message}")
        }
    }

    override fun onStartCommand(intent: Intent?, flags: Int, startId: Int): Int {
        // Handle state updates from Flutter
        if (intent?.action == ACTION_UPDATE_STATE) {
            isMicMuted = intent.getBooleanExtra("micMuted", isMicMuted)
            isPaused = intent.getBooleanExtra("paused", isPaused)
            mediaPlaying = intent.getBooleanExtra("mediaPlaying", mediaPlaying)
            serverName = intent.getStringExtra("serverName") ?: serverName
            updateNotification()
            return START_NOT_STICKY
        }
        if (intent?.action == ACTION_ENABLE_MIC) {
            // Sent when the microphone starts, in case the service started before mic permission.
            startForegroundWithTypes(buildNotification())
            if (android.os.SystemClock.elapsedRealtime() <= pendingHeadsetStartCueUntil) {
                pendingHeadsetStartCueUntil = 0L
                playCue(muted = false)
            }
            return START_NOT_STICKY
        }

        serverName = intent?.getStringExtra("serverName") ?: "AudioBridge"
        isMicMuted = false
        isPaused = false
        mediaPlaying = false

        val notification = buildNotification()

        startForegroundWithTypes(notification)

        acquireWakeLock()
        return START_NOT_STICKY
    }

    override fun onDestroy() {
        if (instance === this) instance = null
        try { unregisterReceiver(actionReceiver) } catch (_: Exception) {}
        mediaSession?.run { isActive = false; release() }
        mediaSession = null
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

        val pauseIcon = if (mediaPlaying) android.R.drawable.ic_media_pause else android.R.drawable.ic_media_play
        val pauseLabel = if (mediaPlaying) "Pause" else "Play"

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
        if (wakeLock?.isHeld != true) {
            val powerManager = getSystemService(Context.POWER_SERVICE) as PowerManager
            wakeLock = powerManager.newWakeLock(PowerManager.PARTIAL_WAKE_LOCK, "AudioBridge::AudioWaitLock").apply {
                acquire(24 * 60 * 60 * 1000L)
            }
        }

        // Prevent Wi-Fi from entering power save mode — without this, the Wi-Fi chip
        // sleeps between packets and causes 50-200ms blackouts in UDP reception (audible pops).
        // WIFI_MODE_FULL_LOW_LATENCY disables PSM and DTIM coalescing entirely.
        if (wifiLock?.isHeld != true) {
            val wifiManager = applicationContext.getSystemService(Context.WIFI_SERVICE) as WifiManager
            val lockMode = if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.Q)
                WifiManager.WIFI_MODE_FULL_LOW_LATENCY
            else
                WifiManager.WIFI_MODE_FULL_HIGH_PERF
            wifiLock = wifiManager.createWifiLock(lockMode, "AudioBridge::WifiLock").apply {
                acquire()
            }
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
