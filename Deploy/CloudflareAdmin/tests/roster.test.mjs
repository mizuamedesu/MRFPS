// Exercise the real Worker and Durable Object against disposable SQLite.
// All keys and participants below are fixtures, and no network is contacted.
import test from 'node:test';
import assert from 'node:assert/strict';
import { readFileSync } from 'node:fs';
import { mkdtemp, readFile, rm, writeFile } from 'node:fs/promises';
import { createHmac, timingSafeEqual, webcrypto } from 'node:crypto';
import { DatabaseSync } from 'node:sqlite';
import { execFileSync } from 'node:child_process';
import { tmpdir } from 'node:os';
import { join } from 'node:path';
import { fileURLToPath } from 'node:url';
import vm from 'node:vm';
import { encryptRosterKey, decryptRosterKey } from '../roster-backup.mjs';

let now=1_800_000_000_000;
const base=now, key='a'.repeat(64), a='a'.repeat(32), b='b'.repeat(32);
class FakeDate extends Date { static now() { return now; } }
class FakeDO { constructor(ctx,env) { this.ctx=ctx; this.env=env; } }
const subtle={digest:(...x)=>webcrypto.subtle.digest(...x),importKey:(...x)=>webcrypto.subtle.importKey(...x),
  verify:(...x)=>webcrypto.subtle.verify(...x),timingSafeEqual:(x,y)=>timingSafeEqual(Buffer.from(x),Buffer.from(y))};
const source=readFileSync(new URL('../src/worker.js',import.meta.url),'utf8')
  .replace(/^import .*;\r?\n/gm,'').replace('export class DeviceCodes','class DeviceCodes')
  .replace('export default {','globalThis.worker = {');
const context=vm.createContext({DurableObject:FakeDO,crypto:{subtle},TextEncoder,TextDecoder,atob,btoa,
  Response,Request,URL,Date:FakeDate,panel:'',script:'',style:'',cryptoScript:''});
vm.runInContext(source+'\nglobalThis.TestDeviceCodes=DeviceCodes;',context);

function fixture(t) {
  now=base;
  const db=new DatabaseSync(':memory:'); t.after(()=>db.close());
  const storage={transactionSync(fn) { db.exec('BEGIN'); try {const result=fn(); db.exec('COMMIT'); return result;} catch(error) {db.exec('ROLLBACK'); throw error;} },sql:{exec(sql,...args) {
    const statement=db.prepare(sql);
    const rows=/^\s*SELECT\b/i.test(sql)?statement.all(...args):(statement.run(...args),[]);
    return {toArray:()=>rows};
  }}};
  const object=new context.TestDeviceCodes({storage,blockConcurrencyWhile:fn=>fn()},{});
  const env={DEVICE_CODES:{getByName:()=>object},MRFPS_ROSTER_HMAC_KEY:key,
    MRFPS_ADMIN_USERNAME:'fixture-user',MRFPS_ADMIN_PASSWORD:'fixture-pass'};
  const row=(id,role,ping_ms=null,ping_at_ms=null)=>({id,name:`User ${id}`,role,ping_ms,ping_at_ms});
  const data=(instance=a,seq=1,participants=[])=>({instance,seq,captured_at_ms:now,participants});
  async function publish(body, signatureKey=key) {
    const text=JSON.stringify(body), sig=createHmac('sha256',signatureKey).update(text).digest('hex');
    return context.worker.fetch(new Request('https://fixture.invalid/api/server/roster',{method:'POST',
      headers:{'content-type':'application/json','x-mrfps-signature':sig},body:text}),env);
  }
  async function read(auth=true) {
    const headers=auth?{authorization:'Basic '+btoa('fixture-user:fixture-pass')}:{},
      response=await context.worker.fetch(new Request('https://fixture.invalid/api/admin/participants',{headers}),env);
    return {status:response.status,body:await response.json()};
  }
  return {row,data,publish,read,object,env,db};
}

test('read requires Basic and writes require a valid HMAC over the exact body',async t=>{
  const f=fixture(t);
  assert.equal((await f.read(false)).status,401);
  assert.deepEqual(await f.read(),{status:200,body:{online:false,participants:[]}});
  assert.equal((await f.publish(f.data(), 'b'.repeat(64))).status,401);
  assert.deepEqual(await f.read(),{status:200,body:{online:false,participants:[]}});
  assert.equal((await f.publish(f.data())).status,200);
  assert.equal((await f.read()).body.online,true);
});

