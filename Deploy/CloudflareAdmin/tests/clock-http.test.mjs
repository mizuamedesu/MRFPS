// Exercise the Worker HTTP route and actual DO against disposable SQLite.
import test from 'node:test';
import assert from 'node:assert/strict';
import { readFileSync } from 'node:fs';
import { DatabaseSync } from 'node:sqlite';
import { timingSafeEqual } from 'node:crypto';
import vm from 'node:vm';
import { newCode } from '../src/crypto.browser.js';

let source=readFileSync(new URL('../src/worker.js',import.meta.url),'utf8')
  .replace(/^import .*;\s*$/gm,'').replace('export class DeviceCodes','class DeviceCodes')
  .replace('export default {','const handler = {');
let clock=Date.now();
class FakeDate extends Date { static now() { return clock; } }
class FakeDurableObject { constructor(ctx,env) { this.ctx=ctx; this.env=env; } }
const subtle={digest:(...a)=>crypto.subtle.digest(...a),importKey:(...a)=>crypto.subtle.importKey(...a),
              verify:(...a)=>crypto.subtle.verify(...a),timingSafeEqual:(a,b)=>timingSafeEqual(Buffer.from(a),Buffer.from(b))};
const context=vm.createContext({DurableObject:FakeDurableObject,crypto:{subtle},TextEncoder,TextDecoder,atob,btoa,Date:FakeDate,Response,URL});
vm.runInContext(source+'\nglobalThis.testing={DeviceCodes,handler};',context);
const {DeviceCodes,handler}=context.testing;

function harness() {
  const db=new DatabaseSync(':memory:');let alarm=null;
  const storage={sql:{exec(sql,...args){const stmt=db.prepare(sql);const rows=/^\s*SELECT\b/i.test(sql)?stmt.all(...args):(stmt.run(...args),[]);return {toArray:()=>rows};}},
                 getAlarm:async()=>alarm,setAlarm:async value=>{alarm=value;}};
  const object=new DeviceCodes({storage,blockConcurrencyWhile:fn=>fn()},{});
  const env={DEVICE_CODES:{getByName:()=>object},MRFPS_ADMIN_USERNAME:'fixture',MRFPS_ADMIN_PASSWORD:'fixture-password'};
  return {object,env,close:()=>db.close()};
}

async function fixture(t, requestedLifetime=300) {
  const original=clock; t.after(()=>{clock=original;}); const h=harness(); t.after(h.close);
  const owner=crypto.randomUUID(); h.object.start(owner);
  const pair=await crypto.subtle.generateKey('Ed25519',true,['sign','verify']);
  const spki=Buffer.from(await crypto.subtle.exportKey('spki',pair.publicKey)).toString('base64');
  const key={privateKey:pair.privateKey,publicKey:`-----BEGIN PUBLIC KEY-----\n${spki}\n-----END PUBLIC KEY-----\n`};
  const entry=await newCode('inspector',owner,'a'.repeat(64),key);
  const claims=JSON.parse(Buffer.from(entry.body.token.split('.')[1],'base64url'));
  if (requestedLifetime!==300) entry.body.expires_at=claims.iat+requestedLifetime;
  clock=(claims.iat-1)*1000;
  const headers={'content-type':'application/json',authorization:'Basic '+Buffer.from('fixture:fixture-password').toString('base64')};
  const call=(path,body,auth=true)=>handler.fetch(new Request('https://fixture.invalid'+path,{method:'POST',headers:auth?headers:{'content-type':'application/json'},body:JSON.stringify(body)}),h.env);
  const accepted=await call('/api/admin/publish',entry.body);
  assert.equal(accepted.status,200);
  const result=await accepted.json();
  assert.equal(result.expires_at,requestedLifetime===300 ? result.server_now+300 : entry.body.expires_at);
  return {call,entry,result,env:h.env};
}

test('the complete HTTP exchange still accepts the code just before 300 seconds, once only',async t=>{
  const {call,entry,result}=await fixture(t);
  clock=(result.expires_at-1)*1000;
  assert.equal((await call('/api/device/exchange',{role:'inspector',code:entry.code},false)).status,200);
  assert.equal((await call('/api/device/exchange',{role:'inspector',code:entry.code},false)).status,401);
});

test('the complete HTTP exchange rejects the code at the 300-second expiry',async t=>{
  const {call,entry,result}=await fixture(t);
  clock=result.expires_at*1000;
  assert.equal((await call('/api/device/exchange',{role:'inspector',code:entry.code},false)).status,401);
});

test('an old shorter-lived issuer keeps its original expiry',async t=>{
  const {call,entry,result}=await fixture(t,30);
  assert.equal(result.expires_at-result.server_now,31);
  clock=result.expires_at*1000;
  assert.equal((await call('/api/device/exchange',{role:'inspector',code:entry.code},false)).status,401);
});

test('authenticated HTTP status reports the executing DO lifetime without bearer material',async t=>{
  const {entry,env}=await fixture(t);
  const auth='Basic '+Buffer.from('fixture:fixture-password').toString('base64');
  const response=await handler.fetch(new Request('https://fixture.invalid/api/admin/status',{headers:{authorization:auth}}),env);
  assert.equal(response.status,200);
  const body=await response.json();
  assert.equal(body.code_lifetime_seconds,300);
  assert.equal(body.codes.length,1);
  assert.deepEqual(Object.keys(body.codes[0]).sort(),['consumed','expires_at','id','owner','role']);
  assert.equal(body.codes[0].id,entry.body.id);
  assert.equal((await handler.fetch(new Request('https://fixture.invalid/api/admin/status'),env)).status,401);
});

test('status from an older DO is forwarded without inventing a lifetime',async()=>{
  const old={authorize:()=>true,status:()=>({codes:[]})};
  const env={DEVICE_CODES:{getByName:()=>old},MRFPS_ADMIN_USERNAME:'fixture',MRFPS_ADMIN_PASSWORD:'fixture-password'};
  const auth='Basic '+Buffer.from('fixture:fixture-password').toString('base64');
  const response=await handler.fetch(new Request('https://fixture.invalid/api/admin/status',{headers:{authorization:auth}}),env);
  assert.equal(response.status,200);
  const body=await response.json();
  assert.deepEqual(body.codes,[]);
  assert.equal(Object.hasOwn(body,'code_lifetime_seconds'),false);
});
