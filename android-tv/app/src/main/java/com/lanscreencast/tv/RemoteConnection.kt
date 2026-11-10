package com.lanscreencast.tv

import android.app.AlertDialog
import android.content.Context
import android.os.Build
import android.text.InputType
import android.util.Base64
import android.widget.EditText
import android.widget.LinearLayout
import org.java_websocket.client.WebSocketClient
import org.java_websocket.drafts.Draft_6455
import org.java_websocket.handshake.ServerHandshake
import org.json.JSONObject
import java.net.URI
import java.security.MessageDigest
import java.security.cert.CertificateException
import java.security.cert.X509Certificate
import javax.net.ssl.SSLContext
import javax.net.ssl.SSLParameters
import javax.net.ssl.X509TrustManager

internal class RemoteDevice private constructor(val address: String, private val code: String) {
    private val parts = code.split('.')
    val uri = URI("wss://[$address]:47476/remote")
    val token get() = parts[2]
    fun withAddress(address: String) = parse(address, code)

    fun sslContext(): SSLContext {
        val expected = Base64.decode(parts[1], Base64.URL_SAFE or Base64.NO_WRAP or Base64.NO_PADDING)
        return SSLContext.getInstance("TLS").apply {
            init(null, arrayOf(object : X509TrustManager {
                override fun checkClientTrusted(chain: Array<out X509Certificate>?, authType: String?) {
                    throw CertificateException("Unexpected client certificate")
                }
                override fun checkServerTrusted(chain: Array<out X509Certificate>?, authType: String?) {
                    val cert = chain?.firstOrNull() ?: throw CertificateException("Missing remote certificate")
                    val actual = MessageDigest.getInstance("SHA-256").digest(cert.encoded)
                    if (!MessageDigest.isEqual(expected, actual)) throw CertificateException("Remote certificate pin mismatch")
                    cert.checkValidity()
                }
                override fun getAcceptedIssuers(): Array<X509Certificate> = emptyArray()
            }), null)
        }
    }

    fun save(context: Context) {
        prefs(context).edit().putString("address", address).putString("code", code).apply()
    }

    companion object {
        private val ipv6 = Regex("[0-9a-fA-F:]{2,39}")
        private val pairing = Regex("LSC1\\.[A-Za-z0-9_-]{43}\\.[A-Za-z0-9_-]{43}")

        fun parse(address: String, code: String): RemoteDevice {
            val host = address.trim().removeSurrounding("[", "]")
            require(ipv6.matches(host) && ':' in host && runCatching {
                URI("wss://[$host]:47476/remote").host != null
            }.getOrDefault(false)) { "请输入电脑的 IPv6 地址" }
            val credential = code.trim()
            require(pairing.matches(credential)) { "请粘贴 Windows 生成的完整配对码" }
            return RemoteDevice(host, credential)
        }

        private fun prefs(context: Context) = context.getSharedPreferences("remote_device", Context.MODE_PRIVATE)
        fun load(context: Context): RemoteDevice? {
            val values = prefs(context)
            return runCatching { parse(values.getString("address", "")!!, values.getString("code", "")!!) }.getOrNull()
        }
        fun forget(context: Context) { prefs(context).edit().clear().apply() }
    }
}

internal class RemoteConnection(
    val device: RemoteDevice,
    private val message: (RemoteConnection, String) -> Unit,
    private val closed: (RemoteConnection, String?) -> Unit,
) : WebSocketClient(device.uri, Draft_6455(emptyList(), 1024 * 1024), emptyMap(), 5000) {
    @Volatile private var canceled = false

    init { setSocketFactory(device.sslContext().socketFactory) }

    override fun onSetSSLParameters(sslParameters: SSLParameters) {
        // The copied SHA-256 pin identifies this specific computer even when its IPv6 changes.
        if (Build.VERSION.SDK_INT >= 24) sslParameters.endpointIdentificationAlgorithm = null
        sslParameters.protocols = sslParameters.protocols.filter { it == "TLSv1.2" || it == "TLSv1.3" }.toTypedArray()
    }

    override fun onOpen(handshake: ServerHandshake) {
        if (canceled) { close(); return }
        send(JSONObject().put("type", "remote_auth").put("version", 1).put("token", device.token).toString())
    }
    override fun onMessage(value: String) { if (!canceled) message(this, value) }
    override fun onClose(code: Int, reason: String, remote: Boolean) { closed(this, null) }
    override fun onError(error: Exception) {
        val certificate = generateSequence<Throwable>(error) { it.cause }.any { it is CertificateException }
        closed(this, if (certificate) "证书校验失败，请核对电脑地址和配对码" else "远程连接失败，请检查电脑和 TCP 47476")
    }

    fun cancel() {
        canceled = true
        if (isOpen) close()
        else runCatching { socket?.close() }
    }
}

internal fun pairComputerDialog(
    context: Context,
    current: RemoteDevice?,
    onForget: () -> Unit,
    onConnect: (RemoteDevice) -> Unit,
): AlertDialog {
    val address = EditText(context).apply {
        hint = "电脑的公网 IPv6 地址"
        setText(current?.address.orEmpty())
        inputType = InputType.TYPE_CLASS_TEXT or InputType.TYPE_TEXT_FLAG_NO_SUGGESTIONS
        setSingleLine()
    }
    val code = EditText(context).apply {
        hint = if (current == null) "粘贴 Windows 配对码" else "留空沿用已配对的凭据"
        inputType = InputType.TYPE_CLASS_TEXT or InputType.TYPE_TEXT_VARIATION_PASSWORD
        setSingleLine()
    }
    val content = LinearLayout(context).apply {
        orientation = LinearLayout.VERTICAL
        val inset = (20 * resources.displayMetrics.density).toInt()
        setPadding(inset, 0, inset, 0)
        addView(address)
        addView(code)
    }
    val builder = AlertDialog.Builder(context).setTitle("配对家里电脑").setView(content)
        .setNegativeButton("取消", null).setPositiveButton("配对并连接", null)
    if (current != null) builder.setNeutralButton("忘记电脑") { _, _ -> onForget() }
    val dialog = builder.create()
    dialog.setOnShowListener {
        dialog.getButton(AlertDialog.BUTTON_POSITIVE).setOnClickListener {
            val device = try {
                val host = address.text.toString()
                val credential = code.text.toString()
                if (current != null && credential.isBlank()) current.withAddress(host)
                else RemoteDevice.parse(host, credential)
            }
            catch (error: IllegalArgumentException) {
                if (error.message?.contains("IPv6") == true) address.error = error.message
                else code.error = error.message
                return@setOnClickListener
            }
            dialog.dismiss()
            onConnect(device)
        }
    }
    return dialog
}
