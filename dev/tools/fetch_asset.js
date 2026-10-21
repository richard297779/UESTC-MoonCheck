#!/usr/bin/env node
/** 抓取公开静态资源（用于补齐未被 Burp 记录的 JS）。
 *  用法: node tools/fetch_asset.js <url> <outFile>
 */
const fs = require('fs');
const path = require('path');

const url = process.argv[2];
const out = process.argv[3];
if (!url || !out) {
  console.error('usage: node tools/fetch_asset.js <url> <outFile>');
  process.exit(2);
}

(async () => {
  const res = await fetch(url, {
    headers: {
      'User-Agent':
        'Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/155.0.0.0 Safari/537.36',
      Accept: '*/*',
      Referer: 'https://idas.uestc.edu.cn/authserver/login',
    },
  });
  const text = await res.text();
  fs.mkdirSync(path.dirname(out), { recursive: true });
  fs.writeFileSync(out, text, 'utf8');
  console.log(`status=${res.status} bytes=${text.length} ctype=${res.headers.get('content-type')} -> ${out}`);
})().catch((e) => {
  console.error('ERR', e.message);
  process.exit(1);
});