test('signature shape and HMAC are checked before JSON parsing',async t=>{
  const f=fixture(t), malformed='{';
  const request=(signature,body=malformed,extra={})=>new Request('https://fixture.invalid/api/server/roster',{method:'POST',
    headers:{'content-type':'application/json',...(signature?{'x-mrfps-signature':signature}:{}),...extra},body});
  const valid=createHmac('sha256',key).update(malformed).digest('hex');
  assert.equal((await context.worker.fetch(request(''),f.env)).status,401);
  assert.equal((await context.worker.fetch(request('0'.repeat(64)),f.env)).status,401);
  assert.equal((await context.worker.fetch(request(valid),f.env)).status,400);
  assert.equal((await context.worker.fetch(request(valid,malformed,{'content-length':'524289'}),f.env)).status,400);
});

test('only measured fresh pings are displayed and stale server status expires',async t=>{
  const f=fixture(t);
  assert.equal((await f.publish(f.data(a,1,[f.row(1,'player',0,now),f.row(2,'inspector'),f.row(3,'admin',42,now),f.row(0,'bot')]))).status,200);
  let view=(await f.read()).body;
  assert.equal(view.participants[0].ping_ms,0); assert.equal(view.participants[1].ping_ms,null);
  assert.equal(view.participants[2].ping_ms,42);
  assert.equal(view.participants[3].role,'bot'); assert.equal(view.participants[3].ping_ms,null);
  now=base+5001; view=(await f.read()).body;
  assert.equal(view.online,true); assert(view.participants.every(p=>p.ping_ms===null));
  now=base+10001; assert.deepEqual((await f.read()).body,{online:false,participants:[]});
});

test('concurrent duplicate and reordered writes cannot roll back the roster',async t=>{
  const f=fixture(t), first=f.data(a,1,[f.row(1,'player')]);
  assert.equal((await f.publish(first)).status,200);
  assert.equal((await f.publish(first)).status,409);
  const second=f.data(a,2,[f.row(2,'inspector')]);
  const simultaneous=await Promise.all([f.publish(second),f.publish(second)]);
  assert.deepEqual(simultaneous.map(r=>r.status).sort(),[200,409]);
  assert.equal((await f.publish(first)).status,409);
  assert.equal((await f.read()).body.participants[0].id,2);
  assert.equal((await f.publish(f.data(b,1,[f.row(3,'admin')]))).status,409);
  now=base+10001;
  assert.equal((await f.publish(f.data(b,1,[f.row(3,'admin')]))).status,200);
  assert.equal((await f.publish(f.data(a,999,[f.row(1,'player')]))).status,409);
  assert.equal((await f.read()).body.participants[0].id,3);
  now=base+86_410_000;
  assert.equal((await f.publish(f.data(b,2,[f.row(3,'admin')]))).status,200);
  assert.equal((await f.publish(f.data(a,1000,[f.row(1,'player')]))).status,409);
});

test('publisher boot history is bounded without evicting permanent replay fences',async t=>{
  const f=fixture(t);
  assert.equal((await f.publish(f.data())).status,200);
  const insert=f.db.prepare('INSERT INTO roster_publishers(instance,last_seq,retired,last_seen_ms) VALUES(?,?,1,?)');
  f.db.exec('BEGIN');
  for (let i=0;i<4095;i++) insert.run(i.toString(16).padStart(32,'0'),1,base);
  f.db.exec('COMMIT');
  assert.equal((await f.publish(f.data(a,2))).status,200);
  now=base+10001;
  assert.deepEqual(await (await f.publish(f.data(b,1))).json(),{error:'publisher_limit'});
  assert.equal((await f.publish(f.data('0'.repeat(32),2))).status,409);
});

