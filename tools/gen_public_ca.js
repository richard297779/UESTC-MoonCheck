#!/usr/bin/env node
/**
 * 生成"公网通知通道"用的 CA 信任集合（与校园站点的 kUestcCaPem 分开）。
 * 输出：firmware/include/public_ca_pem.h （常量 kPublicCaPem）
 *
 * 为什么单独一份：
 *   - HttpClient 对 *.uestc.edu.cn 只做"抓包链校验"（严格、可复现）；
 *   - 推送服务（Bark / ntfy / Server酱 / 钉钉 …）用的是公共 CA，且会轮换，
 *     把它们混进平台 CA 会让校园站的校验变得不确定。
 *
 * 用法：
 *   node tools/gen_public_ca.js                        # 内置常用推送域名
 *   node tools/gen_public_ca.js ntfy.sh api.day.app    # 指定域名（替换默认列表）
 *   node tools/gen_public_ca.js --add self-host.ntfy   # 在默认列表上追加
 *   node tools/gen_public_ca.js --anchors              # 只内置信任锚点（更省 Flash，跨签链可能验不过）
 *
 * 默认把采集到的**非叶子证书全部内置**（中间 CA + 根 CA，约 20 KB Flash）：实测只留锚点时
 * 钉钉链会验不过（GlobalSign R46 被 R3 交叉签名）。真在乎 Flash 时可用 --anchors 试一次，握手通过再留。
 * 若某条链里一张自签根都没有，会自动沿证书里的 AIA caIssuers 地址往上取一级。
 */
const fs = require('fs');
const path = require('path');
const tls = require('tls');
const crypto = require('crypto');

/** 常用推送/告警服务域名（用户可用 argv 追加或替换）。 */
const DEFAULT_HOSTS = [
  'api.day.app',       // Bark
  'ntfy.sh',           // ntfy 官方公共服务器
  'sctapi.ftqq.com',   // Server酱·Turbo
  'oapi.dingtalk.com', // 钉钉自定义机器人
];

const OUT = path.join('firmware', 'include', 'public_ca_pem.h');

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
            selfSigned: !!(cert.subject && cert.issuer && cert.subject.CN === cert.issuer.CN),
            der: cert.raw,
            fp: cert.fingerprint256,
          });
          if (!cert.issuerCertificate || cert.issuerCertificate === cert) break;
          cert = cert.issuerCertificate;
        }
        socket.end();
        resolve(chain);
      }
    );
    socket.on('error', reject);
    socket.setTimeout(10000, () => {
      socket.destroy();
      reject(new Error('timeout'));
    });
  });
}

/**
 * 直接抓取厂商发布的自签根（DER 或 PEM），用于补全"交叉签名链"缺的那一层。
 * 实测钉钉 oapi 链：leaf <- GlobalSign GCC R46 OV TLS CA 2025 <- GlobalSign Root R46(被 R3 交叉签)，
 * 握手里拿不到自签版 R46，OpenSSL 视角验不过 —— mbedTLS 也许宽松，但不能赌。
 *
 * 注意：这里**刻意不用 require('https')** —— 开发机常把 HTTP(S)_PROXY 指向本地代理，
 * 代理没开时 Node 的 https 会 ECONNREFUSED，而 tls.connect 是直连、不受代理变量影响。
 */
function fetchCert(url) {
  return new Promise((resolve, reject) => {
    const u = new URL(url);
    const isHttp = u.protocol === 'http:';
    // AIA 地址多是 http(80) 的裸 DER，且**不能走 http/https 模块**（会被本地代理变量劫持），
    // 所以 http 用 net、https 用 tls，都手写一行请求。
    // 注意 net.connect 的 opts 里**不能出现 path 字段** —— 那会被当成 Unix socket 路径（ENOENT）。
    const mod = isHttp ? require('net') : require('tls');
    const requestPath = u.pathname + u.search;
    const opts = isHttp
      ? { host: u.hostname, port: u.port || 80, timeout: 10000 }
      : { host: u.hostname, port: u.port || 443, servername: u.hostname, rejectUnauthorized: false, timeout: 10000 };
    const sock = mod.connect(opts, () => {
      sock.write(
        `GET ${requestPath} HTTP/1.0\r\nHost: ${u.hostname}\r\n` +
          'User-Agent: mooncheck-ca-gen/1.0\r\nAccept: */*\r\nConnection: close\r\n\r\n'
      );
    });
    const chunks = [];
    sock.on('data', (d) => chunks.push(d));
    sock.on('end', () => {
      const buf = Buffer.concat(chunks);
      const sep = buf.indexOf('\r\n\r\n');
      const head = sep >= 0 ? buf.slice(0, sep).toString('latin1') : '';
      const body = sep >= 0 ? buf.slice(sep + 4) : buf;
      if (head && !/^HTTP\/1\.[01] 200/m.test(head)) {
        return reject(new Error((head.split('\r\n')[0] || 'bad response') + ' for ' + url));
      }
      if (body.length < 100) return reject(new Error('response too small for ' + url));
      resolve(body);
    });
    sock.on('error', reject);
    sock.setTimeout(12000, () => {
      sock.destroy();
      reject(new Error('timeout'));
    });
  });
}

