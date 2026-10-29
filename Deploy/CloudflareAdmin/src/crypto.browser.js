// Shared by the browser and the local Node CLI. Private keys never leave this module.
const enc = new TextEncoder();
const backupTag = 'MRFPS-ISSUER-1';
const iterations = 310000;
function b64(data) { return btoa(String.fromCharCode(...new Uint8Array(data))); }
function fromB64(value) {
  if (typeof value !== 'string' || !/^[A-Za-z0-9+/]+={0,2}$/.test(value)) throw Error('invalid backup');
  return Uint8Array.from(atob(value), c => c.charCodeAt(0));
}
function b64url(data) { return b64(data).replace(/\+/g, '-').replace(/\//g, '_').replace(/=+$/, ''); }
async function derive(passphrase, salt) {
  if (typeof passphrase !== 'string' || enc.encode(passphrase).length < 12) throw Error('パスフレーズは12文字以上必要です');
  const key = await crypto.subtle.importKey('raw', enc.encode(passphrase), 'PBKDF2', false, ['deriveKey']);
  return crypto.subtle.deriveKey({ name: 'PBKDF2', hash: 'SHA-256', salt, iterations }, key,
    { name: 'AES-GCM', length: 256 }, false, ['encrypt', 'decrypt']);
}
function pem(bytes) { return '-----BEGIN PUBLIC KEY-----\n' + b64(bytes) + '\n-----END PUBLIC KEY-----\n'; }
async function verifyPair(privateKey, publicKey) {
  const contents = /^-----BEGIN PUBLIC KEY-----\n([A-Za-z0-9+/]{59}=)\n-----END PUBLIC KEY-----\n$/.exec(publicKey);
  if (!contents) throw Error('invalid public key');
  const pub = await crypto.subtle.importKey('spki', fromB64(contents[1]), 'Ed25519', false, ['verify']);
  const msg = crypto.getRandomValues(new Uint8Array(32));
  if (!await crypto.subtle.verify('Ed25519', pub, await crypto.subtle.sign('Ed25519', privateKey, msg), msg)) throw Error('key mismatch');
}
export async function generateBackup(passphrase) {
  const pair = await crypto.subtle.generateKey('Ed25519', true, ['sign', 'verify']);
  const publicKey = pem(await crypto.subtle.exportKey('spki', pair.publicKey));
  const privateBytes = new Uint8Array(await crypto.subtle.exportKey('pkcs8', pair.privateKey));
  const salt = crypto.getRandomValues(new Uint8Array(16));
  const iv = crypto.getRandomValues(new Uint8Array(12));
  try {
    const protection = await derive(passphrase, salt);
    const encrypted = await crypto.subtle.encrypt({ name: 'AES-GCM', iv, additionalData: enc.encode(backupTag) }, protection, privateBytes);
    const backup = { version: backupTag, kdf: 'PBKDF2-SHA256', iterations, salt: b64(salt), iv: b64(iv), ciphertext: b64(encrypted), public_key: publicKey };
    const privateKey = await crypto.subtle.importKey('pkcs8', privateBytes, 'Ed25519', false, ['sign']);
    return { privateKey, publicKey, backup };
  } finally { privateBytes.fill(0); }
}
export async function restoreBackup(backup, passphrase) {
  if (!backup || backup.version !== backupTag || backup.kdf !== 'PBKDF2-SHA256' || backup.iterations !== iterations) throw Error('invalid backup');
  const salt = fromB64(backup.salt), iv = fromB64(backup.iv), ciphertext = fromB64(backup.ciphertext);
  if (salt.length !== 16 || iv.length !== 12 || ciphertext.length < 16 || ciphertext.length > 1024) throw Error('invalid backup');
  const protection = await derive(passphrase, salt);
  const privateBytes = new Uint8Array(await crypto.subtle.decrypt({ name: 'AES-GCM', iv, additionalData: enc.encode(backupTag) }, protection, ciphertext));
  try {
    const privateKey = await crypto.subtle.importKey('pkcs8', privateBytes, 'Ed25519', false, ['sign']);
    await verifyPair(privateKey, backup.public_key);
    return { privateKey, publicKey: backup.public_key, backup };
  } finally { privateBytes.fill(0); }
}
export async function newCode(role, owner, fingerprint, key) {
  if (!['player','inspector','admin'].includes(role) || !/^[0-9a-f]{64}$/.test(fingerprint)) throw Error('invalid role or server fingerprint');
  const now = Math.floor(Date.now() / 1000);
  const claims = { iss: 'mrfps-issuer', aud: 'mrfps', sub: role + '-' + crypto.randomUUID(), role,
    iat: now, nbf: now, exp: now + 3600, jti: crypto.randomUUID(), dtls_fp: fingerprint };
  const data = b64url(enc.encode(JSON.stringify({ alg:'EdDSA', typ:'JWT' }))) + '.' + b64url(enc.encode(JSON.stringify(claims)));
  const sig = await crypto.subtle.sign('Ed25519', key.privateKey, enc.encode(data));
  let random;
  do { random = crypto.getRandomValues(new Uint32Array(1))[0]; } while (random >= 4294000000);
  const code = String(random % 1000000).padStart(6, '0');
  const code_hash = Array.from(new Uint8Array(await crypto.subtle.digest('SHA-256', enc.encode(role + ':' + code))), b => b.toString(16).padStart(2, '0')).join('');
  return { code, body: { role, owner, id: crypto.randomUUID(), code_hash, expires_at: now + 30, token: data + '.' + b64url(sig), public_key: key.publicKey } };
}
