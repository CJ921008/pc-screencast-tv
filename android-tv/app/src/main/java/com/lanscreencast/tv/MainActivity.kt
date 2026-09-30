package com.lanscreencast.tv

import android.os.Bundle
import androidx.activity.ComponentActivity
import androidx.activity.compose.setContent
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.padding
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.unit.dp
import androidx.tv.material3.MaterialTheme
import androidx.tv.material3.Text
import com.lanscreencast.tv.logging.FileLogger

class MainActivity : ComponentActivity() {
    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        FileLogger.log(this, "INFO", "APP", "STARTED", "Receiver waiting for connection")
        setContent {
            MaterialTheme {
                Column(
                    modifier = Modifier.fillMaxSize().padding(48.dp),
                    verticalArrangement = Arrangement.Center,
                    horizontalAlignment = Alignment.CenterHorizontally,
                ) {
                    Text("LAN ScreenCast", style = MaterialTheme.typography.displayMedium)
                    Text("等待连接", style = MaterialTheme.typography.headlineMedium)
                    Text("请在电脑上打开 LAN ScreenCast")
                    Text("确保电脑和电视连接到同一个局域网")
                }
            }
        }
    }
}