/** 把 DER 或 PEM 统一成 DER Buffer。 */
function toDer(buf) {
  if (buf.slice(0, 60).toString('latin1').includes('BEGIN CERTIFICATE')) {
    const m = buf.toString('utf8').match(/-----BEGIN CERTIFICATE-----([\s\S]*?)-----END CERTIFICATE-----/);
    if (!m) throw new Error('PEM parse failed');
    return Buffer.from(m[1].replace(/[^A-Za-z0-9+/=]/g, ''), 'base64');
  }
  if (buf[0] !== 0x30) throw new Error('neither PEM nor DER');
  return buf;
}

/** 从 DER 里扫出 AIA 的 caIssuers 地址（跳过 ocsp/crl，只认 .crt/.cer/.pem）。 */
function issuerUrlsInDer(der) {
  const text = der.toString('latin1');
  const found = [...text.matchAll(/(https?:\/\/[A-Za-z0-9._\-/]+?\.(?:crt|cer|pem))/g)].map((m) => m[1]);
  return [...new Set(found)].filter((u) => !/ocsp|crl/i.test(u));
}

/**
 * 沿 AIA 往上爬到**自签根**。
 * 为什么需要：服务器握手下发的链常常止于"被别的根交叉签名的根"。实测钉钉：
 *   leaf <- GlobalSign GCC R46 OV TLS CA 2025 <- GlobalSign Root R46 (issuer=GlobalSign Root CA)
 * 最后这层不是自签的，而 TLS 校验要求信任锚点是自签根，否则 UNABLE_TO_GET_ISSUER_CERT。
 * AIA 地址就写在上一级证书的 DER 里，直接取来用即可 —— 比硬编码厂商 URL 更通用。
 */
async function climbToSelfSigned(cert, maxHops = 3) {
  const climbed = [];
  let cur = cert;
  for (let hop = 0; hop < maxHops; hop++) {
    if (cur.selfSigned) break;
    let fetched = null;
    for (const url of issuerUrlsInDer(cur.der)) {
      try {
        const x = new crypto.X509Certificate(toDer(await fetchCert(url)));
        fetched = {
          cn: (x.subject.match(/CN=([^\n\r]+)/) || [])[1] || '?',
          issuer: (x.issuer.match(/CN=([^\n\r]+)/) || [])[1] || '?',
          selfSigned: x.subject === x.issuer,
          der: x.raw,
          fp: x.fingerprint256.toUpperCase(),
          from: url,
        };
        break;
      } catch (e) {
        console.error(`    · AIA ${url} 取不到（${e.message}），试下一个`);
      }
    }
    if (!fetched) break;
    climbed.push(fetched);
    cur = fetched;
  }
  return { top: cur, finalSelfSigned: !!cur.selfSigned, climbed };
}