test('capture timestamps and malformed measurements fail closed',async t=>{
  const f=fixture(t), valid=f.data();
  for (const bad of [
    {...valid,captured_at_ms:now-10001}, {...valid,captured_at_ms:now+2001},
    {...valid,participants:[f.row(1,'player',1,now+1)]},
    {...valid,participants:[f.row(1,'player',-1,now)]},
    {...valid,participants:[f.row(1,'player'),f.row(1,'admin')]},
    {...valid,participants:[f.row(1,'unknown')]},
    {...valid,participants:[f.row(0,'bot',42,now)]},
    {...valid,participants:Array.from({length:4097},(_,id)=>f.row(id,'inspector'))},
  ]) assert.equal((await f.publish(bad)).status,400);
  const delayed={...valid,captured_at_ms:now-9999};
  assert.equal((await f.publish(delayed)).status,200);
  now+=2; assert.equal((await f.read()).body.online,false);
});

test('encrypted roster backup is authenticated, unique and refuses wrong passwords',()=>{
  const secret='ab'.repeat(32),passphrase='only-a-test-passphrase';
  const backup=encryptRosterKey(secret,passphrase), second=encryptRosterKey(secret,passphrase);
  assert.equal(decryptRosterKey(backup,passphrase),secret);
  assert(!JSON.stringify(backup).includes(secret)); assert.notEqual(backup.ciphertext,second.ciphertext);
  assert.throws(()=>decryptRosterKey(backup,'a-different-passphrase'));
  const changed={...backup,ciphertext:backup.ciphertext.slice(0,-4)+'AAAA'};
  assert.throws(()=>decryptRosterKey(changed,passphrase));
  assert.throws(()=>encryptRosterKey(secret,'too-short'));
});

test('private roster key creation is idempotent and never prints key material',async t=>{
  const folder=await mkdtemp(join(tmpdir(),'mrfps-roster-test-'));
  t.after(()=>rm(folder,{recursive:true,force:true}));
  const cli=fileURLToPath(new URL('../admin-cli.mjs',import.meta.url));
  const run=()=>execFileSync(process.execPath,[cli,'roster-key-ensure','--state-dir',folder],{encoding:'utf8',windowsHide:true});
  const first=run(), path=JSON.parse(first).key_file, keyBefore=await readFile(path,'utf8');
  const second=run(), keyAfter=await readFile(path,'utf8');
  assert.match(keyBefore,/^[a-f0-9]{64}$/); assert.equal(keyBefore,keyAfter);
  assert(!first.includes(keyBefore) && !second.includes(keyBefore));
  await writeFile(path,'bad-key');
  assert.throws(()=>run());
  assert.equal(await readFile(path,'utf8'),'bad-key');
});

test('backup and restore CLI are idempotent, encrypted and refuse a conflicting key',async t=>{
  const folder=await mkdtemp(join(tmpdir(),'mrfps-roster-backup-'));
  t.after(()=>rm(folder,{recursive:true,force:true}));
  const cli=fileURLToPath(new URL('../admin-cli.mjs',import.meta.url));
  const src=join(folder,'source'), dst=join(folder,'destination'), pass=join(folder,'pass.txt'), backup=join(folder,'roster.json');
  await writeFile(pass,'fixture-passphrase-123');
  const run=(command,dir)=>execFileSync(process.execPath,[cli,command,'--state-dir',dir,'--backup-file',backup,'--passphrase-file',pass],{encoding:'utf8',windowsHide:true,stdio:['ignore','pipe','pipe']});
  run('roster-key-ensure',src);
  const keyBefore=await readFile(join(src,'roster-hmac.key'),'utf8');
  const first=run('roster-backup',src), second=run('roster-backup',src);
  assert.equal(JSON.parse(first).reused,false); assert.equal(JSON.parse(second).reused,true);
  assert(!(await readFile(backup,'utf8')).includes(keyBefore));
  assert.equal(JSON.parse(run('roster-restore',dst)).reused,false);
  assert.equal(await readFile(join(dst,'roster-hmac.key'),'utf8'),keyBefore);
  assert.equal(JSON.parse(run('roster-restore',dst)).reused,true);
  await writeFile(join(dst,'roster-hmac.key'),'f'.repeat(64));
  assert.throws(()=>run('roster-restore',dst));
  assert.equal(await readFile(join(dst,'roster-hmac.key'),'utf8'),'f'.repeat(64));
});
