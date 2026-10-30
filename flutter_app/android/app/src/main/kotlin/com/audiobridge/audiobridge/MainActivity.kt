package com.audiobridge.audiobridge

import android.bluetooth.BluetoothAdapter
import android.bluetooth.BluetoothManager
import android.bluetooth.BluetoothProfile
import android.content.BroadcastReceiver
import android.content.Context
import android.content.Intent
import android.content.IntentFilter
import android.database.ContentObserver
import android.media.AudioDeviceInfo
import android.media.AudioManager
import android.os.Build
import android.os.Handler
import android.os.Looper
import android.provider.Settings
import android.content.pm.PackageManager
import android.util.Log
import androidx.core.content.ContextCompat
import io.flutter.embedding.android.FlutterActivity
import io.flutter.embedding.engine.FlutterEngine
import io.flutter.plugin.common.MethodChannel
import kotlin.math.pow

class MainActivity : FlutterActivity() {
    private val CHANNEL = "com.audiobridge/audio"
    private var nativeStarted = false
    private var bluetoothScoActive = false
    private var volumeReceiverRegistered = false

    private val volumeReceiver = object : BroadcastReceiver() {
        override fun onReceive(context: Context?, intent: Intent?) {
            syncSystemVolume()
        }
    }

    private val volumeObserver = object : ContentObserver(Handler(Looper.getMainLooper())) {
        override fun onChange(selfChange: Boolean) {
            super.onChange(selfChange)
            syncSystemVolume()
        }
    }

    companion object {
        var methodChannel: MethodChannel? = null
        private const val TAG = "AudioBridge"

        init {
            System.loadLibrary("native_audio")
        }
    }

    // Existing native methods
    private external fun nativeStartAudio(): Boolean
    private external fun nativeStopAudio()
    private external fun nativeFeedAudio(data: ByteArray)
    private external fun nativeGetLatency(): Double
    private external fun nativeStartRecording(serverIp: String, port: Int): Boolean
    private external fun nativeStopRecording()
    private external fun nativeSetMuted(muted: Boolean)
    private external fun nativeSetVolume(volume: Float)
    private external fun nativeSetSystemVolume(volume: Float)
    private external fun nativeGetLatencyBreakdown(): DoubleArray
    private external fun nativeGetAudioState(): DoubleArray
    private external fun nativeGetOutputPeakLevel(): Float
    private external fun nativeGetInputPeakLevel(): Float

    // New ConnectionManager native methods
    private external fun nativeConnect(serverIp: String, serverPort: Int,
                                       deviceName: String, deviceId: String,
                                       pskHex: String): String
    private external fun nativeDisconnect()
    private external fun nativeSendControl(cmd: Int)
    private external fun nativeSendKeepalive()
    private external fun nativeSendPing()
    private external fun nativeGetRtt(): Double
    private external fun nativeIsConnected(): Boolean
    private external fun nativeIsDtlsActive(): Boolean

    // Called from native recv thread via JNI to notify Dart of events
    @Suppress("unused")
    fun onNativeEvent(method: String, data: String) {
        runOnUiThread {
            try {
                methodChannel?.invokeMethod(method, data)
            } catch (e: Exception) {
                Log.w(TAG, "Failed to invoke Dart method '$method': ${e.message}")
            }
        }
    }

