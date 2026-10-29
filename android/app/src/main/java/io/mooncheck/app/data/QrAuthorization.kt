package io.mooncheck.app.data

import java.net.HttpCookie
import java.net.HttpURLConnection
import java.net.URI
import java.net.URLEncoder

/** An isolated, non-persistent QR session; no WebView or browser cookie store. */
class QrAuthorization(private val connectionFactory: (String) -> HttpURLConnection = { URI(it).toURL().openConnection() as HttpURLConnection }) {
    companion object {
        const val CALLBACK = "https://yybadaccess.3g.qq.com/pc_yyb/pcyyb_oauth"
        private fun encode(value: String) = URLEncoder.encode(value, "UTF-8")
        val QR_URL = "https://open.weixin.qq.com/connect/qrconnect?appid=wxd44977328b36e647" +
            "&redirect_uri=" + encode("$CALLBACK?login_type=WX") +
            "&response_type=code&scope=snsapi_login%2Csnsapi_runtime_pcsdk&state=web&fast_login=1&self_redirect=true"
        private val hosts = setOf("open.weixin.qq.com", "long.open.weixin.qq.com", "yybadaccess.3g.qq.com")
        fun trusted(url: String): Boolean = runCatching {
            val uri = URI(url)
            uri.scheme == "https" && uri.host in hosts && uri.port in setOf(-1, 443) && uri.userInfo == null
        }.getOrDefault(false)
        fun uuid(html: String): String = Regex("/connect/qrcode/([A-Za-z0-9_-]{8,128})").find(html)?.groupValues?.get(1)
            ?: error("微信页面未提供二维码，请刷新或手动导入")
        fun parsePoll(text: String): Pair<Int, String?> {
            val status = Regex("\\b(?:window\\.)?wx_errcode\\s*=\\s*(\\d{3})\\s*;").find(text)?.groupValues?.get(1)?.toInt()
                ?: error("扫码状态格式已变化，请使用手动导入")
            require(status in setOf(402, 403, 404, 405, 408)) { "微信返回未知扫码状态，请刷新" }
            val code = if (status == 405) Regex("\\b(?:window\\.)?wx_code\\s*=\\s*[\"']([A-Za-z0-9_-]{1,256})[\"']").find(text)?.groupValues?.get(1)
                ?: error("授权确认未返回有效 code") else null
            return status to code
        }
        fun fields(cookies: Map<String, String>): Map<String, String> {
            val result = mapOf("openid" to cookies["openid"].orEmpty(), "access" to cookies["accesstoken"].orEmpty(), "refresh" to cookies["refreshtoken"].orEmpty())
            require(result.values.all { it.isNotBlank() && it.length <= 16384 && it.none { c -> c in "\r\n;" } }) {
                "回调未返回完整的三个授权 Cookie，请重新扫码或手动导入"
            }
            return result + ("expiresAt" to "0")
        }
    }

    private var qrId: String? = null
    private val cookies = mutableMapOf<String, String>()
    @Volatile private var cancelled = false
    @Volatile private var active: HttpURLConnection? = null

    fun close() {
        cancelled = true
        active?.disconnect()
        synchronized(cookies) { cookies.clear() }
        qrId = null
    }

    private fun get(initial: String, limit: Int, timeout: Int = 20000): ByteArray {
        var url = initial
        repeat(6) {
            check(!cancelled) { "授权已取消" }
            require(trusted(url)) { "已阻止未允许的授权地址" }
            val connection = connectionFactory(url)
            active = connection
            try {
                connection.instanceFollowRedirects = false
                connection.useCaches = false
                connection.setRequestProperty("Cache-Control", "no-store")
                connection.connectTimeout = 10000
                connection.readTimeout = timeout
                connection.setRequestProperty("User-Agent", "Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 Chrome/131 Safari/537.36")
                connection.setRequestProperty("Referer", QR_URL)
                if (URI(url).host == "yybadaccess.3g.qq.com") synchronized(cookies) {
                    if (cookies.isNotEmpty()) connection.setRequestProperty("Cookie", cookies.entries.joinToString("; ") { "${it.key}=${it.value}" })
                }
                val status = connection.responseCode
                if (URI(url).host == "yybadaccess.3g.qq.com") {
                    connection.headerFields.filterKeys { it?.equals("Set-Cookie", ignoreCase = true) == true }.values.flatten().forEach { header ->
                        HttpCookie.parse(header).forEach { cookie ->
                            if (cookie.name in setOf("openid", "accesstoken", "refreshtoken") &&
                                (cookie.domain == null || cookie.domain.trimStart('.') in setOf("yybadaccess.3g.qq.com", "3g.qq.com", "qq.com"))) {
                                synchronized(cookies) {
                                    if (!cancelled) {
                                        if (cookie.hasExpired()) cookies.remove(cookie.name) else cookies[cookie.name] = cookie.value
                                    }
                                }
                            }
                        }
                    }
                }
                if (status in setOf(301, 302, 303, 307, 308)) {
                    // All credentials can arrive on this 302 before a cross-host
                    // landing page. Finish here instead of visiting that page.
                    if (URI(url).host == "yybadaccess.3g.qq.com" && synchronized(cookies) {
                        runCatching { fields(cookies) }.isSuccess
                    }) {
                        check(!cancelled) { "授权已取消" }
                        return ByteArray(0)
                    }
                    val target = URI(url).resolve(connection.getHeaderField("Location") ?: error("授权跳转无地址")).toString()
                    require(trusted(target) && (URI(url).host != "yybadaccess.3g.qq.com" || URI(target).host == "yybadaccess.3g.qq.com")) { "授权服务跳转被阻止" }
                    url = target
                } else {
                    check(status == 200) { "微信授权服务暂不可用" }
                    val bytes = connection.inputStream.use { it.readBytesBounded(limit) }
                    check(!cancelled) { "授权已取消" }
                    return bytes
                }
            } finally {
                connection.disconnect()
                active = null
            }
        }
        error("授权跳转次数过多")
    }

    fun start(): ByteArray {
        val id = uuid(get(QR_URL, 262144).toString(Charsets.UTF_8))
        qrId = id
        return get("https://open.weixin.qq.com/connect/qrcode/$id", 524288)
    }

    fun poll(): Pair<Int, Map<String, String>?> {
        val id = qrId ?: error("请先获取二维码")
        val (status, code) = parsePoll(get("https://long.open.weixin.qq.com/connect/l/qrconnect?uuid=$id", 65536, 35000).toString(Charsets.UTF_8))
        if (status != 405) return status to null
        synchronized(cookies) { cookies.clear() }
        try {
            get("$CALLBACK?login_type=WX&code=${URLEncoder.encode(code, "UTF-8")}&state=web", 65536)
            return 405 to synchronized(cookies) { fields(cookies) }
        } finally { close() }
    }
}

private fun java.io.InputStream.readBytesBounded(limit: Int): ByteArray {
    val out = java.io.ByteArrayOutputStream()
    val buffer = ByteArray(8192)
    while (true) {
        val read = read(buffer)
        if (read < 0) break
        require(out.size() + read <= limit) { "授权响应过大，已停止" }
        out.write(buffer, 0, read)
    }
    return out.toByteArray()
}
