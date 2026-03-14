package com.audiobridge.audiobridge

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

    override fun configureFlutterEngine(flutterEngine: FlutterEngine) {
        super.configureFlutterEngine(flutterEngine)

        MethodChannel(flutterEngine.dartExecutor.binaryMessenger, CHANNEL).setMethodCallHandler { call, result ->
            when (call.method) {
                "startAudio" -> {
                    if (!nativeStarted) {
                        nativeStarted = nativeStartAudio()
                    }
                    result.success(nativeStarted)
                }
                "stopAudio" -> {
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
                else -> result.notImplemented()
            }
        }
    }
}
