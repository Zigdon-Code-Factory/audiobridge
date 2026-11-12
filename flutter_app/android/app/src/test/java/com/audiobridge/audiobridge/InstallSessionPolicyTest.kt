package com.audiobridge.audiobridge

import org.junit.Assert.*
import org.junit.Test

class InstallSessionPolicyTest {
    private val receipt = InstallReceipt(42, "4c487f52-c932-4e61-9062-bcb93930d544", 7, "0.2.1",
        InstallPhase.INSTALLING, "Installing")

    @Test fun acceptsOnlyTheCurrentSessionAndPrivateCallbackIdentity() {
        val uri = InstallSessionPolicy.callbackUri(receipt)
        assertTrue(InstallSessionPolicy.accepts(receipt, 42, uri))
        assertFalse(InstallSessionPolicy.accepts(receipt, 43, uri))
        assertFalse(InstallSessionPolicy.accepts(receipt, 42, null))
        assertFalse(InstallSessionPolicy.accepts(receipt, 42, uri + "/forged"))
        assertFalse(InstallSessionPolicy.accepts(receipt.copy(token = "untrusted"), 42, uri))
    }
    @Test fun staleCallbackCannotAffectRetriedSession() {
        val retry = receipt.copy(sessionId = 43, token = "cf4a3bf1-bf62-44cb-97fa-50e64b7bbf0f")
        assertFalse(InstallSessionPolicy.accepts(retry, 42, InstallSessionPolicy.callbackUri(receipt)))
    }
    @Test fun successReceiptDoesNotPublishAnUnverifiedUpgrade() {
        val next = InstallSessionPolicy.result(receipt, InstallResult.SUCCESS, 6, "0.2.0", "")
        assertEquals(InstallPhase.INSTALLING, next.phase)
        assertTrue(next.message.contains("verify"))
    }
    @Test fun actualInstalledVersionConfirmsSuccessAfterProcessReplacement() {
        val next = InstallSessionPolicy.reconcile(receipt, 7, "0.2.1")
        assertEquals(InstallPhase.COMPLETE, next.phase)
        assertTrue(next.message.contains("0.2.1"))
    }
    @Test fun lateFailureCannotContradictInstalledUpgrade() {
        assertEquals(InstallPhase.COMPLETE, InstallSessionPolicy.result(receipt, InstallResult.FAILURE, 7, "0.2.1", "Failure").phase)
    }
    @Test fun approvalFallbackRetainsSystemConfirmation() {
        val next = InstallSessionPolicy.result(receipt, InstallResult.CONFIRM, 6, "0.2.0", "Approval required", "intent:system")
        assertEquals(InstallPhase.CONFIRMATION, next.phase)
        assertEquals("intent:system", next.confirmation)
    }
    @Test fun missingSafeConfirmationCannotProceed() {
        val next = InstallSessionPolicy.result(receipt, InstallResult.CONFIRM, 6, "0.2.0", "Approval", "")
        assertEquals(InstallPhase.ERROR, next.phase)
    }
    @Test fun cancellationClearsObsoleteConfirmation() {
        val pending = receipt.copy(phase = InstallPhase.CONFIRMATION, confirmation = "intent:system")
        val next = InstallSessionPolicy.result(pending, InstallResult.FAILURE, 6, "0.2.0", "Canceled")
        assertEquals(InstallPhase.ERROR, next.phase)
        assertEquals("", next.confirmation)
    }
}
