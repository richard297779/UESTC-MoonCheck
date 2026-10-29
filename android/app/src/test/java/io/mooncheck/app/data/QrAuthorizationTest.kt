package io.mooncheck.app.data

import org.junit.Assert.*
import org.junit.Test
import java.io.ByteArrayInputStream
import java.net.HttpURLConnection
import java.net.URL

class QrAuthorizationTest {
    @Test fun callback302EndsWithCompleteCookiesButBlocksPartialCredentials() {
        for (complete in listOf(true, false)) {
            val urls = mutableListOf<String>()
            val client = QrAuthorization { url ->
                urls.add(url)
                val step = urls.size
                val body = when (step) {
                    1 -> "<img src='/connect/qrcode/fixture_uuid_123'>"
                    2 -> "fixture-image"
                    3 -> "window.wx_errcode=405;window.wx_code='fixture_code';"
                    else -> ""
                }
                val values = mutableListOf("openid=fixture-id; Path=/; Secure; HttpOnly")
                if (complete) values.addAll(listOf("accesstoken=fixture-access; Path=/; HttpOnly", "refreshtoken=fixture-refresh; Path=/; HttpOnly"))
                val headers = if (step == 4) mapOf("Set-Cookie" to values, "Location" to listOf("https://landing.invalid/never-follow")) else emptyMap()
                object : HttpURLConnection(URL(url)) {
                    override fun connect() {}
                    override fun disconnect() {}
                    override fun usingProxy() = false
                    override fun getResponseCode() = if (step == 4) 302 else 200
                    override fun getHeaderFields(): Map<String, List<String>> = headers
                    override fun getHeaderField(name: String): String? = headers[name]?.firstOrNull()
                    override fun getInputStream() = ByteArrayInputStream(body.toByteArray())
                }
            }
            try {
                client.start()
                val result = runCatching { client.poll() }
                assertEquals(complete, result.isSuccess)
                if (complete) assertEquals("fixture-access", result.getOrThrow().second?.get("access"))
                assertEquals(4, urls.size)
                assertFalse(urls.any { "landing.invalid" in it })
            } finally { client.close() }
        }
    }

    @Test fun confirmedCodeReadsOnlyCallbackCookiesAndStopsAfterSuccess() {
        val urls = mutableListOf<String>()
        val client = QrAuthorization { url ->
            urls.add(url)
            val (body, headers) = when (urls.size) {
                1 -> "<img src='/connect/qrcode/fixture_uuid_123'>" to emptyMap()
                2 -> "fixture-image" to emptyMap()
                3 -> "window.wx_errcode=405;window.wx_code='fixture_code';" to emptyMap()
                else -> "{}" to mapOf("Set-Cookie" to listOf("openid=fixture-id; Path=/; Secure; HttpOnly", "accesstoken=fixture-access; Path=/", "refreshtoken=fixture-refresh; Path=/"))
            }
            object : HttpURLConnection(URL(url)) {
                override fun connect() {}
                override fun disconnect() {}
                override fun usingProxy() = false
                override fun getResponseCode() = 200
                override fun getHeaderFields(): Map<String, List<String>> = headers
                override fun getInputStream() = ByteArrayInputStream(body.toByteArray())
            }
        }
        client.start()
        val (status, fields) = client.poll()
        assertEquals(405, status)
        assertEquals("fixture-access", fields?.get("access"))
        assertTrue(urls.last().startsWith(QrAuthorization.CALLBACK + "?login_type=WX&code="))
        assertTrue(runCatching { client.poll() }.isFailure)
    }

    @Test fun parsesOnlyBoundedQrIdentifiersAndPollCodes() {
        assertEquals("fixture_uuid_123", QrAuthorization.uuid("<img src='/connect/qrcode/fixture_uuid_123'>"))
        assertEquals(405 to "fixture_code", QrAuthorization.parsePoll("window.wx_errcode=405;window.wx_code='fixture_code';"))
        assertEquals(408 to null, QrAuthorization.parsePoll("window.wx_errcode=408;"))
        assertEquals(404 to null, QrAuthorization.parsePoll("window.wx_errcode=404;"))
        assertTrue(runCatching { QrAuthorization.parsePoll("window.wx_errcode=405;window.wx_code='bad;code';") }.isFailure)
    }

    @Test fun allowsOnlyExpectedHttpsEndpoints() {
        assertTrue(QrAuthorization.trusted(QrAuthorization.QR_URL))
        assertTrue(QrAuthorization.trusted(QrAuthorization.CALLBACK))
        listOf("http://open.weixin.qq.com/", "https://open.weixin.qq.com.evil.invalid/",
            "https://user@yybadaccess.3g.qq.com/", "https://yybadaccess.3g.qq.com:8443/").forEach {
            assertFalse(QrAuthorization.trusted(it))
        }
    }

    @Test fun importsAllThreeCookiesWithRefreshBeforeFirstUse() {
        val fields = QrAuthorization.fields(mapOf("openid" to "fixture-id", "accesstoken" to "fixture-access", "refreshtoken" to "fixture-refresh"))
        assertEquals("fixture-id", fields["openid"])
        assertEquals("fixture-access", fields["access"])
        assertEquals("0", fields["expiresAt"])
        assertTrue(runCatching { QrAuthorization.fields(mapOf("openid" to "fixture-id")) }.isFailure)
        assertTrue(runCatching { QrAuthorization.fields(mapOf("openid" to "fixture-id", "accesstoken" to "bad\nheader", "refreshtoken" to "fixture-refresh")) }.isFailure)
    }
}
