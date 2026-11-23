package com.lanscreencast.tv

import org.json.JSONArray
import org.json.JSONObject
import org.webrtc.DataChannel
import java.nio.ByteBuffer
import java.util.concurrent.ScheduledExecutorService
import java.util.concurrent.ScheduledFuture
import java.util.concurrent.TimeUnit.MILLISECONDS
import java.util.concurrent.atomic.AtomicReference

internal enum class GamepadMode { UNAVAILABLE, DESKTOP, STARTING, GAME, STOPPING }

internal data class GamepadStatus(
    val mode: GamepadMode = GamepadMode.UNAVAILABLE,
    val message: String? = null,
) {
    val inGame: Boolean get() = mode == GamepadMode.STARTING ||
        mode == GamepadMode.GAME || mode == GamepadMode.STOPPING
}

internal data class GamepadAxes(
    val lx: Int = 0, val ly: Int = 0, val rx: Int = 0, val ry: Int = 0,
    val lt: Int = 0, val rt: Int = 0,
)

// Channel lifetime and transitions belong to the receiver's signaling worker.
internal class GamepadLink(
    private val worker: ScheduledExecutorService,
    private val dispatch: (() -> Unit) -> Unit,
    private val onStatus: (GamepadStatus) -> Unit,
) {
    companion object {
        const val BUTTONS = "lsc-gamepad-v1"
        const val AXES = "lsc-gamepad-axes-v1"
    }

    private var reliable: DataChannel? = null
    private var transient: DataChannel? = null
    private var mode = GamepadMode.UNAVAILABLE
    private var epoch = 0L
    private var sequence = 0L
    private val axes = AtomicReference(GamepadAxes())
    private var timer: ScheduledFuture<*>? = null

    fun attach(channel: DataChannel): Boolean {
        val label = channel.label()
        when {
            label == BUTTONS && reliable == null -> reliable = channel
            label == AXES && transient == null -> transient = channel
            else -> return false
        }
        channel.registerObserver(object : DataChannel.Observer {
            override fun onBufferedAmountChange(previousAmount: Long) = Unit
            override fun onStateChange() = dispatch {
                if (owns(channel)) updateChannels()
            }
            override fun onMessage(buffer: DataChannel.Buffer) {
                if (label != BUTTONS || buffer.binary || buffer.data.remaining() > 512) return
                val bytes = ByteArray(buffer.data.remaining()).also { buffer.data.get(it) }
                dispatch {
                    if (reliable === channel) runCatching { receive(JSONObject(bytes.decodeToString())) }
                }
            }
        })
        updateChannels()
        return true
    }

    private fun owns(channel: DataChannel) = reliable === channel || transient === channel

    private fun updateChannels() {
        val channels = listOfNotNull(reliable, transient)
        if (channels.any { it.state() == DataChannel.State.CLOSING || it.state() == DataChannel.State.CLOSED }) {
            close("手柄连接已断开")
        } else if (mode == GamepadMode.UNAVAILABLE && channels.size == 2 &&
            channels.all { it.state() == DataChannel.State.OPEN }
        ) {
            update(GamepadMode.DESKTOP)
        }
    }

    fun start() {
        if (mode != GamepadMode.DESKTOP) return
        ++epoch
        sequence = 0
        axes.set(GamepadAxes())
        if (!writeReliable(packet("start"))) return
        update(GamepadMode.STARTING)
        timer = worker.schedule({
            if (mode == GamepadMode.STARTING) stop("Windows 未确认启动游戏模式")
        }, 4500, MILLISECONDS)
    }

    fun stop(message: String? = null) {
        if (mode != GamepadMode.STARTING && mode != GamepadMode.GAME) return
        clearTimer()
        axes.set(GamepadAxes())
        if (!writeReliable(packet("stop"))) return
        update(GamepadMode.STOPPING, message)
        // Windows also clears the controller if axis reports stop arriving.
        timer = worker.schedule({
            if (mode == GamepadMode.STOPPING) close(message ?: "Windows 未确认退出游戏模式")
        }, 1000, MILLISECONDS)
    }

    fun setAxes(value: GamepadAxes) { axes.set(value) }

    fun buttons(value: Int) {
        if (mode == GamepadMode.GAME) writeReliable(packet("buttons").put("buttons", value))
    }

    private fun receive(message: JSONObject) {
        if (message.optString("type") != "gamepad_status" ||
            message.optLong("epoch", -1) != epoch ||
            (mode != GamepadMode.STARTING && mode != GamepadMode.GAME && mode != GamepadMode.STOPPING)
        ) return
        when (message.optString("state")) {
            "active" -> if (mode == GamepadMode.STARTING) {
                clearTimer()
                update(GamepadMode.GAME)
                timer = worker.scheduleWithFixedDelay(::writeAxes, 0, 16, MILLISECONDS)
            }
            "stopped" -> desktop()
            "expired" -> desktop("手柄连接超时，已返回桌面模式")
            "not_allowed" -> close("Windows 未允许手柄游戏模式")
            "driver_unavailable" -> desktop("Windows 无法启用虚拟手柄，请检查 ViGEmBus")
        }
    }

    private fun desktop(message: String? = null) {
        clearTimer()
        axes.set(GamepadAxes())
        update(GamepadMode.DESKTOP, message)
    }

    private fun writeAxes() {
        val channel = transient ?: return
        if (channel.state() != DataChannel.State.OPEN || channel.bufferedAmount() >= 2048) return
        val a = axes.get()
        val values = JSONArray().put(a.lx).put(a.ly).put(a.rx).put(a.ry).put(a.lt).put(a.rt)
        write(channel, packet("axes").put("sequence", ++sequence).put("axes", values))
    }

    private fun writeReliable(message: JSONObject): Boolean {
        val channel = reliable
        if (channel != null && channel.state() == DataChannel.State.OPEN &&
            channel.bufferedAmount() < 8192 && write(channel, message)
        ) return true
        close("手柄连接已断开")
        return false
    }

    private fun packet(type: String) = JSONObject().put("type", type).put("epoch", epoch)

    private fun write(channel: DataChannel, message: JSONObject): Boolean = channel.send(
        DataChannel.Buffer(ByteBuffer.wrap(message.toString().encodeToByteArray()), false),
    )

    private fun clearTimer() {
        timer?.cancel(false)
        timer = null
    }

    private fun update(next: GamepadMode, message: String? = null) {
        mode = next
        onStatus(GamepadStatus(next, message))
    }

    fun close(message: String? = null) {
        clearTimer()
        axes.set(GamepadAxes())
        val channels = listOfNotNull(reliable, transient)
        reliable = null
        transient = null
        channels.forEach {
            it.unregisterObserver()
            it.close()
            it.dispose()
        }
        update(GamepadMode.UNAVAILABLE, message)
    }
}
