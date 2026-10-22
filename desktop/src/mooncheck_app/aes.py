"""AES-128-CBC 加密（纯 Python 实现）。

单文件 exe 的打包体积与依赖越少越稳，这里只实现登录密码加密所需的
AES-128-CBC + PKCS#7，不引入 pycryptodome，也不含解密路径
（解密仅在自检里用到，见 self_test）。

算法与固件 mbedtls 侧保持一致：
    明文 = randomString(64) + 密码
    key  = pwdEncryptSalt（登录页隐藏域，16 字符）
    iv   = randomString(16)
    输出 = base64(AES-128-CBC/PKCS7(明文))
"""
import base64
import os

_SBOX_HEX = (
    '637c777bf26b6fc53001672bfed7ab76'
    'ca82c97dfa5947f0add4a2af9ca472c0'
    'b7fd9326363ff7cc34a5e5f171d83115'
    '04c723c31896059a071280e2eb27b275'
    '09832c1a1b6e5aa0523bd6b329e32f84'
    '53d100ed20fcb15b6acbbe394a4c58cf'
    'd0efaafb434d338545f9027f503c9fa8'
    '51a3408f929d38f5bcb6da2110fff3d2'
    'cd0c13ec5f974417c4a77e3d645d1973'
    '60814fdc222a908846eeb814de5e0bdb'
    'e0323a0a4906245cc2d3ac629195e479'
    'e7c8376d8dd54ea96c56f4ea657aae08'
    'ba78252e1ca6b4c6e8dd741f4bbd8b8a'
    '703eb5664803f60e613557b986c11d9e'
    'e1f8981169d98e949b1e87e9ce5528df'
    '8ca1890dbfe6426841992d0fb054bb16'
)
SBOX = bytes.fromhex(_SBOX_HEX)


def _xtime(a):
    a <<= 1
    if a & 0x100:
        a ^= 0x11B
    return a & 0xFF


def _mul(a, b):
    r = 0
    for _ in range(8):
        if b & 1:
            r ^= a
        b >>= 1
        a = _xtime(a)
    return r


def _expand_key(key):
    if len(key) != 16:
        raise ValueError('AES-128 需要 16 字节密钥')
    w = [list(key[4 * i:4 * i + 4]) for i in range(4)]
    rcon = 1
    for i in range(4, 44):
        t = list(w[i - 1])
        if i % 4 == 0:
            t = t[1:] + t[:1]
            t = [SBOX[b] for b in t]
            t[0] ^= rcon
            rcon = _xtime(rcon)
        w.append([w[i - 4][j] ^ t[j] for j in range(4)])
    return w


def _add_round_key(s, w, rnd):
    for c in range(4):
        for r in range(4):
            s[4 * c + r] ^= w[4 * rnd + c][r]


def _sub_bytes(s):
    return [SBOX[b] for b in s]


def _shift_rows(s):
    # 状态按列优先摆放：s[4*c + r]
    return [s[4 * ((c + r) % 4) + r] for c in range(4) for r in range(4)]


def _mix_columns(s):
    out = [0] * 16
    for c in range(4):
        a0, a1, a2, a3 = s[4 * c:4 * c + 4]
        out[4 * c + 0] = _mul(a0, 2) ^ _mul(a1, 3) ^ a2 ^ a3
        out[4 * c + 1] = a0 ^ _mul(a1, 2) ^ _mul(a2, 3) ^ a3
        out[4 * c + 2] = a0 ^ a1 ^ _mul(a2, 2) ^ _mul(a3, 3)
        out[4 * c + 3] = _mul(a0, 3) ^ a1 ^ a2 ^ _mul(a3, 2)
    return out


class AES128(object):
    """只做加密的 AES-128（ECB 单块）。"""

    def __init__(self, key):
        self._w = _expand_key(bytes(key))

    def encrypt_block(self, block):
        if len(block) != 16:
            raise ValueError('AES 分组必须是 16 字节')
        s = list(block)
        _add_round_key(s, self._w, 0)
        for rnd in range(1, 10):
            s = _sub_bytes(s)
            s = _shift_rows(s)
            s = _mix_columns(s)
            _add_round_key(s, self._w, rnd)
        s = _sub_bytes(s)
        s = _shift_rows(s)
        _add_round_key(s, self._w, 10)
        return bytes(s)


def pkcs7_pad(data, block=16):
    n = block - (len(data) % block)
    return data + bytes([n]) * n


def encrypt_cbc(key, iv, plaintext):
    if len(iv) != 16:
        raise ValueError('IV 必须是 16 字节')
    cipher = AES128(key)
    data = pkcs7_pad(plaintext)
    prev = bytes(iv)
    out = []
    for i in range(0, len(data), 16):
        block = bytes(a ^ b for a, b in zip(data[i:i + 16], prev))
        prev = cipher.encrypt_block(block)
        out.append(prev)
    return b''.join(out)


def encrypt_cas_password(password, salt):
    """按平台方式加密密码，返回 base64 文本。"""
    chars = ('ABCDEFGHJKMNPQRSTWXYZabcdefhijkmnprstwxyz2345678')
    prefix = ''.join(chars[os.urandom(1)[0] % len(chars)] for _ in range(64))
    iv = ''.join(chars[os.urandom(1)[0] % len(chars)] for _ in range(16))
    plain = (prefix + password).encode('utf-8')
    return base64.b64encode(
        encrypt_cbc(salt.encode('utf-8'), iv.encode('utf-8'), plain)
    ).decode('ascii')


def self_test():
    """FIPS-197 标准向量 + 加密长度自检，返回 (ok, detail)。"""
    key = bytes(range(16))
    plain = bytes.fromhex('00112233445566778899aabbccddeeff')
    cipher = AES128(key).encrypt_block(plain)
    if cipher.hex() != '69c4e0d86a7b0430d8cdb78070b4c55a':
        return False, 'AES 标准向量不匹配：%s' % cipher.hex()

    salt = 'SUcjRLLQehfjKUv7'
    out = encrypt_cas_password('Test123456', salt)
    raw = base64.b64decode(out)
    if len(raw) % 16 or len(raw) < 80:
        return False, '密文长度异常：%d 字节' % len(raw)
    return True, 'AES-128-CBC 标准向量与密文长度均通过'
