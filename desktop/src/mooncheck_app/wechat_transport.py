"""Bounded MMTLS transport ported from firmware wx_mmtls.cpp / wx_dns.cpp."""
from http.client import HTTPConnection, HTTPException
import hmac
import json
import os
import socket
import struct
import time

from . import wechat_wire as w

LONG = 'longcloud.weixin.com'
SHORT = 'shortcloud.weixin.com'
TYPE8 = bytes.fromhex('00000010080000000b01000000060012')


def remaining(deadline):
    value = deadline - time.monotonic()
    if value <= 0: raise w.ProtocolError('WeChat operation timed out')
    return value


def http(host, port, virtual_host, method, path, body, deadline):
    connection = HTTPConnection(host, port, timeout=remaining(deadline))
    try:
        connection.request(method, path, body=body, headers={
            'Host': virtual_host, 'User-Agent': 'MicroMessenger Client',
            'Content-Type': 'application/octet-stream', 'Connection': 'close',
            'Upgrade': 'mmtls', 'X-Online-Host': virtual_host})
        stream = connection.sock
        stream.settimeout(remaining(deadline))
        response = connection.getresponse()
        chunks = []; total = 0
        while not response.isclosed():
            remaining(deadline)
            # HTTPConnection may detach a close-delimited response's socket.
            stream.settimeout(remaining(deadline))
            data = response.read1(8192)
            if not data: break
            total += len(data)
            if total > 262144: raise w.ProtocolError('WeChat HTTP response too large')
            chunks.append(data)
        if response.status != 200: raise w.ProtocolError('WeChat HTTP request rejected')
        return b''.join(chunks)
    finally:
        connection.close()


def discover(deadline):
    for host in ('aedns.weixin.qq.com', '180.153.202.85'):
        try:
            phase = min(deadline, time.monotonic() + 3)
            data = http(host,80,'aedns.weixin.qq.com','GET','/cgi-bin/default/getdns?clientversion=0&devicetype=Windows&uin=0&format=json',b'',phase)
            dns = json.loads(data)['dns']
            if dns.get('retcode') != 0: continue
            result = {}
            for entry in dns.get('domainlist',[]):
                if entry.get('name') not in (LONG,SHORT): continue
                ips = [x['ip'] for x in entry.get('iplist',[]) if isinstance(x.get('ip'),str)]
                ports = [p for x in entry.get('protocollist',[]) if x.get('name') == ('http' if entry['name']==SHORT else 'mmtlsovertcp') for p in x.get('portlist',[]) if isinstance(p,int) and 0<p<65536]
                result[entry['name']] = [(ip,p) for ip in ips for p in ports]
            return result
        except (OSError, ValueError, KeyError, TypeError, HTTPException, w.ProtocolError):
            continue
    return {}


class Connection:
    def __init__(self, stream, deadline):
        self.stream = stream; self.deadline = deadline
        self.tx = self.rx = 0; self.buffer = b''; self.transcript = b''
        self.tickets = []; self.cert_hash = b''

    def send(self, kind, payload, key=None, iv=None):
        if key is not None: payload = w.record_crypt(key,iv,self.tx,kind,payload)
        self.stream.settimeout(remaining(self.deadline))
        self.stream.sendall(w.record(kind,payload)); self.tx += 1

    def receive(self, key=None, iv=None):
        while True:
            frames, consumed = w.records(self.buffer)
            if frames:
                kind, payload = frames[0]
                # Consume exactly the first record, retaining all later records.
                self.buffer = self.buffer[5 + len(payload):]
                if key is not None: payload = w.record_crypt(key,iv,self.rx,kind,payload,True)
                self.rx += 1
                return kind,payload
            if len(self.buffer) > 131080: raise w.ProtocolError('MMTLS receive buffer exceeded')
            self.stream.settimeout(remaining(self.deadline))
            part = self.stream.recv(8192)
            if not part: raise w.ProtocolError('MMTLS connection closed')
            self.buffer += part

    def authenticate(self):
        private = w.keypair()
        hello = w.client_hello(os.urandom(32),int(time.time()) & 0xffffffff,w.public(private),w.public(w.keypair()))
        self.transcript = hello; self.send(22,hello)
        kind, data = self.receive(); message, body = w.split_handshake(data)
        if kind != 22 or message != 2: raise w.ProtocolError('Unexpected server hello')
        self.transcript += data
        secret = w.sha(w.shared(private,w.server_hello(body)))
        ck,sk,ci,si = w.bi_keys(secret,b'handshake key expansion',self.transcript)
        for _ in range(64):
            kind,data = self.receive(sk,si)
            if kind != 22: raise w.ProtocolError('Handshake rejected')
            message,body = w.split_handshake(data)
            if message == 20:
                digest = w.sha(self.transcript)
                expected = hmac.digest(w.expand(secret,b'server finished'),digest,'sha256')
                if len(body)!=34 or body[:2]!=b'\x00\x20' or not hmac.compare_digest(body[2:],expected): raise w.ProtocolError('Server Finished authentication failed')
                master = w.expand(secret,b'expanded secret',digest)
                self.app = w.bi_keys(master,b'application data key expansion',self.transcript)
                finished = hmac.digest(w.expand(secret,b'client finished'),digest,'sha256')
                self.send(22,w.handshake(20,b'\x00\x20'+finished),ck,ci)
                if not self.cert_hash: raise w.ProtocolError('Missing PSK transcript')
                for entry in self.tickets:
                    if entry[0]==1 and int.from_bytes(entry[1:5],'big')>0:
                        self.psk = w.expand(secret,b'PSK_ACCESS',self.cert_hash)
                        self.ticket = entry
                        return
                raise w.ProtocolError('Missing access PSK')
            self.transcript += data
            if message == 15: self.cert_hash = w.sha(self.transcript)
            if message == 4:
                if not body: raise w.ProtocolError('Missing session tickets')
                pos = 1
                for _ in range(body[0]):
                    if pos+4>len(body): raise w.ProtocolError('Truncated ticket')
                    size = int.from_bytes(body[pos:pos+4],'big'); pos += 4
                    if size<5 or pos+size>len(body): raise w.ProtocolError('Invalid session ticket')
                    self.tickets.append(body[pos:pos+size]); pos += size
                if pos != len(body): raise w.ProtocolError('Unexpected ticket trailer')
        raise w.ProtocolError('Handshake message limit exceeded')

    def login(self, meta):
        self.authenticate()
        body,temporary = w.login_body(meta)
        ck,sk,ci,si = self.app
        self.send(23,w.shortlink(0xD7D,0,body),ck,ci)
        kind,data = self.receive(sk,si)
        if kind != 23: raise w.ProtocolError('ManualAuth rejected')
        cmd,body = w.parse_shortlink(data)
        if cmd != 0xD7D: raise w.ProtocolError('Unexpected ManualAuth command')
        return w.login_response(body,temporary), self.psk, self.ticket


