package com.audiobridge.audiobridge

import android.content.Intent
import android.os.Build
import android.content.pm.PackageManager
import androidx.core.content.ContextCompat
import io.flutter.embedding.android.FlutterActivity
import io.flutter.embedding.engine.FlutterEngine
import io.flutter.plugin.common.MethodChannel

class MainActivity : FlutterActivity() {
    private val CHANNEL = "com.audiobridge/audio"
    private var nativeStarted = false

    companion object {
        init {
            System.loadLibrary("native_audio")
        }
    }

    // Native methods
    private external fun nativeStartAudio(): Boolean
    private external fun nativeStopAudio()
    private external fun nativeFeedAudio(data: ByteArray)
    private external fun nativeGetLatency(): Double
    private external fun nativeStartRecording(serverIp: String, port: Int): Boolean
    private external fun nativeStopRecording()

    override fun configureFlutterEngine(flutterEngine: FlutterEngine) {
        super.configureFlutterEngine(flutterEngine)

        MethodChannel(flutterEngine.dartExecutor.binaryMessenger, CHANNEL).setMethodCallHandler { call, result ->
            when (call.method) {
                "startAudio" -> {
                    if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.TIRAMISU) {
                        if (ContextCompat.checkSelfPermission(this@MainActivity, android.Manifest.permission.POST_NOTIFICATIONS) != PackageManager.PERMISSION_GRANTED) {
                            requestPermissions(arrayOf(android.Manifest.permission.POST_NOTIFICATIONS), 101)
                        }
                    }
                    val serviceIntent = Intent(this@MainActivity, AudioForegroundService::class.java)
                    if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.O) {
                        startForegroundService(serviceIntent)
                    } else {
                        startService(serviceIntent)
                    }

                    if (!nativeStarted) {
                        nativeStarted = nativeStartAudio()
                    }
                    result.success(nativeStarted)
                }
                "stopAudio" -> {
                    val serviceIntent = Intent(this@MainActivity, AudioForegroundService::class.java)
                    stopService(serviceIntent)

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
                "startRecording" -> {
                    // RECORD_AUDIO is a dangerous permission — must request at runtime
                    if (ContextCompat.checkSelfPermission(this@MainActivity, android.Manifest.permission.RECORD_AUDIO) != PackageManager.PERMISSION_GRANTED) {
                        requestPermissions(arrayOf(android.Manifest.permission.RECORD_AUDIO), 102)
                        result.error("PERMISSION_DENIED", "Microphone permission not granted", null)
                    } else {
                        val args = call.arguments as? Map<String, Any>
                        val ip = args?.get("ip") as? String
                        val port = args?.get("port") as? Int
                        if (ip != null && port != null) {
                            result.success(nativeStartRecording(ip, port))
                        } else {
                            result.error("INVALID_ARGS", "Missing ip or port", null)
                        }
                    }
                }
                "stopRecording" -> {
                    nativeStopRecording()
                    result.success(null)
                }
                else -> result.notImplemented()
            }
        }
    }
}
