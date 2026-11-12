package com.audiobridge.audiobridge

import android.app.Activity
import android.content.Intent
import android.content.pm.PackageManager
import android.net.Uri
import android.os.Build
import android.provider.Settings
import io.flutter.plugin.common.MethodChannel
import org.json.JSONObject
import java.io.File
import java.io.ByteArrayOutputStream
import java.net.URL
import java.security.MessageDigest
import java.util.concurrent.Executors
import javax.net.ssl.HttpsURLConnection

class AppUpdater(private val activity: Activity, channel: MethodChannel) {
    companion object {
        private const val REPOSITORY = "Zigdon-Code-Factory/audiobridge"
        private const val ASSET = "AudioBridge-Android.apk"
        private const val MAX_BYTES = 300L * 1024 * 1024
    }

    private data class Release(val version: String, val url: String, val size: Long, val sha256: String)
    private val executor = Executors.newSingleThreadExecutor()
    private val lock = Any()
    private val installed = activity.packageManager.getPackageInfo(activity.packageName, 0)
    private val currentVersion = installed.versionName ?: "0.2.1"
    private val updateDirectory = File(activity.cacheDir, "updates")
    private val apk = File(updateDirectory, "AudioBridge.apk")
    private val cachedRelease = File(updateDirectory, "verified-release.json")
    private val installer = UpdateInstaller(activity.applicationContext)
    private var lastInstallRevision = -1L
    private var canCancel = false
    private var release: Release? = null
    private var phase = "idle"
    private var message = "Check the official GitHub release for a newer version."
    private var progress = 0.0
    private var awaitingPermission = false
    @Volatile private var closed = false

    init {
        channel.setMethodCallHandler { call, result ->
            try {
                when (call.method) {
                    "getUpdateState" -> result.success(snapshot())
                    "checkForUpdates" -> { check(); result.success(null) }
                    "installUpdate" -> { download(); result.success(null) }
                    "launchInstaller" -> { launchInstaller(); result.success(null) }
                    "confirmInstallation" -> { installer.confirm(activity); refreshInstallation(); result.success(null) }
                    "cancelInstallation" -> { installer.cancel(); refreshInstallation(); result.success(null) }
                    else -> result.notImplemented()
                }
            } catch (error: Exception) {
                result.error("update_error", error.message ?: "Update request failed", null)
            }
        }
        restoreDownload()
        refreshInstallation()
    }

    private fun snapshot(): Map<String, Any> {
        refreshInstallation()
        return synchronized(lock) {
            mutableMapOf<String, Any>("currentVersion" to currentVersion, "phase" to phase,
                "message" to message, "progress" to progress, "canCancel" to canCancel, "installRevision" to lastInstallRevision).also { state ->
                release?.let { state["version"] = it.version }
            }
        }
    }

    private fun refreshInstallation() {
        try {
        val receipt = InstallSessionStore.refresh(activity) ?: return
        val active = InstallSessionStore.active(activity, receipt)
        synchronized(lock) {
            canCancel = active
            if (receipt.revision != lastInstallRevision) {
                lastInstallRevision = receipt.revision
                phase = receipt.phase.name.lowercase()
                message = receipt.message
                progress = 0.0
            }
        }
        } catch (_: Exception) {
            setState("error", "Installation recovery status could not be read. Close and reopen AudioBridge before retrying.")
        }
    }

    private fun restoreDownload() {
        try {
            if (!cachedRelease.isFile || cachedRelease.length() > 8192) return
            val json = JSONObject(cachedRelease.readText())
            val selected = Release(json.getString("version"), json.getString("url"), json.getLong("size"), json.getString("sha256"))
            version(selected.version)
            require(selected.url == "https://github.com/$REPOSITORY/releases/download/v${selected.version}/$ASSET" &&
                selected.size in 1..MAX_BYTES && Regex("[0-9a-f]{64}").matches(selected.sha256))
            if (newer(selected.version) && apk.isFile && apk.length() == selected.size) {
                release = selected
                setState("ready", "A downloaded update is available. It will be verified again before installation.")
            }
        } catch (_: Exception) {
            setState("error", "The previous download could not be restored. Check for updates to download it again.")
        }
    }

    private fun saveDownload(selected: Release) {
        cachedRelease.writeText(JSONObject().put("version", selected.version).put("url", selected.url)
            .put("size", selected.size).put("sha256", selected.sha256).toString())
    }

    private fun fileHash(file: File): String {
        val digest = MessageDigest.getInstance("SHA-256")
        file.inputStream().use { input ->
            val buffer = ByteArray(64 * 1024)
            while (true) {
                val count = input.read(buffer)
                if (count < 0) break
                digest.update(buffer, 0, count)
            }
        }
        return digest.digest().joinToString("") { "%02x".format(it) }
    }

