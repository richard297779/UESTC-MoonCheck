#!/usr/bin/env node
/**
 * 从抓到的 CAS 登录密文反推密码加密算法（不打印明文密码，只打印结构判定）。
 * 用法: node tools/probe_cas_crypto.js
 */
const fs = require('fs');
const crypto = require('crypto');

const post = fs.readFileSync('capture/raw/#2-login-POST.txt', 'utf8');
const html = fs.readFileSync('capture/raw/idas_login_page.html', 'utf8');

const body = post.split(/\r?\n\r?\n/).pop();
const m = body.match(/password=([^&]+)/);
if (!m) { console.error('未在抓包中找到 password 字段'); process.exit(1); }
const encStr = decodeURIComponent(m[1]);

const htmlSalt = (html.match(/id="pwdEncryptSalt"\s+value="([^"]+)"/) || [])[1];
const DEFAULT_SALT = 'rjBFAaHsNkKAhpoi';

const buf = Buffer.from(encStr, 'base64');
console.log(`ciphertext(base64) len=${encStr.length}  decoded=${buf.length} bytes`);
console.log(`salted-prefix? ${buf.subarray(0, 8).toString('latin1') === 'Salted__'}`);
console.log(`html salt = ${htmlSalt}  (len ${htmlSalt ? htmlSalt.length : 0})`);
console.log(`default salt = ${DEFAULT_SALT}`);

function aesCbcDecrypt(key, iv, ct) {
  const d = crypto.createDecipheriv('aes-128-cbc', key, iv);
  return Buffer.concat([d.update(ct), d.final()]); // 抛异常即 padding/解密失败
}

function describe(label, key, iv, ct) {
  try {
    const out = aesCbcDecrypt(key, iv, ct);
    const txt = out.toString('utf8');
    const printable = /^[\x20-\x7e\u4e00-\u9fa5]+$/.test(txt);
    const startsWithSalt = htmlSalt ? txt.startsWith(htmlSalt) : false;
    // 只暴露结构：已知盐部分原样保留，其余掩码
    const masked = startsWithSalt ? txt.slice(0, 16) + '*'.repeat(Math.max(0, txt.length - 16)) : '*'.repeat(txt.length);
    console.log(`[OK ] ${label}: len=${out.length} printable=${printable} startsWithSalt=${startsWithSalt} masked=${masked}`);
    return true;
  } catch (e) {
    console.log(`[FAIL] ${label}: ${e.message}`);
    return false;
  }
}

const salts = [...new Set([htmlSalt, DEFAULT_SALT].filter(Boolean))];
const ivBytes = buf.subarray(0, 16);
const ctBytes = buf.subarray(16);

// 变体 A: base64(iv_bytes || ciphertext)
for (const salt of salts) {
  describe(`A1 iv=bytes,key=salt(${salt})`, Buffer.from(salt, 'utf8'), ivBytes, ctBytes);
  describe(`A2 iv=bytes,key=md5(salt)`, crypto.createHash('md5').update(salt).digest(), ivBytes, ctBytes);
}

// 变体 B: iv_str(16 chars) + base64(ciphertext)
const ivStr = encStr.slice(0, 16);
const ctB64 = encStr.slice(16);
let ctB = null;
try { ctB = Buffer.from(ctB64, 'base64'); } catch (_) {}
if (ctB && ctB.length % 16 === 0) {
  for (const salt of salts) {
    describe(`B1 iv=str,key=salt(${salt})`, Buffer.from(salt, 'utf8'), Buffer.from(ivStr, 'utf8'), ctB);
    describe(`B2 iv=str,key=md5(salt)`, crypto.createHash('md5').update(salt).digest(), Buffer.from(ivStr, 'utf8'), ctB);
  }
} else {
  console.log(`变体 B 不适用: ct len=${ctB ? ctB.length : 'n/a'}`);
}

// 变体 C: key/iv 均来自 salt 派生（ECB / CBC-zero-iv）
for (const salt of salts) {
  describe(`C1 cbc iv=0,key=salt(${salt})`, Buffer.from(salt, 'utf8'), Buffer.alloc(16, 0), ctBytes);
  try {
    const d = crypto.createDecipheriv('aes-128-ecb', Buffer.from(salt, 'utf8'), null);
    const out = Buffer.concat([d.update(buf), d.final()]);
    console.log(`[OK ] C2 ecb key=salt(${salt}): len=${out.length} masked=${'*'.repeat(out.length)}`);
  } catch (e) { console.log(`[FAIL] C2 ecb key=salt(${salt}): ${e.message}`); }
}
