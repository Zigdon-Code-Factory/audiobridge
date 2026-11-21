package com.audiobridge.audiobridge

import android.content.ComponentName
import android.content.Context
import android.net.Uri
import android.os.Build
import android.os.Bundle
import android.os.Handler
import android.os.Looper
import android.os.OutcomeReceiver
import android.telecom.CallAudioState
import android.telecom.CallEndpoint
import android.telecom.CallEndpointException
import android.telecom.Connection
import android.telecom.ConnectionRequest
import android.telecom.ConnectionService
import android.telecom.DisconnectCause
import android.telecom.PhoneAccount
import android.telecom.PhoneAccountHandle
import android.telecom.TelecomManager
import android.util.Log
import androidx.annotation.RequiresApi
import androidx.core.content.ContextCompat

/**
 * Self-managed Telecom ConnectionService so Bluetooth HFP headsets (such as AirPods)
 * see an active call (+CIEV: call=1) while the Bluetooth mic is in use and deliver
 * headset button events (AT+CHUP -> onDisconnect, or mute state changes) to AudioBridge.
 */
@RequiresApi(Build.VERSION_CODES.O)
class AudioCallService : ConnectionService() {

    companion object {
        private const val TAG = "AudioBridge"
        private const val ACCOUNT_ID = "audiobridge_bt_mic"

        @Volatile
        private var activeConnection: AudioCallConnection? = null

        @Volatile
        private var callWanted = false

        private val mainHandler = Handler(Looper.getMainLooper())
        private var restartRunnable: Runnable? = null

        private fun accountHandle(context: Context): PhoneAccountHandle =
            PhoneAccountHandle(ComponentName(context, AudioCallService::class.java), ACCOUNT_ID)

        fun startCall(context: Context) {
            if (Build.VERSION.SDK_INT < Build.VERSION_CODES.O) return
            callWanted = true
            restartRunnable?.let { mainHandler.removeCallbacks(it) }
            restartRunnable = null
            if (activeConnection != null) return
            try {
                val appContext = context.applicationContext
                val telecom = appContext.getSystemService(Context.TELECOM_SERVICE) as? TelecomManager ?: return
                val handle = accountHandle(appContext)
                val account = PhoneAccount.Builder(handle, "AudioBridge")
                    .setCapabilities(PhoneAccount.CAPABILITY_SELF_MANAGED)
                    .addSupportedUriScheme(PhoneAccount.SCHEME_SIP)
                    .build()
                telecom.registerPhoneAccount(account)

                val extras = Bundle().apply {
                    putParcelable(TelecomManager.EXTRA_PHONE_ACCOUNT_HANDLE, handle)
                }
                val uri = Uri.fromParts(PhoneAccount.SCHEME_SIP, "audiobridge", null)
                telecom.placeCall(uri, extras)
                Log.i(TAG, "Telecom self-managed call requested")
            } catch (e: Exception) {
                Log.w(TAG, "Telecom startCall failed: ${e.message}")
            }
        }

        fun endCall() {
            callWanted = false
            restartRunnable?.let { mainHandler.removeCallbacks(it) }
            restartRunnable = null
            val conn = activeConnection ?: return
            activeConnection = null
            try {
                conn.endByApp()
                Log.i(TAG, "Telecom self-managed call ended")
            } catch (e: Exception) {
                Log.w(TAG, "Telecom endCall failed: ${e.message}")
            }
        }

        private fun recycleAfterHeadsetDisconnect(context: Context, conn: AudioCallConnection) {
            if (activeConnection === conn) {
                activeConnection = null
            }
            if (!callWanted) return
            val appContext = context.applicationContext
            restartRunnable?.let { mainHandler.removeCallbacks(it) }
            val restart = Runnable {
                restartRunnable = null
                if (callWanted && activeConnection == null) {
                    startCall(appContext)
                }
            }
            restartRunnable = restart
            mainHandler.postDelayed(restart, 250)
        }
    }

    override fun onCreateOutgoingConnection(
        connectionManagerPhoneAccount: PhoneAccountHandle?,
        request: ConnectionRequest?
    ): Connection {
        Log.i(TAG, "Telecom onCreateOutgoingConnection (wanted=$callWanted)")
        if (!callWanted) {
            return Connection.createCanceledConnection()
        }
        activeConnection?.endByApp()
        val conn = AudioCallConnection(this)
        activeConnection = conn
        return conn
    }

    override fun onCreateOutgoingConnectionFailed(
        connectionManagerPhoneAccount: PhoneAccountHandle?,
        request: ConnectionRequest?
    ) {
        Log.w(TAG, "Telecom onCreateOutgoingConnectionFailed")
    }

    private class AudioCallConnection(private val service: AudioCallService) : Connection() {
        @Volatile
        private var endingByApp = false

