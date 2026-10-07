package com.lanscreencast.tv

import android.graphics.Bitmap
import android.os.Bundle
import android.view.View
import android.view.WindowManager
import androidx.activity.ComponentActivity
import androidx.activity.compose.setContent
import androidx.compose.foundation.Image
import androidx.compose.foundation.background
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.padding
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.setValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.graphics.asImageBitmap
import androidx.compose.ui.layout.ContentScale
import androidx.compose.ui.unit.dp
import androidx.tv.material3.MaterialTheme
import androidx.tv.material3.Text
import com.lanscreencast.tv.logging.FileLogger

class MainActivity : ComponentActivity() {
    private var frame by mutableStateOf<Bitmap?>(null)
    private var status by mutableStateOf("等待连接")
    private lateinit var receiver: ScreenReceiver

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        window.addFlags(WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON)
        FileLogger.log(this, "INFO", "APP", "STARTED", "Receiver waiting for connection")
        receiver = ScreenReceiver(
            this,
            onFrame = { bitmap -> runOnUiThread { frame = bitmap; showFullscreen(true) } },
            onStatus = { value -> runOnUiThread {
                status = value
                if (value != "正在投屏") { frame = null; showFullscreen(false) }
            } },
        )
        receiver.start()
        setContent {
            MaterialTheme {
                val currentFrame = frame
                if (currentFrame != null) {
                    Image(
                        bitmap = currentFrame.asImageBitmap(),
                        contentDescription = "Windows 投屏画面",
                        modifier = Modifier.fillMaxSize().background(Color.Black),
                        contentScale = ContentScale.Fit,
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
                        Text("${ScreenReceiver.localAddress() ?: "未连接局域网"}:${ScreenReceiver.PORT}", style = MaterialTheme.typography.headlineMedium)
                        Text("电脑和接收设备须连接到同一局域网")
                    }
                }
            }
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
        super.onDestroy()
    }
}
