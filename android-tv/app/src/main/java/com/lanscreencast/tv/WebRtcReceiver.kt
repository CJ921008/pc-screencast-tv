package com.lanscreencast.tv

import android.content.Context
import android.media.MediaCodecList
import com.lanscreencast.tv.logging.FileLogger
import org.java_websocket.WebSocket
import org.java_websocket.handshake.ClientHandshake
import org.java_websocket.server.WebSocketServer
import org.json.JSONObject
import org.webrtc.*
import java.net.InetSocketAddress
import java.util.UUID
import java.util.concurrent.Executors
import java.util.concurrent.TimeUnit
import java.util.concurrent.atomic.AtomicLong

class WebRtcReceiver(
    private val context: Context,
    private val gate: SessionGate,
    private val onRequest: (String, (Boolean) -> Unit) -> Unit,
    private val onStatus: (String, Boolean) -> Unit,
) {
    companion object { const val PORT = 47475 }
    private val worker = Executors.newSingleThreadScheduledExecutor()
    private val egl = EglBase.create()
    private var renderer: SurfaceViewRenderer? = null
    private var video: VideoTrack? = null
    private var peer: PeerConnection? = null
    private var socket: WebSocket? = null
    private var session = ""
    private val state = SignalState()
    private var lastAlive = 0L
    private var deadline = 0L
    private var decoded = 0L
    private var receivedBytes = 0L
    private var statsTime = 0L
    private var lostPackets = 0L
    private var receivedPackets = 0L
    private var generation = 0
    private val renderedFrames = AtomicLong()
    private var previousRendered = 0L
    @Volatile private var acceptedAt = 0L
    private val supports1080 = runCatching {
        MediaCodecList(MediaCodecList.ALL_CODECS).codecInfos.any { info ->
            !info.isEncoder && info.supportedTypes.any { type ->
                type.equals("video/avc", true) &&
                    info.getCapabilitiesForType(type).videoCapabilities?.areSizeAndRateSupported(1920, 1080, 30.0) == true
            }
        }
    }.getOrDefault(false)
    private val decoderFactory: VideoDecoderFactory
    private val factory: PeerConnectionFactory
    private val server: WebSocketServer

    init {
        PeerConnectionFactory.initialize(PeerConnectionFactory.InitializationOptions.builder(context).createInitializationOptions())
        val defaults = DefaultVideoDecoderFactory(egl.eglBaseContext)
        decoderFactory = object : VideoDecoderFactory {
            override fun getSupportedCodecs(): Array<VideoCodecInfo> =
                defaults.supportedCodecs.filter { it.name.equals("H264", true) &&
                    (it.params["profile-level-id"] ?: "42e01f").startsWith("42") }.map {
                    val params = HashMap(it.params)
                    params["profile-level-id"] = if (supports1080) "42e028" else "42e01f"
                    VideoCodecInfo(it.payload, it.name, params)
                }.toTypedArray()
            override fun createDecoder(info: VideoCodecInfo): VideoDecoder? {
                val result = defaults.createDecoder(info)
                FileLogger.log(context, "INFO", "VIDEO", "DECODER", result?.implementationName ?: "unavailable")
                return result
            }
        }
        factory = PeerConnectionFactory.builder().setVideoDecoderFactory(decoderFactory).createPeerConnectionFactory()
        server = object : WebSocketServer(InetSocketAddress(PORT)) {
            override fun onOpen(conn: WebSocket, handshake: ClientHandshake) {
                if (handshake.resourceDescriptor != "/signaling") conn.close(1008, "Invalid path")
            }
            override fun onMessage(conn: WebSocket, message: String) {
                dispatch {
                    try { handle(conn, message) } catch (error: Exception) {
                        send(conn, "error", JSONObject().put("code", "INVALID_MESSAGE").put("message", error.message ?: "Invalid message"))
                        if (conn === socket) end("信令错误：" + error.message) else conn.close(1008, "Invalid message")
                    }
                }
            }
            override fun onClose(conn: WebSocket, code: Int, reason: String, remote: Boolean) {
                dispatch { if (conn === socket) end("连接已断开") }
            }
            override fun onError(conn: WebSocket?, error: Exception) {
                FileLogger.log(context, "ERROR", "SIGNAL", "ERROR", error.message.orEmpty())
            }
            override fun onStart() { FileLogger.log(context, "INFO", "SIGNAL", "LISTENING", PORT.toString()) }
        }
    }

    private fun dispatch(action: () -> Unit) {
        if (worker.isShutdown) return
        runCatching { worker.execute(action) }.onFailure {
            if (!worker.isShutdown) FileLogger.log(context, "ERROR", "SIGNAL", "DISPATCH_FAILED", it.message.orEmpty())
        }
    }

    fun start() {
        server.start()
        worker.scheduleAtFixedRate({
            val conn = socket ?: return@scheduleAtFixedRate
            val now = System.currentTimeMillis()
            if (now - lastAlive >= 6000) { end("连接超时"); return@scheduleAtFixedRate }
            if (state.stage != SignalState.Stage.ACTIVE && now > deadline) {
                end("连接建立超时"); return@scheduleAtFixedRate
            }
            send(conn, "ping", JSONObject())
            collectStats()
        }, 2, 2, TimeUnit.SECONDS)
    }

    private fun handle(conn: WebSocket, raw: String) {
        require(raw.length <= 1024 * 1024)
        val message = JSONObject(raw)
        if (message.optInt("protocolVersion") != 2) {
            send(conn, "error", JSONObject().put("code", "VERSION_MISMATCH").put("message", "需要信令版本 2"))
            conn.close(1008, "Version mismatch")
            return
        }
        require(message.keys().asSequence().toSet() == setOf("protocolVersion", "type", "requestId", "timestamp", "payload"))
        UUID.fromString(message.getString("requestId"))
        require(message.getLong("timestamp") >= 0)
        val type = message.getString("type")
        val payload = message.getJSONObject("payload")
        if (type == "hello") {
            val requestedSession = payload.getString("sessionId")
            UUID.fromString(requestedSession)
            val name = payload.getString("computerName").take(128)
            if (socket != null || !gate.acquire("webrtc")) {
                send(conn, "error", JSONObject().put("sessionId", requestedSession).put("code", "BUSY").put("message", "接收端正在使用"))
                conn.close(1008, "Busy")
                return
            }
            session = requestedSession
            socket = conn
            state.hello()
            lastAlive = System.currentTimeMillis()
            deadline = lastAlive + 30000 // Human acceptance is separate from the media handshake.
            onRequest(name) { allowed -> dispatch {
                if (socket !== conn || state.stage != SignalState.Stage.PENDING) return@dispatch
                if (!allowed) {
                    send(conn, "error", JSONObject().put("code", "REJECTED").put("message", "接收端拒绝连接"))
                    end("已拒绝连接")
                } else {
                    state.accept()
                    acceptedAt = System.currentTimeMillis()
                    deadline = System.currentTimeMillis() + 10000
                    send(conn, "capabilities", JSONObject().put("h264", decoderFactory.supportedCodecs.isNotEmpty())
                        .put("supports1080p30", supports1080))
                    onStatus("正在建立视频连接", true)
                }
            } }
            return
        }
        require(conn === socket && payload.getString("sessionId") == session) { "Invalid session" }
        lastAlive = System.currentTimeMillis()
        when (type) {
            "ping" -> send(conn, "pong", JSONObject())
            "pong" -> Unit
            "offer" -> {
                state.offer()
                val settings = payload.getJSONObject("video")
                require(settings.getInt("width") in listOf(1280, 1920) && settings.getInt("fps") in 10..30)
                require(settings.getInt("height") == if (settings.getInt("width") == 1280) 720 else 1080)
                require(settings.getInt("width") != 1920 || supports1080)
                createPeer()
                val active = peer!!
                active.setRemoteDescription(object : SdpObserverAdapter() {
                    override fun onSetSuccess() { dispatch {
                        if (peer !== active) return@dispatch
                        state.remoteDescriptionReady().forEach { addIce(it) }
                        active.createAnswer(object : SdpObserverAdapter() {
                            override fun onCreateSuccess(description: SessionDescription) {
                                dispatch {
                                if (peer !== active) return@dispatch
                                active.setLocalDescription(object : SdpObserverAdapter() {
                                    override fun onSetSuccess() { dispatch {
                                        if (peer === active) send(conn, "answer", JSONObject().put("sdp", description.description))
                                    } }
                                    override fun onSetFailure(error: String) { fail(active, error) }
                                }, description)
                                }
                            }
                            override fun onCreateFailure(error: String) { fail(active, error) }
                        }, MediaConstraints())
                    } }
                    override fun onSetFailure(error: String) { fail(active, error) }
                }, SessionDescription(SessionDescription.Type.OFFER, payload.getString("sdp")))
            }
            "ice_candidate" -> state.candidate(payload.getString("candidate"), payload.optString("mid", "video"),
                payload.optInt("mLineIndex", 0)).forEach { addIce(it) }
            "disconnect" -> end("投屏已停止")
            else -> error("Unknown signaling message")
        }
    }

    private fun createPeer() {
        val epoch = ++generation
        val config = PeerConnection.RTCConfiguration(emptyList())
        config.sdpSemantics = PeerConnection.SdpSemantics.UNIFIED_PLAN
        config.tcpCandidatePolicy = PeerConnection.TcpCandidatePolicy.DISABLED
        peer = factory.createPeerConnection(config, object : PeerConnection.Observer {
            override fun onSignalingChange(value: PeerConnection.SignalingState) {}
            override fun onIceConnectionChange(value: PeerConnection.IceConnectionState) {}
            override fun onIceConnectionReceivingChange(value: Boolean) {}
            override fun onIceGatheringChange(value: PeerConnection.IceGatheringState) {}
            override fun onIceCandidate(value: IceCandidate) { dispatch {
                if (epoch != generation) return@dispatch
                socket?.let { send(it, "ice_candidate", JSONObject().put("candidate", value.sdp)
                    .put("mid", value.sdpMid).put("mLineIndex", value.sdpMLineIndex)) }
            } }
            override fun onIceCandidatesRemoved(value: Array<out IceCandidate>) {}
            override fun onAddStream(value: MediaStream) {}
            override fun onRemoveStream(value: MediaStream) {}
            override fun onDataChannel(value: DataChannel) { value.close() }
            override fun onRenegotiationNeeded() {}
            override fun onConnectionChange(value: PeerConnection.PeerConnectionState) { dispatch {
                if (epoch != generation) return@dispatch
                when (value) {
                    PeerConnection.PeerConnectionState.CONNECTED -> {
                        if (state.stage == SignalState.Stage.OFFER) state.activate()
                        onStatus("正在投屏 · H.264", true)
                    }
                    PeerConnection.PeerConnectionState.FAILED, PeerConnection.PeerConnectionState.DISCONNECTED -> end("视频连接已断开")
                    else -> Unit
                }
            } }
            override fun onTrack(value: RtpTransceiver) { dispatch {
                if (epoch != generation) return@dispatch
                val track = value.receiver.track() as? VideoTrack ?: return@dispatch
                video = track
                renderer?.let { track.addSink(it) }
            } }
        }) ?: error("Cannot create WebRTC receiver")
        decoded = 0; receivedBytes = 0; lostPackets = 0; receivedPackets = 0
        statsTime = System.currentTimeMillis()
        renderedFrames.set(0); previousRendered = 0
    }

    private fun addIce(value: Triple<String, String, Int>) {
        peer?.addIceCandidate(IceCandidate(value.second, value.third, value.first))
    }
    private fun fail(active: PeerConnection, message: String) { dispatch { if (peer === active) end(message) } }

    fun createRenderer(): SurfaceViewRenderer = SurfaceViewRenderer(context).also { view ->
        view.init(egl.eglBaseContext, object : RendererCommon.RendererEvents {
            override fun onFirstFrameRendered() {
                FileLogger.log(context, "INFO", "VIDEO", "FIRST_FRAME_MS", (System.currentTimeMillis() - acceptedAt).toString())
            }
            override fun onFrameResolutionChanged(width: Int, height: Int, rotation: Int) {
                FileLogger.log(context, "INFO", "VIDEO", "RESOLUTION", width.toString() + "x" + height + " rotation=" + rotation)
            }
        })
        view.addFrameListener({ renderedFrames.incrementAndGet() }, 0f)
        view.setScalingType(RendererCommon.ScalingType.SCALE_ASPECT_FIT)
        view.setEnableHardwareScaler(true)
        dispatch { renderer = view; video?.addSink(view) }
    }
    fun releaseRenderer(view: SurfaceViewRenderer) { if (!worker.isShutdown) dispatch {
        if (renderer === view) {
            video?.removeSink(view)
            renderer = null
            view.release()
        }
    } }

    private fun collectStats() {
        val active = peer ?: return
        active.getStats { report -> dispatch {
            if (peer !== active) return@dispatch
            val inbound = report.statsMap.values.firstOrNull { it.type == "inbound-rtp" && it.members["kind"] == "video" }
                ?: return@dispatch
            fun number(name: String) = (inbound.members[name] as? Number)?.toLong() ?: 0L
            val now = System.currentTimeMillis()
            val elapsed = (now - statsTime).coerceAtLeast(1)
            val frames = number("framesDecoded")
            val bytes = number("bytesReceived")
            val lost = number("packetsLost")
            val received = number("packetsReceived")
            val packetDelta = (received - receivedPackets + lost - lostPackets).coerceAtLeast(1)
            val loss = ((lost - lostPackets).coerceAtLeast(0) * 100.0 / packetDelta)
            val pair = report.statsMap.values.firstOrNull { it.type == "candidate-pair" && it.members["state"] == "succeeded" }
            val rtt = ((pair?.members?.get("currentRoundTripTime") as? Number)?.toDouble() ?: 0.0) * 1000
            val rendered = renderedFrames.get()
            val stats = JSONObject().put("fps", (rendered - previousRendered) * 1000.0 / elapsed)
                .put("bitrate", (bytes - receivedBytes) * 8000.0 / elapsed).put("rttMs", rtt).put("lossPercent", loss)
                .put("decoder", inbound.members["decoderImplementation"]?.toString() ?: "unknown")
            socket?.let { send(it, "stats", stats) }
            FileLogger.log(context, "INFO", "VIDEO", "STATS", stats.toString())
            decoded = frames; receivedBytes = bytes; statsTime = now; receivedPackets = received; lostPackets = lost
            previousRendered = rendered
        } }
    }

    private fun send(conn: WebSocket, type: String, payload: JSONObject) {
        if (!conn.isOpen) return
        if (!payload.has("sessionId")) payload.put("sessionId", session)
        conn.send(JSONObject().put("protocolVersion", 2).put("type", type).put("requestId", UUID.randomUUID().toString())
            .put("timestamp", System.currentTimeMillis()).put("payload", payload).toString())
    }
    private fun end(message: String) {
        ++generation
        val old = socket
        socket = null
        video?.let { track -> renderer?.let { track.removeSink(it) } }
        video = null
        peer?.close(); peer?.dispose(); peer = null
        state.reset()
        gate.release("webrtc")
        old?.close()
        onStatus(message, false)
        FileLogger.log(context, "INFO", "SIGNAL", "CLOSED", message)
    }
    fun close() {
        server.stop(500)
        worker.submit { end("接收端已停止"); renderer?.release(); renderer = null }.get(2, TimeUnit.SECONDS)
        worker.shutdownNow()
        factory.dispose()
        egl.release()
    }
}

open class SdpObserverAdapter : SdpObserver {
    override fun onCreateSuccess(value: SessionDescription) {}
    override fun onSetSuccess() {}
    override fun onCreateFailure(error: String) {}
    override fun onSetFailure(error: String) {}
}
