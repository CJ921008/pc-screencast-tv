package com.lanscreencast.tv
import java.net.BindException
import java.net.InetSocketAddress
import java.net.ServerSocket
import java.net.SocketException

object ReusableListener {
    fun bind(port: Int, running: () -> Boolean, timeoutMs: Long = 5000): ServerSocket {
        val deadline = System.nanoTime() + timeoutMs * 1000000
        while (running()) {
            val socket = ServerSocket()
            try {
                socket.reuseAddress = true
                socket.bind(InetSocketAddress(port))
                return socket
            } catch (error: BindException) {
                socket.close()
                if (System.nanoTime() >= deadline) throw error
                Thread.sleep(50)
            } catch (error: Exception) { socket.close(); throw error }
        }
        throw SocketException("Receiver stopped")
    }
}
