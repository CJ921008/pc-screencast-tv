package com.lanscreencast.tv

import android.graphics.Bitmap
import android.app.AlertDialog
import android.graphics.Color
import android.graphics.Paint
import android.graphics.RectF
import android.os.Bundle
import android.os.SystemClock
import android.view.SurfaceView
import android.view.View
import android.view.WindowManager
import androidx.activity.ComponentActivity
import androidx.activity.compose.setContent
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.background
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.aspectRatio
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.padding
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.setValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.graphics.Color as ComposeColor
import androidx.compose.ui.Modifier
import androidx.compose.ui.unit.dp
import androidx.compose.ui.viewinterop.AndroidView
import androidx.tv.material3.MaterialTheme
import androidx.tv.material3.Text
import com.lanscreencast.tv.logging.FileLogger
import kotlin.math.min

class MainActivity : ComponentActivity() {
    private var casting by mutableStateOf(false)
    private var rtcCasting by mutableStateOf(false)
    private var status by mutableStateOf("等待连接")
    private lateinit var receiver: ScreenReceiver
    private lateinit var rtcReceiver: WebRtcReceiver
    private val gate = SessionGate()
    private var requestDialog: AlertDialog? = null
    @Volatile private var surfaceView: SurfaceView? = null
    private val framePaint = Paint(Paint.FILTER_BITMAP_FLAG)
    private var sampleStart = SystemClock.elapsedRealtime()
    private var renderedFrames = 0

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        window.addFlags(WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON)
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
                showFullscreen(active)
            } })
        rtcReceiver.start()
        receiver = ScreenReceiver(
            this,
            onFrame = ::drawFrame,
            onStatus = { value -> runOnUiThread {
                status = value
                casting = value == "正在投屏"
                if (!casting) surfaceView = null
                showFullscreen(casting)
            } },
            acquire = { gate.acquire("jpeg") },
            release = { gate.release("jpeg") },
        )
        receiver.start()
        setContent {
            MaterialTheme {
                if (rtcCasting) {
                    Box(Modifier.fillMaxSize().background(ComposeColor.Black), contentAlignment = Alignment.Center) {
                    AndroidView(factory = { rtcReceiver.createRenderer() },
                        onRelease = { rtcReceiver.releaseRenderer(it) }, modifier = Modifier.fillMaxWidth().aspectRatio(16f / 9f))
                    }
                } else if (casting) {
                    AndroidView(
                        factory = { context -> SurfaceView(context).also { surfaceView = it } },
                        modifier = Modifier.fillMaxSize(),
                    )
                } else {
                    Column(
                        modifier = Modifier.fillMaxSize().padding(48.dp),
                        verticalArrangement = Arrangement.Center,
                        horizontalAlignment = Alignment.CenterHorizontally,
                    ) {
                        Text("LAN 投屏接收器", style = MaterialTheme.typography.displayMedium)
                        Text(status, style = MaterialTheme.typography.headlineMedium)
                        Text("在 Windows 端输入以下 IP，点击开始投屏")
                        Text(ScreenReceiver.localAddress() ?: "未连接局域网", style = MaterialTheme.typography.headlineMedium)
                        Text("H.264 / WebRTC · 47475    JPEG 兼容 · 47474")
                        Text("电脑和接收设备须连接到同一局域网")
                    }
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
        @Suppress("DEPRECATION")
        window.decorView.systemUiVisibility = if (fullscreen) {
            View.SYSTEM_UI_FLAG_FULLSCREEN or View.SYSTEM_UI_FLAG_HIDE_NAVIGATION or
                View.SYSTEM_UI_FLAG_IMMERSIVE_STICKY
        } else 0
    }

    override fun onDestroy() {
        receiver.stop()
        requestDialog?.dismiss()
        rtcReceiver.close()
        surfaceView = null
        super.onDestroy()
    }
}
