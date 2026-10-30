// Execute the actual DO methods against disposable in-memory SQLite. No
// production Worker, credentials, network service, or bearer is used.
import test from 'node:test';
import assert from 'node:assert/strict';
import { readFileSync } from 'node:fs';
import { DatabaseSync } from 'node:sqlite';
import vm from 'node:vm';
import { newCode } from '../src/crypto.browser.js';

const workerSource=readFileSync(new URL('../src/worker.js',import.meta.url),'utf8');
const relevant=workerSource.slice(workerSource.indexOf('const roles ='),workerSource.indexOf('export default'))
  .replace('export class DeviceCodes','class DeviceCodes');
let clock=Date.now();
class FakeDate extends Date { static now() { return clock; } }
class FakeDurableObject { constructor(ctx,env) { this.ctx=ctx; this.env=env; } }
const context=vm.createContext({DurableObject:FakeDurableObject,crypto,TextEncoder,TextDecoder,atob,btoa,Date:FakeDate});
vm.runInContext(relevant+'\nglobalThis.testing={DeviceCodes,validatePublish};',context);
const {DeviceCodes,validatePublish}=context.testing;

function harness(seed=null) {
  const db=new DatabaseSync(':memory:');
  if (seed) {
    db.exec(`CREATE TABLE codes (role TEXT PRIMARY KEY,id TEXT NOT NULL,owner TEXT NOT NULL,
      expires_at INTEGER NOT NULL,code_hash TEXT NOT NULL,token TEXT,public_key TEXT,
      token_expires_at INTEGER NOT NULL,consumed INTEGER NOT NULL DEFAULT 0)`);
    db.prepare('INSERT INTO codes VALUES (?,?,?,?,?,?,?,?,0)').run(seed.role,seed.id,seed.owner,seed.expires_at,
      seed.code_hash,seed.token,seed.public_key,seed.token_expires_at);
  }
  let alarm=null;
  const storage={sql:{exec(sql,...args) {
    const statement=db.prepare(sql);
    const rows=/^\s*SELECT\b/i.test(sql)?statement.all(...args):(statement.run(...args),[]);
    return {toArray:()=>rows};
  }},getAlarm:async()=>alarm,setAlarm:async value=>{alarm=value;}};
  const ctx={storage,blockConcurrencyWhile:fn=>fn()};
  return {object:new DeviceCodes(ctx,{}),close:()=>db.close(),db,storage};
}

async function keyPair() {
  const pair=await crypto.subtle.generateKey('Ed25519',true,['sign','verify']);
  const spki=Buffer.from(await crypto.subtle.exportKey('spki',pair.publicKey)).toString('base64');
  return {privateKey:pair.privateKey,publicKey:`-----BEGIN PUBLIC KEY-----\n${spki}\n-----END PUBLIC KEY-----\n`};
}
async function entry(role,owner,key) {
  const value=await newCode(role,owner,'a'.repeat(64),key);
  clock=Date.now();
  return {code:value.code,data:await validatePublish(value.body),body:value.body};
}

test('identical retries preserve active or consumed state; a code exchanges once',async t=>{
  const h=harness(); t.after(h.close); const owner=crypto.randomUUID(), key=await keyPair();
  h.object.start(owner); const item=await entry('player',owner,key);
  assert.equal((await h.object.publish(item.data)).ok,true);
  const retry=await h.object.publish(item.data);
  assert.equal(retry.reused,true); assert.equal(retry.consumed,false);
  const first=h.object.exchange('player',item.data.code_hash,'fixture-1');
  assert.equal(typeof first.token,'string');
  const consumed=await h.object.publish(item.data);
  assert.equal(consumed.reused,true); assert.equal(consumed.consumed,true);
  assert.equal(h.object.exchange('player',item.data.code_hash,'fixture-2').error,'invalid_code');
});

test('same ID with different content or role is rejected without changing the original',async t=>{
  const h=harness(); t.after(h.close); const owner=crypto.randomUUID(), key=await keyPair();
  h.object.start(owner); const item=await entry('player',owner,key);
  await h.object.publish(item.data);
  const different=await validatePublish({...item.body,code_hash:'f'.repeat(64)});
  assert.equal((await h.object.publish(different)).error,'issuance_id_conflict');
  const other=await entry('inspector',owner,key);
  const conflicting=await validatePublish({...other.body,id:item.body.id});
  assert.equal((await h.object.publish(conflicting)).error,'issuance_id_conflict');
  assert.equal(typeof h.object.exchange('player',item.data.code_hash,'fixture-3').token,'string');
});

test('superseded and consumed IDs never replace a newer role code',async t=>{
  const h=harness(); t.after(h.close); const owner=crypto.randomUUID(), key=await keyPair();
  h.object.start(owner); const old=await entry('player',owner,key), newer=await entry('player',owner,key);
  await h.object.publish(old.data); await h.object.publish(newer.data);
  assert.equal((await h.object.publish(old.data)).error,'issuance_retired');
  assert.equal(typeof h.object.exchange('player',newer.data.code_hash,'fixture-4').token,'string');
  const latest=await entry('player',owner,key); await h.object.publish(latest.data);
  assert.equal((await h.object.publish(newer.data)).consumed,true);
  assert.equal(typeof h.object.exchange('player',latest.data.code_hash,'fixture-5').token,'string');
});

