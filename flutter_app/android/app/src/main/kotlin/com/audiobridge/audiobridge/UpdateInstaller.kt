package com.audiobridge.audiobridge

import android.Manifest
import android.app.Activity
import android.app.NotificationChannel
import android.app.NotificationManager
import android.app.PendingIntent
import android.content.BroadcastReceiver
import android.content.ComponentName
import android.content.Context
import android.content.Intent
import android.content.pm.PackageInstaller
import android.content.pm.PackageManager
import android.net.Uri
import android.os.Build
import android.util.Log
import androidx.core.app.NotificationCompat
import androidx.core.content.ContextCompat
import org.json.JSONObject
import java.io.File
import java.util.UUID

object InstallSessionStore {
    const val ACTION_STATUS = "com.audiobridge.audiobridge.UPDATE_INSTALL_STATUS"
    private const val PREFERENCES = "update_install_session"
    private val lock = Any()

    fun read(context: Context): InstallReceipt? = synchronized(lock) {
        val value = context.getSharedPreferences(PREFERENCES, Context.MODE_PRIVATE).getString("receipt", null)
            ?: return@synchronized null
        val json = JSONObject(value)
        InstallReceipt(json.getInt("session"), json.getString("token"), json.getLong("code"),
            json.getString("version"), InstallPhase.valueOf(json.getString("phase")),
            json.getString("message"), json.optString("confirmation"), json.getLong("revision"))
    }

    fun write(context: Context, receipt: InstallReceipt): InstallReceipt = synchronized(lock) {
        val previous = read(context)
        val saved = receipt.copy(revision = (previous?.revision ?: 0) + 1)
        val json = JSONObject().put("session", saved.sessionId).put("token", saved.token)
            .put("code", saved.targetCode).put("version", saved.targetVersion).put("phase", saved.phase.name)
            .put("message", saved.message).put("confirmation", saved.confirmation).put("revision", saved.revision)
        check(context.getSharedPreferences(PREFERENCES, Context.MODE_PRIVATE).edit().putString("receipt", json.toString()).commit()) {
            "Could not save installation recovery state"
        }
        saved
    }

    fun modify(context: Context, action: (InstallReceipt) -> InstallReceipt): InstallReceipt? = synchronized(lock) {
        val previous = read(context) ?: return@synchronized null
        val next = action(previous)
        if (next == previous) previous else write(context, next)
    }

    fun installed(context: Context): Pair<Long, String> {
        val info = context.packageManager.getPackageInfo(context.packageName, 0)
        @Suppress("DEPRECATION")
        val code = if (Build.VERSION.SDK_INT >= 28) info.longVersionCode else info.versionCode.toLong()
        return code to (info.versionName ?: code.toString())
    }

    fun refresh(context: Context): InstallReceipt? {
        val installed = installed(context)
        return modify(context) { previous ->
            val current = InstallSessionPolicy.reconcile(previous, installed.first, installed.second)
            if (current.phase in listOf(InstallPhase.PREPARING, InstallPhase.INSTALLING, InstallPhase.CONFIRMATION) &&
                context.packageManager.packageInstaller.getSessionInfo(current.sessionId) == null) {
                current.copy(phase = InstallPhase.ERROR,
                    message = "Installation did not finish or its outcome is unavailable. Installed version: ${installed.second}. Check for updates to retry.",
                    confirmation = "")
            } else current
        }
    }

    fun active(context: Context, receipt: InstallReceipt?): Boolean {
        if (receipt == null || receipt.phase == InstallPhase.COMPLETE) return false
        val info = context.packageManager.packageInstaller.getSessionInfo(receipt.sessionId) ?: return false
        return info.installerPackageName == context.packageName
    }

    fun safeConfirmation(context: Context, intent: Intent): Intent? {
        val resolved = intent.resolveActivity(context.packageManager) ?: return null
        val info = context.packageManager.getApplicationInfo(resolved.packageName, 0)
        if (info.flags and (android.content.pm.ApplicationInfo.FLAG_SYSTEM or android.content.pm.ApplicationInfo.FLAG_UPDATED_SYSTEM_APP) == 0) return null
        return Intent(intent).setComponent(ComponentName(resolved.packageName, resolved.className))
            .setPackage(resolved.packageName).apply {
                clipData = null
                flags = flags and (Intent.FLAG_GRANT_READ_URI_PERMISSION or Intent.FLAG_GRANT_WRITE_URI_PERMISSION or
                    Intent.FLAG_GRANT_PERSISTABLE_URI_PERMISSION or Intent.FLAG_GRANT_PREFIX_URI_PERMISSION).inv()
                addFlags(Intent.FLAG_ACTIVITY_NEW_TASK)
            }
    }
}

