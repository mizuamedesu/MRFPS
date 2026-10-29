// Exercise a real upgrade from the previous Worker against disposable local DO
// storage. Requires git history containing the previous released source.
import assert from 'node:assert/strict';
import { generateBackup, newCode } from '../src/crypto.browser.js';
import { execFileSync, spawn } from 'node:child_process';
import { createServer } from 'node:net';
import { mkdtemp, mkdir, readFile, writeFile, copyFile, rm } from 'node:fs/promises';
import { dirname, join, resolve } from 'node:path';
import { tmpdir } from 'node:os';

const root=resolve(tmpdir()), folder=await mkdtemp(join(root,'mrfps-worker-upgrade-'));
if(dirname(resolve(folder))!==root) throw Error('unsafe temporary directory');
const src=join(folder,'src'); await mkdir(src);
for(const name of ['panel.html','panel.browser.js','panel.css','crypto.browser.js']) await copyFile(join('src',name),join(src,name));
const old=execFileSync('git',['show','d661d269b7ae1095cccfb63faa1ccc25067d8da4:Deploy/CloudflareAdmin/src/worker.js'],{encoding:'utf8'});
const current=await readFile('src/worker.js','utf8');
assert(!old.includes('CREATE TABLE IF NOT EXISTS issuers'),'fixture is not the legacy Worker');
await writeFile(join(src,'worker.js'),old);
await writeFile(join(folder,'wrangler.jsonc'),JSON.stringify({name:'mrfps-upgrade-fixture',main:'src/worker.js',compatibility_date:'2026-10-23',
  rules:[{type:'Text',globs:['**/*.html','**/*.css','**/*.browser.js'],fallthrough:true}],
  durable_objects:{bindings:[{name:'DEVICE_CODES',class_name:'DeviceCodes'}]},migrations:[{tag:'v1',new_sqlite_classes:['DeviceCodes']}]}));
await writeFile(join(folder,'.dev.vars'),'MRFPS_ADMIN_USERNAME=fixture-user\nMRFPS_ADMIN_PASSWORD=fixture-password\n');
const socket=createServer(); await new Promise(done=>socket.listen(0,'127.0.0.1',done));
const port=socket.address().port; await new Promise(done=>socket.close(done));
const url=`http://127.0.0.1:${port}`, auth='Basic '+Buffer.from('fixture-user:fixture-password').toString('base64');
const bin=resolve('node_modules/wrangler/bin/wrangler.js');
const server=spawn(process.execPath,[bin,'dev','--local','--config',join(folder,'wrangler.jsonc'),'--persist-to',join(folder,'state'),
  '--ip','127.0.0.1','--port',String(port)],{cwd:folder,windowsHide:true,stdio:'ignore'});
const req=(path,body)=>fetch(url+path,{method:body?'POST':'GET',headers:{authorization:auth,...(body?{'content-type':'application/json'}:{})},
  body:body?JSON.stringify(body):undefined,signal:AbortSignal.timeout(1500)});
async function waitUntil(fn, label, ms=30000) {
  const end=Date.now()+ms;
  while(Date.now()<end) { try {const result=await fn(); if(result)return result;}catch{} await new Promise(done=>setTimeout(done,250)); }
  throw Error('timeout: '+label);
}
try {
  await waitUntil(async()=> (await req('/api/admin/status')).status===200,'legacy Worker');
  const key=await generateBackup('migration-fixture-password'), fp='a'.repeat(64);
  const legacyA=crypto.randomUUID(), legacyB=crypto.randomUUID(), fresh=crypto.randomUUID();
  const oldA=await newCode('player',legacyA,fp,key), oldB=await newCode('inspector',legacyB,fp,key);
  assert.equal((await req('/api/admin/publish',oldA.body)).status,200);
  assert.equal((await req('/api/admin/publish',oldB.body)).status,200);
  await writeFile(join(src,'worker.js'),current);
  await waitUntil(async()=> (await req('/api/admin/owner/start',{owner:fresh})).status===200,'updated Worker');
  assert.equal((await (await req('/api/admin/status?owner='+legacyA)).json()).owner_state,'unknown');
  const oldRetry=await newCode('player',legacyA,fp,key);
  assert.equal((await req('/api/admin/publish',oldRetry.body)).status,409,'old page must not publish');
  const freshCode=await newCode('player',fresh,fp,key); freshCode.body.takeover=true;
  assert.equal((await req('/api/admin/publish',freshCode.body)).status,200);
  assert.equal((await (await req('/api/admin/status?owner='+legacyA)).json()).owner_state,'stopped');
  assert.equal((await req('/api/admin/owner/start',{owner:legacyA})).status,409,'takeover must tombstone old owner');
  assert.equal((await req('/api/admin/stop-all',{})).status,200);
  assert.equal((await (await req('/api/admin/status?owner='+legacyB)).json()).owner_state,'stopped');
  assert.equal((await req('/api/admin/owner/start',{owner:legacyB})).status,409,'stop-all must tombstone old owner');
  assert.deepEqual((await (await req('/api/admin/status')).json()).codes,[]);
  console.log('PASS: legacy Worker upgrade, old publish rejection, takeover and stop-all tombstones');
} finally {
  if(process.platform==='win32') {
    try {execFileSync('taskkill.exe',['/PID',String(server.pid),'/T','/F'],{stdio:'ignore',windowsHide:true});} catch {}
  } else server.kill('SIGTERM');
  await new Promise(done=>server.exitCode!==null?done():server.once('exit',done));
  await rm(folder,{recursive:true,force:true});
}
