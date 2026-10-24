#include "wx_wire.h"

#include <esp_random.h>

#include "logger.h"

#include <mbedtls/base64.h>

namespace wx {

Bytes manualAuthServerPub() {
  static const uint8_t kPub[] = {
      0x04, 0xef, 0x87, 0x87, 0x6d, 0x64, 0x78, 0xb1, 0x5f, 0x17, 0x96, 0xea,
      0xb1, 0x20, 0x68, 0x61, 0x05, 0x41, 0x17, 0x3b, 0x71, 0x76, 0xb6, 0x7f,
      0x1d, 0xcc, 0x86, 0x68, 0x3e, 0x90, 0x1a, 0xcd, 0x44, 0xd1, 0x8b, 0x4a,
      0xc3, 0x69, 0x38, 0x25, 0x1d, 0x08, 0x12, 0xdd, 0x0c, 0xf8, 0x42, 0xaa,
      0x2d, 0x6c, 0xbb, 0x81, 0x15, 0x71, 0x2d, 0x1c, 0x00, 0x87, 0xdc, 0xef,
      0xc1, 0x4a, 0x44, 0xcd, 0x58};
  return Bytes(kPub, kPub + sizeof(kPub));
}

namespace {
const char* kHostAppIdDefault = "wxd44977328b36e647";
const char* kIlinkDevice = "ilinkapp_060000b7b93f6c";
const char* kTransferUrl = "/ilink/ilinkapp/mp/wxaruntime_transfer";
const char* kTransferHost = "shortcloud.weixin.com";
const char* kJsLoginUrl = "/cgi-bin/mmbiz-bin/js-login";
const char* kWindowsPlugin = "WindowsxWebPlugin";
const char* kWindowsName = "Windows";
const char* kUnifiedPcWindows = "UnifiedPCWindows";
const char* kSessionKeyLiteral = "sessionkey";

const char* kLabelExpandedSecret = "expanded secret";
const char* kLabelHandshakeKeys = "handshake key expansion";
const char* kLabelAppDataKeys = "application data key expansion";
const char* kLabelClientFinished = "client finished";
const char* kLabelServerFinished = "server finished";
const char* kLabelPskAccess = "PSK_ACCESS";
const char* kLabelPskRefresh = "PSK_REFRESH";
const char* kEarlyKeysLabel = "early data key expansion";

Bytes strBytes(const char* s) { return Bytes(s, s + strlen(s)); }
}  // namespace

// ==== MMTLS 记录 / 握手 ====

Bytes buildRecord(uint8_t contentType, const Bytes& payload) {
  Bytes out;
  out.reserve(5 + payload.size());
  appendU8(out, contentType);
  appendU16Be(out, kRecordVersion);
  appendU16Be(out, static_cast<uint16_t>(payload.size()));
  append(out, payload);
  return out;
}

bool parseRecords(const Bytes& data, std::vector<Record>& out, size_t& consumed) {
  consumed = 0;
  size_t offset = 0;
  while (offset + 5 <= data.size()) {
    uint8_t contentType = data[offset];
    uint16_t version = (static_cast<uint16_t>(data[offset + 1]) << 8) | data[offset + 2];
    uint16_t length = (static_cast<uint16_t>(data[offset + 3]) << 8) | data[offset + 4];
    bool knownType = contentType == kCtAlert || contentType == kCtHandshake ||
                     contentType == kCtAppData || contentType == kCtPskHandshake ||
                     contentType == 0x14;
    if (version != kRecordVersion || !knownType) return false;
    size_t end = offset + 5 + length;
    if (end > data.size()) break;  // 不完整，等待更多数据
    Record rec;
    rec.contentType = contentType;
    rec.payload.assign(data.begin() + offset + 5, data.begin() + end);
    out.push_back(std::move(rec));
    offset = end;
  }
  consumed = offset;
  return true;
}

Bytes buildHandshake(uint8_t messageType, const Bytes& body) {
  Bytes payload;
  payload.reserve(1 + body.size());
  payload.push_back(messageType);
  append(payload, body);
  Bytes out;
  out.reserve(4 + payload.size());
  appendU32Be(out, static_cast<uint32_t>(payload.size()));
  append(out, payload);
  return out;
}

bool splitHandshake(const Bytes& payload, uint8_t& messageType, Bytes& body) {
  if (payload.size() < 5) return false;
  uint32_t total = (static_cast<uint32_t>(payload[0]) << 24) |
                   (static_cast<uint32_t>(payload[1]) << 16) |
                   (static_cast<uint32_t>(payload[2]) << 8) | payload[3];
  if (total == 0 || total + 4 > payload.size()) return false;
  messageType = payload[4];
  body.assign(payload.begin() + 5, payload.begin() + total + 4);
  return true;
}

Bytes buildClientHello(const Bytes& clientRandom32, uint32_t timestamp,
                       const Bytes& g1Public65, const Bytes& g2Public65) {
  if (clientRandom32.size() != 32 || g1Public65.size() != kKeyLen ||
      g2Public65.size() != kKeyLen) {
    return Bytes{};
  }
  Bytes body;
  const uint8_t prefix[] = {0x03, 0xF1, 0x01};
  append(body, prefix, 3);
  appendU16Be(body, kCipherEcdhe);
  append(body, clientRandom32);
  appendU32Be(body, timestamp);
  // key_shares 扩展
  Bytes keyShares;
  const uint8_t ksPrefix[] = {0x00, 0x10, 0x02};
  append(keyShares, ksPrefix, 3);
  const Bytes* keys[] = {&g1Public65, &g2Public65};
  const uint32_t groups[] = {kGroupSecp256r1, 0x02};
  for (int i = 0; i < 2; i++) {
    Bytes share;
    appendU32Be(share, groups[i]);
    appendU16Be(share, static_cast<uint16_t>(keys[i]->size()));
    append(share, *keys[i]);
    appendU32Be(keyShares, static_cast<uint32_t>(share.size()));
    append(keyShares, share);
  }
  const uint8_t ksSuffix[] = {0x00, 0x00, 0x00, 0x01};
  append(keyShares, ksSuffix, 4);
  Bytes extension;
  appendU8(extension, kExtKeyShare);
  appendU32Be(extension, static_cast<uint32_t>(keyShares.size()));
  append(extension, keyShares);
  appendU32Be(body, static_cast<uint32_t>(extension.size()));
  append(body, extension);
  return buildHandshake(kHsClientHello, body);
}

Bytes buildPskClientHello(const Bytes& clientRandom32, uint32_t timestamp,
                          const Bytes& ticketEntry) {
  if (clientRandom32.size() != 32) return Bytes{};
  Bytes body;
  const uint8_t prefix[] = {0x03, 0xF1, 0x01};
  append(body, prefix, 3);
  appendU16Be(body, kCipherPsk);
  append(body, clientRandom32);
  appendU32Be(body, timestamp);
  Bytes extensionData;
  const uint8_t edPrefix[] = {0x00, 0x0F, 0x01};
  append(extensionData, edPrefix, 3);
  appendU32Be(extensionData, static_cast<uint32_t>(ticketEntry.size()));
  append(extensionData, ticketEntry);
  Bytes extension;
  appendU8(extension, kExtKeyShare);
  appendU32Be(extension, static_cast<uint32_t>(extensionData.size()));
  append(extension, extensionData);
  appendU32Be(body, static_cast<uint32_t>(extension.size()));
  append(body, extension);
  return buildHandshake(kHsClientHello, body);
}

bool parseServerHello(const Bytes& body, ServerHello& out) {
  if (body.size() < 40) return false;
  out.cipher = (static_cast<uint16_t>(body[2]) << 8) | body[3];
  out.serverRandom.assign(body.begin() + 4, body.begin() + 36);
  uint32_t extLen = (static_cast<uint32_t>(body[36]) << 24) |
                    (static_cast<uint32_t>(body[37]) << 16) |
                    (static_cast<uint32_t>(body[38]) << 8) | body[39];
  size_t extStart = 40;
  size_t extEnd = extStart + extLen;
  if (extEnd > body.size()) return false;
  Bytes ext(body.begin() + extStart, body.begin() + extEnd);
  out.serverGroup = kGroupSecp256r1;
  out.serverPubKey.clear();
  if (!ext.empty() && ext[0] == kExtKeyShare) {
    if (ext.size() < 7) return false;
    size_t cursor = 7;
    if (cursor + 6 > ext.size()) return false;
    out.serverGroup = (static_cast<uint32_t>(ext[cursor]) << 24) |
                      (static_cast<uint32_t>(ext[cursor + 1]) << 16) |
                      (static_cast<uint32_t>(ext[cursor + 2]) << 8) | ext[cursor + 3];
    cursor += 4;
    uint16_t keyLen = (static_cast<uint16_t>(ext[cursor]) << 8) | ext[cursor + 1];
    cursor += 2;
    if (cursor + keyLen > ext.size() || keyLen != 65) return false;
    out.serverPubKey.assign(ext.begin() + cursor, ext.begin() + cursor + keyLen);
  } else {
    // 后备：扫描 0x00 0x41 0x04（65 字节未压缩点）
    size_t limit = ext.size() >= 67 ? ext.size() - 67 + 1 : 0;
    for (size_t i = 0; i < limit; i++) {
      if (ext[i] == 0x00 && ext[i + 1] == 0x41 && ext[i + 2] == 0x04) {
        out.serverPubKey.assign(ext.begin() + i + 2, ext.begin() + i + 2 + 65);
        break;
      }
    }
  }
  return !out.serverPubKey.empty();
}

Bytes buildFinished(const Bytes& verifyData32) {
  if (verifyData32.size() != 32) return Bytes{};
  Bytes body;
  appendU16Be(body, 32);
  append(body, verifyData32);
  return buildHandshake(kHsFinished, body);
}

bool parseFinished(const Bytes& body, Bytes& verifyData) {
  if (body.size() < 2) return false;
  uint16_t length = (static_cast<uint16_t>(body[0]) << 8) | body[1];
  if (length != body.size() - 2) return false;
  verifyData.assign(body.begin() + 2, body.end());
  return true;
}

bool parseNewSessionTicket(const Bytes& body, std::vector<PSKEntry>& out) {
  if (body.empty()) return false;
  size_t count = body[0];
  size_t offset = 1;
  for (size_t i = 0; i < count; i++) {
    if (offset + 4 > body.size()) return false;
    uint32_t entryLen = (static_cast<uint32_t>(body[offset]) << 24) |
                        (static_cast<uint32_t>(body[offset + 1]) << 16) |
                        (static_cast<uint32_t>(body[offset + 2]) << 8) |
                        body[offset + 3];
    offset += 4;
    if (entryLen < 5 || offset + entryLen > body.size()) return false;
    Bytes entry(body.begin() + offset, body.begin() + offset + entryLen);
    offset += entryLen;
    PSKEntry psk;
    psk.pskType = entry[0];
    psk.lifetime = (static_cast<uint32_t>(entry[1]) << 24) |
                   (static_cast<uint32_t>(entry[2]) << 16) |
                   (static_cast<uint32_t>(entry[3]) << 8) | entry[4];
    psk.ticketEntry = std::move(entry);  // 整个 entry 即 ticket
    out.push_back(std::move(psk));
  }
  return offset == body.size();
}

// ==== 记录加解密 ====

Bytes buildRecordAad(uint64_t sequence, uint8_t contentType, size_t cipherLen) {
  Bytes aad;
  aad.reserve(13);
  appendU64Be(aad, sequence);
  appendU8(aad, contentType);
  aad.push_back(0xF1);
  aad.push_back(0x03);
  appendU16Be(aad, static_cast<uint16_t>(cipherLen));
  return aad;
}

static Bytes recordNonce(const Bytes& iv, uint64_t sequence) {
  Bytes nonce = iv;  // 12 字节
  Bytes seqBytes(8);
  uint64_t v = sequence;
  for (int i = 7; i >= 0; i--) {
    seqBytes[i] = static_cast<uint8_t>(v & 0xFF);
    v >>= 8;
  }
  for (int i = 0; i < 8; i++) nonce[kAesIvLen - 8 + i] ^= seqBytes[i];
  return nonce;
}

bool encryptRecordPayload(const Bytes& key, const Bytes& iv, uint64_t sequence,
                          uint8_t contentType, const Bytes& plaintext, Bytes& out) {
  Bytes aad = buildRecordAad(sequence, contentType, plaintext.size() + kGcmTagLen);
  GcmResult r = aesGcmEncrypt(key, recordNonce(iv, sequence), aad, plaintext.data(),
                              plaintext.size());
  if (!r.ok) return false;
  out = std::move(r.data);
  return true;
}

bool decryptRecordPayload(const Bytes& key, const Bytes& iv, uint64_t sequence,
                          uint8_t contentType, const Bytes& ciphertext, Bytes& out) {
  Bytes aad = buildRecordAad(sequence, contentType, ciphertext.size());
  GcmResult r = aesGcmDecrypt(key, recordNonce(iv, sequence), aad,
                              ciphertext.data(), ciphertext.size());
  if (!r.ok) return false;
  out = std::move(r.data);
  return true;
}

// ==== 密钥派生 ====

Bytes deriveMaster(const Bytes& secret, const Bytes& handshakeHash) {
  return expandLabel(secret, kLabelExpandedSecret, handshakeHash, 32);
}

static bool splitBiKeys(const Bytes& keyBlock, TrafficKeys& out) {
  if (keyBlock.size() != 56) return false;
  out.clientKey.assign(keyBlock.begin(), keyBlock.begin() + 16);
  out.serverKey.assign(keyBlock.begin() + 16, keyBlock.begin() + 32);
  out.clientIv.assign(keyBlock.begin() + 32, keyBlock.begin() + 44);
  out.serverIv.assign(keyBlock.begin() + 44, keyBlock.begin() + 56);
  return true;
}

bool deriveHandshakeKeys(const Bytes& secret, const Bytes& handshakeHash,
                         TrafficKeys& out) {
  return splitBiKeys(expandLabel(secret, kLabelHandshakeKeys, handshakeHash, 56), out);
}

bool deriveAppKeys(const Bytes& master, const Bytes& handshakeHash, TrafficKeys& out) {
  return splitBiKeys(expandLabel(master, kLabelAppDataKeys, handshakeHash, 56), out);
}

Bytes finishedVerifyData(const Bytes& secret, bool isClient,
                         const Bytes& handshakeHash) {
  // 两步（与参考实现一致）：先派生 finished_key，再对握手哈希做 HMAC
  Bytes finishedKey =
      expandLabel(secret, isClient ? kLabelClientFinished : kLabelServerFinished,
                  Bytes{}, 32);
  return hmacSha256(finishedKey, handshakeHash);
}

Bytes derivePsk(const Bytes& secret, const Bytes& certVerifyHash, uint8_t pskType) {
  return expandLabel(secret, pskType == 1 ? kLabelPskAccess : kLabelPskRefresh,
                     certVerifyHash, 32);
}

bool derivePskOneWayKeys(const Bytes& psk, const char* label, const Bytes& hash,
                         TrafficKeys& out) {
  Bytes block = expandLabel(psk, label, hash, 28);
  out.key.assign(block.begin(), block.begin() + 16);
  out.iv.assign(block.begin() + 16, block.begin() + 28);
  return true;
}

const char* earlyKeysLabel() { return kEarlyKeysLabel; }
const char* handshakeKeysLabel() { return kLabelHandshakeKeys; }

// ==== 短连接包 ====

Bytes buildShortlink(uint32_t cmd, uint32_t seq, const Bytes& body, uint16_t version) {
  if (version == 0) version = kShortlinkVersion;
  Bytes out;
  out.reserve(16 + body.size());
  appendU32Be(out, static_cast<uint32_t>(16 + body.size()));
  appendU16Be(out, kShortlinkMagic);
  appendU16Be(out, version);
  appendU32Be(out, cmd);
  appendU32Be(out, seq);
  append(out, body);
  return out;
}

bool parseShortlink(const Bytes& packet, ShortlinkPacket& out) {
  if (packet.size() < 16) return false;
  out.totalLen = (static_cast<uint32_t>(packet[0]) << 24) |
                 (static_cast<uint32_t>(packet[1]) << 16) |
                 (static_cast<uint32_t>(packet[2]) << 8) | packet[3];
  out.magic = (static_cast<uint16_t>(packet[4]) << 8) | packet[5];
  out.ver = (static_cast<uint16_t>(packet[6]) << 8) | packet[7];
  out.cmd = (static_cast<uint32_t>(packet[8]) << 24) |
            (static_cast<uint32_t>(packet[9]) << 16) |
            (static_cast<uint32_t>(packet[10]) << 8) | packet[11];
  out.seq = (static_cast<uint32_t>(packet[12]) << 24) |
            (static_cast<uint32_t>(packet[13]) << 16) |
            (static_cast<uint32_t>(packet[14]) << 8) | packet[15];
  if (out.totalLen < 16 || out.totalLen > packet.size()) return false;
  out.body.assign(packet.begin() + 16, packet.begin() + out.totalLen);
  return true;
}

// ==== WPKG 头 ====

Bytes wpkgHead(const std::vector<std::pair<uint32_t, uint64_t>>& ints,
               const std::vector<std::pair<uint32_t, Bytes>>& byts) {
  Bytes result = encodeVarint(1);
  for (const auto& [field, value] : ints) {
    append(result, encodeVarint(field));
    append(result, encodeVarint(value));
  }
  append(result, encodeVarint(0));
  for (const auto& [field, value] : byts) {
    append(result, encodeVarint(field));
    append(result, encodeVarint(value.size()));
    append(result, value);
  }
  size_t prefixLength = result.size();
  result.push_back(0);
  append(result, encodeVarint(prefixLength + 1));
  return result;
}

bool decodeWpkgHead(const Bytes& data, WpkgHead& out) {
  size_t offset = 0;
  uint64_t version;
  if (!readVarint(data.data(), data.size(), offset, version) || version != 1)
    return false;
  while (true) {
    uint64_t field;
    if (!readVarint(data.data(), data.size(), offset, field)) return false;
    if (field == 0) break;
    if (field > ((1 << 29) - 1)) return false;
    uint64_t value;
    if (!readVarint(data.data(), data.size(), offset, value)) return false;
    out.ints.emplace_back(static_cast<uint32_t>(field), value);
  }
  while (true) {
    uint64_t field;
    if (!readVarint(data.data(), data.size(), offset, field)) return false;
    if (field == 0) break;
    if (field > ((1 << 29) - 1)) return false;
    uint64_t length;
    if (!readVarint(data.data(), data.size(), offset, length)) return false;
    if (length > data.size() - offset) return false;
    out.byts.emplace_back(
        static_cast<uint32_t>(field),
        Bytes(data.begin() + offset, data.begin() + offset + length));
    offset += static_cast<size_t>(length);
  }
  size_t trailerOffset = offset;
  uint64_t declared;
  if (!readVarint(data.data(), data.size(), offset, declared)) return false;
  if (declared != trailerOffset) return false;
  out.offset = offset;
  return true;
}

// ==== login_buffer / ManualAuth ====

bool parseLoginBuffer(const String& loginBufferB64, LoginMeta& out) {
  // base64 直接按上界分配解码（mbedtls 首次探测会返回 BUFFER_TOO_SMALL，属正常）
  size_t cap = (loginBufferB64.length() / 4) * 3 + 3;
  if (cap == 0 || cap > 4096) return false;
  Bytes raw(cap);
  size_t olen = 0;
  if (mbedtls_base64_decode(raw.data(), raw.size(), &olen,
                            reinterpret_cast<const uint8_t*>(loginBufferB64.c_str()),
                            loginBufferB64.length()) != 0)
    return false;
  raw.resize(olen);
  PbField f1, f2, f3;
  if (!pbGet(raw.data(), raw.size(), 1, f1) || !f1.isBytes || f1.bytesValue.empty())
    return false;
  if (!pbGet(raw.data(), raw.size(), 2, f2) || !f2.isBytes || f2.bytesValue.empty())
    return false;
  out.ticket = f1.bytesValue;
  out.deviceId = f2.bytesValue;
  if (pbGet(raw.data(), raw.size(), 3, f3) && f3.isBytes) {
    out.hostAppId = f3.bytesValue;
  }
  return true;
}

namespace {

Bytes randomSessionDevice() {
  uint8_t mac[6];
  esp_fill_random(mac, 6);
  mac[0] = (mac[0] | 0x02) & 0xFE;
  char buf[18];
  snprintf(buf, sizeof(buf), "%02X-%02X-%02X-%02X-%02X-%02X", mac[0], mac[1], mac[2],
           mac[3], mac[4], mac[5]);
  return strBytes(buf);
}

Bytes buildManualAuthRequest(const LoginMeta& meta, const Bytes& appDeviceId) {
  Bytes appBase = pbLen(1, appDeviceId);
  append(appBase, pbVar(2, kAppVersion));
  Bytes thirdApp = pbLen(1, meta.ticket);
  Bytes request = pbLen(1, appBase);
  append(request, pbLen(3, thirdApp));
  append(request, pbVar(4, 4));
  append(request, pbLen(6, Bytes{}));
  append(request, pbVar(7, 0));
  append(request, pbVar(8, 6));
  return request;
}

Bytes manualAuthHead(const Bytes& deviceId) {
  const std::vector<std::pair<uint32_t, uint64_t>> ints = {
      {1, 1},   {2, 0},   {3, 0},        {4, 0},  {5, kClientVersion}, {6, 11},
      {7, 0},   {8, 0},   {9, 0},        {10, 1}, {11, 0},            {12, 0},
      {13, 0},  {17, 0},  {18, 1},       {20, 1504}, {21, 0},         {22, 0},
      {23, 0},  {25, 17}, {26, 4},       {28, 1}, {29, 1},            {30, 0}};
  return wpkgHead(ints, {{14, Bytes{}}, {24, deviceId}, {27, Bytes{}}});
}

/// 混合 ECDH 加密（secret=SHA256(ECDH)，CEK 随机，双层 AES-GCM）。
bool hybridEcdhEncrypt(const Bytes& plaintext, const Bytes& serverPublicKey,
                       EcdhKey*& ephemeralOut, Bytes& okmOut, Bytes& compBodyOut,
                       Bytes& out) {
  EcdhKey* ephemeral = new EcdhKey();
  do {
    if (!ephemeral->generate()) { LOG_W("WX","hyb: gen fail"); break; }
    Bytes ephemeralPub;
    if (!ephemeral->publicKey(ephemeralPub)) { LOG_W("WX","hyb: pubkey fail"); break; }
    Bytes shared;
    if (!ephemeral->rawShared(serverPublicKey, shared)) { LOG_W("WX","hyb: rawshared fail"); break; }
    Bytes secret = sha256(shared);
    // hash1 = SHA256("1" || "415" || ephemeralPub)
    Bytes h1input = strBytes("1");
    append(h1input, strBytes("415"));
    append(h1input, ephemeralPub);
    Bytes hash1 = sha256(h1input);
    Bytes cek(32);
    esp_fill_random(cek.data(), cek.size());
    Bytes secretKey(secret.begin(), secret.begin() + 24);
    Bytes encryptedKey;
    if (!gcmEncryptLayout(secretKey, hash1, cek.data(), cek.size(), encryptedKey,
                          nullptr)) { LOG_W("WX","hyb: enc key fail"); break; }
    Bytes prk = hkdfExtract(strBytes("security hdkf expand"), cek);
    Bytes okm = hkdfExpand(prk, hash1, 56);
    // hash2 = SHA256("1" || "415" || ephemeralPub || encryptedKey)
    Bytes h2input = h1input;
    append(h2input, encryptedKey);
    Bytes hash2 = sha256(h2input);
    Bytes compressed = lz4AllLiteral(plaintext);
    Bytes okmKey(okm.begin(), okm.begin() + 24);
    Bytes encryptedBody;
    if (!gcmEncryptLayout(okmKey, hash2, compressed.data(), compressed.size(),
                          encryptedBody, nullptr)) { LOG_W("WX","hyb: enc body fail"); break; }
    Bytes ecdhKey = pbVar(1, 415);
    append(ecdhKey, pbLen(2, ephemeralPub));
    out = pbVar(1, 1);
    append(out, pbLen(2, ecdhKey));
    append(out, pbLen(3, encryptedKey));
    append(out, pbLen(4, Bytes{}));
    append(out, pbLen(5, encryptedBody));
    ephemeralOut = ephemeral;
    okmOut = std::move(okm);
    compBodyOut = std::move(compressed);
    return true;
  } while (false);
  delete ephemeral;
  return false;
}

}  // namespace

bool buildLoginBody(const String& loginBuffer, const Bytes& deviceId,
                    const Bytes& appDeviceId, ManualAuthTemp& temp, Bytes& out) {
  LoginMeta meta;
  if (!parseLoginBuffer(loginBuffer, meta)) { LOG_W("WX","loginbody: parse lb fail"); return false; }
  Bytes request = buildManualAuthRequest(meta, appDeviceId);
  Bytes encrypted;
  EcdhKey* ephemeral = nullptr;
  Bytes okm, compBody;
  if (!hybridEcdhEncrypt(request, manualAuthServerPub(), ephemeral, okm, compBody,
                         encrypted))
    return false;
  delete temp.ephemeral;
  temp.ephemeral = ephemeral;
  temp.okm = std::move(okm);
  temp.compBody = std::move(compBody);
  out = manualAuthHead(deviceId);
  append(out, encrypted);
  return true;
}

bool parseLoginResponse(const Bytes& body, ManualAuthTemp& temp, Bytes& out) {
  // 定位混合加密响应体：优先 WPKG 头后的 0x0A，后备 marker 搜索
  Bytes hybrid;
  bool found = false;
  WpkgHead head;
  if (decodeWpkgHead(body, head) && head.offset < body.size() &&
      body[head.offset] == 0x0A) {
    hybrid.assign(body.begin() + head.offset, body.end());
    found = true;
  }
  if (!found) {
    static const uint8_t marker[] = {0x08, 0x9F, 0x03, 0x12, 0x41, 0x04};
    for (size_t i = 2; i + sizeof(marker) <= body.size(); i++) {
      if (memcmp(body.data() + i, marker, sizeof(marker)) == 0) {
        hybrid.assign(body.begin() + i - 2, body.end());
        found = true;
        break;
      }
    }
  }
  if (!found || temp.ephemeral == nullptr || temp.okm.size() != 56 ||
      temp.compBody.empty())
    return false;
  PbField f1, f2, f3;
  if (!pbGet(hybrid.data(), hybrid.size(), 1, f1) || !f1.isBytes) return false;
  if (!pbGet(hybrid.data(), hybrid.size(), 3, f3) || !f3.isBytes) return false;
  PbField serverPubField;
  if (!pbGet(f1.bytesValue.data(), f1.bytesValue.size(), 2, serverPubField) ||
      !serverPubField.isBytes)
    return false;
  uint64_t credentialType = 1;  // 字段缺失时默认 1（与参考实现一致）
  if (pbGet(hybrid.data(), hybrid.size(), 2, f2)) {
    if (f2.isBytes) return false;
    credentialType = f2.intValue;
  }
  Bytes shared;
  if (!temp.ephemeral->rawShared(serverPubField.bytesValue, shared)) return false;
  Bytes secret = sha256(shared);
  // aad = SHA256(okm[24:56] || compBody || "415" || serverPub || str(credentialType))
  Bytes aadInput(temp.okm.begin() + 24, temp.okm.end());
  append(aadInput, temp.compBody);
  append(aadInput, strBytes("415"));
  append(aadInput, serverPubField.bytesValue);
  char credStr[24];
  snprintf(credStr, sizeof(credStr), "%llu",
           static_cast<unsigned long long>(credentialType));
  append(aadInput, strBytes(credStr));
  Bytes aad = sha256(aadInput);
  Bytes secretKey(secret.begin(), secret.begin() + 24);
  Bytes compressed;
  if (!gcmDecryptLayout(secretKey, aad, f3.bytesValue, compressed)) return false;
  return lz4Decompress(compressed.data(), compressed.size(), out);
}

bool extractSession(const Bytes& manualAuthResponse, AppSession& out) {
  PbField f3;
  if (!pbGet(manualAuthResponse.data(), manualAuthResponse.size(), 3, f3) ||
      !f3.isBytes)
    return false;
  PbField sessionBlock, identityBlock;
  if (!pbGet(f3.bytesValue.data(), f3.bytesValue.size(), 2, sessionBlock) ||
      !sessionBlock.isBytes)
    return false;
  if (!pbGet(f3.bytesValue.data(), f3.bytesValue.size(), 3, identityBlock) ||
      !identityBlock.isBytes)
    return false;
  PbField sendKey, recvKey, f9, ticket, uin;
  const auto* sb = &sessionBlock.bytesValue;
  if (!pbGet(sb->data(), sb->size(), 1, sendKey) || !sendKey.isBytes) return false;
  if (!pbGet(sb->data(), sb->size(), 2, recvKey) || !recvKey.isBytes) return false;
  pbGet(sb->data(), sb->size(), 9, f9);
  pbGet(sb->data(), sb->size(), 12, ticket);
  const auto* ib = &identityBlock.bytesValue;
  if (!pbGet(ib->data(), ib->size(), 1, uin) || uin.isBytes) return false;
  if (sendKey.bytesValue.size() == 16 || sendKey.bytesValue.size() == 24 ||
      sendKey.bytesValue.size() == 32)
    out.sendKey = sendKey.bytesValue;
  else
    return false;
  if (recvKey.bytesValue.size() == 16 || recvKey.bytesValue.size() == 24 ||
      recvKey.bytesValue.size() == 32)
    out.recvKey = recvKey.bytesValue;
  else
    return false;
  if (uin.intValue <= 0) return false;
  out.sendKey = sendKey.bytesValue;
  out.recvKey = recvKey.bytesValue;
  out.f9 = f9.isBytes ? f9.bytesValue : Bytes{};
  out.ticket = ticket.isBytes ? ticket.bytesValue : Bytes{};
  out.uin = uin.intValue;
  out.deviceId = strBytes(kIlinkDevice);
  return true;
}

// ==== js-login ====

namespace {

Bytes sessionInfo(uint64_t uin, const char* platform, const Bytes& sessionDevice) {
  uint32_t uin32 = static_cast<uint32_t>(uin & 0xFFFFFFFF);
  Bytes out = pbLen(1, strBytes(kSessionKeyLiteral));
  append(out, pbVar(2, uin32));
  append(out, pbLen(3, sessionDevice));
  append(out, pbVar(4, kSessionClientVersion));
  append(out, pbLen(5, strBytes(platform)));
  append(out, pbVar(6, 0));
  return out;
}

Bytes sessionWpkgHead(uint64_t uin, const Bytes& f9, const Bytes& deviceId) {
  Bytes dev = deviceId.empty() ? strBytes(kIlinkDevice) : deviceId;
  const std::vector<std::pair<uint32_t, uint64_t>> ints = {
      {1, 1},          {2, uin},  {3, 0},   {4, 0},  {5, kClientVersion}, {6, 11},
      {7, 0},          {8, 0},    {9, 0},   {10, 1}, {11, 0},            {12, 0},
      {13, 0},         {17, 0},   {18, 1},  {20, 1504}, {21, 0},         {22, uin},
      {23, 0},         {25, 16},  {26, 4},  {28, 1}, {29, 1},            {30, 0}};
  return wpkgHead(ints, {{14, Bytes{}}, {24, dev}, {27, f9}});
}

}  // namespace

Bytes buildJsapiPlaintext(uint64_t uin, const String& appId, const Bytes& url,
                          uint32_t cmd, const Bytes& hostAppId,
                          const Bytes& sessionDevice) {
  Bytes realUrl = url.empty() ? strBytes(kJsLoginUrl) : url;
  if (cmd == 0) cmd = kJsLoginCmdId;
  Bytes host = hostAppId.empty() ? strBytes(kHostAppIdDefault) : hostAppId;
  Bytes device = sessionDevice.empty() ? randomSessionDevice() : sessionDevice;
  Bytes appIdBytes(appId.begin(), appId.end());
  Bytes body = pbLen(1, sessionInfo(uin, kUnifiedPcWindows, device));
  append(body, pbLen(2, appIdBytes));
  append(body, pbVar(4, 1));
  append(body, pbLen(5, Bytes{}));
  append(body, pbLen(6, Bytes{}));
  append(body, pbVar(7, 1));
  Bytes out = pbLen(1, sessionInfo(uin, kWindowsName, device));
  append(out, pbLen(2, realUrl));
  append(out, pbLen(3, host));
  append(out, pbVar(4, 5));
  append(out, pbLen(5, body));
  append(out, pbLen(6, appIdBytes));
  append(out, pbVar(7, cmd));
  append(out, pbVar(8, 1610627409));
  append(out, pbLen(9, strBytes(kWindowsPlugin)));
  append(out, pbVar(10, 573651281));
  return out;
}

bool buildTransferPacket(const AppSession& session, const Bytes& plaintext,
                         Bytes& out) {
  Bytes compressed = lz4AllLiteral(plaintext);
  Bytes encrypted;
  if (!gcmEncryptLayout(session.sendKey, Bytes{}, compressed.data(), compressed.size(),
                        encrypted, nullptr))
    return false;
  Bytes head = sessionWpkgHead(session.uin, session.f9, session.deviceId);
  append(head, encrypted);
  Bytes shortlink = buildShortlink(kTransferCmd, 0, head);
  Bytes url = strBytes(kTransferUrl);
  Bytes host = strBytes(kTransferHost);
  Bytes envelope;
  appendU16Be(envelope, static_cast<uint16_t>(url.size()));
  append(envelope, url);
  appendU16Be(envelope, static_cast<uint16_t>(host.size()));
  append(envelope, host);
  appendU32Be(envelope, static_cast<uint32_t>(shortlink.size()));
  append(envelope, shortlink);
  out.clear();
  appendU32Be(out, static_cast<uint32_t>(envelope.size()));
  append(out, envelope);
  return true;
}

bool sessionDecrypt(const Bytes& body, const Bytes& recvKey, Bytes& out) {
  // 候选偏移：WPKG 头尾 + 前 220 字节逐个尝试（与参考实现一致的探测策略）
  std::vector<size_t> offsets;
  WpkgHead head;
  if (decodeWpkgHead(body, head)) offsets.push_back(head.offset);
  for (size_t i = 0; i < 220 && i < body.size(); i++) offsets.push_back(i);
  for (size_t offset : offsets) {
    if (body.size() - offset < 29) continue;
    Bytes blob(body.begin() + offset, body.end());
    Bytes compressed;
    if (!gcmDecryptLayout(recvKey, Bytes{}, blob, compressed)) continue;
    if (lz4Decompress(compressed.data(), compressed.size(), out)) return true;
  }
  return false;
}

bool extractCode(const Bytes& response, Bytes& code) {
  PbField nested;
  if (!pbGet(response.data(), response.size(), 2, nested) || !nested.isBytes)
    return false;
  PbField codeField;
  if (!pbGet(nested.bytesValue.data(), nested.bytesValue.size(), 3, codeField) ||
      !codeField.isBytes)
    return false;
  code = codeField.bytesValue;
  return true;
}

}  // namespace wx
