// Offline encrypted backup for the status writer key; never logs the key.
import { createCipheriv, createDecipheriv, pbkdf2Sync, randomBytes } from 'node:crypto';
const version='MRFPS-ROSTER-HMAC-1', iterations=310000, kdf='PBKDF2-SHA256';
const aad=Buffer.from(version,'utf8');
function derive(passphrase,salt) {
  if (typeof passphrase!=='string' || Buffer.byteLength(passphrase,'utf8')<12) throw Error('passphrase requires at least 12 bytes');
  return pbkdf2Sync(passphrase,salt,iterations,32,'sha256');
}
function decode(value,length) {
  if (typeof value!=='string' || !/^[A-Za-z0-9+/]+={0,2}$/.test(value)) throw Error('invalid roster backup');
  const bytes=Buffer.from(value,'base64');
  if (bytes.length!==length || bytes.toString('base64')!==value) throw Error('invalid roster backup');
  return bytes;
}
export function encryptRosterKey(secret,passphrase) {
  if (typeof secret!=='string' || !/^[a-f0-9]{64}$/.test(secret)) throw Error('invalid roster key');
  const salt=randomBytes(16),iv=randomBytes(12), plain=Buffer.from(secret,'hex'), key=derive(passphrase,salt);
  try {
    const cipher=createCipheriv('aes-256-gcm',key,iv); cipher.setAAD(aad);
    const encrypted=Buffer.concat([cipher.update(plain),cipher.final(),cipher.getAuthTag()]);
    return {version,kdf,iterations,salt:salt.toString('base64'),iv:iv.toString('base64'),ciphertext:encrypted.toString('base64')};
  } finally { plain.fill(0); key.fill(0); }
}
export function decryptRosterKey(backup,passphrase) {
  if (!backup || typeof backup!=='object' || Array.isArray(backup) ||
      Object.keys(backup).sort().join(',')!=='ciphertext,iterations,iv,kdf,salt,version' ||
      backup.version!==version || backup.kdf!==kdf || backup.iterations!==iterations) throw Error('invalid roster backup');
  const salt=decode(backup.salt,16),iv=decode(backup.iv,12), encrypted=decode(backup.ciphertext,48),key=derive(passphrase,salt);
  let plain;
  try {
    const cipher=createDecipheriv('aes-256-gcm',key,iv); cipher.setAAD(aad); cipher.setAuthTag(encrypted.subarray(32));
    plain=Buffer.concat([cipher.update(encrypted.subarray(0,32)),cipher.final()]);
    if (plain.length!==32) throw Error('invalid roster backup');
    return plain.toString('hex');
  } finally { if (plain) plain.fill(0); key.fill(0); }
}
