#!/usr/bin/env node
/** 检查目标站点的 TLS 证书链，用于决定 ESP32 侧 TLS 策略。
 *  用法: node tools/check_tls.js [host...]
 */
const tls = require('tls');

const hosts = process.argv.slice(2).length
  ? process.argv.slice(2)
  : ['idas.uestc.edu.cn', 'jzsz.uestc.edu.cn'];

function probe(host) {
  return new Promise((resolve) => {
    const socket = tls.connect(
      { host, port: 443, servername: host, rejectUnauthorized: false },
      () => {
        const chain = [];
        let cert = socket.getPeerCertificate(true);
        const seen = new Set();
        while (cert && cert.subject && !seen.has(cert.fingerprint256)) {
          seen.add(cert.fingerprint256);
          chain.push({
            subject: cert.subject.CN,
            issuer: cert.issuer ? cert.issuer.CN : null,
            o: cert.issuer ? cert.issuer.O : null,
            valid_from: cert.valid_from,
            valid_to: cert.valid_to,
            self_signed: cert.subject.CN === (cert.issuer && cert.issuer.CN),
          });
          if (!cert.issuerCertificate || cert.issuerCertificate === cert) break;
          cert = cert.issuerCertificate;
        }
        resolve({
          host,
          protocol: socket.getProtocol(),
          alpn: socket.alpnProtocol || null,
          authorized: socket.authorized,
          auth_error: socket.authorizationError ? String(socket.authorizationError) : null,
          chain,
        });
        socket.end();
      }
    );
    socket.on('error', (e) => resolve({ host, error: e.message }));
    socket.setTimeout(8000, () => { socket.destroy(); resolve({ host, error: 'timeout' }); });
  });
}

(async () => {
  for (const host of hosts) {
    const r = await probe(host);
    console.log('=== ' + host + ' ===');
    console.log(JSON.stringify(r, null, 2));
  }
})();