    private fun isBluetoothHeadsetConnected(): Boolean {
        try {
            if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.S) {
                if (ContextCompat.checkSelfPermission(this, android.Manifest.permission.BLUETOOTH_CONNECT) != PackageManager.PERMISSION_GRANTED) {
                    return false
                }
            }
            val bluetoothManager = getSystemService(Context.BLUETOOTH_SERVICE) as? BluetoothManager
            val adapter = bluetoothManager?.adapter ?: return false
            // Check if any headset or hands-free device is connected
            return adapter.getProfileConnectionState(BluetoothProfile.HEADSET) == BluetoothProfile.STATE_CONNECTED
        } catch (e: Exception) {
            Log.w(TAG, "Error checking Bluetooth headset: ${e.message}")
            return false
        }
    }

    private fun startBluetoothSco() {
        if (bluetoothScoActive) return
        try {
            val audioManager = getSystemService(Context.AUDIO_SERVICE) as AudioManager
            audioManager.mode = AudioManager.MODE_IN_COMMUNICATION
            audioManager.startBluetoothSco()
            audioManager.isBluetoothScoOn = true
            bluetoothScoActive = true
            Log.i(TAG, "Bluetooth SCO started - using BT mic")
        } catch (e: Exception) {
            Log.w(TAG, "Failed to start Bluetooth SCO: ${e.message}")
        }
    }

    private fun stopBluetoothSco() {
        if (!bluetoothScoActive) return
        try {
            val audioManager = getSystemService(Context.AUDIO_SERVICE) as AudioManager
            audioManager.isBluetoothScoOn = false
            audioManager.stopBluetoothSco()
            audioManager.mode = AudioManager.MODE_NORMAL
            bluetoothScoActive = false
            Log.i(TAG, "Bluetooth SCO stopped")
        } catch (e: Exception) {
            Log.w(TAG, "Failed to stop Bluetooth SCO: ${e.message}")
        }
    }

    private fun syncSystemVolume() {
        try {
            val audioManager = getSystemService(Context.AUDIO_SERVICE) as AudioManager
            val maxVol = audioManager.getStreamMaxVolume(AudioManager.STREAM_MUSIC)
            val minVol = if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.P) {
                audioManager.getStreamMinVolume(AudioManager.STREAM_MUSIC)
            } else 0
            val curVol = audioManager.getStreamVolume(AudioManager.STREAM_MUSIC)
            val isMuted = if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.M) {
                audioManager.isStreamMute(AudioManager.STREAM_MUSIC)
            } else (curVol <= 0)

            val gain: Float = when {
                isMuted || curVol <= minVol -> 0.0f
                curVol >= maxVol -> 1.0f
                else -> {
                    var computed: Float? = null
                    if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.P) {
                        try {
                            var devType = AudioDeviceInfo.TYPE_BUILTIN_SPEAKER
                            for (dev in audioManager.getDevices(AudioManager.GET_DEVICES_OUTPUTS)) {
                                when (dev.type) {
                                    AudioDeviceInfo.TYPE_WIRED_HEADPHONES,
                                    AudioDeviceInfo.TYPE_WIRED_HEADSET,
                                    AudioDeviceInfo.TYPE_USB_HEADSET,
                                    AudioDeviceInfo.TYPE_USB_DEVICE,
                                    AudioDeviceInfo.TYPE_BLUETOOTH_A2DP,
                                    AudioDeviceInfo.TYPE_BLE_HEADSET -> {
                                        devType = dev.type
                                        break
                                    }
                                }
                            }
                            val db = audioManager.getStreamVolumeDb(AudioManager.STREAM_MUSIC, curVol, devType)
                            computed = if (db <= -90f) 0.0f else 10.0.pow((db / 20.0)).toFloat().coerceIn(0.0f, 1.0f)
                        } catch (_: Exception) {}
                    }
                    if (computed != null) {
                        computed
                    } else {
                        val span = (maxVol - minVol).coerceAtLeast(1)
                        val norm = (curVol - minVol).toDouble() / span.toDouble()
                        10.0.pow((-48.0 * (1.0 - norm)) / 20.0).toFloat().coerceIn(0.0f, 1.0f)
                    }
                }
            }
            nativeSetSystemVolume(gain)
        } catch (e: Exception) {
            Log.w(TAG, "Failed to sync system volume: ${e.message}")
        }
    }

    private fun registerVolumeTracking() {
        if (volumeReceiverRegistered) return
        try {
            val filter = IntentFilter("android.media.VOLUME_CHANGED_ACTION")
            if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.TIRAMISU) {
                registerReceiver(volumeReceiver, filter, Context.RECEIVER_EXPORTED)
            } else {
                registerReceiver(volumeReceiver, filter)
            }
            contentResolver.registerContentObserver(Settings.System.CONTENT_URI, true, volumeObserver)
            volumeReceiverRegistered = true
        } catch (e: Exception) {
            Log.w(TAG, "Failed to register volume tracking: ${e.message}")
        }
    }

    private fun unregisterVolumeTracking() {
        if (!volumeReceiverRegistered) return
        try { unregisterReceiver(volumeReceiver) } catch (_: Exception) {}
        try { contentResolver.unregisterContentObserver(volumeObserver) } catch (_: Exception) {}
        volumeReceiverRegistered = false
    }

    override fun configureFlutterEngine(flutterEngine: FlutterEngine) {
        super.configureFlutterEngine(flutterEngine)
        registerVolumeTracking()
        syncSystemVolume()

        val channel = MethodChannel(flutterEngine.dartExecutor.binaryMessenger, CHANNEL)
        methodChannel = channel

        channel.setMethodCallHandler { call, result ->
            when (call.method) {
                "requestPermissions" -> {
                    val perms = mutableListOf<String>()
                    if (ContextCompat.checkSelfPermission(this@MainActivity, android.Manifest.permission.RECORD_AUDIO) != PackageManager.PERMISSION_GRANTED) {
                        perms.add(android.Manifest.permission.RECORD_AUDIO)
                    }
                    if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.TIRAMISU) {
                        if (ContextCompat.checkSelfPermission(this@MainActivity, android.Manifest.permission.POST_NOTIFICATIONS) != PackageManager.PERMISSION_GRANTED) {
                            perms.add(android.Manifest.permission.POST_NOTIFICATIONS)
                        }
                    }
                    if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.S) {
                        if (ContextCompat.checkSelfPermission(this@MainActivity, android.Manifest.permission.BLUETOOTH_CONNECT) != PackageManager.PERMISSION_GRANTED) {
                            perms.add(android.Manifest.permission.BLUETOOTH_CONNECT)
                        }
                    }
                    if (perms.isNotEmpty()) {
                        requestPermissions(perms.toTypedArray(), 100)
                    }
                    result.success(null)
                }
                "startAudio" -> {
                    if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.TIRAMISU) {
                        if (ContextCompat.checkSelfPermission(this@MainActivity, android.Manifest.permission.POST_NOTIFICATIONS) != PackageManager.PERMISSION_GRANTED) {
                            requestPermissions(arrayOf(android.Manifest.permission.POST_NOTIFICATIONS), 101)
                        }
                    }

                    val serverName = call.argument<String>("serverName") ?: "AudioBridge"
                    val serviceIntent = Intent(this@MainActivity, AudioForegroundService::class.java).apply {
                        putExtra("serverName", serverName)
                    }
                    if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.O) {
                        startForegroundService(serviceIntent)
                    } else {
                        startService(serviceIntent)
                    }

                    syncSystemVolume()
                    if (!nativeStarted) {
                        nativeStarted = nativeStartAudio()
                    }
                    syncSystemVolume()
                    result.success(nativeStarted)
                }
                "stopAudio" -> {
                    val serviceIntent = Intent(this@MainActivity, AudioForegroundService::class.java)
                    stopService(serviceIntent)

                    stopBluetoothSco()
                    if (nativeStarted) {
                        nativeStopAudio()
                        nativeStarted = false
                    }
                    result.success(null)
                }
                "feedAudio" -> {
                    val data = call.arguments as? ByteArray
                    if (data != null && nativeStarted) {
                        nativeFeedAudio(data)
                    }
                    result.success(null)
                }
                "getLatency" -> {
                    result.success(if (nativeStarted) nativeGetLatency() else 0.0)
                }
                "getLatencyBreakdown" -> {
                    if (nativeStarted) {
                        val breakdown = nativeGetLatencyBreakdown()
                        result.success(mapOf(
                            "jitterBufferMs" to breakdown[0],
                            "outputBufferMs" to breakdown[1]
                        ))
                    } else {
                        result.success(mapOf(
                            "jitterBufferMs" to 0.0,
                            "outputBufferMs" to 0.0
                        ))
                    }
                }
                "startRecording" -> {
                    if (ContextCompat.checkSelfPermission(this@MainActivity, android.Manifest.permission.RECORD_AUDIO) != PackageManager.PERMISSION_GRANTED) {
                        requestPermissions(arrayOf(android.Manifest.permission.RECORD_AUDIO), 102)
                        result.error("PERMISSION_DENIED", "Microphone permission not granted", null)
                    } else {
                        // Request BLUETOOTH_CONNECT if needed (Android 12+)
                        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.S) {
                            if (ContextCompat.checkSelfPermission(this@MainActivity, android.Manifest.permission.BLUETOOTH_CONNECT) != PackageManager.PERMISSION_GRANTED) {
                                requestPermissions(arrayOf(android.Manifest.permission.BLUETOOTH_CONNECT), 103)
                            }
                        }

                        val args = call.arguments as? Map<String, Any>
                        val micSource = args?.get("micSource") as? String ?: "auto"

                        // Manage Bluetooth SCO based on mic source selection
                        stopBluetoothSco() // Reset first
                        when (micSource) {
                            "auto" -> {
                                if (isBluetoothHeadsetConnected()) startBluetoothSco()
                            }
                            "bluetooth" -> startBluetoothSco()
                            "phone" -> { /* no SCO, use built-in mic */ }
                        }

                        val ip = args?.get("ip") as? String ?: ""
                        val port = args?.get("port") as? Int ?: 0
                        result.success(nativeStartRecording(ip, port))
                    }
                }
                "stopRecording" -> {
                    nativeStopRecording()
                    stopBluetoothSco()
                    result.success(null)
                }
                "setMuted" -> {
                    val muted = call.arguments as? Boolean ?: false
                    nativeSetMuted(muted)
                    result.success(null)
                }
                "setVolume" -> {
                    val volume = (call.arguments as? Double)?.toFloat() ?: 1.0f
                    nativeSetVolume(volume)
                    result.success(null)
                }
                "updateServiceState" -> {
                    val args = call.arguments as? Map<String, Any>
                    if (args != null) {
                        val updateIntent = Intent(this@MainActivity, AudioForegroundService::class.java).apply {
                            action = AudioForegroundService.ACTION_UPDATE_STATE
                            putExtra("micMuted", args["micMuted"] as? Boolean ?: false)
                            putExtra("paused", args["paused"] as? Boolean ?: false)
                            putExtra("mediaPlaying", args["mediaPlaying"] as? Boolean ?: false)
                            putExtra("serverName", args["serverName"] as? String ?: "AudioBridge")
                        }
                        startService(updateIntent)
                    }
                    result.success(null)
                }
                // New ConnectionManager methods
                "connect" -> {
                    val args = call.arguments as? Map<String, Any>
                    val ip = args?.get("ip") as? String ?: ""
                    val port = args?.get("port") as? Int ?: 4012
                    val devName = args?.get("deviceName") as? String ?: "AudioBridge"
                    val devId = args?.get("deviceId") as? String ?: ""
                    val psk = args?.get("pskHex") as? String ?: ""

                    // Run nativeConnect on a background thread since it blocks
                    Thread {
                        val connectResult = nativeConnect(ip, port, devName, devId, psk)
                        runOnUiThread {
                            result.success(connectResult)
                        }
                    }.start()
                }
                "disconnect" -> {
                    nativeDisconnect()
                    result.success(null)
                }
                "sendControl" -> {
                    val cmd = call.arguments as? Int ?: 0
                    nativeSendControl(cmd)
                    result.success(null)
                }
                "sendKeepalive" -> {
                    nativeSendKeepalive()
                    result.success(null)
                }
                "sendPing" -> {
                    nativeSendPing()
                    result.success(null)
                }
                "getRtt" -> {
                    result.success(nativeGetRtt())
                }
                "isConnected" -> {
                    result.success(nativeIsConnected())
                }
                "isDtlsActive" -> {
                    result.success(nativeIsDtlsActive())
                }
                "getAudioLevels" -> {
                    result.success(mapOf(
                        "output" to nativeGetOutputPeakLevel().toDouble(),
                        "input" to nativeGetInputPeakLevel().toDouble()
                    ))
                }
                "getAudioState" -> {
                    val state = nativeGetAudioState()
                    result.success(mapOf(
                        "playing" to (state[0] != 0.0),
                        "recording" to (state[1] != 0.0),
                        "muted" to (state[2] != 0.0),
                        "volume" to state[3],
                        "systemVolume" to state[4]
                    ))
                }
                else -> result.notImplemented()
            }
        }
    }

    override fun onDestroy() {
        unregisterVolumeTracking()
        methodChannel = null
        super.onDestroy()
    }
}
