package com.lanscreencast.tv
import org.junit.Assert.*
import org.junit.Test
import java.net.ServerSocket
import java.net.SocketException
import kotlin.concurrent.thread

class ReusableListenerTest {
    @Test fun waitsForOldListenerToReleasePort() {
        val old = ServerSocket(0)
        val port = old.localPort
        val release = thread { Thread.sleep(100); old.close() }
        ReusableListener.bind(port, { true }, 2000).use {
            assertEquals(port, it.localPort)
            assertTrue(it.reuseAddress)
        }
        release.join()
        ReusableListener.bind(port, { true }).close()
    }
    @Test fun canceledStartupDoesNotBind() {
        assertThrows(SocketException::class.java) { ReusableListener.bind(0, { false }) }
    }
}