    private fun setState(next: String, text: String, fraction: Double = 0.0) = synchronized(lock) {
        phase = next
        message = text
        progress = fraction
    }

    private fun version(value: String): List<Int> {
        require(Regex("(?:0|[1-9][0-9]{0,4})\\.(?:0|[1-9][0-9]{0,4})\\.(?:0|[1-9][0-9]{0,4})").matches(value)) { "Unsupported release version" }
        return value.split('.').map { it.toInt().also { part -> require(part <= 65535) } }
    }

    private fun newer(candidate: String): Boolean {
        val proposed = version(candidate)
        val current = version(currentVersion)
        for (part in proposed.indices) {
            if (proposed[part] != current[part]) return proposed[part] > current[part]
        }
        return false
    }

    private fun connection(address: String): HttpsURLConnection {
        var url = URL(address)
        repeat(5) {
            require(url.protocol == "https" && url.userInfo == null && (url.port == -1 || url.port == 443) &&
                url.host in listOf("api.github.com", "github.com", "release-assets.githubusercontent.com", "objects.githubusercontent.com")) { "Untrusted update address" }
            val connection = url.openConnection() as HttpsURLConnection
            connection.instanceFollowRedirects = false
            connection.connectTimeout = 10000
            connection.readTimeout = 15000
            connection.setRequestProperty("User-Agent", "AudioBridge/$currentVersion")
            connection.setRequestProperty("Accept", "application/vnd.github+json")
            val status = connection.responseCode
            if (status in listOf(301, 302, 303, 307, 308)) {
                val location = connection.getHeaderField("Location")
                connection.disconnect()
                require(!location.isNullOrEmpty()) { "Update redirect is missing" }
                url = URL(url, location)
            } else {
                if (status != 200) { connection.disconnect(); error("GitHub returned HTTP $status. Try again later.") }
                return connection
            }
        }
        error("Too many update redirects")
    }

    private fun check() {
        refreshInstallation()
        synchronized(lock) {
            if (canCancel || phase in listOf("checking", "downloading", "preparing", "installing", "permission")) return
            release = null
            setState("checking", "Checking the official GitHub release...")
        }
        executor.execute {
            try {
                val connection = connection("https://api.github.com/repos/$REPOSITORY/releases/latest")
                val bytes = try { connection.inputStream.use { input ->
                    val output = ByteArrayOutputStream()
                    val buffer = ByteArray(8192)
                    while (true) {
                        val length = input.read(buffer)
                        if (length < 0) break
                        require(output.size() + length <= 2 * 1024 * 1024) { "Release information is too large" }
                        output.write(buffer, 0, length)
                    }
                    output.toByteArray()
                } } finally { connection.disconnect() }
                require(bytes.size <= 2 * 1024 * 1024) { "Release information is too large" }
                val json = JSONObject(String(bytes, Charsets.UTF_8))
                require(!json.optBoolean("draft") && !json.optBoolean("prerelease")) { "No stable release is available" }
                val tag = json.getString("tag_name")
                require(tag.startsWith("v")) { "Unsupported release tag" }
                val next = tag.substring(1)
                if (!newer(next)) {
                    setState("current", "You are up to date. Installed version: $currentVersion.")
                    return@execute
                }
                val assets = json.getJSONArray("assets")
                var found: Release? = null
                for (index in 0 until assets.length()) {
                    val asset = assets.getJSONObject(index)
                    if (asset.getString("name") != ASSET) continue
                    require(found == null) { "Duplicate release asset" }
                    val size = asset.getLong("size")
                    val digest = asset.optString("digest")
                    val url = asset.getString("browser_download_url")
                    require(size in 1..MAX_BYTES && Regex("sha256:[0-9a-f]{64}").matches(digest)) { "Release has no verified download" }
                    require(url == "https://github.com/$REPOSITORY/releases/download/$tag/$ASSET") { "Unexpected release download" }
                    found = Release(next, url, size, digest.substring(7))
                }
                synchronized(lock) { release = found ?: error("This release has no Android update") }
                setState("available", "Version $next is available. Download and install it without losing your settings.")
            } catch (error: Exception) {
                setState("error", "Could not check for updates: ${error.message}. Your installed app is unchanged.")
            }
        }
    }

