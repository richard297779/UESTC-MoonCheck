// 一次性自检：用刚生成的 public_ca_pem.h 作为唯一信任锚，验证各推送域名握手可通过。
// 用法: node out/verify_public_ca.js   （out/ 不进仓库）
const fs = require('fs');
const tls = require('tls');
const path = require('path');

const header = fs.readFileSync(path.join('firmware', 'include', 'public_ca_pem.h'), 'utf8');
const begin = header.indexOf('R"PEMCERT(');
const end = header.indexOf(')PEMCERT"');
if (begin < 0 || end < 0) throw new Error('kPublicCaPem not found');
const pem = header.slice(begin + 'R"PEMCERT('.length, end);

const hosts = process.argv.slice(2).length
  ? process.argv.slice(2)
  : ['api.day.app', 'ntfy.sh', 'sctapi.ftqq.com', 'oapi.dingtalk.com'];

let done = 0;
let failed = 0;
hosts.forEach((h) => {
  const sock = tls.connect(
    { host: h, port: 443, servername: h, ca: pem, rejectUnauthorized: true },
    () => {
      console.log('VERIFY-OK  ', h, '(root count =', (pem.match(/BEGIN CERTIFICATE/g) || []).length, ')');
      sock.destroy();
      if (++done === hosts.length) process.exit(failed ? 1 : 0);
    }
  );
  sock.setTimeout(9000);
  sock.on('error', (e) => {
    console.log('VERIFY-FAIL', h, e.code || e.message);
    failed++;
    sock.destroy();
    if (++done === hosts.length) process.exit(1);
  });
});
