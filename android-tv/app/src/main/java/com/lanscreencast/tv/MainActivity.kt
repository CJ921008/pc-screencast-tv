package com.lanscreencast.tv

import android.graphics.Bitmap
import android.app.AlertDialog
import android.graphics.Color
import android.graphics.Paint
import android.graphics.RectF
import android.media.AudioManager
import android.os.Bundle
import android.os.SystemClock
import android.view.SurfaceView
import android.view.WindowManager
import androidx.activity.ComponentActivity
import androidx.activity.compose.BackHandler
import androidx.activity.compose.setContent
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.setValue
import androidx.compose.ui.Modifier
import androidx.compose.ui.viewinterop.AndroidView
import androidx.core.view.WindowCompat
import androidx.core.view.WindowInsetsCompat
import androidx.core.view.WindowInsetsControllerCompat
import com.lanscreencast.tv.logging.FileLogger
import kotlin.math.min

class MainActivity : ComponentActivity() {
    private var casting by mutableStateOf(false)
    private var rtcCasting by mutableStateOf(false)
    private var control by mutableStateOf<ControlView?>(null)
    private var direct by mutableStateOf(false)
    private var dragging by mutableStateOf(false)
    private var remoteActive by mutableStateOf(false)
    private var remoteDevice by mutableStateOf<RemoteDevice?>(null)
    private var audioState by mutableStateOf<AudioPlaybackState?>(null)
    private var status by mutableStateOf("等待连接")
    private lateinit var receiver: ScreenReceiver
    private lateinit var rtcReceiver: WebRtcReceiver
    private val gate = SessionGate()
    private var requestDialog: AlertDialog? = null
    private var keyboardDialog: AlertDialog? = null
    private var pairingDialog: AlertDialog? = null
    @Volatile private var surfaceView: SurfaceView? = null
    private val framePaint = Paint(Paint.FILTER_BITMAP_FLAG)
    private var sampleStart = SystemClock.elapsedRealtime()
    private var renderedFrames = 0

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        remoteDevice = RemoteDevice.load(this)
        volumeControlStream = AudioManager.STREAM_MUSIC
        window.addFlags(WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON)
        WindowCompat.setDecorFitsSystemWindows(window, false)
        showFullscreen(false)
        FileLogger.log(this, "INFO", "APP", "STARTED", "Receiver waiting for connection")
        rtcReceiver = WebRtcReceiver(this, gate,
            onRequest = { name, reply -> runOnUiThread {
                requestDialog?.dismiss()
                requestDialog = AlertDialog.Builder(this).setTitle("允许投屏？")
                    .setMessage(name + " 请求向此设备投屏")
                    .setPositiveButton("接受") { _, _ -> reply(true) }
                    .setNegativeButton("拒绝") { _, _ -> reply(false) }
                    .setOnCancelListener { reply(false) }.show()
            } },
            onStatus = { value, active -> runOnUiThread {
                status = value; rtcCasting = active
                if (!active) requestDialog?.dismiss()
                showFullscreen(rtcCasting || casting)
            } },
            onControl = { value -> runOnUiThread {
                control = value
                if (value == null) {
                    direct = false
                    dragging = false
                    keyboardDialog?.dismiss()
                }
            } },
            onRemoteState = { active -> runOnUiThread {
                remoteActive = active
                if (!active) remoteDevice = RemoteDevice.load(this)
            } },
            onAudioState = { value -> runOnUiThread { audioState = value } },
        )
        rtcReceiver.start()
        receiver = ScreenReceiver(
            this,
            onFrame = ::drawFrame,
            onStatus = { value -> runOnUiThread {
                status = value
                casting = value == "正在投屏"
                if (!casting) surfaceView = null
                showFullscreen(rtcCasting || casting)
            } },
            acquire = { gate.acquire("jpeg") },
            release = { gate.release("jpeg") },
        )
        receiver.start()
        setContent {
            CastTheme {
                BackHandler(enabled = remoteActive) { rtcReceiver.disconnectRemote() }
                if (rtcCasting) {
                    ControlledVideo(rtcReceiver, status == "正在建立视频连接", control, direct, dragging,
                        onMode = { dragging = false; direct = !direct },
                        onDrag = { dragging = it },
                        onText = {
                            keyboardDialog = textInputDialog(this) { rtcReceiver.sendControl(it) }.also { it.show() }
                        },
                        onShortcuts = {
                            keyboardDialog = shortcutDialog(this) { rtcReceiver.sendControl(it) }.also { it.show() }
                        },
                        onDisconnect = if (remoteActive) rtcReceiver::disconnectRemote else null,
                        audioState = audioState,
                        onAudioToggle = {
                            val current = audioState
                            rtcReceiver.setAudioMuted(current != null && !current.muted && !current.interrupted)
                        },
                    )
                } else if (casting) {
                    AndroidView(
                        factory = { context -> SurfaceView(context).also { surfaceView = it } },
                        modifier = Modifier.fillMaxSize(),
                    )
                } else {
                    ReceiverHome(
                        status = status,
                        localAddress = ScreenReceiver.localAddress(),
                        remoteActive = remoteActive,
                        paired = remoteDevice != null,
                        onConnect = { remoteDevice?.let(rtcReceiver::connectRemote) },
                        onPair = {
                            pairingDialog = pairComputerDialog(this@MainActivity, remoteDevice,
                                onForget = { RemoteDevice.forget(this@MainActivity); remoteDevice = null },
                                onConnect = rtcReceiver::connectRemote,
                            ).also { it.show() }
                        },
                        onCancel = rtcReceiver::disconnectRemote,
                    )
                }
            }
        }
    }

    private fun drawFrame(bitmap: Bitmap) {
        try {
            val holder = surfaceView?.holder ?: return
            if (!holder.surface.isValid) return
            val canvas = holder.lockCanvas() ?: return
            try {
                canvas.drawColor(Color.BLACK)
                val scale = min(canvas.width.toFloat() / bitmap.width, canvas.height.toFloat() / bitmap.height)
                val width = bitmap.width * scale
                val height = bitmap.height * scale
                val target = RectF(
                    (canvas.width - width) / 2f, (canvas.height - height) / 2f,
                    (canvas.width + width) / 2f, (canvas.height + height) / 2f,
                )
                canvas.drawBitmap(bitmap, null, target, framePaint)
            } finally {
                holder.unlockCanvasAndPost(canvas)
            }
            renderedFrames++
            val now = SystemClock.elapsedRealtime()
            if (now - sampleStart >= 5000) {
                val fps = renderedFrames * 1000 / (now - sampleStart)
                FileLogger.log(this, "INFO", "VIDEO", "RENDER_FPS", "$fps")
                sampleStart = now
                renderedFrames = 0
            }
        } catch (error: Exception) {
            FileLogger.log(this, "ERROR", "VIDEO", "DRAW_FAILED", error.message.orEmpty())
        } finally {
            bitmap.recycle()
        }
    }

    private fun showFullscreen(fullscreen: Boolean) {
        val insets = WindowCompat.getInsetsController(window, window.decorView)
        insets.isAppearanceLightStatusBars = false
        insets.isAppearanceLightNavigationBars = false
        insets.systemBarsBehavior = WindowInsetsControllerCompat.BEHAVIOR_SHOW_TRANSIENT_BARS_BY_SWIPE
        if (fullscreen) insets.hide(WindowInsetsCompat.Type.systemBars())
        else insets.show(WindowInsetsCompat.Type.systemBars())
    }

    override fun onDestroy() {
        receiver.stop()
        requestDialog?.dismiss()
        keyboardDialog?.dismiss()
        pairingDialog?.dismiss()
        rtcReceiver.close()
        surfaceView = null
        super.onDestroy()
    }

    override fun onResume() {
        super.onResume()
        rtcReceiver.setControlForeground(true)
        rtcReceiver.setAudioForeground(true)
    }

    override fun onPause() {
        rtcReceiver.setControlForeground(false)
        rtcReceiver.setAudioForeground(false)
        dragging = false
        keyboardDialog?.dismiss()
        super.onPause()
    }
}