test('takeover and stop retire every affected role, not just the changed role',async t=>{
  const h=harness(); t.after(h.close); const a=crypto.randomUUID(), b=crypto.randomUUID(), key=await keyPair();
  h.object.start(a); h.object.start(b);
  const first=await entry('player',a,key), inspector=await entry('inspector',a,key), replacement=await entry('player',b,key);
  await h.object.publish(first.data); await h.object.publish(inspector.data);
  replacement.data.takeover=true;
  assert.equal((await h.object.publish(replacement.data)).ok,true);
  assert.equal((await h.object.publish(first.data)).error,'owner_stopped');
  assert.equal((await h.object.publish(inspector.data)).error,'owner_stopped');
  assert.equal(h.object.exchange('inspector',inspector.data.code_hash,'fixture-6').error,'invalid_code');
  h.object.stopAll();
  assert.equal((await h.object.publish(replacement.data)).error,'owner_stopped');
});

test('existing pre-upgrade code still exchanges but cannot be republished',async t=>{
  const owner=crypto.randomUUID(), key=await keyPair(), legacy=await entry('player',owner,key);
  const h=harness(legacy.data); t.after(h.close); h.object.start(owner);
  assert.equal((await h.object.publish(legacy.data)).error,'legacy_issuance');
  assert.equal(typeof h.object.exchange('player',legacy.data.code_hash,'fixture-7').token,'string');
  assert.equal((await h.object.publish(legacy.data)).error,'legacy_issuance');
  assert.equal(h.object.exchange('player',legacy.data.code_hash,'fixture-8').error,'invalid_code');
});

test('concurrent exchange and repeated publish cannot return a second token',async t=>{
  const h=harness(); t.after(h.close); const owner=crypto.randomUUID(), key=await keyPair();
  h.object.start(owner); const item=await entry('player',owner,key); await h.object.publish(item.data);
  const results=await Promise.all(Array.from({length:16},(_,i)=>i%2
    ? h.object.publish(item.data)
    : Promise.resolve(h.object.exchange('player',item.data.code_hash,'fixture-'+i))));
  assert.equal(results.filter(result=>typeof result.token==='string').length,1);
  assert.equal(h.object.exchange('player',item.data.code_hash,'fixture-end').error,'invalid_code');
});

test('expired issuance cannot be republished or exchanged',async t=>{
  const h=harness(); t.after(h.close); const owner=crypto.randomUUID(), key=await keyPair();
  h.object.start(owner); const item=await entry('player',owner,key); await h.object.publish(item.data);
  const prior=clock; t.after(()=>{clock=prior;}); clock+=31000;
  assert.equal((await h.object.publish(item.data)).error,'issuance_expired');
  assert.equal(h.object.exchange('player',item.data.code_hash,'fixture-expired').error,'invalid_code');
});

test('a failed alarm schedule is repaired on the same ID without restoring a consumed code',async t=>{
  const h=harness(); t.after(h.close); const owner=crypto.randomUUID(), key=await keyPair();
  h.object.start(owner); const item=await entry('player',owner,key);
  const setAlarm=h.storage.setAlarm; let calls=0;
  h.storage.setAlarm=async value=>{calls++; if(calls===1) throw Error('fixture alarm failure'); await setAlarm(value);};
  // This fixture conservatively models SQL already persisted despite the
  // schedule failure; Cloudflare may instead roll the batch back atomically.
  await assert.rejects(h.object.publish(item.data),/fixture alarm failure/);
  const retried=await h.object.publish(item.data);
  assert.equal(retried.reused,true); assert.equal(retried.consumed,false); assert.equal(calls,2);
  assert((await h.storage.getAlarm())>0);
  assert.equal(typeof h.object.exchange('player',item.data.code_hash,'fixture-alarm-1').token,'string');
  assert.equal((await h.object.publish(item.data)).consumed,true);
  assert.equal(h.object.exchange('player',item.data.code_hash,'fixture-alarm-2').error,'invalid_code');
});

test('clock-ahead issuer is accepted but server expiry stays within thirty seconds and retries retain identity',async t=>{
  const h=harness(); t.after(h.close); const owner=crypto.randomUUID(), key=await keyPair();
  const originalClock=clock; t.after(()=>{clock=originalClock;}); h.object.start(owner);
  const item=await newCode('inspector',owner,'a'.repeat(64),key);
  const claims=JSON.parse(Buffer.from(item.body.token.split('.')[1],'base64url'));
  clock=(claims.iat-1)*1000;
  const validated=await validatePublish(item.body);
  const first=await h.object.publish(validated);
  assert.equal(first.ok,true);
  assert.equal(first.expires_at,first.server_now+30);
  assert.equal(first.expires_at,item.body.expires_at-1);
  clock+=1000;
  const retry=await h.object.publish(await validatePublish(item.body));
  assert.equal(retry.reused,true); assert.equal(retry.expires_at,first.expires_at);
  clock=first.expires_at*1000;
  assert.equal(h.object.exchange('inspector',item.body.code_hash,'fixture-clock').error,'invalid_code');
});

test('clock tolerance never permits a code longer than the signed thirty-second window',async t=>{
  const h=harness(); t.after(h.close); const owner=crypto.randomUUID(), key=await keyPair();
  const originalClock=clock; t.after(()=>{clock=originalClock;}); h.object.start(owner);
  const item=await newCode('inspector',owner,'a'.repeat(64),key);
  const claims=JSON.parse(Buffer.from(item.body.token.split('.')[1],'base64url'));
  clock=claims.iat*1000;
  await assert.rejects(validatePublish({...item.body,expires_at:item.body.expires_at+1}),/claims/);
  clock=(claims.iat-6)*1000;
  await assert.rejects(validatePublish(item.body),/expired/);
});