    private fun verifyApk(file: File, selected: Release): Long {
        val flags = if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.P) PackageManager.GET_SIGNING_CERTIFICATES or PackageManager.GET_SIGNATURES else PackageManager.GET_SIGNATURES
        val archive = activity.packageManager.getPackageArchiveInfo(file.absolutePath, flags) ?: error("The download is not an Android package")
        val installed = activity.packageManager.getPackageInfo(activity.packageName, flags)
        require(archive.packageName == activity.packageName) { "Package identity does not match AudioBridge" }
        val archiveCode = if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.P) archive.longVersionCode else archive.versionCode.toLong()
        val installedCode = if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.P) installed.longVersionCode else installed.versionCode.toLong()
        require(archiveCode > installedCode && archive.versionName == selected.version) { "The package is not a newer build of this release" }
        val proposed = (if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.P) archive.signingInfo?.apkContentsSigners else null) ?: archive.signatures
        val current = (if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.P) installed.signingInfo?.apkContentsSigners else null) ?: installed.signatures
        require(!proposed.isNullOrEmpty() && !current.isNullOrEmpty() && proposed.toSet() == current.toSet()) { "Update signing identity does not match this installation" }
        return archiveCode
    }

    private fun download() {
        val selected = synchronized(lock) {
            require(phase == "available" && release != null) { "Check for an update first" }
            setState("downloading", "Downloading and verifying the update...")
            release!!
        }
        executor.execute {
            val partial = File(updateDirectory, "download.apk")
            try {
                require(updateDirectory.isDirectory || updateDirectory.mkdirs()) { "Cannot create update cache" }
                if (apk.isFile && apk.length() == selected.size && fileHash(apk) == selected.sha256) {
                    verifyApk(apk, selected)
                    saveDownload(selected)
                    setState("ready", "Cached download verified. Ready to install the update.")
                    return@execute
                }
                val digest = MessageDigest.getInstance("SHA-256")
                val connection = connection(selected.url)
                var count = 0L
                val deadline = System.nanoTime() + 5L * 60 * 1000000000
                try {
                    connection.inputStream.use { input -> partial.outputStream().use { output ->
                        val buffer = ByteArray(64 * 1024)
                        while (true) {
                            check(!closed && System.nanoTime() < deadline) { "Download interrupted or timed out" }
                            val length = input.read(buffer)
                            if (length < 0) break
                            count += length
                            require(count <= selected.size) { "Download is larger than the release asset" }
                            digest.update(buffer, 0, length)
                            output.write(buffer, 0, length)
                            synchronized(lock) { progress = count.toDouble() / selected.size }
                        }
                    } }
                } finally { connection.disconnect() }
                val actual = digest.digest().joinToString("") { "%02x".format(it) }
                require(count == selected.size && actual == selected.sha256) { "Download verification failed; retry the update" }
                verifyApk(partial, selected)
                if (apk.exists()) require(apk.delete()) { "Cannot replace cached update" }
                require(partial.renameTo(apk)) { "Cannot prepare the verified update" }
                saveDownload(selected)
                setState("ready", "Download verified. Ready for Android installation.")
            } catch (error: Exception) {
                partial.delete()
                setState("error", "Could not download the update: ${error.message}. Your installed app is unchanged. Check again to retry.")
            }
        }
    }

    private fun launchInstaller() {
        refreshInstallation()
        val selected = synchronized(lock) {
            require(phase == "ready" && !canCancel) { "No verified update is ready, or an installation is already active" }
            release ?: error("Check for updates to restore this download")
        }
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.O && !activity.packageManager.canRequestPackageInstalls()) {
            awaitingPermission = true
            setState("permission", "Allow AudioBridge to install its updates, then return here. Android may still require confirmation.")
            activity.startActivity(Intent(Settings.ACTION_MANAGE_UNKNOWN_APP_SOURCES, Uri.parse("package:${activity.packageName}")))
            return
        }
        setState("preparing", "Verifying the download and preparing Android installation...")
        executor.execute {
            try {
                require(apk.length() == selected.size && fileHash(apk) == selected.sha256) { "Cached download verification failed" }
                val code = verifyApk(apk, selected)
                check(!closed) { "Update canceled while closing the app" }
                installer.install(apk, selected.version, code)
                refreshInstallation()
            } catch (error: Exception) {
                refreshInstallation()
                if (synchronized(lock) { phase != "complete" }) {
                    setState("error", "Could not prepare installation: ${error.message}. Check for updates to retry.")
                }
            }
        }
    }

    fun onResume() {
        refreshInstallation()
        if (!awaitingPermission) return
        awaitingPermission = false
        setState("ready", "Installation permission was not granted. Continue installation to retry, or keep your current version.")
        if (Build.VERSION.SDK_INT < Build.VERSION_CODES.O || activity.packageManager.canRequestPackageInstalls()) {
            try { launchInstaller() } catch (error: Exception) { setState("error", "Could not open the installer: ${error.message}") }
        }
    }

    fun close() { closed = true; executor.shutdownNow() }
}