class UpdateInstaller(private val context: Context) {
    fun install(file: File, version: String, code: Long) {
        check(!InstallSessionStore.active(context, InstallSessionStore.refresh(context))) { "An earlier installation is still active. Cancel it before retrying." }
        val installer = context.packageManager.packageInstaller
        val params = PackageInstaller.SessionParams(PackageInstaller.SessionParams.MODE_FULL_INSTALL).apply {
            setAppPackageName(context.packageName)
            setSize(file.length())
            if (Build.VERSION.SDK_INT >= 31) setRequireUserAction(PackageInstaller.SessionParams.USER_ACTION_NOT_REQUIRED)
        }
        val sessionId = installer.createSession(params)
        try {
            val receipt = InstallSessionStore.write(context, InstallReceipt(sessionId, UUID.randomUUID().toString(), code, version,
                InstallPhase.PREPARING, "Preparing the verified update for Android..."))
            Log.i("AudioBridgeUpdate", "Preparing own update session $sessionId for $version ($code)")
            installer.openSession(sessionId).use { session ->
                file.inputStream().use { input ->
                    session.openWrite("base.apk", 0, file.length()).use { output ->
                        input.copyTo(output, 64 * 1024)
                        session.fsync(output)
                    }
                }
                val intent = Intent(context, UpdateInstallReceiver::class.java)
                    .setAction(InstallSessionStore.ACTION_STATUS).setData(Uri.parse(InstallSessionPolicy.callbackUri(receipt)))
                val flags = PendingIntent.FLAG_UPDATE_CURRENT or if (Build.VERSION.SDK_INT >= 31) PendingIntent.FLAG_MUTABLE else 0
                val pending = PendingIntent.getBroadcast(context, sessionId, intent, flags)
                InstallSessionStore.modify(context) { it.copy(phase = InstallPhase.INSTALLING,
                    message = "Android is installing the verified update. It may request confirmation.") }
                session.commit(pending.intentSender)
                Log.i("AudioBridgeUpdate", "Committed verified self-update session $sessionId")
            }
        } catch (error: Exception) {
            try { installer.abandonSession(sessionId) } catch (_: Exception) {}
            InstallSessionStore.modify(context) {
                if (it.sessionId == sessionId) it.copy(phase = InstallPhase.ERROR,
                    message = "Installation could not be confirmed: ${error.message}. Check for updates to retry.") else it
            }
            throw error
        }
    }

    fun confirm(activity: Activity) {
        check(!activity.isFinishing && !activity.isDestroyed && activity.hasWindowFocus()) {
            "Open AudioBridge before continuing the Android confirmation"
        }
        val receipt = InstallSessionStore.refresh(context) ?: error("No installation is pending")
        check(receipt.phase == InstallPhase.CONFIRMATION && InstallSessionStore.active(context, receipt)) { "This installation is no longer awaiting confirmation" }
        val original = Intent.parseUri(receipt.confirmation, Intent.URI_INTENT_SCHEME)
        val safe = InstallSessionStore.safeConfirmation(context, original) ?: error("Android confirmation could not be verified")
        activity.startActivity(safe)
        InstallSessionStore.modify(context) { current ->
            if (current.sessionId == receipt.sessionId && current.phase == InstallPhase.CONFIRMATION)
                current.copy(phase = InstallPhase.INSTALLING, message = "Finish the Android installation confirmation. Canceling keeps the installed version.")
            else current
        }
    }

    fun cancel() {
        val receipt = InstallSessionStore.refresh(context) ?: return
        if (receipt.phase == InstallPhase.COMPLETE) return
        if (InstallSessionStore.active(context, receipt)) context.packageManager.packageInstaller.abandonSession(receipt.sessionId)
        InstallSessionStore.modify(context) { current ->
            val installed = InstallSessionStore.installed(context)
            val reconciled = InstallSessionPolicy.reconcile(current, installed.first, installed.second)
            if (reconciled.phase == InstallPhase.COMPLETE) reconciled else reconciled.copy(phase = InstallPhase.ERROR,
                message = "Cancellation requested. Installed version: ${installed.second}. Check for updates to retry.", confirmation = "")
        }
    }
}