(async () => {
  const argv = process.argv.slice(2);
  const wantAnchorsOnly = argv.includes('--anchors');
  const flagRe = /^--/;
  let hosts = argv.filter((a) => !flagRe.test(a));
  if (argv.includes('--add')) {
    const i = argv.indexOf('--add');
    hosts = DEFAULT_HOSTS.concat(argv.slice(i + 1).filter((a) => !flagRe.test(a)));
  }
  if (hosts.length === 0) hosts = DEFAULT_HOSTS;

  const anchors = new Map(); // fingerprint -> cert（内置用）
  const every = new Map();   // fingerprint -> cert（默认内置的完整集合）
  for (const host of hosts) {
    let chain;
    try {
      chain = await fetchChain(host);
    } catch (e) {
      console.error(`  ! ${host}: ${e.message}（跳过，可稍后单独重跑）`);
      continue;
    }
    console.log(`  · ${host}: ${chain.map((c) => c.cn).join('  <-  ')}`);
    for (let i = 1; i < chain.length; i++) {
      const c = { ...chain[i], from: host };
      if (!every.has(c.fp)) every.set(c.fp, c);
      // 锚点：自签根，或该链最后一级（跨签时握手里拿不到自签根）
      if (c.selfSigned || i === chain.length - 1) {
        if (!anchors.has(c.fp)) anchors.set(c.fp, c);
      }
    }
    // 只有当整条链里**没有任何自签根**时才需要爬 AIA 补锚点（实测钉钉：链末级 R46 被 R3 交叉签，
    // 服务器不下发自签版，mbedTLS/OpenSSL 都会 UNABLE_TO_GET_ISSUER_CERT）。
    // 自签版 rootr46.crt 的下载地址写在它下一级（中间 CA）的 AIA caIssuers 里，所以要逐级找。
    const hasSelfSignedRoot = chain.slice(1).some((c) => c.selfSigned);
    if (!hasSelfSignedRoot && !argv.includes('--no-climb')) {
      for (let i = 1; i < chain.length; i++) {
        if (chain[i].selfSigned) continue;
        const { climbed, finalSelfSigned } = await climbToSelfSigned({ ...chain[i], from: host });
        for (const got of climbed) {
          if (!every.has(got.fp)) every.set(got.fp, { ...got, from: host });
          if (got.selfSigned && !anchors.has(got.fp)) anchors.set(got.fp, { ...got, from: host });
        }
        if (finalSelfSigned) {
          console.log(`      ↑ AIA 补到自签根 ${climbed[climbed.length - 1].cn}`);
          break;
        }
      }
      if (!chain.slice(1).some((c) => c.selfSigned) && !anchors.has(chain[chain.length - 1].fp)) {
        console.error(`  ! ${host}: 没找到任何自签根 —— 已内置整条链兜底，若握手失败请改用局域网中转`);
      }
    }
  }

  const picked = [...(wantAnchorsOnly ? anchors : every).values()];
  if (picked.length === 0) {
    throw new Error('no CA collected — 当前网络无法访问这些域名');
  }

  const lines = [];
  lines.push('#pragma once');
  lines.push('// ============================================================================');
  lines.push('// 本文件由 tools/gen_public_ca.js 自动生成，请勿手工编辑。');
  lines.push('// 用途：HttpClient 在校验模式下访问**公网通知服务**（Bark/ntfy/Server酱/钉钉 …）。');
  lines.push('// 采集自：' + hosts.join(', ') + (wantAnchorsOnly ? '   [--anchors: 仅信任锚点]' : ''));
  lines.push('// 证书清单：');
  picked.forEach((c, i) => {
    lines.push(
      `//   [${i}] CN=${c.cn}  (issuer=${c.issuer}${c.selfSigned ? ', 自签根' : ', 链末级'}, via ${c.from})`
    );
  });
  lines.push('// 说明：叶子与中间证书服务器都会随握手下发，这里只内置信任锚点。');
  lines.push('//      换服务或证书轮换后重跑： node tools/gen_public_ca.js <host> ...');
  lines.push('// ============================================================================');
  lines.push('');
  lines.push('/// @brief 公网推送服务 CA 集合（PEM，多证书拼接）。');
  lines.push('static const char kPublicCaPem[] = R"PEMCERT(');
  picked.forEach((c) => lines.push(derToPem(c.der)));
  lines.push(')PEMCERT";');
  lines.push('');

  fs.mkdirSync(path.dirname(OUT), { recursive: true });
  fs.writeFileSync(OUT, lines.join('\n'), 'utf8');
  console.log(
    `anchors=${anchors.size} all=${every.size} embedded=${picked.length} -> ${OUT} (${fs.statSync(OUT).size} B)`
  );
})().catch((e) => {
  console.error('ERR', e.message);
  process.exit(1);
});
