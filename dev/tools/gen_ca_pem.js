#!/usr/bin/env node
/**
 * 从目标站点抓取真实证书链，生成固件可用的 CA PEM 头文件。
 * 生成物：firmware/include/uestc_ca_pem.h （内含 kUestcCaPem 常量）
 *
 * 用法: node tools/gen_ca_pem.js
 */
const fs = require('fs');
const path = require('path');
const tls = require('tls');

const HOSTS = ['idas.uestc.edu.cn', 'jzsz.uestc.edu.cn'];
const OUT = path.join('firmware', 'include', 'uestc_ca_pem.h');

function derToPem(der) {
  const b64 = der.toString('base64').replace(/(.{64})/g, '$1\n').trim();
  return `-----BEGIN CERTIFICATE-----\n${b64}\n-----END CERTIFICATE-----`;
}

function fetchChain(host) {
  return new Promise((resolve, reject) => {
    const socket = tls.connect(
      { host, port: 443, servername: host, rejectUnauthorized: false },
      () => {
        const chain = [];
        let cert = socket.getPeerCertificate(true);
        const seen = new Set();
        while (cert && cert.raw && !seen.has(cert.fingerprint256)) {
          seen.add(cert.fingerprint256);
          chain.push({
            cn: cert.subject && cert.subject.CN,
            issuer: cert.issuer && cert.issuer.CN,
            selfSigned: cert.subject && cert.issuer && cert.subject.CN === cert.issuer.CN,
            der: cert.raw,
          });
          if (!cert.issuerCertificate || cert.issuerCertificate === cert) break;
          cert = cert.issuerCertificate;
        }
        socket.end();
        resolve(chain);
      }
    );
    socket.on('error', reject);
    socket.setTimeout(10000, () => { socket.destroy(); reject(new Error('timeout')); });
  });
}

(async () => {
  const chain = await fetchChain(HOSTS[0]);
  // 叶子证书由服务器下发，不需要内置；内置"中间 CA + 根 CA"
  const cas = chain.filter((c) => !c.selfSigned || chain.length === 1);
  const lines = [];
  lines.push('#pragma once');
  lines.push('// ============================================================================');
  lines.push('// 本文件由 tools/gen_ca_pem.js 自动生成，请勿手工编辑。');
  lines.push('// 用途：HttpClient 在 TLS 模式下校验 *.uestc.edu.cn 的证书链。');
  lines.push('// 证书链：');
  chain.forEach((c, i) => {
    lines.push(`//   [${i}] CN=${c.cn}  (issuer=${c.issuer}${c.selfSigned ? ', 自签根' : ''})`);
  });
  lines.push('// 说明：叶子证书由服务器随握手下发；这里内置中间 CA 与根 CA。');
  lines.push('// ============================================================================');
  lines.push('');
  lines.push('/// @brief 站点 CA 证书链（PEM，多证书拼接）。');
  lines.push('static const char kUestcCaPem[] = R"PEMCERT(');
  cas.forEach((c) => {
    lines.push(derToPem(c.der));
  });
  lines.push(')PEMCERT";');
  lines.push('');

  fs.mkdirSync(path.dirname(OUT), { recursive: true });
  fs.writeFileSync(OUT, lines.join('\n'), 'utf8');
  console.log(`chain=${chain.length} embedded=${cas.length} -> ${OUT} (${fs.statSync(OUT).size} B)`);
  chain.forEach((c, i) => console.log(`  [${i}] ${c.cn} <- ${c.issuer}${c.selfSigned ? ' (root)' : ''}`));
})().catch((e) => {
  console.error('ERR', e.message);
  process.exit(1);
});
