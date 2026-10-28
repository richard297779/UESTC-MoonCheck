"""Offline protocol parity and business isolation tests. No real accounts."""
import copy
import hashlib
import hmac
import json
import os
from pathlib import Path
import struct
import sys
import tempfile
import time
import unittest
import contextlib
import io
import subprocess
from unittest.mock import Mock, patch

sys.path.insert(0,str(Path(__file__).resolve().parents[1]/'src'))
from mooncheck_app import config, service, wechat, wechat_wire as w, wechat_transport as t, checkin_api

VECTORS = json.loads((Path(__file__).parent/'fixtures/wx_vectors.json').read_text())
CRED = dict(openid='fixture-openid',access='fixture-access',refresh='fixture-refresh',expires_at=9999999999)


class WireTests(unittest.TestCase):
    def test_http_reads_complete_bounded_response_and_keeps_virtual_host(self):
        class Reply:
            status=200
            def __init__(self):self.parts=[b'first',b'second']
            def isclosed(self):return not self.parts
            def read1(self,size):return self.parts.pop(0)
        connection=Mock();connection.getresponse.return_value=Reply()
        with patch.object(t,'HTTPConnection',return_value=connection):
            self.assertEqual(b'firstsecond',t.http('192.0.2.10',80,t.SHORT,'POST','/mmtls/fixture',b'body',time.monotonic()+5))
        self.assertEqual(t.SHORT,connection.request.call_args.kwargs['headers']['Host'])
        connection.close.assert_called_once()

    def test_http_rejects_oversize_and_non_success(self):
        for size,status in ((262145,200),(4,403)):
            reply=Mock(status=status);reply.isclosed.side_effect=[False,True];reply.read1.return_value=b'x'*size
            connection=Mock();connection.getresponse.return_value=reply
            with patch.object(t,'HTTPConnection',return_value=connection):
                with self.assertRaises(w.ProtocolError):t.http('192.0.2.10',80,t.SHORT,'POST','/fixture',b'',time.monotonic()+5)
            connection.close.assert_called_once()

    def test_firmware_golden_vectors(self):
        rand = bytes(i*7%256 for i in range(32)); key = bytes(range(32))
        pub1 = b'\4'+bytes(i*3%256 for i in range(32))+bytes(i*5%256 for i in range(32))
        pub2 = b'\5'+bytes(i*11%256 for i in range(64))
        values = {
            'client_hello':w.client_hello(rand,0x67E8F109,pub1,pub2),
            'psk_client_hello':w.client_hello(rand,0x67E8F109,ticket=b'TICKET'*5),
            'record_crypt':w.record_crypt(bytes(range(16)),bytes(range(12)),4,23,b'server flight'),
            'master':w.expand(key,b'expanded secret',rand),
            'handshake_keys':w.expand(key,b'handshake key expansion',rand,56),
            'app_keys':w.expand(key,b'application data key expansion',rand,56),
            'finished_client':hmac.digest(w.expand(key,b'client finished'),rand,'sha256'),
            'finished_server':hmac.digest(w.expand(key,b'server finished'),rand,'sha256'),
            'psk_oneway':w.expand(key,b'early data key expansion',rand,28),
            'lz4_short':w.literal(b'hello'), 'lz4_long':w.literal(bytes(range(64))),
            'shortlink':w.shortlink(0xB41,7,b'payload-bytes'),
            'record_frame':w.record(22,b'\1\2\3')}
        for name,value in values.items():
            with self.subTest(name=name): self.assertEqual(VECTORS[name],value.hex())

    def test_transfer_plain_matches_firmware(self):
        session=dict(send=bytes(range(16)),recv=b'0'*16,uin=42,f9=b'\xf9'*8)
        with patch.object(w.os,'urandom',side_effect=[bytes.fromhex('aabbccddeeff'),b'\0'*12]):
            envelope = w.transfer(session,{2:b'device',3:b'wxd44977328b36e647'},wechat.APP_ID)
        pos=4
        size=int.from_bytes(envelope[pos:pos+2],'big');pos+=2+size
        size=int.from_bytes(envelope[pos:pos+2],'big');pos+=2+size
        size=int.from_bytes(envelope[pos:pos+4],'big');pos+=4
        cmd,body = w.parse_shortlink(envelope[pos:pos+size])
        self.assertEqual(0xB41,cmd)
        self.assertEqual(VECTORS['transfer_plain'],w.session_decrypt(body,session['send']).hex())

    def test_gcm_192_authentication_and_layout(self):
        key=bytes(range(24)); encrypted=w.layout_encrypt(key,b'aad',b'fixture',b'\0'*12)
        self.assertEqual(b'fixture',w.layout_decrypt(key,b'aad',encrypted))
        for data,aad in ((encrypted[:-1]+bytes([encrypted[-1]^1]),b'aad'),(encrypted,b'wrong')):
            with self.assertRaises(w.ProtocolError):w.layout_decrypt(key,aad,data)

    def test_manualauth_hybrid_response_roundtrip_and_tamper(self):
        request,temporary=w.login_body({1:b'fixture-ticket',2:b'fixture-device'})
        self.assertGreater(len(request),100)
        peer=w.keypair();peer_public=w.public(peer)
        client,okm,compressed=temporary
        keys=w.pb(1,b'S'*24)+w.pb(2,b'R'*24)+w.pb(9,b'f9')
        plain=w.pb(3,w.pb(2,keys)+w.pb(3,w.pv(1,42)))
        aad=w.sha(okm[24:]+compressed+b'415'+peer_public+b'1')
        encrypted=w.layout_encrypt(w.sha(w.shared(peer,w.public(client)))[:24],aad,w.literal(plain))
        response=w.header(b'fixture-device')+w.pb(1,w.pv(1,415)+w.pb(2,peer_public))+w.pv(2,1)+w.pb(3,encrypted)
        session=w.login_response(response,temporary)
        self.assertEqual((b'S'*24,b'R'*24,42),(session['send'],session['recv'],session['uin']))
        with self.assertRaises(w.ProtocolError):w.login_response(response[:-1]+bytes([response[-1]^1]),temporary)

    def test_zero_rtt_response_authentication_and_code(self):
        psk=b'P'*32;recv=b'R'*16
        hello=w.client_hello(b'A'*32,1234,ticket=b'fixture-ticket');type8=t.TYPE8+struct.pack('>I',1234)
        server=w.handshake(2,b'server-response')
        code=w.pv(1,0)+w.pb(2,w.pb(3,b'fixture-wx-code'))
        wrapped=w.shortlink(0xB41,0,w.header(b'device')+w.layout_encrypt(recv,b'',w.literal(code)))
        block=w.expand(psk,b'handshake key expansion',w.sha(hello+type8+server),28)
        encrypted=w.record_crypt(block[:16],block[16:],3,23,wrapped)
        response=w.record(22,server)+w.record(23,encrypted)
        self.assertEqual('fixture-wx-code',t.zero_response(response,psk,hello,type8,recv))
        with self.assertRaises(w.ProtocolError):t.zero_response(response[:-1]+bytes([response[-1]^1]),psk,hello,type8,recv)

    def test_ecdh_and_invalid_points(self):
        first=w.keypair();second=w.keypair()
        self.assertEqual(w.shared(first,w.public(second)),w.shared(second,w.public(first)))
        with self.assertRaises(w.ProtocolError):w.shared(first,b'\4'+b'\0'*64)

    def test_parser_limits_and_compression(self):
        for blob in (b'\x80'*11,b'\x12\xff',b'\x0f\0\0'):
            with self.assertRaises(w.ProtocolError):w.fields(blob)
        with self.assertRaises(w.ProtocolError):w.uncompress(b'\xf0'+b'\xff'*20+b'\0',limit=100)
        with self.assertRaises(w.ProtocolError):w.uncompress(b'\0\0\0')
        for n in (0,1,14,15,270,4096):self.assertEqual(b'x'*n,w.uncompress(w.literal(b'x'*n)))

    def test_records_incremental_and_response_code(self):
        frame=w.record(22,b'payload')
        self.assertEqual(([],0),w.records(frame[:-1]))
        self.assertEqual(([(22,b'payload')],len(frame)),w.records(frame))
        self.assertEqual('test-code',w.response_code(w.pb(1,w.pv(1,0))+w.pb(2,w.pb(3,b'test-code'))))
        with self.assertRaises(w.ProtocolError):w.response_code(w.pb(1,w.pv(1,9))+w.pb(2,w.pb(3,b'test-code')))

    def test_socket_partial_reads_and_sendall(self):
        sock=Mock();frame=w.record(22,b'hello')+w.record(23,b'world')
        sock.recv.side_effect=[bytes([b]) for b in frame]
        connection=t.Connection(sock,time.monotonic()+20)
        self.assertEqual((22,b'hello'),connection.receive())
        self.assertEqual((23,b'world'),connection.receive())
        connection.send(22,b'hello');sock.sendall.assert_called_once_with(w.record(22,b'hello'))

    def test_finished_verification_with_real_gcm_and_fragmented_socket(self):
        sock=Mock(); wire=bytearray(); server=w.keypair()
        def send(data):
            if sock.sendall.call_count == 1:
                hello=data[5:]
                # Key-share offset parsed structurally from the client hello.
                _,body=w.split_handshake(hello)
                ext=body[45:];shares=ext[5:];share=shares[7:]
                peer=share[6:71]
                secret=w.sha(w.shared(server,peer))
                extension=b'\1'+b'\0'*6+struct.pack('>IH',1,65)+w.public(server)
                sh=w.handshake(2,b'\x03\xf1'+struct.pack('>H',0xC02B)+b'R'*32+struct.pack('>I',len(extension))+extension)
                transcript=hello+sh
                ck,sk,ci,si=w.bi_keys(secret,b'handshake key expansion',transcript)
                ticket=w.handshake(4,b'\1'+struct.pack('>I',6)+b'\1'+struct.pack('>I',3600)+b'X')
                cert=w.handshake(15,b'fixture-certificate')
                flight=[ticket,cert]
                digest=w.sha(transcript+ticket+cert)
                finished=w.handshake(20,b'\0\x20'+hmac.digest(w.expand(secret,b'server finished'),digest,'sha256'))
                wire.extend(w.record(22,sh))
                for seq,payload in enumerate(flight+[finished],1):wire.extend(w.record(22,w.record_crypt(sk,si,seq,22,payload)))
        def recv(_size):
            out=bytes(wire[:7]);del wire[:7];return out
        sock.sendall.side_effect=send;sock.recv.side_effect=recv
        connection=t.Connection(sock,time.monotonic()+20)
        connection.authenticate()
        self.assertEqual(32,len(connection.psk));self.assertEqual(2,connection.tx)


