package com.audiobridge.audiobridge

/** Microphone controls that the foreground service can use without the activity. */
object NativeMic {
    init {
        System.loadLibrary("native_audio")
    }

    external fun setMuted(muted: Boolean)
    external fun isMuted(): Boolean
    external fun isRunning(): Boolean
}
