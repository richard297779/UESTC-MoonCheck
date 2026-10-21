#!/usr/bin/env node
/**
 * 解析 Burp Suite 导出的 items XML：生成全量请求清单、Set-Cookie 清单，
 * 并抽取关键请求/响应（默认过滤 daily-checkin / authserver / checkLogin）。
 *
 * 用法: node tools/extract_burp.js [xml] [outDir] [filterRegex]
 */
const fs = require('fs');
const path = require('path');
const zlib = require('zlib');

const xmlPath = process.argv[2] || 'capture/raw/burp-items.xml';
const outDir = process.argv[3] || 'capture/analysis';
const filterSrc = process.argv[4] || 'daily-checkin|authserver|checkLogin';
const filter = new RegExp(filterSrc, 'i');

const xml = fs.readFileSync(xmlPath, 'utf8');
const itemsDir = path.join(outDir, 'items');
fs.mkdirSync(itemsDir, { recursive: true });

function pick(block, name) {
  // (?=[\s>]) 防止 <response> 误匹配 <responselength>
  const m = block.match(new RegExp(`<${name}(?=[\\s>])([^>]*)>([\\s\\S]*?)</${name}>`));
  if (!m) return { attrs: '', value: '', found: false };
  let value = m[2];
  const cdata = value.match(/^\s*<!\[CDATA\[([\s\S]*?)\]\]>\s*$/);
  if (cdata) value = cdata[1];
  return { attrs: m[1], value, found: true };
}

function pickBytes(block, name) {
  const { attrs, value, found } = pick(block, name);
  if (!found) return null;
  return /base64="true"/.test(attrs) ? Buffer.from(value.trim(), 'base64') : Buffer.from(value, 'utf8');
}

function splitRaw(buf) {
  if (!buf || !buf.length) return { head: '', body: Buffer.alloc(0) };
  const i = buf.indexOf('\r\n\r\n');
  if (i < 0) return { head: buf.toString('latin1'), body: Buffer.alloc(0) };
  return { head: buf.toString('latin1', 0, i), body: buf.subarray(i + 4) };
}

function decodeBody(head, body) {
  if (!body || !body.length) return '';
  const enc = (head.match(/^Content-Encoding:\s*(.+)$/im) || [, ''])[1].trim().toLowerCase();
  try {
    if (enc.includes('gzip')) body = zlib.gunzipSync(body);
    else if (enc.includes('br')) body = zlib.brotliDecompressSync(body);
    else if (enc.includes('deflate')) body = zlib.inflateSync(body);
  } catch (e) {
    return `[decompress failed: ${e.message}]\n${body.toString('latin1').slice(0, 2000)}`;
  }
  return body.toString('utf8');
}

const inventory = [];
const cookieRows = [];
let idx = 0;
let written = 0;

const itemRe = /<item>([\s\S]*?)<\/item>/g;
let m;
while ((m = itemRe.exec(xml)) !== null) {
  const block = m[1];
  idx++;
  const url = pick(block, 'url').value.trim();
  const method = pick(block, 'method').value.trim();
  const status = pick(block, 'status').value.trim();
  const mime = pick(block, 'mimetype').value.trim();
  const rlen = pick(block, 'responselength').value.trim();
  const time = pick(block, 'time').value.trim();
  const comment = pick(block, 'comment').value.trim();

  let host = '';
  let pathQ = url;
  try {
    const u = new URL(url);
    host = u.host;
    pathQ = u.pathname + u.search;
  } catch (_) {}
  inventory.push([idx, time, method, status, mime, rlen, host, pathQ, comment].join('\t'));

  const req = splitRaw(pickBytes(block, 'request'));
  const res = splitRaw(pickBytes(block, 'response'));

  for (const sc of res.head.matchAll(/^Set-Cookie:\s*(.+)$/gim)) {
    cookieRows.push(`${idx}\t${url}\t${sc[1].trim()}`);
  }

  const isAsset = /^image|^CSS|^font/i.test(mime);
  if (!filter.test(url) || isAsset) continue;

  const parts = [
    `=== #${idx} ${method} ${url}`,
    `status=${status} mime=${mime} time=${time} comment=${comment} rlen=${rlen}`,
    '--- REQUEST ---',
    req.head,
    decodeBody(req.head, req.body),
    '--- RESPONSE ---',
    res.head,
    decodeBody(res.head, res.body),
  ];
  const safe = pathQ.replace(/[^\w.\-]/g, '_').slice(0, 90);
  fs.writeFileSync(path.join(itemsDir, `${String(idx).padStart(4, '0')}_${method}_${safe}.txt`), parts.join('\n'), 'utf8');
  written++;
}

fs.writeFileSync(path.join(outDir, 'inventory.tsv'), inventory.join('\n'), 'utf8');
fs.writeFileSync(path.join(outDir, 'setcookie.tsv'), cookieRows.join('\n'), 'utf8');

console.log(`items=${idx} written=${written} out=${outDir}`);
const byHost = {};
for (const row of inventory) {
  const h = row.split('\t')[6];
  byHost[h] = (byHost[h] || 0) + 1;
}
console.log('--- top hosts ---');
console.log(
  Object.entries(byHost)
    .sort((a, b) => b[1] - a[1])
    .slice(0, 6)
    .map(([k, v]) => `${v}\t${k}`)
    .join('\n')
);