class TicketTests(unittest.TestCase):
    def test_cli_import_status_clear_and_old_config_migration(self):
        from mooncheck_app.cli import main_cli
        with tempfile.TemporaryDirectory() as folder, patch.dict(os.environ,MOONCHECK_CONFIG=str(Path(folder)/'config.json')):
            path=Path(folder)/'authorization.json';path.write_text(json.dumps(CRED))
            output=io.StringIO()
            with contextlib.redirect_stdout(output):
                self.assertEqual(0,main_cli(['wx','import','--file',str(path)]))
                self.assertEqual(0,main_cli(['wx','status']))
            self.assertNotIn('fixture-access',output.getvalue())
            self.assertEqual('fixture-access',config.load()['wechat']['access'])
            with contextlib.redirect_stdout(output):self.assertEqual(0,main_cli(['wx','clear']))
            self.assertEqual({},config.load()['wechat'])
            Path(os.environ['MOONCHECK_CONFIG']).write_text(json.dumps({'student_id':'old','unknown_key':42}))
            migrated=config.load()
            self.assertEqual(2,migrated['config_schema']);self.assertEqual(42,migrated['unknown_key']);self.assertEqual({},migrated['wechat'])

    def test_interprocess_lock_rejects_concurrent_owner(self):
        from mooncheck_app.operation_lock import operation_lock
        with tempfile.TemporaryDirectory() as folder:
            path=str(Path(folder)/'config.json')
            code="import sys;sys.path.insert(0,%r)\nfrom mooncheck_app.operation_lock import operation_lock,OperationBusy\ntry:\n with operation_lock(%r):sys.exit(2)\nexcept OperationBusy:sys.exit(0)"%(str(Path(__file__).resolve().parents[1]/'src'),path)
            with operation_lock(path):self.assertEqual(0,subprocess.run([sys.executable,'-c',code],timeout=5).returncode)
            with operation_lock(path):pass

    def test_identity_change_clears_old_wechat_and_session(self):
        cfg=config.default_config();cfg.update(student_id='old',wechat=CRED,session_cookie='old-session')
        config.set_value(cfg,'student_id','new')
        self.assertEqual({},cfg['wechat']);self.assertEqual('',cfg['session_cookie'])
    def test_platform_client_has_no_cas_cookie_and_uses_only_mp_cookie(self):
        client=Mock();client.request.return_value=Mock(status=200,json=lambda:{'ok':True,'data':{'ticket':'T'*43}})
        factory=Mock(return_value=client)
        wechat.platform_post('checkin-ticket',{'code':'fixture'},20,'mp-session',client_factory=factory)
        factory.assert_called_once_with(timeout=20)
        headers=client.request.call_args.kwargs['headers']
        self.assertEqual({'platform':'mp-weixin','Cookie':'SESSION=mp-session'},headers)
    def test_two_codes_and_separate_platform_session(self):
        cfg=config.default_config();cfg['wechat']=copy.deepcopy(CRED);acct=config.effective_accounts(cfg)[0]
        with patch.object(wechat,'login_buffer',side_effect=['buffer-1','buffer-2']) as buffer, patch.object(t,'get_code',side_effect=['code-1','code-2']) as codes, patch.object(wechat,'platform_post',side_effect=[{'bind':True,'sessionId':'mp-only'},{'ticket':'T'*43}]) as post:
            self.assertEqual('T'*43,wechat.ensure_ticket(cfg,acct))
        self.assertEqual(2,buffer.call_count);self.assertEqual(2,codes.call_count)
        self.assertEqual({'code':'code-1','sn':''},post.call_args_list[0].args[1])
        self.assertEqual('mp-only',post.call_args_list[1].args[3])
        self.assertEqual({'code':'code-2'},post.call_args_list[1].args[1])
        self.assertNotIn('ticket',cfg)

    def test_rotation_persisted_before_buffer_and_second_rejection_stops(self):
        cfg=config.default_config();cfg['wechat']=dict(CRED,expires_at=0);acct=config.effective_accounts(cfg)[0]
        rotated=dict(CRED,access='rotated')
        with patch.object(wechat,'refresh',return_value=rotated),patch.object(config,'save') as save,patch.object(wechat,'login_buffer',side_effect=wechat.AuthorizationRejected('rejected')) as buffer:
            with self.assertRaises(wechat.AuthorizationRejected):wechat.ensure_ticket(cfg,acct)
        self.assertEqual(2,save.call_count);self.assertEqual('rotated',cfg['wechat']['access']);self.assertEqual(2,buffer.call_count)

    def test_account_isolation_and_redacted_export(self):
        cfg=config.default_config();cfg['wechat']=dict(CRED)
        cfg['accounts']=[dict(label='A',wechat=dict(CRED,access='only-A')),dict(label='B')]
        with self.assertRaises(w.ProtocolError):wechat.ensure_ticket(cfg,cfg['accounts'][1])
        out=json.dumps(config.redact_copy(cfg))
        for secret in ('only-A','fixture-openid','fixture-refresh'):self.assertNotIn(secret,out)

    def test_bad_ticket_or_unbound_account_stops(self):
        cfg=config.default_config();acct=dict(wechat=CRED)
        for replies in ([{'bind':False}], [{'bind':True,'sessionId':'mp'},{'ticket':'bad'}]):
            with patch.object(wechat,'login_buffer',return_value='buffer'),patch.object(t,'get_code',return_value='code'),patch.object(wechat,'platform_post',side_effect=replies):
                with self.assertRaises(w.ProtocolError):wechat.ensure_ticket(cfg,acct)

    def test_api_requires_ticket_and_preserves_platform_headers(self):
        client=Mock();api=checkin_api.CheckinApi(client)
        with self.assertRaises(ValueError):api.submit({})
        client.request.assert_not_called();api.submit({},'T'*43)
        self.assertEqual('T'*43,client.request.call_args.kwargs['headers']['X-Checkin-Ticket'])
        self.assertEqual('h5',client.request.call_args.kwargs['headers']['Platform'])

    def test_service_does_not_submit_without_credentials_or_after_ticket_failure(self):
        cfg=config.default_config();acct=dict(label='A')
        with patch.object(service,'_ensure_session') as session:
            self.assertEqual('not_configured',service._checkin_one(cfg,lambda _:None,acct,False,False)[0]);session.assert_not_called()
        api=Mock();acct['wechat']=CRED
        with patch.object(service,'_ensure_session',return_value=(Mock(ua='fixture'),api,True,'',{})),patch.object(wechat,'ensure_ticket',side_effect=w.ProtocolError('no ticket')):
            self.assertEqual('fail',service._checkin_one(cfg,lambda _:None,acct,False,False)[0]);api.submit.assert_not_called()

    def test_ticket_rejection_does_not_replay_submission(self):
        cfg=config.default_config();acct=dict(label='A',wechat=CRED);api=Mock()
        api.submit.return_value=Mock(status=403,json=lambda:{'code':46003})
        with patch.object(service,'_ensure_session',return_value=(Mock(ua='fixture'),api,True,'',{})),patch.object(wechat,'ensure_ticket',return_value='T'*43) as issue:
            self.assertEqual('fail',service._checkin_one(cfg,lambda _:None,acct,True,False)[0])
            self.assertEqual(1,api.submit.call_count);issue.assert_called_once()

    def test_window_is_rechecked_after_slow_ticket_acquisition(self):
        cfg=config.default_config();acct=dict(label='A',wechat=CRED);api=Mock()
        with patch.object(service,'_ensure_session',return_value=(Mock(ua='fixture'),api,True,'',{})),patch.object(wechat,'ensure_ticket',return_value='T'*43),patch.object(config,'in_window',return_value=False):
            self.assertEqual('warn',service._checkin_one(cfg,lambda _:None,acct,False,False)[0])
            api.submit.assert_not_called()

    def test_signed_headers_match_firmware_formula(self):
        body='{"fixture":1}'
        headers=wechat.signed_headers(body,1234,'0001')
        self.assertEqual(hashlib.md5((body+'1234'+wechat.ACCESS_KEY+'0001').encode()).hexdigest(),headers['Ual-Access-Signature'])


if __name__=='__main__':unittest.main()
