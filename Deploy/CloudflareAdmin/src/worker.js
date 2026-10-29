import { DurableObject } from 'cloudflare:workers';
import panel from './panel.html';
import script from './panel.browser.js';
import style from './panel.css';
import cryptoScript from './crypto.browser.js';

const roles = new Set(['player', 'inspector', 'admin']);
const encoder = new TextEncoder();
const commonHeaders = {
  'cache-control': 'no-store',
  'x-content-type-options': 'nosniff',
  'referrer-policy': 'no-referrer',
  'x-frame-options': 'DENY',
  'strict-transport-security': 'max-age=31536000',
  'content-security-policy': "default-src 'none'; script-src 'self'; style-src 'self'; connect-src 'self'; img-src 'self'; base-uri 'none'; form-action 'none'; frame-ancestors 'none'",
};

function json(value, status = 200, extra = {}) {
  return Response.json(value, { status, headers: { ...commonHeaders, ...extra } });
}
function textBody(value, type) {
  return new Response(value, { headers: { ...commonHeaders, 'content-type': type } });
}
async function digest(value) {
  const bytes = new Uint8Array(await crypto.subtle.digest('SHA-256', encoder.encode(value)));
  return Array.from(bytes, n => n.toString(16).padStart(2, '0')).join('');
}
async function credentialsValid(request, env) {
  if (!env.MRFPS_ADMIN_USERNAME || !env.MRFPS_ADMIN_PASSWORD) return false;
  let supplied = '';
  try {
    const auth = request.headers.get('authorization') || '';
    if (auth.length > 1024 || !auth.startsWith('Basic ')) return false;
    supplied = atob(auth.slice(6));
  } catch { return false; }
  const actual = await crypto.subtle.digest('SHA-256', encoder.encode(supplied));
  const expected = await crypto.subtle.digest('SHA-256', encoder.encode(`${env.MRFPS_ADMIN_USERNAME}:${env.MRFPS_ADMIN_PASSWORD}`));
  return crypto.subtle.timingSafeEqual(actual, expected);
}
async function bodyJson(request, limit = 8192) {
  if (!/^application\/json(?:\s*;|$)/i.test(request.headers.get('content-type') || '')) throw Error('content_type');
  const reader = request.body?.getReader();
  if (!reader) throw Error('body');
  let count = 0;
  const parts = [];
  while (true) {
    const { done, value } = await reader.read();
    if (done) break;
    count += value.byteLength;
    if (count > limit) { await reader.cancel(); throw Error('body'); }
    parts.push(value);
  }
  const bytes = new Uint8Array(count);
  let offset = 0;
  for (const part of parts) { bytes.set(part, offset); offset += part.byteLength; }
  const parsed = JSON.parse(new TextDecoder('utf-8', { fatal: true }).decode(bytes));
  if (!parsed || Array.isArray(parsed) || typeof parsed !== 'object') throw Error('body');
  return parsed;
}
function b64decode(s) {
  if (!/^[A-Za-z0-9_-]+$/.test(s)) throw Error('encoding');
  const raw = atob(s.replace(/-/g, '+').replace(/_/g, '/') + '='.repeat((4 - s.length % 4) % 4));
  const bytes = Uint8Array.from(raw, c => c.charCodeAt(0));
  const canonical = btoa(String.fromCharCode(...bytes)).replace(/\+/g, '-').replace(/\//g, '_').replace(/=+$/, '');
  if (s !== canonical) throw Error('encoding');
  return bytes;
}
function decodePart(part) {
  const raw = new TextDecoder('ascii', { fatal: true }).decode(b64decode(part));
  // The issuer emits flat ASCII objects without escapes. Reject alternate profiles.
  if (raw.includes('\\') || !/^[\x20-\x7e]+$/.test(raw)) throw Error('profile');
  const parsed = JSON.parse(raw);
  if (JSON.stringify(parsed) !== raw) throw Error('profile');
  return parsed;
}
async function validatePublish(input) {
  if (!roles.has(input.role) || !/^[0-9a-f]{64}$/.test(input.code_hash || '') ||
      !/^[0-9a-f]{8}-[0-9a-f]{4}-4[0-9a-f]{3}-[89ab][0-9a-f]{3}-[0-9a-f]{12}$/.test(input.id || '') ||
      !/^[0-9a-f]{8}-[0-9a-f]{4}-4[0-9a-f]{3}-[89ab][0-9a-f]{3}-[0-9a-f]{12}$/.test(input.owner || '') ||
      !Number.isSafeInteger(input.expires_at)) throw Error('invalid');
  const now = Math.floor(Date.now() / 1000);
  if (input.expires_at <= now || input.expires_at > now + 30) throw Error('expired');
  const match = /^-----BEGIN PUBLIC KEY-----\n([A-Za-z0-9+/]{59}=)\n-----END PUBLIC KEY-----\n$/.exec(input.public_key || '');
  if (!match) throw Error('key');
  const keyBytes = Uint8Array.from(atob(match[1]), c => c.charCodeAt(0));
  const key = await crypto.subtle.importKey('spki', keyBytes, 'Ed25519', false, ['verify']);
  if (typeof input.token !== 'string' || input.token.length > 2048) throw Error('token');
  const parts = input.token.split('.');
  if (parts.length !== 3) throw Error('token');
  const header = decodePart(parts[0]);
  const claims = decodePart(parts[1]);
  if (JSON.stringify(Object.keys(header).sort()) !== JSON.stringify(['alg', 'typ']) || header.alg !== 'EdDSA' || header.typ !== 'JWT') throw Error('profile');
  if (JSON.stringify(Object.keys(claims).sort()) !== JSON.stringify(['aud','dtls_fp','exp','iat','iss','jti','nbf','role','sub']) ||
      claims.iss !== 'mrfps-issuer' || claims.aud !== 'mrfps' || claims.role !== input.role ||
      !/^[A-Za-z0-9][A-Za-z0-9._-]{0,63}$/.test(claims.sub || '') || !/^[0-9a-f]{64}$/.test(claims.dtls_fp || '') ||
      !/^[0-9a-f]{8}-[0-9a-f]{4}-4[0-9a-f]{3}-[89ab][0-9a-f]{3}-[0-9a-f]{12}$/.test(claims.jti || '') ||
      !Number.isSafeInteger(claims.iat) || !Number.isSafeInteger(claims.nbf) || !Number.isSafeInteger(claims.exp) ||
      claims.iat < 0 || claims.iat > now + 5 || claims.nbf > now + 5 || claims.iat > claims.nbf ||
      claims.nbf >= claims.exp || claims.exp - claims.iat > 3600 || claims.exp <= input.expires_at) throw Error('claims');
  const sig = b64decode(parts[2]);
  if (sig.length !== 64 || !await crypto.subtle.verify('Ed25519', key, sig, encoder.encode(parts[0] + '.' + parts[1]))) throw Error('signature');
  return { ...input, token_expires_at: claims.exp };
}

// One coordinated object for this one game server. SQLite operations in each method
// do not yield, so concurrent redemption cannot both take the same token.
export class DeviceCodes extends DurableObject {
  constructor(ctx, env) {
    super(ctx, env);
    ctx.blockConcurrencyWhile(async () => {
      this.ctx.storage.sql.exec(`CREATE TABLE IF NOT EXISTS codes (
        role TEXT PRIMARY KEY, id TEXT NOT NULL, owner TEXT NOT NULL,
        expires_at INTEGER NOT NULL, code_hash TEXT NOT NULL, token TEXT,
        public_key TEXT, token_expires_at INTEGER NOT NULL, consumed INTEGER NOT NULL DEFAULT 0)`);
      this.ctx.storage.sql.exec(`CREATE TABLE IF NOT EXISTS attempts (
        key TEXT PRIMARY KEY, window INTEGER NOT NULL, count INTEGER NOT NULL)`);
      this.ctx.storage.sql.exec('CREATE INDEX IF NOT EXISTS attempts_window_idx ON attempts(window)');
    });
  }
  attempt(key, now, max) {
    const window = Math.floor(now / 60);
    const sql = this.ctx.storage.sql;
    const rows = sql.exec('SELECT window, count FROM attempts WHERE key = ?', key).toArray();
    const row = rows[0];
    return { allowed: !row || row.window !== window || row.count < max, window };
  }
  fail(key, window) {
    this.ctx.storage.sql.exec('DELETE FROM attempts WHERE window < ?', window - 1);
    this.ctx.storage.sql.exec(`INSERT INTO attempts (key, window, count) VALUES (?, ?, 1)
      ON CONFLICT(key) DO UPDATE SET count = CASE WHEN window = excluded.window THEN count + 1 ELSE 1 END, window = excluded.window`, key, window);
  }
  authorize(ip, correct) {
    const now = Math.floor(Date.now() / 1000);
    const ipState = this.attempt('basic:' + ip, now, 5);
    const globalState = this.attempt('basic:global', now, 30);
    if (!ipState.allowed || !globalState.allowed) return false;
    if (!correct) { this.fail('basic:' + ip, ipState.window); this.fail('basic:global', globalState.window); }
    return correct;
  }
  async publish(data) {
    const sql = this.ctx.storage.sql;
    const previous = sql.exec('SELECT owner,expires_at FROM codes WHERE role=?', data.role).toArray()[0];
    if (previous && previous.owner !== data.owner && previous.expires_at > Math.floor(Date.now() / 1000) && data.takeover !== true) {
      return { error: 'owner_conflict' };
    }
    sql.exec(`INSERT INTO codes (role,id,owner,expires_at,code_hash,token,public_key,token_expires_at,consumed)
      VALUES (?,?,?,?,?,?,?,?,0) ON CONFLICT(role) DO UPDATE SET id=excluded.id, owner=excluded.owner,
      expires_at=excluded.expires_at, code_hash=excluded.code_hash, token=excluded.token,
      public_key=excluded.public_key, token_expires_at=excluded.token_expires_at, consumed=0`,
      data.role, data.id, data.owner, data.expires_at, data.code_hash, data.token, data.public_key, data.token_expires_at);
    // An alarm removes unused bearer material even if the browser goes away.
    const alarm = await this.ctx.storage.getAlarm();
    const due = (data.expires_at + 1) * 1000;
    if (!alarm || alarm > due) await this.ctx.storage.setAlarm(due);
    return { ok: true, id: data.id, expires_at: data.expires_at };
  }
  status() {
    const now = Math.floor(Date.now() / 1000);
    this.ctx.storage.sql.exec('UPDATE codes SET token=NULL, public_key=NULL WHERE expires_at <= ?', now);
    return this.ctx.storage.sql.exec('SELECT role,id,owner,expires_at,consumed FROM codes').toArray();
  }
  revoke(owner) {
    this.ctx.storage.sql.exec('DELETE FROM codes WHERE owner=?', owner);
    return { ok: true };
  }
  exchange(role, codeHash, ip) {
    const now = Math.floor(Date.now() / 1000);
    const ipState = this.attempt('device:' + ip, now, 5);
    const globalState = this.attempt('device:global', now, 30);
    if (!ipState.allowed || !globalState.allowed) return { error: 'rate_limited' };
    const sql = this.ctx.storage.sql;
    const row = sql.exec('SELECT token,public_key,token_expires_at FROM codes WHERE role=? AND code_hash=? AND expires_at>? AND token IS NOT NULL', role, codeHash, now).toArray()[0];
    if (!row) {
      this.fail('device:' + ip, ipState.window);
      this.fail('device:global', globalState.window);
      return { error: 'invalid_code' };
    }
    sql.exec('UPDATE codes SET token=NULL,public_key=NULL,consumed=1 WHERE role=?', role);
    return { token: row.token, public_key: row.public_key, expires_at: row.token_expires_at };
  }
  async alarm() {
    const now = Math.floor(Date.now() / 1000);
    const sql = this.ctx.storage.sql;
    sql.exec('UPDATE codes SET token=NULL, public_key=NULL WHERE expires_at <= ?', now);
    sql.exec('DELETE FROM attempts WHERE window < ?', Math.floor(now / 60) - 1);
    const next = sql.exec('SELECT MIN(expires_at) AS t FROM codes WHERE token IS NOT NULL').toArray()[0]?.t;
    if (next) await this.ctx.storage.setAlarm((next + 1) * 1000);
  }
}

export default {
  async fetch(request, env) {
    const url = new URL(request.url);
    if (url.protocol !== 'https:' && !['localhost', '127.0.0.1', '[::1]'].includes(url.hostname)) {
      url.protocol = 'https:';
      return new Response(null, { status: 308, headers: { ...commonHeaders, location: url.toString() } });
    }
    const ip = request.headers.get('cf-connecting-ip') || 'local';
    const stub = env.DEVICE_CODES.getByName('mrfps');
    if (url.pathname === '/api/device/exchange') {
      if (request.method !== 'POST') return json({ error: 'method' }, 405);
      let body;
      try { body = await bodyJson(request, 512); } catch { return json({ error: 'invalid_code' }, 400); }
      if (!roles.has(body.role) || typeof body.code !== 'string' || !/^\d{6}$/.test(body.code)) return json({ error: 'invalid_code' }, 400);
      const result = await stub.exchange(body.role, await digest(body.role + ':' + body.code), ip);
      return json(result, result.error === 'rate_limited' ? 429 : result.error ? 401 : 200,
        result.error === 'rate_limited' ? { 'retry-after': '60' } : {});
    }
    const correct = await credentialsValid(request, env);
    if (!await stub.authorize(ip, correct)) return json({ error: 'unauthorized' }, 401, { 'www-authenticate': 'Basic realm="MRFPS Admin", charset="UTF-8"' });
    if (request.method === 'GET' && url.pathname === '/') return textBody(panel, 'text/html; charset=utf-8');
    if (request.method === 'GET' && url.pathname === '/panel.js') return textBody(script, 'text/javascript; charset=utf-8');
    if (request.method === 'GET' && url.pathname === '/crypto.js') return textBody(cryptoScript, 'text/javascript; charset=utf-8');
    if (request.method === 'GET' && url.pathname === '/panel.css') return textBody(style, 'text/css; charset=utf-8');
    if (request.method === 'GET' && url.pathname === '/api/admin/status') return json({ codes: await stub.status() });
    if (request.method === 'POST' && url.pathname.startsWith('/api/admin/')) {
      const origin = request.headers.get('origin');
      if (origin && origin !== url.origin) return json({ error: 'origin' }, 403);
      if (url.pathname === '/api/admin/revoke') {
        let body;
        try { body = await bodyJson(request, 512); } catch { return json({ error: 'invalid' }, 400); }
        if (!/^[0-9a-f]{8}-[0-9a-f]{4}-4[0-9a-f]{3}-[89ab][0-9a-f]{3}-[0-9a-f]{12}$/.test(body.owner || '')) return json({ error: 'invalid' }, 400);
        return json(await stub.revoke(body.owner));
      }
      if (url.pathname === '/api/admin/publish') {
        try { const result = await stub.publish(await validatePublish(await bodyJson(request))); return json(result, result.error ? 409 : 200); }
        catch { return json({ error: 'invalid_publish' }, 400); }
      }
    }
    return json({ error: 'not_found' }, 404);
  }
};
