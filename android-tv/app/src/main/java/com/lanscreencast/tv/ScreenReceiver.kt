package com.lanscreencast.tv

import android.content.Context
import android.graphics.Bitmap
import android.graphics.BitmapFactory
import com.lanscreencast.tv.logging.FileLogger
import java.io.DataInputStream
import java.io.EOFException
import java.net.Inet4Address
import java.net.NetworkInterface
import java.net.ServerSocket
import java.net.Socket
import java.net.SocketTimeoutException
import kotlin.concurrent.thread

/** Receives length-prefixed JPEG frames from one sender on the local network. */
class ScreenReceiver(
    private val context: Context,
    private val onFrame: (Bitmap) -> Unit,
    private val onStatus: (String) -> Unit,
    private val acquire: () -> Boolean = { true },
    private val release: () -> Unit = {},
) {
    companion object {
        const val PORT = 47474
        private const val MAX_FRAME_BYTES = 8 * 1024 * 1024

        fun localAddress(): String? = runCatching {
            NetworkInterface.getNetworkInterfaces().toList()
                .filter { it.isUp && !it.isLoopback }
                .sortedBy { if (it.name.startsWith("wlan") || it.name.startsWith("eth")) 0 else 1 }
                .flatMap { it.inetAddresses.toList() }
                .filterIsInstance<Inet4Address>()
                .firstOrNull { it.isSiteLocalAddress }
                ?.hostAddress
        }.getOrNull()
    }

    @Volatile private var running = false
    @Volatile private var server: ServerSocket? = null
    @Volatile private var client: Socket? = null

    fun start() {
        if (running) return
        running = true
        thread(name = "screen-receiver", isDaemon = true) {
            try {
                ReusableListener.bind(PORT, { running }).use { listening ->
                    server = listening
                    listening.soTimeout = 1000
                    FileLogger.log(context, "INFO", "NETWORK", "LISTENING", "TCP port $PORT")
                    while (running) {
                        try {
                            listening.accept().use socketUse@ { socket ->
                                if (!acquire()) return@socketUse
                                client = socket
                                socket.soTimeout = 5000
                                receive(socket)
                            }
                        } catch (_: SocketTimeoutException) {
                            // Accept checks running periodically; an idle client is disconnected.
                        } catch (_: EOFException) {
                            if (running) FileLogger.log(context, "INFO", "NETWORK", "DISCONNECTED", "Sender closed connection")
                        } catch (error: Exception) {
                            if (running) {
                                FileLogger.log(context, "ERROR", "NETWORK", "CLIENT_ERROR", error.message.orEmpty())
                                onStatus("连接已断开，等待重新连接")
                            }
                        } finally {
                            if (client != null) {
                                release()
                                if (running) onStatus("等待连接")
                            }
                            client = null
                        }
                    }
                }
            } catch (error: Exception) {
                if (running) {
                    FileLogger.log(context, "ERROR", "NETWORK", "LISTEN_FAILED", error.message.orEmpty())
                    onStatus("接收服务启动失败：${error.message}")
                }
            } finally {
                server = null
                running = false
            }
        }
    }

    private fun receive(socket: Socket) {
        val input = DataInputStream(socket.getInputStream())
        val magic = ByteArray(4)
        input.readFully(magic)
        require(magic.contentEquals(byteArrayOf(0x4c, 0x53, 0x43, 0x31))) { "Invalid sender protocol" }
        FileLogger.log(context, "INFO", "NETWORK", "CONNECTED", socket.inetAddress.hostAddress.orEmpty())
        onStatus("正在投屏")
        while (running) {
            val length = input.readInt()
            require(length in 1..MAX_FRAME_BYTES) { "Invalid frame size: $length" }
            val bytes = ByteArray(length)
            input.readFully(bytes)
            BitmapFactory.decodeByteArray(bytes, 0, length)?.let(onFrame)
        }
    }

    fun stop() {
        running = false
        try { client?.close() } catch (_: Exception) {}
        try { server?.close() } catch (_: Exception) {}
    }
}
