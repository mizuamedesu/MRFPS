// Real local Worker + process-level CLI test. Test-only credentials and keys.
import assert from 'node:assert/strict';
import { createServer } from 'node:http';
import { spawn } from 'node:child_process';
import { mkdtemp, writeFile, readFile, rm } from 'node:fs/promises';
import { dirname, join, resolve } from 'node:path';
import { tmpdir } from 'node:os';
import { generateBackup } from '../src/crypto.browser.js';

const upstream='http://127.0.0.1:18787';
let unavailable=false, blockedRequests=0;
const proxy=createServer(async (request,response) => {
  const chunks=[]; for await (const chunk of request) chunks.push(chunk);
  if(unavailable==='hang') { blockedRequests++; return; }
  if(unavailable) { response.writeHead(503,{'content-type':'application/json'}); response.end('{"error":"temporary_fixture"}'); return; }
  try {
    const headers={}; for(const [name,value] of Object.entries(request.headers)) if(name!=='host'&&value!==undefined) headers[name]=value;
    const result=await fetch(upstream+request.url,{method:request.method,headers,body:chunks.length?Buffer.concat(chunks):undefined});
    response.writeHead(result.status,{'content-type':result.headers.get('content-type')||'application/json'});
    response.end(Buffer.from(await result.arrayBuffer()));
  } catch { response.writeHead(503,{'content-type':'application/json'}); response.end('{"error":"temporary_fixture"}'); }
});
await new Promise(resolveListen => proxy.listen(0,'127.0.0.1',resolveListen));
const url='http://127.0.0.1:'+proxy.address().port;
const tempRoot=resolve(tmpdir()), folder=await mkdtemp(join(tempRoot,'mrfps-watch-test-'));
if(dirname(resolve(folder))!==tempRoot) throw Error('unsafe temporary directory');
const stateDir=join(folder,'state'), keyFile=join(folder,'issuer.json'), passFile=join(folder,'pass.txt'), credentialsFile=join(folder,'credentials.json');
const fixture=await generateBackup('local-watch-fixture-password');
await writeFile(keyFile,JSON.stringify(fixture.backup));
await writeFile(passFile,'local-watch-fixture-password');
await writeFile(credentialsFile,JSON.stringify({username:'fixture-user',password:'fixture-password'}));
const cli=['admin-cli.mjs'], common=['--url',url,'--credentials-file',credentialsFile,'--state-dir',stateDir];
const signer=['--key-file',keyFile,'--passphrase-file',passFile,'--fingerprint','a'.repeat(64),'--role','player'];
function launch(args) {
  const child=spawn(process.execPath,[...cli,...args],{cwd:process.cwd(),windowsHide:true,stdio:['ignore','pipe','pipe']});
  let stdout='',stderr=''; child.stdout.on('data',data=>stdout+=data); child.stderr.on('data',data=>stderr+=data);
  const done=new Promise(resolveDone => child.on('exit',(code,signal)=>resolveDone({code,signal,stdout,stderr})));
  return {child,done};
}
async function run(args,expected=0) {
  const result=await launch(args).done;
  assert.equal(result.code,expected,'unexpected CLI exit'); return result;
}
async function state() { try {return JSON.parse(await readFile(join(stateDir,'state.json'),'utf8'));} catch {return null;} }
async function waitFor(predicate,description,timeout=20000) {
  const until=Date.now()+timeout;
  while(Date.now()<until) { const value=await state(); if(value&&predicate(value)) return value; await new Promise(r=>setTimeout(r,150)); }
  throw Error('timeout: '+description);
}
const children=[];
try {
  const watch=launch(['watch',...common,...signer]); children.push(watch.child);
  let active=await waitFor(s=>s.watch?.status==='active','initial issuer');
  const firstOwner=active.owner;
  unavailable=true;
  await waitFor(s=>s.watch?.status==='retrying'&&s.watch.consecutive_errors>=1,'transient failure');
  unavailable=false;
  active=await waitFor(s=>s.owner===firstOwner&&s.watch?.status==='active','automatic recovery');
  assert.equal(active.stopped,false);
  unavailable=true;
  await waitFor(s=>s.watch?.status==='retrying','second transient failure');
  const auth='Basic '+Buffer.from('fixture-user:fixture-password').toString('base64');
  const stop=await fetch(upstream+'/api/admin/stop-all',{method:'POST',headers:{authorization:auth,'content-type':'application/json'},body:'{}'});
  assert.equal(stop.status,200);
  unavailable=false;
  const stopped=await Promise.race([watch.done,new Promise((_,reject)=>setTimeout(()=>reject(Error('watch did not stop')),20000))]);
  assert.equal(stopped.code,0); assert.match(stopped.stdout,/"status":"stopped"/);
  const status=await (await fetch(upstream+'/api/admin/status?owner='+firstOwner,{headers:{authorization:auth}})).json();
  assert.equal(status.owner_state,'stopped'); assert.equal(status.codes.length,0);
  await run(['ensure',...common,...signer]);
  const restarted=await state(); assert.notEqual(restarted.owner,firstOwner);
  const watch2=launch(['watch',...common,...signer]); children.push(watch2.child);
  await waitFor(s=>s.watch?.status==='active','second watcher');
  unavailable=true;
  await waitFor(s=>s.watch?.status==='retrying','local stop outage');
  await run(['stop',...common],1);
  const local=await state(); assert.equal(local.stopped,true); assert.equal(local.stop_pending,'owner');
  const stopped2=await Promise.race([watch2.done,new Promise((_,reject)=>setTimeout(()=>reject(Error('local stop did not stop watch')),20000))]);
  assert.equal(stopped2.code,0);
  await run(['ensure',...common,...signer],1); // An unconfirmed stop cannot silently restart.
  unavailable=false;
  await run(['stop',...common]);
  await run(['ensure',...common,...signer]);
  const watch3=launch(['watch',...common,...signer]); children.push(watch3.child);
  await waitFor(s=>s.watch?.status==='active','third watcher');
  unavailable='hang';
  const deadline=Date.now()+10000;
  while(!blockedRequests && Date.now()<deadline) await new Promise(r=>setTimeout(r,100));
  assert(blockedRequests,'watch did not reach the stalled connection');
  const stopDuringHang=launch(['stop',...common]); children.push(stopDuringHang.child);
  setTimeout(()=>{unavailable=false;},700);
  const waited=await stopDuringHang.done;
  assert.equal(waited.code,0,'stop should wait for timed-out watch lock');
  assert.equal((await Promise.race([watch3.done,new Promise((_,reject)=>setTimeout(()=>reject(Error('watch did not exit after stop')),20000))])).code,0);
  await run(['stop',...common]);
  console.log('PASS: watch retries, remote stop priority, outage stop, explicit restart, stalled request and lock contention');
} finally {
  for(const child of children) if(child.exitCode===null) child.kill();
  await new Promise(resolveClose=>proxy.close(resolveClose));
  await rm(folder,{recursive:true,force:true});
}
