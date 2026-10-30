// Real Worker handler and Durable Object methods with disposable SQLite and
// a controlled clock. This does not contact a Worker or use credentials.
import test from 'node:test';
import assert from 'node:assert/strict';
import { readFileSync } from 'node:fs';
import { createHash } from 'node:crypto';
import { DatabaseSync } from 'node:sqlite';
import vm from 'node:vm';

let now=1_800_000_000_000; // exactly a UTC minute boundary
const base=now;
class FakeDate extends Date { static now() { return now; } }
class FakeDurableObject { constructor(ctx,env) { this.ctx=ctx; this.env=env; } }
const source=readFileSync(new URL('../src/worker.js',import.meta.url),'utf8')
  .replace(/^import .*;\r?\n/gm,'')
  .replace('export class DeviceCodes','class DeviceCodes')
  .replace('export default {','globalThis.worker = {');
const context=vm.createContext({DurableObject:FakeDurableObject,crypto,TextEncoder,TextDecoder,
  atob,btoa,Response,Request,URL,Date:FakeDate,panel:'',script:'',style:'',cryptoScript:''});
vm.runInContext(source+'\nglobalThis.TestDeviceCodes = DeviceCodes;',context);

function fixture(t) {
  now=base;
  const db=new DatabaseSync(':memory:'); t.after(()=>db.close());
  const storage={sql:{exec(sql,...args) {
    const statement=db.prepare(sql);
    const rows=/^\s*SELECT\b/i.test(sql)?statement.all(...args):(statement.run(...args),[]);
    return {toArray:()=>rows};
  }}};
  const object=new context.TestDeviceCodes({storage,blockConcurrencyWhile:fn=>fn()},{});
  const hash=createHash('sha256').update('inspector:123456').digest('hex');
  db.prepare('INSERT INTO codes VALUES (?,?,?,?,?,?,?,?,0)').run('inspector','fixture-id','fixture-owner',
    Math.floor(base/1000)+120,hash,'fixture-token','fixture-key',Math.floor(base/1000)+3600);
  const env={DEVICE_CODES:{getByName:()=>object}};
  async function exchange(code,ip='fixture-ip') {
    const request=new Request('https://fixture.invalid/api/device/exchange',{method:'POST',
      headers:{'content-type':'application/json','cf-connecting-ip':ip},body:JSON.stringify({role:'inspector',code})});
    return context.worker.fetch(request,env);
  }
  return {exchange,object,db,env};
}

test('per-IP Retry-After is the remaining window, then the same valid code works once',async t=>{
  const h=fixture(t);
  for(let i=0;i<5;i++) assert.equal((await h.exchange('000000')).status,401);
  for (const [offset,expected] of [[0,'60'],[30_000,'30'],[59_999,'1']]) {
    now=base+offset;
    const response=await h.exchange('123456');
    assert.equal(response.status,429);
    assert.equal(response.headers.get('retry-after'),expected);
    assert.deepEqual(await response.json(),{error:'rate_limited'});
  }
  assert.equal(h.db.prepare('SELECT count FROM attempts WHERE key=?').get('device:fixture-ip').count,5);
  now=base+60_000;
  assert.equal((await h.exchange('123456')).status,200);
  assert.equal((await h.exchange('123456')).status,401);
});

test('global exhaustion also returns remaining seconds and preserves the 30-attempt cap',async t=>{
  const h=fixture(t); now=base+52_000;
  for(let ip=0;ip<6;ip++) for(let i=0;i<5;i++)
    assert.equal((await h.exchange('000000','fixture-'+ip)).status,401);
  const response=await h.exchange('123456','new-ip');
  assert.equal(response.status,429); assert.equal(response.headers.get('retry-after'),'8');
  assert.equal(h.db.prepare('SELECT count FROM attempts WHERE key=?').get('device:global').count,30);
  now=base+60_000;
  assert.equal((await h.exchange('123456','new-ip')).status,200);
});

test('simultaneous redemption remains single-use and the same-IP budget stays bounded',async t=>{
  const h=fixture(t); now=base+59_000;
  const results=await Promise.all(Array.from({length:20},()=>h.exchange('123456')));
  assert.equal(results.filter(r=>r.status===200).length,1);
  assert.equal(results.filter(r=>r.status===401).length,5);
  const blocked=results.filter(r=>r.status===429);
  assert.equal(blocked.length,14);
  assert(blocked.every(r=>r.headers.get('retry-after')==='1'));
  assert.equal(h.db.prepare('SELECT count FROM attempts WHERE key=?').get('device:fixture-ip').count,5);
});

test('older Durable Object replies conservatively retain the previous header',async t=>{
  const h=fixture(t); h.env.DEVICE_CODES={getByName:()=>({exchange:()=>({error:'rate_limited'})})};
  const response=await h.exchange('123456');
  assert.equal(response.status,429); assert.equal(response.headers.get('retry-after'),'60');
  assert.deepEqual(await response.json(),{error:'rate_limited'});
});
