package com.audiobridge.audiobridge

enum class InstallPhase { PREPARING, INSTALLING, CONFIRMATION, ERROR, COMPLETE }
enum class InstallResult { CONFIRM, SUCCESS, FAILURE }

data class InstallReceipt(
    val sessionId: Int,
    val token: String,
    val targetCode: Long,
    val targetVersion: String,
    val phase: InstallPhase,
    val message: String,
    val confirmation: String = "",
    val revision: Long = 0,
)

object InstallSessionPolicy {
    fun callbackUri(receipt: InstallReceipt): String =
        "audiobridge-update://install/${receipt.sessionId}/${receipt.token}"

    fun accepts(receipt: InstallReceipt, sessionId: Int, data: String?): Boolean =
        receipt.sessionId >= 0 && receipt.targetCode > 0 &&
            Regex("[0-9a-f]{8}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{12}").matches(receipt.token) &&
            receipt.sessionId == sessionId && data == callbackUri(receipt)

    fun reconcile(receipt: InstallReceipt, installedCode: Long, installedVersion: String): InstallReceipt =
        if (installedCode >= receipt.targetCode && receipt.targetCode > 0) {
            receipt.copy(phase = InstallPhase.COMPLETE,
                message = "Update complete. Installed version: $installedVersion.", confirmation = "")
        } else receipt

    fun result(receipt: InstallReceipt, result: InstallResult, installedCode: Long,
               installedVersion: String, message: String, confirmation: String = ""): InstallReceipt {
        val current = reconcile(receipt, installedCode, installedVersion)
        if (current.phase == InstallPhase.COMPLETE) return current
        return when (result) {
            InstallResult.CONFIRM -> if (confirmation.isNotEmpty()) {
                current.copy(phase = InstallPhase.CONFIRMATION, message = message, confirmation = confirmation)
            } else current.copy(phase = InstallPhase.ERROR, message = "Android did not provide a safe installation confirmation.", confirmation = "")
            InstallResult.SUCCESS -> current.copy(phase = InstallPhase.INSTALLING,
                message = "Android reported installation success. Waiting to verify the installed version.", confirmation = "")
            InstallResult.FAILURE -> current.copy(phase = InstallPhase.ERROR, message = message, confirmation = "")
        }
    }
}
