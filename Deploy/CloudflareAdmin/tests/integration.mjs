// Run against a disposable local `wrangler dev`; uses test-only credentials.
import assert from 'node:assert/strict';
import { createPublicKey, verify } from 'node:crypto';
import { generateBackup, newCode } from '../src/crypto.browser.js';
const url = process.env.MRFPS_ADMIN_TEST_URL || 'http://127.0.0.1:18787';
const auth = 'Basic ' + Buffer.from('fixture-user:fixture-password').toString('base64');
async function req(path, body, options={}) {
  const headers = { ...(body ? {'content-type':'application/json'} : {}), ...(options.auth ? {authorization:auth} : {}), ...(options.headers || {}) };
  return fetch(url + path, {method:body ? 'POST':'GET',headers,body:body ? JSON.stringify(body):undefined});
}
async function publish(role, opts = {}) {
  const entry = await newCode(role, owner, fingerprint, key);
  if (opts.expires_at) entry.body.expires_at = opts.expires_at;
  const result = await req('/api/admin/publish', entry.body, {auth:true, headers: opts.headers});
  assert.equal(result.status, opts.expected || 200, 'publish');
  return entry;
}
const key = await generateBackup('integration-test-passphrase');
const owner = crypto.randomUUID(), fingerprint = 'a'.repeat(64);
assert.equal((await req('/')).status, 401);
assert.equal((await req('/',null,{auth:true})).status, 200);
assert.equal((await req('/api/admin/status')).status, 401);
assert.equal((await req('/api/admin/owner/start',{owner},{auth:true})).status,200);
await publish('player', {headers:{origin:'https://attacker.invalid'},expected:403});
const forged = await newCode('admin', owner, fingerprint, key);
forged.body.token = forged.body.token.slice(0, -4) + 'AAAA';
assert.equal((await req('/api/admin/publish',forged.body,{auth:true})).status,400,'forged token accepted');
const player = await publish('player');
const stranger = crypto.randomUUID();
assert.equal((await req('/api/admin/owner/start',{owner:stranger},{auth:true})).status,200);
const foreign = await newCode('player', stranger, fingerprint, key);
assert.equal((await req('/api/admin/publish',foreign.body,{auth:true})).status,409,'unintended issuer takeover');
assert.equal((await req('/api/admin/revoke',{owner:stranger},{auth:true})).status,200);
assert.equal((await req('/api/admin/owner/start',{owner:stranger},{auth:true})).status,409,'stopped owner restarted');
assert.equal((await req('/api/admin/publish',foreign.body,{auth:true})).status,409,'stopped owner published');
assert.equal((await (await req('/api/admin/status',null,{auth:true})).json()).codes.find(x=>x.role==='player').owner,owner,'foreign stop changed owner');
const short = await publish('inspector', {expires_at:Math.floor(Date.now()/1000)+2});
await new Promise(resolve => setTimeout(resolve,2500));
assert.equal((await req('/api/device/exchange',{role:'inspector',code:short.code})).status,401,'expired code accepted');
const exchange = () => req('/api/device/exchange', {role:'player',code:player.code});
const races = await Promise.all(Array.from({length:6}, exchange));
assert.equal(races.filter(r => r.status === 200).length, 1, 'one-use concurrent exchange');
const result = await races.find(r => r.status === 200).json();
const [header, payload, signature] = result.token.split('.');
assert(verify(null, Buffer.from(header+'.'+payload), createPublicKey(result.public_key), Buffer.from(signature,'base64url')));
assert.equal(JSON.parse(Buffer.from(payload,'base64url')).role, 'player');
// Repeating the identical publish must not restore a consumed bearer.
const repeated=await req('/api/admin/publish',player.body,{auth:true});
assert.equal(repeated.status,200);
assert.equal((await repeated.json()).consumed,true,'retry did not report prior consumption');
const freshIp={'cf-connecting-ip':'192.0.2.80'};
assert.equal((await req('/api/device/exchange',{role:'player',code:player.code},{headers:freshIp})).status,401,
  'a republished consumed code was exchanged twice');
const changed={...player.body,code_hash:'f'.repeat(64)};
const conflict=await req('/api/admin/publish',changed,{auth:true});
assert.equal(conflict.status,409);
assert.equal((await conflict.json()).error,'issuance_id_conflict');
// A delayed retry for an unconsumed but superseded ID must not overwrite the
// newer code. Both cases run on the actual local Durable Object runtime.
const olderAdmin=await publish('admin');
const newerAdmin=await publish('admin');
const stale=await req('/api/admin/publish',olderAdmin.body,{auth:true});
assert.equal(stale.status,409);
assert.equal((await stale.json()).error,'issuance_retired');
assert.equal((await req('/api/device/exchange',{role:'admin',code:newerAdmin.code},{headers:freshIp})).status,200,
  'a stale retry replaced the newer code');
// The failed concurrent redemptions exhaust this IP's rate limit.
assert.equal((await req('/api/device/exchange', {role:'player',code:'000000'})).status,429);
// Admin revoke remains idempotent and removes every published code.
assert.equal((await req('/api/admin/revoke',{owner}, {auth:true})).status,200);
assert.equal((await req('/api/admin/revoke',{owner}, {auth:true})).status,200);
assert.deepEqual((await (await req('/api/admin/status',null,{auth:true})).json()).codes, []);
const raceOwner=crypto.randomUUID();
assert.equal((await req('/api/admin/owner/start',{owner:raceOwner},{auth:true})).status,200);
const raced=await newCode('player',raceOwner,fingerprint,key);
const race=await Promise.all([req('/api/admin/publish',raced.body,{auth:true}),req('/api/admin/stop-all',{}, {auth:true})]);
assert([200,409].includes(race[0].status));
assert.equal(race[1].status,200);
assert.equal((await req('/api/admin/publish',raced.body,{auth:true})).status,409,'stopped racing owner resurrected');
assert.equal((await (await req('/api/admin/status?owner='+raceOwner,null,{auth:true})).json()).owner_state,'stopped');
assert.equal((await req('/api/admin/owner/start',{owner:raceOwner},{auth:true})).status,409,'stopped racing owner restarted');
const restarted=crypto.randomUUID();
assert.equal((await req('/api/admin/owner/start',{owner:restarted},{auth:true})).status,200,'explicit new owner');
const fresh=await newCode('player',restarted,fingerprint,key);
assert.equal((await req('/api/admin/publish',fresh.body,{auth:true})).status,200,'explicit restart did not publish');
assert.equal((await req('/api/admin/revoke',{owner:restarted},{auth:true})).status,200);
const otherIp = {'cf-connecting-ip':'192.0.2.77'};
for (let i=0; i<8; ++i) assert.equal((await req('/',null,{headers:otherIp})).status,401);
assert.equal((await req('/',null,{auth:true,headers:otherIp})).status,200,'normal Basic challenges must not lock out a valid user');
const badAuth={...otherIp,authorization:'Basic '+Buffer.from('fixture-user:wrong').toString('base64')};
for (let i=0; i<5; ++i) assert.equal((await req('/',null,{headers:badAuth})).status,401);
assert.equal((await req('/',null,{auth:true,headers:otherIp})).status,401,'basic brute-force lockout');
console.log('PASS: auth, CSRF, forgery, expiry, ownership, single-use, limits, concurrent stop/publish and explicit restart');