def zero_request(psk,ticket,envelope,now):
    hello = w.client_hello(os.urandom(32),now,ticket=ticket)
    block = w.expand(psk,b'early data key expansion',w.sha(hello),28)
    key,iv = block[:16],block[16:]
    type8 = TYPE8 + struct.pack('>I',now)
    out = w.record(25,hello)
    for seq,kind,data in ((1,25,type8),(2,23,envelope),(3,21,bytes.fromhex('00000003000101'))):
        out += w.record(kind,w.record_crypt(key,iv,seq,kind,data))
    return out,hello,type8


def zero_response(data,psk,hello,type8,recv_key):
    frames,used = w.records(data)
    if used != len(data): raise w.ProtocolError('Truncated short response')
    server = next((p for k,p in frames if k==22),None)
    app = next((p for k,p in frames if k==23),None)
    if server is None or app is None or w.split_handshake(server)[0]!=2: raise w.ProtocolError('Missing short response records')
    for transcript in (hello+server,hello+type8+server,hello+server+type8):
        block = w.expand(psk,b'handshake key expansion',w.sha(transcript),28)
        for seq in (2,1,3):
            try:
                plain = w.record_crypt(block[:16],block[16:],seq,23,app,True)
            except w.ProtocolError: continue
            try:
                cmd,body = w.parse_shortlink(plain)
                if cmd != 0xB41: raise w.ProtocolError('Unexpected transfer command')
                response = w.session_decrypt(body,recv_key)
            except w.ProtocolError:
                response = w.session_decrypt(plain,recv_key)
            return w.response_code(response)
    raise w.ProtocolError('Unable to authenticate 0-RTT response')


def get_code(buffer, appid, timeout=60):
    deadline = time.monotonic()+timeout
    meta = w.login_meta(buffer)
    dns = discover(deadline)
    targets = sorted(dns.get(LONG,[]),key=lambda x: (443,8080,80,5000).index(x[1]) if x[1] in (443,8080,80,5000) else 4)
    targets = list(dict.fromkeys(targets))[:5] + [(LONG,443)]
    state = None
    for i,target in enumerate(targets):
        phase = time.monotonic()+remaining(deadline)/(len(targets)-i)
        try:
            with socket.create_connection(target,timeout=remaining(phase)) as stream:
                state = Connection(stream,phase).login(meta)
            break
        except (OSError,w.ProtocolError,ValueError,KeyError,TypeError): continue
    if state is None: raise w.ProtocolError('微信 MMTLS 登录失败，请检查网络或重新扫码授权')
    session,psk,ticket = state
    envelope = w.transfer(session,meta,appid)
    targets = list(dict.fromkeys(t for t in dns.get(SHORT,[]) if t[1]==80)) + [('120.241.131.173',80)]
    for i,(host,port) in enumerate(targets):
        phase = time.monotonic()+remaining(deadline)/(len(targets)-i)
        try:
            now = int(time.time()) & 0xffffffff
            request,hello,type8 = zero_request(psk,ticket,envelope,now)
            response = http(host,port,SHORT,'POST','/mmtls/%08x'%now,request,phase)
            return zero_response(response,psk,hello,type8,session['recv'])
        except (OSError,w.ProtocolError,ValueError,KeyError,TypeError,HTTPException): continue
    raise w.ProtocolError('微信 js-login 失败，请检查授权与网络')
