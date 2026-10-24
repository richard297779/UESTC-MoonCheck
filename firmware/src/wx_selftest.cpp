#include "wx_selftest.h"

#include "aesgcm_vectors.h"
#include "golden_vectors.h"
#include "wx_aesgcm.h"
#include "wx_mmtls.h"
#include "wx_wire.h"

namespace wx {
namespace {
// 与 gen_golden_vectors.py 完全一致的固定输入
const uint8_t kK32[32] = {0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15,
                          16, 17, 18, 19, 20, 21, 22, 23, 24, 25, 26, 27, 28, 29, 30, 31};
const uint8_t kK16[16] = {0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15};
const uint8_t kIV12[12] = {0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11};
Bytes makeRand32() {
  Bytes v(32);
  for (int i = 0; i < 32; i++) v[i] = static_cast<uint8_t>((i * 7) % 256);
  return v;
}
Bytes makePub1() {
  // 与 Python 一致：[0x04] + [(i*3)%256 for i in range(32)] + [(i*5)%256 for i in range(32)]
  Bytes v;
  v.push_back(0x04);
  for (int i = 0; i < 32; i++) v.push_back(static_cast<uint8_t>((i * 3) % 256));
  for (int i = 0; i < 32; i++) v.push_back(static_cast<uint8_t>((i * 5) % 256));
  return v;
}
Bytes makePub2() {
  Bytes v;
  v.push_back(0x05);
  for (int i = 0; i < 64; i++) v.push_back(static_cast<uint8_t>((i * 11) % 256));
  return v;
}

Bytes fromStr(const char* s, size_t len) {
  return Bytes(reinterpret_cast<const uint8_t*>(s),
               reinterpret_cast<const uint8_t*>(s) + len);
}
int g_pass = 0, g_total = 0;
String g_report;

void check(const char* name, const Bytes& actual, const char* expectedHex) {
  g_total++;
  String actualHex = toHex(actual);
  String expected(expectedHex);
  if (actualHex == expected) {
    g_pass++;
    return;
  }
  // 失败输出：长度 + 首个差异偏移 + 两侧各 16 字节上下文（日志行短，防截断）
  g_report += "FAIL ";
  g_report += name;
  g_report += " len=";
  g_report += String(actual.size());
  g_report += "/";
  g_report += String(expected.length() / 2);
  size_t hexDiff = 0;
  size_t maxHex = actualHex.length() < expected.length() ? actualHex.length()
                                                         : expected.length();
  while (hexDiff < maxHex && actualHex[hexDiff] == expected[hexDiff]) hexDiff++;
  g_report += " diff@";
  g_report += String(hexDiff / 2);
  g_report += "\n";
  size_t ctxStart = (hexDiff / 2) > 8 ? (hexDiff / 2) - 8 : 0;
  g_report += "  exp:";
  g_report += expected.substring(ctxStart * 2, ctxStart * 2 + 32);
  g_report += "\n  act:";
  g_report += actualHex.substring(ctxStart * 2, ctxStart * 2 + 32);
  g_report += "\n";
}

void checkBool(const char* name, bool ok) {
  g_total++;
  if (ok) {
    g_pass++;
  } else {
    g_report += "FAIL ";
    g_report += name;
    g_report += " (bool)\n";
  }
}
}  // namespace

int runSelftest(int& totalOut, String& report) {
  g_pass = 0;
  g_total = 0;
  g_report = "";

  Bytes k32(kK32, kK32 + 32), k16(kK16, kK16 + 16), iv12(kIV12, kIV12 + 12);
  Bytes r32 = makeRand32();
  Bytes pub1 = makePub1(), pub2 = makePub2();

  // 1. ClientHello
  check("client_hello",
        buildClientHello(r32, 0x67E8F109, pub1, pub2), golden::kclient_hello);
  // 2. PSK ClientHello
  Bytes ticket(30, 'X');
  for (size_t i = 0; i < 30; i++) ticket[i] = "TICKET"[i % 6];
  check("psk_client_hello", buildPskClientHello(r32, 0x67E8F109, ticket),
        golden::kpsk_client_hello);
  // 3. 记录加密
  Bytes plain12 = fromStr("server flight", 13);
  Bytes recordCt;
  if (encryptRecordPayload(k16, iv12, 4, kCtAppData, plain12, recordCt))
    check("record_crypt", recordCt, golden::krecord_crypt);
  else
    checkBool("record_crypt", false);
  // 4-8. 密钥派生
  check("master", deriveMaster(k32, r32), golden::kmaster);
  TrafficKeys hk, ak, pk;
  deriveHandshakeKeys(k32, r32, hk);
  Bytes hkk;
  append(hkk, hk.clientKey);
  append(hkk, hk.serverKey);
  append(hkk, hk.clientIv);
  append(hkk, hk.serverIv);
  check("handshake_keys", hkk, golden::khandshake_keys);
  deriveAppKeys(k32, r32, ak);
  Bytes akk;
  append(akk, ak.clientKey);
  append(akk, ak.serverKey);
  append(akk, ak.clientIv);
  append(akk, ak.serverIv);
  check("app_keys", akk, golden::kapp_keys);
  check("finished_client", finishedVerifyData(k32, true, r32), golden::kfinished_client);
  check("finished_server", finishedVerifyData(k32, false, r32), golden::kfinished_server);
  derivePskOneWayKeys(k32, earlyKeysLabel(), r32, pk);
  Bytes pkk;
  append(pkk, pk.key);
  append(pkk, pk.iv);
  check("psk_oneway", pkk, golden::kpsk_oneway);
  // 9. WPKG 编解码回环（上游 Go 对拍样例）
  {
    Bytes upstream = fromHex(
        "01010102959aef3a0300040005818220060b0700080009000a010b000c000d00"
        "1100120114e00b150016959aef3a170019101a041c011d011e00000e001806"
        "6465766963651b026639004a");
    WpkgHead head;
    if (decodeWpkgHead(upstream, head)) {
      Bytes rebuilt = wpkgHead(head.ints, head.byts);
      check("wpkg_roundtrip", rebuilt, golden::kwpkg_roundtrip);
      bool fieldsOk = false;
      for (auto& [f, v] : head.byts) {
        if (f == 24) {
          String s(v.data(), v.size());
          fieldsOk = s == "device";
        }
      }
      checkBool("wpkg_field24", fieldsOk);
    } else {
      checkBool("wpkg_roundtrip", false);
    }
  }
  // 10. LZ4
  Bytes hello = fromStr("hello", 5);
  check("lz4_short", lz4AllLiteral(hello), golden::klz4_short);
  Bytes range64(64);
  for (int i = 0; i < 64; i++) range64[i] = static_cast<uint8_t>(i);
  check("lz4_long", lz4AllLiteral(range64), golden::klz4_long);
  // 11. jsapi plaintext（固定 session device）
  Bytes macAddr = fromStr("AA-BB-CC-DD-EE-FF", 17);
  Bytes jsapiUrl = fromStr("/cgi-bin/mmbiz-bin/js-login", 27);
  Bytes host = fromStr("wxd44977328b36e647", 18);
  check("jsapi",
        buildJsapiPlaintext(123456789, "wxaa78bf2905f01cee", jsapiUrl, kJsLoginCmdId,
                            host, macAddr),
        golden::kjsapi);
  // 12. shortlink
  Bytes payload8 = fromStr("payload-bytes", 13);
  check("shortlink", buildShortlink(kTransferCmd, 7, payload8), golden::kshortlink);
  // 13. 记录帧
  Bytes body3 = {0x01, 0x02, 0x03};
  check("record_frame", buildRecord(kCtHandshake, body3), golden::krecord_frame);
  // 14. 传输包回环（加密→解密，Python 生成的明文做基准）
  {
    AppSession session;
    session.sendKey = k16;
    session.recvKey = Bytes(16);
    for (int i = 0; i < 16; i++) session.recvKey[i] = static_cast<uint8_t>(16 + i);
    session.f9 = Bytes(8, 0xF9);
    session.uin = 42;
    // C++ 端用同样输入重建明文（空 url/cmd → 默认值）
    Bytes plain2 = buildJsapiPlaintext(42, "wxaa78bf2905f01cee", Bytes{}, 0, host,
                                       macAddr);
    check("transfer_plain", plain2, golden::ktransfer_plain);
    Bytes envelope;
    if (buildTransferPacket(session, plain2, envelope)) {
      checkBool("transfer_envelope_built", true);
      // 解析信封：4B len + 2B urllen + url + 2B hostlen + host + 4B slen + shortlink
      Bytes inner(envelope.begin() + 4, envelope.end());
      size_t urlLen = (inner[0] << 8) | inner[1];
      size_t pos = 2 + urlLen;
      size_t hostLen = (inner[pos] << 8) | inner[pos + 1];
      pos += 2 + hostLen;
      size_t slLen = (inner[pos] << 24) | (inner[pos + 1] << 16) | (inner[pos + 2] << 8) |
                     inner[pos + 3];
      pos += 4;
      Bytes shortlinkB(inner.begin() + pos, inner.begin() + pos + slLen);
      ShortlinkPacket pkt2;
      String stage = "ok";
      if (!parseShortlink(shortlinkB, pkt2)) {
        stage = "parseShortlink";
      } else {
        Bytes decrypted;
        // 回环：加密在 buildTransferPacket 内用 sendKey，这里同钥解密
        if (!sessionDecrypt(pkt2.body, session.sendKey, decrypted)) {
          stage = "sessionDecrypt len=" + String(pkt2.body.size());
        } else {
          checkBool("transfer_roundtrip", decrypted == plain2);
        }
      }
      if (stage != "ok") {
        g_total++;
        g_report += "FAIL transfer_roundtrip stage=";
        g_report += stage;
        g_report += "\n";
      }
    } else {
      checkBool("transfer_envelope_built", false);
    }
  }
  // 14b. AES-192-GCM 黄金向量（cryptography 库对拍）+ 回环
  {
    Bytes k24(24);
    for (int i = 0; i < 24; i++) k24[i] = static_cast<uint8_t>(i);
    Bytes nonce12(12);
    for (int i = 0; i < 12; i++) nonce12[i] = static_cast<uint8_t>(i);
    Bytes aad1 = fromStr("aad-1", 5);
    Bytes pt1 = fromStr("hello manual auth", 17);
    Bytes ct1;
    checkBool("gcm192_enc_v1",
              aesGcm192Encrypt(k24, nonce12, aad1, pt1.data(), pt1.size(), ct1) &&
                  ct1 == fromHex(String(gcmvec::kV1)));
    Bytes back1;
    checkBool("gcm192_dec_v1",
              aesGcm192Decrypt(k24, nonce12, aad1, ct1.data(), ct1.size(), back1) &&
                  back1 == pt1);
    Bytes pt2(33, 'x');
    Bytes ct2;
    checkBool("gcm192_enc_v2",
              aesGcm192Encrypt(k24, nonce12, Bytes{}, pt2.data(), pt2.size(), ct2) &&
                  ct2 == fromHex(String(gcmvec::kV2)));
    Bytes aad3 = fromStr("only-aad", 8);
    Bytes ct3;
    checkBool("gcm192_enc_v3",
              aesGcm192Encrypt(k24, nonce12, aad3, nullptr, 0, ct3) &&
                  ct3 == fromHex(String(gcmvec::kV3)));
    // 篡改 tag 必须拒绝
    if (!ct1.empty()) {
      Bytes bad = ct1;
      bad.back() ^= 1;
      Bytes out;
      checkBool("gcm192_tamper",
                !aesGcm192Decrypt(k24, nonce12, aad1, bad.data(), bad.size(), out));
    } else {
      checkBool("gcm192_tamper", false);
    }
  }

  // 15. LZ4 解码：用向量里的压缩串解回 range64
  {
    Bytes compressed = fromHex(String(golden::klz4_long));
    Bytes back;
    checkBool("lz4_decompress",
              lz4Decompress(compressed.data(), compressed.size(), back) && back == range64);
  }

  totalOut = g_total;
  report = g_report;
  return g_pass;
}

}  // namespace wx