class UpdateInstallReceiver : BroadcastReceiver() {
    override fun onReceive(context: Context, intent: Intent) {
        try {
            if (intent.action == Intent.ACTION_MY_PACKAGE_REPLACED) {
                val receipt = InstallSessionStore.refresh(context)
                Log.i("AudioBridgeUpdate", "Reconciled installed package after replacement")
                if (receipt?.phase == InstallPhase.COMPLETE) notifyComplete(context, receipt)
                return
            }
            if (intent.action != InstallSessionStore.ACTION_STATUS) return
            val receipt = InstallSessionStore.read(context) ?: return
            if (!InstallSessionPolicy.accepts(receipt, intent.getIntExtra(PackageInstaller.EXTRA_SESSION_ID, -1), intent.dataString)) return
            val installed = InstallSessionStore.installed(context)
            val status = intent.getIntExtra(PackageInstaller.EXTRA_STATUS, PackageInstaller.STATUS_FAILURE)
            Log.i("AudioBridgeUpdate", "Accepted installation callback for session ${receipt.sessionId}, status $status")
            val result = when (status) {
                PackageInstaller.STATUS_PENDING_USER_ACTION -> InstallResult.CONFIRM
                PackageInstaller.STATUS_SUCCESS -> InstallResult.SUCCESS
                else -> InstallResult.FAILURE
            }
            val confirmation = if (result == InstallResult.CONFIRM) {
                val proposed = if (Build.VERSION.SDK_INT >= 33) intent.getParcelableExtra(Intent.EXTRA_INTENT, Intent::class.java)
                    else @Suppress("DEPRECATION") (intent.getParcelableExtra(Intent.EXTRA_INTENT) as? Intent)
                proposed?.let { InstallSessionStore.safeConfirmation(context, it)?.toUri(Intent.URI_INTENT_SCHEME) } ?: ""
            } else ""
            val message = when (status) {
                PackageInstaller.STATUS_PENDING_USER_ACTION -> "Android requires installation approval. Continue with the system confirmation."
                PackageInstaller.STATUS_FAILURE_ABORTED -> "Installation was canceled. The installed version is unchanged. Check for updates to retry."
                PackageInstaller.STATUS_FAILURE_STORAGE -> "Android could not install the update because storage is full. Free space, then retry."
                PackageInstaller.STATUS_FAILURE_CONFLICT -> "Android rejected the update's package or signing identity. The installed app was not replaced."
                else -> "Android did not complete installation. Check for updates to retry."
            }
            val updated = InstallSessionStore.modify(context) { current ->
                if (InstallSessionPolicy.accepts(current, receipt.sessionId, intent.dataString))
                    InstallSessionPolicy.result(current, result, installed.first, installed.second, message, confirmation)
                else current
            }
            if (updated?.phase == InstallPhase.COMPLETE) notifyComplete(context, updated)
        } catch (error: Exception) {
            Log.e("AudioBridgeUpdate", "Could not reconcile installation status", error)
        }
    }

    private fun notifyComplete(context: Context, receipt: InstallReceipt) {
        if (Build.VERSION.SDK_INT >= 33 && ContextCompat.checkSelfPermission(context, Manifest.permission.POST_NOTIFICATIONS) != PackageManager.PERMISSION_GRANTED) return
        val manager = context.getSystemService(Context.NOTIFICATION_SERVICE) as NotificationManager
        if (Build.VERSION.SDK_INT >= 26) manager.createNotificationChannel(NotificationChannel("app_updates", "App updates", NotificationManager.IMPORTANCE_DEFAULT))
        val open = PendingIntent.getActivity(context, 8702, Intent(context, MainActivity::class.java),
            PendingIntent.FLAG_UPDATE_CURRENT or PendingIntent.FLAG_IMMUTABLE)
        manager.notify(8702, NotificationCompat.Builder(context, "app_updates")
            .setSmallIcon(android.R.drawable.stat_sys_download_done).setContentTitle("AudioBridge updated")
            .setContentText(receipt.message).setContentIntent(open).setAutoCancel(true).setOnlyAlertOnce(true).build())
    }
}