        init {
            connectionProperties = PROPERTY_SELF_MANAGED
            audioModeIsVoip = true
            connectionCapabilities = CAPABILITY_MUTE or CAPABILITY_SUPPORT_HOLD or CAPABILITY_HOLD
            setCallerDisplayName("AudioBridge", TelecomManager.PRESENTATION_ALLOWED)
            setActive()
            Log.i(TAG, "Telecom AudioCallConnection setActive()")
        }

        fun endByApp() {
            endingByApp = true
            setDisconnected(DisconnectCause(DisconnectCause.LOCAL))
            destroy()
        }

        override fun onDisconnect() {
            Log.i(TAG, "Telecom Connection.onDisconnect() endingByApp=$endingByApp")
            if (endingByApp) {
                setDisconnected(DisconnectCause(DisconnectCause.LOCAL))
                destroy()
                return
            }
            // Headset double-tap sent AT+CHUP while call is active: toggle soft mute,
            // complete Telecom disconnect cleanly (avoiding Telecom's 5s watchdog),
            // and re-register the self-managed call so subsequent double-taps keep working.
            endingByApp = true
            AudioForegroundService.triggerHeadsetToggle()
            setDisconnected(DisconnectCause(DisconnectCause.LOCAL))
            destroy()
            recycleAfterHeadsetDisconnect(service, this)
        }

        override fun onAbort() {
            Log.i(TAG, "Telecom Connection.onAbort() endingByApp=$endingByApp")
            if (endingByApp) {
                setDisconnected(DisconnectCause(DisconnectCause.CANCELED))
                destroy()
                return
            }
            endingByApp = true
            AudioForegroundService.triggerHeadsetToggle()
            setDisconnected(DisconnectCause(DisconnectCause.CANCELED))
            destroy()
            recycleAfterHeadsetDisconnect(service, this)
        }

        override fun onHold() {
            Log.i(TAG, "Telecom Connection.onHold()")
            AudioForegroundService.triggerHeadsetToggle()
        }

        override fun onUnhold() {
            Log.i(TAG, "Telecom Connection.onUnhold()")
            AudioForegroundService.triggerHeadsetToggle()
        }

        override fun onSilence() {
            Log.i(TAG, "Telecom Connection.onSilence()")
            AudioForegroundService.triggerHeadsetToggle()
        }

        override fun onMuteStateChanged(isMuted: Boolean) {
            super.onMuteStateChanged(isMuted)
            Log.i(TAG, "Telecom Connection.onMuteStateChanged(isMuted=$isMuted)")
        }

        @Deprecated("Deprecated in API 34")
        override fun onCallAudioStateChanged(state: CallAudioState?) {
            @Suppress("DEPRECATION")
            super.onCallAudioStateChanged(state)
            Log.i(TAG, "Telecom onCallAudioStateChanged: route=${state?.route} muted=${state?.isMuted} supported=${state?.supportedRouteMask}")
            if (Build.VERSION.SDK_INT < Build.VERSION_CODES.UPSIDE_DOWN_CAKE &&
                state != null &&
                state.route != CallAudioState.ROUTE_BLUETOOTH &&
                (state.supportedRouteMask and CallAudioState.ROUTE_BLUETOOTH) != 0) {
                @Suppress("DEPRECATION")
                setAudioRoute(CallAudioState.ROUTE_BLUETOOTH)
            }
        }

        @RequiresApi(Build.VERSION_CODES.UPSIDE_DOWN_CAKE)
        override fun onCallEndpointChanged(callEndpoint: CallEndpoint) {
            super.onCallEndpointChanged(callEndpoint)
            Log.i(TAG, "Telecom onCallEndpointChanged: type=${callEndpoint.endpointType} name=${callEndpoint.endpointName}")
        }

        @RequiresApi(Build.VERSION_CODES.UPSIDE_DOWN_CAKE)
        override fun onAvailableCallEndpointsChanged(availableEndpoints: MutableList<CallEndpoint>) {
            super.onAvailableCallEndpointsChanged(availableEndpoints)
            val bt = availableEndpoints.firstOrNull { it.endpointType == CallEndpoint.TYPE_BLUETOOTH }
            if (bt != null && currentCallEndpoint?.endpointType != CallEndpoint.TYPE_BLUETOOTH) {
                Log.i(TAG, "Telecom requesting Bluetooth endpoint: ${bt.endpointName}")
                requestCallEndpointChange(
                    bt,
                    ContextCompat.getMainExecutor(service),
                    object : OutcomeReceiver<Void, CallEndpointException> {
                        override fun onResult(result: Void?) {
                            Log.i(TAG, "Telecom Bluetooth endpoint selected")
                        }
                        override fun onError(error: CallEndpointException) {
                            Log.w(TAG, "Telecom Bluetooth endpoint error: ${error.message}")
                        }
                    }
                )
            }
        }
    }
}
