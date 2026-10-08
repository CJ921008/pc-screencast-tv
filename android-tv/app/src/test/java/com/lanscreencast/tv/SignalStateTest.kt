package com.lanscreencast.tv
import org.junit.Assert.*
import org.junit.Test

class SignalStateTest {
    @Test fun earlyIceWaitsForRemoteDescription() {
        val state = SignalState()
        state.hello(); state.accept()
        assertTrue(state.candidate("candidate", "video", 0).isEmpty())
        state.offer()
        assertEquals(1, state.remoteDescriptionReady().size)
        state.activate()
        assertEquals(1, state.candidate("next", "video", 0).size)
    }
    @Test fun rejectAndRepeatedStopClearTheSession() {
        val state = SignalState()
        state.hello(); state.reset(); state.reset()
        assertEquals(SignalState.Stage.WAITING, state.stage)
        state.hello(); state.accept(); state.offer(); state.reset()
        assertFalse(state.remoteReady)
        state.hello()
    }
    @Test fun invalidOrderIsRejected() {
        val state = SignalState()
        assertThrows(IllegalStateException::class.java) { state.offer() }
        state.hello()
        assertThrows(IllegalStateException::class.java) { state.activate() }
    }
    @Test fun iceQueueIsBounded() {
        val state = SignalState(); state.hello(); state.accept()
        repeat(128) { state.candidate("candidate", "video", 0) }
        assertThrows(IllegalStateException::class.java) { state.candidate("overflow", "video", 0) }
    }
    @Test fun jpegAndRtcCannotReserveTheSameReceiver() {
        val gate = SessionGate()
        assertTrue(gate.acquire("jpeg")); assertFalse(gate.acquire("webrtc"))
        gate.release("webrtc"); assertFalse(gate.acquire("webrtc"))
        gate.release("jpeg"); assertTrue(gate.acquire("webrtc"))
        gate.release("webrtc"); gate.release("webrtc"); assertTrue(gate.acquire("jpeg"))
    }
}
