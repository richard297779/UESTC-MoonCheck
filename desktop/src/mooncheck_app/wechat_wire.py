"""Python port of this repository's MIT wx_crypto.cpp / wx_wire.cpp.

Protocol constants originate from YYB-Go-Enhanced, as documented in firmware.
No upstream Python implementation is vendored. All input/output is bytes.
"""
import base64
import hashlib
import hmac
import os
import struct


class ProtocolError(ValueError):
    """Deliberately contains no wire data or credentials."""


def crypto():
    try:
        from Crypto.Cipher import AES
        from Crypto.PublicKey import ECC
        return AES, ECC
    except ImportError:
        raise ProtocolError('微信链路需要 PyCryptodome，请安装 desktop/requirements.txt') from None


def sha(data):
    return hashlib.sha256(data).digest()


def expand(secret, label, context=b'', length=32):
    if not 0 <= length <= 8160:
        raise ProtocolError('HKDF length invalid')
    out = block = b''
    for i in range(1, (length + 31) // 32 + 1):
        block = hmac.digest(secret, block + label + context + bytes([i]), 'sha256')
        out += block
    return out[:length]


def keypair():
    return crypto()[1].generate(curve='P-256')


def public(key):
    return b'\x04' + int(key.pointQ.x).to_bytes(32, 'big') + int(key.pointQ.y).to_bytes(32, 'big')


def shared(key, peer):
    if len(peer) != 65 or peer[0] != 4:
        raise ProtocolError('Invalid P-256 key')
    try:
        other = crypto()[1].construct(curve='P-256', point_x=int.from_bytes(peer[1:33], 'big'), point_y=int.from_bytes(peer[33:], 'big'))
        point = other.pointQ * key.d
        if other.pointQ.is_point_at_infinity() or point.is_point_at_infinity(): raise ValueError()
        return int(point.x).to_bytes(32, 'big')
    except ValueError:
        raise ProtocolError('Invalid P-256 point') from None


def encrypt(key, nonce, aad, plain):
    cipher = crypto()[0].new(key, crypto()[0].MODE_GCM, nonce=nonce, mac_len=16)
    cipher.update(aad)
    data, tag = cipher.encrypt_and_digest(plain)
    return data + tag


def decrypt(key, nonce, aad, wire):
    if len(nonce) != 12 or len(wire) < 16:
        raise ProtocolError('Invalid GCM frame')
    cipher = crypto()[0].new(key, crypto()[0].MODE_GCM, nonce=nonce, mac_len=16)
    cipher.update(aad)
    try:
        return cipher.decrypt_and_verify(wire[:-16], wire[-16:])
    except ValueError:
        raise ProtocolError('GCM authentication failed') from None


def layout_encrypt(key, aad, data, nonce=None):
    nonce = os.urandom(12) if nonce is None else nonce
    wire = encrypt(key, nonce, aad, data)
    return wire[:-16] + nonce + wire[-16:]


def layout_decrypt(key, aad, data):
    if len(data) < 28:
        raise ProtocolError('Truncated encrypted body')
    return decrypt(key, data[-28:-16], aad, data[:-28] + data[-16:])


def varint(value):
    if not 0 <= value < 1 << 64:
        raise ProtocolError('Invalid varint')
    out = bytearray()
    while value >= 128:
        out.append((value & 127) | 128)
        value >>= 7
    return bytes(out) + bytes([value])


def readvar(data, offset=0):
    value = 0
    for i in range(10):
        if offset >= len(data):
            raise ProtocolError('Truncated varint')
        c = data[offset]; offset += 1
        if i == 9 and c > 1:
            raise ProtocolError('Varint overflow')
        value |= (c & 127) << (7 * i)
        if not c & 128:
            return value, offset
    raise ProtocolError('Varint too long')


def pv(field, value):
    return varint(field << 3) + varint(value)


def pb(field, value):
    return varint(field << 3 | 2) + varint(len(value)) + value


def fields(data):
    if len(data) > 262144:
        raise ProtocolError('Protobuf too large')
    result = {}; offset = 0
    while offset < len(data):
        tag, offset = readvar(data, offset); field, kind = tag >> 3, tag & 7
        if not 0 < field < 1 << 29:
            raise ProtocolError('Invalid protobuf field')
        if kind == 0:
            value, offset = readvar(data, offset)
        elif kind in (1, 2, 5):
            if kind == 2:
                length, offset = readvar(data, offset)
            else:
                length = 8 if kind == 1 else 4
            if offset + length > len(data):
                raise ProtocolError('Truncated protobuf field')
            value = data[offset:offset + length]; offset += length
        else:
            raise ProtocolError('Unsupported protobuf wire type')
        result[field] = value
    return result


def literal(data):
    out = bytes([min(len(data), 15) << 4])
    if len(data) >= 15:
        n = len(data) - 15
        out += b'\xff' * (n // 255) + bytes([n % 255])
    return out + data


def uncompress(data, limit=262144):
    out = bytearray(); pos = 0
    def length(n):
        nonlocal pos
        if n == 15:
            while True:
                if pos >= len(data): raise ProtocolError('Truncated LZ4 length')
                v = data[pos]; pos += 1; n += v
                if n > limit: raise ProtocolError('LZ4 limit exceeded')
                if v != 255: break
        return n
    while pos < len(data):
        token = data[pos]; pos += 1
        count = length(token >> 4)
        if pos + count > len(data) or len(out) + count > limit: raise ProtocolError('LZ4 literal exceeds limit')
        out += data[pos:pos + count]; pos += count
        if pos == len(data): return bytes(out)
        if pos + 2 > len(data): raise ProtocolError('Truncated LZ4 match')
        offset = int.from_bytes(data[pos:pos + 2], 'little'); pos += 2
        count = length(token & 15) + 4
        if offset == 0 or offset > len(out) or len(out) + count > limit: raise ProtocolError('Invalid LZ4 match')
        for _ in range(count): out.append(out[-offset])
    return bytes(out)


def record(kind, data):
    if len(data) > 65535: raise ProtocolError('Record too large')
    return struct.pack('>BHH', kind, 0xF103, len(data)) + data


def records(data):
    result = []; pos = 0
    while pos + 5 <= len(data):
        kind, version, count = struct.unpack_from('>BHH', data, pos)
        if version != 0xF103 or kind not in (20, 21, 22, 23, 25): raise ProtocolError('Invalid MMTLS header')
        if pos + 5 + count > len(data): break
        result.append((kind, data[pos + 5:pos + 5 + count])); pos += 5 + count
    return result, pos


def handshake(kind, body):
    return struct.pack('>I', len(body) + 1) + bytes([kind]) + body


def split_handshake(data):
    if len(data) < 5 or int.from_bytes(data[:4], 'big') + 4 != len(data): raise ProtocolError('Invalid handshake')
    return data[4], data[5:]


def client_hello(random, timestamp, pub1=None, pub2=None, ticket=None):
    if len(random) != 32: raise ProtocolError('Invalid hello random')
    if ticket is None:
        if len(pub1) != 65 or len(pub2) != 65: raise ProtocolError('Invalid key shares')
        shares = b'\x00\x10\x02'
        for group, key in ((1, pub1), (2, pub2)):
            share = struct.pack('>IH', group, len(key)) + key
            shares += struct.pack('>I', len(share)) + share
        shares += b'\x00\x00\x00\x01'
    else:
        shares = b'\x00\x0f\x01' + struct.pack('>I', len(ticket)) + ticket
    ext = b'\x01' + struct.pack('>I', len(shares)) + shares
    return handshake(1, b'\x03\xf1\x01' + struct.pack('>H', 0xC02B if ticket is None else 0xA8) + random + struct.pack('>II', timestamp, len(ext)) + ext)


def server_hello(body):
    if len(body) < 40: raise ProtocolError('Truncated server hello')
    cipher = int.from_bytes(body[2:4], 'big'); size = int.from_bytes(body[36:40], 'big')
    ext = body[40:40 + size]
    if len(ext) != size: raise ProtocolError('Truncated server extensions')
    if ext[:1] == b'\x01':
        if len(ext) < 13: raise ProtocolError('Truncated key share')
        group, n = struct.unpack_from('>IH', ext, 7); key = ext[13:13 + n]
    else:
        at = ext.find(b'\x00\x41\x04')
        if at < 0: raise ProtocolError('Missing key share')
        group = 1; key = ext[at + 2:at + 67]
    if cipher != 0xC02B or group != 1 or len(key) != 65: raise ProtocolError('Unsupported server key share')
    return key


def record_crypt(key, iv, sequence, kind, data, decoding=False):
    nonce = iv[:4] + bytes(a ^ b for a, b in zip(iv[4:], sequence.to_bytes(8, 'big')))
    aad = struct.pack('>QBHH', sequence, kind, 0xF103, len(data) if decoding else len(data) + 16)
    return (decrypt if decoding else encrypt)(key, nonce, aad, data)


def bi_keys(secret, label, transcript):
    block = expand(secret, label, sha(transcript), 56)
    return block[:16], block[16:32], block[32:44], block[44:]


def shortlink(cmd, seq, body):
    return struct.pack('>IHHII', 16 + len(body), 0x1110, 0x076D, cmd, seq) + body


def parse_shortlink(data):
    if len(data) < 16: raise ProtocolError('Truncated shortlink')
    size, magic, version, cmd, seq = struct.unpack_from('>IHHII', data)
    if size != len(data) or magic != 0x1110: raise ProtocolError('Invalid shortlink')
    return cmd, data[16:]


def wpkg(ints, blobs):
    data = varint(1) + b''.join(varint(k) + varint(v) for k, v in ints) + b'\0'
    data += b''.join(varint(k) + varint(len(v)) + v for k, v in blobs)
    return data + b'\0' + varint(len(data) + 1)


def wpkg_offset(data):
    version, pos = readvar(data)
    if version != 1: raise ProtocolError('Invalid WPKG version')
    for blobs in (False, True):
        while True:
            key, pos = readvar(data, pos)
            if key == 0: break
            value, pos = readvar(data, pos)
            if blobs:
                pos += value
                if pos > len(data): raise ProtocolError('Truncated WPKG')
    trailer = pos; declared, pos = readvar(data, pos)
    if declared != trailer: raise ProtocolError('Invalid WPKG trailer')
    return pos


SERVER_PUB = bytes.fromhex('04ef87876d6478b15f1796eab12068610541173b7176b67f1dcc86683e901acd44d18b4ac36938251d0812dd0cf842aa2d6cbb8115712d1c0087dcefc14a44cd58')


def login_meta(buffer):
    try:
        if len(buffer) > 6000: raise ValueError()
        meta = fields(base64.b64decode(buffer, validate=True))
        if not isinstance(meta.get(1), bytes) or not meta[1] or not isinstance(meta.get(2), bytes) or not meta[2]: raise ValueError()
        return meta
    except (ValueError, KeyError):
        raise ProtocolError('Invalid login buffer') from None


def header(device, uin=0, f9=b'', session=False):
    ints = [(1,1),(2,uin),(3,0),(4,0),(5,524545),(6,11),(7,0),(8,0),(9,0),(10,1),(11,0),(12,0),(13,0),(17,0),(18,1),(20,1504),(21,0),(22,uin),(23,0),(25,16 if session else 17),(26,4),(28,1),(29,1),(30,0)]
    return wpkg(ints, [(14,b''),(24,device),(27,f9)])


def login_body(meta):
    app = pb(1, os.urandom(32)) + pv(2,1901)
    plain = pb(1,app) + pb(3,pb(1,meta[1])) + pv(4,4) + pb(6,b'') + pv(7,0) + pv(8,6)
    key = keypair(); pub = public(key); secret = sha(shared(key, SERVER_PUB))
    prefix = b'1415' + pub; h1 = sha(prefix); cek = os.urandom(32)
    encrypted_key = layout_encrypt(secret[:24], h1, cek)
    okm = expand(hmac.digest(b'security hdkf expand', cek, 'sha256'), b'', h1, 56)
    compressed = literal(plain)
    encrypted_body = layout_encrypt(okm[:24], sha(prefix + encrypted_key), compressed)
    hybrid = pv(1,1) + pb(2,pv(1,415) + pb(2,pub)) + pb(3,encrypted_key) + pb(4,b'') + pb(5,encrypted_body)
    return header(meta[2]) + hybrid, (key, okm, compressed)


def login_response(body, temporary):
    try:
        offset = wpkg_offset(body)
        if body[offset:offset + 1] != b'\x0a': raise ProtocolError('No hybrid body')
    except ProtocolError:
        at = body.find(b'\x08\x9f\x03\x12\x41\x04', 2)
        if at < 2: raise ProtocolError('No hybrid response')
        offset = at - 2
    f = fields(body[offset:]); peer = fields(f[1])[2]
    key, okm, compressed = temporary
    aad = sha(okm[24:] + compressed + b'415' + peer + str(f.get(2,1)).encode())
    decoded = fields(uncompress(layout_decrypt(sha(shared(key, peer))[:24], aad, f[3])))
    session_block = fields(decoded[3]); keys = fields(session_block[2]); identity = fields(session_block[3])
    if len(keys.get(1,b'')) not in (16,24,32) or len(keys.get(2,b'')) not in (16,24,32) or not identity.get(1): raise ProtocolError('Invalid application session')
    return dict(send=keys[1], recv=keys[2], f9=keys.get(9,b''), uin=identity[1])


def transfer(session, meta, appid):
    mac = bytearray(os.urandom(6)); mac[0] = (mac[0] | 2) & 254
    device = '-'.join('%02X' % b for b in mac).encode()
    def info(platform):
        return pb(1,b'sessionkey') + pv(2,session['uin'] & 0xffffffff) + pb(3,device) + pv(4,1661404927) + pb(5,platform) + pv(6,0)
    app = appid.encode(); host = meta.get(3,b'wxd44977328b36e647') or b'wxd44977328b36e647'
    inner = pb(1,info(b'UnifiedPCWindows')) + pb(2,app) + pv(4,1) + pb(5,b'') + pb(6,b'') + pv(7,1)
    plain = pb(1,info(b'Windows')) + pb(2,b'/cgi-bin/mmbiz-bin/js-login') + pb(3,host) + pv(4,5) + pb(5,inner) + pb(6,app) + pv(7,1029) + pv(8,1610627409) + pb(9,b'WindowsxWebPlugin') + pv(10,573651281)
    body = header(meta[2],session['uin'],session['f9'],True) + layout_encrypt(session['send'],b'',literal(plain))
    packet = shortlink(0xB41,0,body)
    url = b'/ilink/ilinkapp/mp/wxaruntime_transfer'; host = b'shortcloud.weixin.com'
    envelope = struct.pack('>H',len(url)) + url + struct.pack('>H',len(host)) + host + struct.pack('>I',len(packet)) + packet
    return struct.pack('>I',len(envelope)) + envelope


def session_decrypt(body, key):
    offsets = list(range(min(220,len(body))))
    try: offsets.insert(0,wpkg_offset(body))
    except ProtocolError: pass
    for offset in offsets:
        if len(body) - offset < 28: continue
        try: return uncompress(layout_decrypt(key,b'',body[offset:]))
        except ProtocolError: continue
    raise ProtocolError('Unable to authenticate application response')


def response_code(data):
    f = fields(data); base = f.get(1)
    if isinstance(base,bytes): base = fields(base).get(1,0)
    if base: raise ProtocolError('WeChat rejected request')
    value = fields(f.get(2,b'')).get(3,b'')
    if not isinstance(value,bytes) or not value or len(value) > 512: raise ProtocolError('Missing wx.login code')
    try: text = value.decode('ascii')
    except UnicodeError: raise ProtocolError('Invalid wx.login code') from None
    if any(ord(c)<32 or ord(c)>126 for c in text): raise ProtocolError('Invalid wx.login code')
    return text
