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
      this.ctx.storage.sql.exec('CREATE TABLE IF NOT EXISTS issuers (owner TEXT PRIMARY KEY, stopped INTEGER NOT NULL DEFAULT 0)');
      this.ctx.storage.sql.exec(`CREATE TABLE IF NOT EXISTS issuance_receipts (
        id TEXT PRIMARY KEY, role TEXT NOT NULL, owner TEXT NOT NULL,
        expires_at INTEGER NOT NULL, request_hash TEXT, state TEXT NOT NULL)`);
      this.ctx.storage.sql.exec('CREATE INDEX IF NOT EXISTS issuance_receipts_expiry_idx ON issuance_receipts(expires_at)');
      // Codes created by the previous revision have no request fingerprint.
      // Preserve their exchange until expiry, but never republish their IDs.
      this.ctx.storage.sql.exec(`INSERT OR IGNORE INTO issuance_receipts (id,role,owner,expires_at,request_hash,state)
        SELECT id,role,owner,expires_at,NULL,CASE WHEN consumed=1 THEN 'consumed' ELSE 'legacy' END
        FROM codes WHERE expires_at>?`, Math.floor(Date.now() / 1000));
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
    // Hash the entire immutable issuance in the DO, not a client-supplied hash.
    // Do this before touching storage so all subsequent checks/writes are one
    // uninterrupted SQLite transaction under the DO input/output gates.
    const requestHash = await digest(JSON.stringify([data.id,data.role,data.owner,data.code_hash,
      data.expires_at,data.token,data.public_key,data.token_expires_at,data.takeover === true]));
    // Read before the synchronous checks/writes. The subsequent setAlarm call
    // is queued without an intervening await, so it coalesces with the SQL
    // writes rather than leaving a persisted token with no expiration alarm.
    const existingAlarm = await this.ctx.storage.getAlarm();
    const sql = this.ctx.storage.sql;
    const now = Math.floor(Date.now() / 1000);
    sql.exec('DELETE FROM issuance_receipts WHERE expires_at <= ?',now);
    if (data.expires_at <= now) return { error:'issuance_expired' };
    const issuer = sql.exec('SELECT stopped FROM issuers WHERE owner=?',data.owner).toArray()[0];
    if (!issuer || issuer.stopped) return { error: 'owner_stopped' };
    const receipt = sql.exec('SELECT request_hash,state,expires_at FROM issuance_receipts WHERE id=?',data.id).toArray()[0];
    if (receipt) {
      if (!receipt.request_hash) return { error: 'legacy_issuance' };
      if (receipt.request_hash !== requestHash) return { error: 'issuance_id_conflict' };
      if (receipt.state === 'consumed') return { ok:true, id:data.id, expires_at:receipt.expires_at, reused:true, consumed:true };
      if (receipt.state !== 'active') return { error: 'issuance_retired' };
      const current = sql.exec('SELECT id,owner,expires_at,consumed,token FROM codes WHERE role=?',data.role).toArray()[0];
      if (!current || current.id !== data.id || current.owner !== data.owner || current.expires_at <= now) {
        sql.exec("UPDATE issuance_receipts SET state='retired' WHERE id=?",data.id);
        return { error: 'issuance_retired' };
      }
      if (current.consumed || !current.token) {
        sql.exec("UPDATE issuance_receipts SET state='consumed' WHERE id=?",data.id);
        return { ok:true, id:data.id, expires_at:receipt.expires_at, reused:true, consumed:true };
      }
      // If a prior alarm write failed after the row became visible, a retry
      // repairs the schedule without replacing or resurrecting the bearer.
      const due=(receipt.expires_at+1)*1000;
      if (!existingAlarm || existingAlarm > due) await this.ctx.storage.setAlarm(due);
      return { ok:true, id:data.id, expires_at:receipt.expires_at, reused:true, consumed:false };
    }
    const previous = sql.exec('SELECT id,owner,expires_at FROM codes WHERE role=?', data.role).toArray()[0];
    if (previous && previous.id === data.id) {
      // Fail closed if a pre-migration code appears without a receipt.
      sql.exec("INSERT OR IGNORE INTO issuance_receipts (id,role,owner,expires_at,request_hash,state) VALUES (?,?,?,?,NULL,'legacy')",
        previous.id,data.role,previous.owner,previous.expires_at);
      return { error:'legacy_issuance' };
    }
    if (previous && previous.owner !== data.owner) {
      const live = sql.exec('SELECT COUNT(*) AS count FROM codes WHERE owner=? AND expires_at>?',previous.owner,now).toArray()[0].count;
      if (live && data.takeover !== true) return { error: 'owner_conflict' };
      // A takeover retires the previous issuer completely, so a sleeping tab
      // or watch process cannot revive it after its displayed code expires.
      // Older deployments have codes but no issuer row. A takeover must
      // tombstone those owners too, not let an old tab register afterwards.
      sql.exec('INSERT INTO issuers (owner,stopped) VALUES (?,1) ON CONFLICT(owner) DO UPDATE SET stopped=1',previous.owner);
      sql.exec("UPDATE issuance_receipts SET state='retired' WHERE owner=? AND state IN ('active','legacy')",previous.owner);
      sql.exec('DELETE FROM codes WHERE owner=?',previous.owner);
    }
    if (previous) sql.exec("UPDATE issuance_receipts SET state='retired' WHERE id=? AND state IN ('active','legacy')",previous.id);
    sql.exec("INSERT INTO issuance_receipts (id,role,owner,expires_at,request_hash,state) VALUES (?,?,?,?,?,'active')",
      data.id,data.role,data.owner,data.expires_at,requestHash);
    sql.exec(`INSERT INTO codes (role,id,owner,expires_at,code_hash,token,public_key,token_expires_at,consumed)
      VALUES (?,?,?,?,?,?,?,?,0) ON CONFLICT(role) DO UPDATE SET id=excluded.id, owner=excluded.owner,
      expires_at=excluded.expires_at, code_hash=excluded.code_hash, token=excluded.token,
      public_key=excluded.public_key, token_expires_at=excluded.token_expires_at, consumed=0`,
      data.role, data.id, data.owner, data.expires_at, data.code_hash, data.token, data.public_key, data.token_expires_at);
    // An alarm removes unused bearer material even if the browser goes away.
    const due = (data.expires_at + 1) * 1000;
    if (!existingAlarm || existingAlarm > due) await this.ctx.storage.setAlarm(due);
    return { ok: true, id: data.id, expires_at: data.expires_at };
  }
  start(owner) {
    const sql=this.ctx.storage.sql;
    const existing=sql.exec('SELECT stopped FROM issuers WHERE owner=?',owner).toArray()[0];
    if(existing?.stopped) return {error:'owner_stopped'};
    if(!existing) sql.exec('INSERT INTO issuers (owner,stopped) VALUES (?,0)',owner);
    return {ok:true};
  }
  status(owner) {
    const now = Math.floor(Date.now() / 1000);
    const sql=this.ctx.storage.sql;
    sql.exec('UPDATE codes SET token=NULL, public_key=NULL WHERE expires_at <= ?', now);
    sql.exec('DELETE FROM issuance_receipts WHERE expires_at <= ?',now);
    const result={codes:sql.exec('SELECT role,id,owner,expires_at,consumed FROM codes').toArray()};
    if(owner) {
      const row=sql.exec('SELECT stopped FROM issuers WHERE owner=?',owner).toArray()[0];
      result.owner_state=!row?'unknown':row.stopped?'stopped':'active';
    }
    return result;
  }
  revoke(owner) {
    const sql=this.ctx.storage.sql;
    sql.exec('INSERT INTO issuers (owner,stopped) VALUES (?,1) ON CONFLICT(owner) DO UPDATE SET stopped=1',owner);
    sql.exec("UPDATE issuance_receipts SET state='retired' WHERE owner=? AND state IN ('active','legacy')",owner);
    sql.exec('DELETE FROM codes WHERE owner=?', owner);
    return { ok: true };
  }
  stopAll() {
    const sql=this.ctx.storage.sql;
    // Import pre-migration owners before revoking. Otherwise a previously
    // running tab could register after its old code was removed and revive.
    sql.exec('INSERT OR IGNORE INTO issuers (owner,stopped) SELECT DISTINCT owner,1 FROM codes');
    sql.exec('UPDATE issuers SET stopped=1 WHERE stopped=0');
    sql.exec("UPDATE issuance_receipts SET state='retired' WHERE state IN ('active','legacy')");
    sql.exec('DELETE FROM codes');
    return {ok:true};
  }
  exchange(role, codeHash, ip) {
    const now = Math.floor(Date.now() / 1000);
    const ipState = this.attempt('device:' + ip, now, 5);
    const globalState = this.attempt('device:global', now, 30);
    if (!ipState.allowed || !globalState.allowed) return { error: 'rate_limited' };
    const sql = this.ctx.storage.sql;
    const row = sql.exec('SELECT id,token,public_key,token_expires_at FROM codes WHERE role=? AND code_hash=? AND expires_at>? AND token IS NOT NULL', role, codeHash, now).toArray()[0];
    if (!row) {
      this.fail('device:' + ip, ipState.window);
      this.fail('device:global', globalState.window);
      return { error: 'invalid_code' };
    }
    sql.exec('UPDATE codes SET token=NULL,public_key=NULL,consumed=1 WHERE role=?', role);
    sql.exec("UPDATE issuance_receipts SET state='consumed' WHERE id=? AND state IN ('active','legacy')",row.id);
    return { token: row.token, public_key: row.public_key, expires_at: row.token_expires_at };
  }
  async alarm() {
    const now = Math.floor(Date.now() / 1000);
    const sql = this.ctx.storage.sql;
    sql.exec('UPDATE codes SET token=NULL, public_key=NULL WHERE expires_at <= ?', now);
    sql.exec('DELETE FROM attempts WHERE window < ?', Math.floor(now / 60) - 1);
    sql.exec('DELETE FROM issuance_receipts WHERE expires_at <= ?',now);
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
    // An initial unauthenticated browser request is a normal Basic challenge,
    // not a password attempt. Counting it locks out valid users after a few
    // fresh browser sessions; attempts carrying Authorization remain limited.
    if (!request.headers.has('authorization')) return json({error:'unauthorized'},401,{'www-authenticate':'Basic realm="MRFPS Admin", charset="UTF-8"'});
    const correct = await credentialsValid(request, env);
    if (!await stub.authorize(ip, correct)) return json({ error: 'unauthorized' }, 401, { 'www-authenticate': 'Basic realm="MRFPS Admin", charset="UTF-8"' });
    if (request.method === 'GET' && url.pathname === '/') return textBody(panel, 'text/html; charset=utf-8');
    if (request.method === 'GET' && url.pathname === '/panel.js') return textBody(script, 'text/javascript; charset=utf-8');
    if (request.method === 'GET' && url.pathname === '/crypto.js') return textBody(cryptoScript, 'text/javascript; charset=utf-8');
    if (request.method === 'GET' && url.pathname === '/panel.css') return textBody(style, 'text/css; charset=utf-8');
    if (request.method === 'GET' && url.pathname === '/api/admin/status') {
      const owner=url.searchParams.get('owner') || '';
      if(owner && !/^[0-9a-f]{8}-[0-9a-f]{4}-4[0-9a-f]{3}-[89ab][0-9a-f]{3}-[0-9a-f]{12}$/.test(owner)) return json({error:'invalid'},400);
      return json(await stub.status(owner));
    }
    if (request.method === 'POST' && url.pathname.startsWith('/api/admin/')) {
      const origin = request.headers.get('origin');
      if (origin && origin !== url.origin) return json({ error: 'origin' }, 403);
      if (url.pathname === '/api/admin/revoke' || url.pathname === '/api/admin/owner/start') {
        let body;
        try { body = await bodyJson(request, 512); } catch { return json({ error: 'invalid' }, 400); }
        if (!/^[0-9a-f]{8}-[0-9a-f]{4}-4[0-9a-f]{3}-[89ab][0-9a-f]{3}-[0-9a-f]{12}$/.test(body.owner || '')) return json({ error: 'invalid' }, 400);
        const result=url.pathname.endsWith('/start')?await stub.start(body.owner):await stub.revoke(body.owner);
        return json(result,result.error?409:200);
      }
      if (url.pathname === '/api/admin/stop-all') {
        try { await bodyJson(request,512); } catch { return json({error:'invalid'},400); }
        return json(await stub.stopAll());
      }
      if (url.pathname === '/api/admin/publish') {
        try { const result = await stub.publish(await validatePublish(await bodyJson(request))); return json(result, result.error ? 409 : 200); }
        catch { return json({ error: 'invalid_publish' }, 400); }
      }
    }
    return json({ error: 'not_found' }, 404);
  }
};
