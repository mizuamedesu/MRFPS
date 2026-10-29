// Upgrade the immediately previous production Worker using disposable local
// Durable Object storage. Never contacts a production route or uses live keys.
// Each Wrangler runs under its own Windows resource-guard job.
import assert from 'node:assert/strict';
import { generateBackup, newCode } from '../src/crypto.browser.js';
import { execFile, execFileSync, spawn } from 'node:child_process';
import { promisify } from 'node:util';
import { createConnection, createServer } from 'node:net';
import { Agent, request as httpRequest } from 'node:http';
import { mkdtemp, mkdir, readFile, writeFile, copyFile, rm, realpath } from 'node:fs/promises';
import { basename, dirname, join, resolve } from 'node:path';
import { tmpdir } from 'node:os';
import { fileURLToPath } from 'node:url';

if (process.platform !== 'win32') throw Error('This restart fixture requires the Windows resource guard');
const cliDir=resolve(dirname(fileURLToPath(import.meta.url)),'..');
const projectRoot=resolve(cliDir,'..','..');
const tempRoot=await realpath(resolve(tmpdir()));
const folder=await mkdtemp(join(tempRoot,'mrfps-receipt-migration-'));
const actual=await realpath(folder);
if (dirname(actual)!==tempRoot || !basename(actual).startsWith('mrfps-receipt-migration-')) throw Error('unsafe fixture directory');
const src=join(folder,'src'); await mkdir(src);
for(const name of ['panel.html','panel.browser.js','panel.css','crypto.browser.js']) {
  await copyFile(join(cliDir,'src',name),join(src,name));
}
const previous=execFileSync('git',['show','bf72143e3e09a1e0edadb3201951f8757c2d7c1f:Deploy/CloudflareAdmin/src/worker.js'],
                            {cwd:projectRoot,encoding:'utf8'});
const current=await readFile(join(cliDir,'src','worker.js'),'utf8');
assert(previous.includes('CREATE TABLE IF NOT EXISTS issuers'),'fixture must include the previous issuer schema');
assert(!previous.includes('issuance_receipts'),'fixture must predate issuance receipts');
assert(current.includes('issuance_receipts'),'updated Worker is missing issuance receipts');
await writeFile(join(src,'worker.js'),previous);
await writeFile(join(folder,'wrangler.jsonc'),JSON.stringify({name:'mrfps-receipt-migration-fixture',main:'src/worker.js',
  compatibility_date:'2026-10-23',rules:[{type:'Text',globs:['**/*.html','**/*.css','**/*.browser.js'],fallthrough:true}],
  durable_objects:{bindings:[{name:'DEVICE_CODES',class_name:'DeviceCodes'}]},migrations:[{tag:'v1',new_sqlite_classes:['DeviceCodes']}]}));
await writeFile(join(folder,'.dev.vars'),'MRFPS_ADMIN_USERNAME=fixture-user\nMRFPS_ADMIN_PASSWORD=fixture-password\n');
const listener=createServer(); await new Promise(done=>listener.listen(0,'127.0.0.1',done));
const port=listener.address().port; await new Promise(done=>listener.close(done));
const auth='Basic '+Buffer.from('fixture-user:fixture-password').toString('base64');
const bin=join(cliDir,'node_modules','wrangler','bin','wrangler.js');
const guard=join(projectRoot,'Scripts','Invoke-MRFPSResourceLimited.ps1');
const psQuote=value=>"'"+String(value).replace(/'/g,"''")+"'";
const runner=join(folder,'run-wrangler.ps1');
const command=[process.execPath,bin,'dev','--local','--config',join(folder,'wrangler.jsonc'),
  '--persist-to',join(folder,'state'),'--ip','127.0.0.1','--port',String(port)].map(psQuote).join(' ');
await writeFile(runner,`$ErrorActionPreference='Stop'\n& ${command}\nexit $LASTEXITCODE\n`);
const execFileAsync=promisify(execFile);
let server=null, phase=0;
function start() {
  phase++;
  const report=join(folder,`guard-${phase}.json`);
  const spawnedAt=Date.now();
  const child=spawn('powershell.exe',['-NoProfile','-NonInteractive','-ExecutionPolicy','Bypass','-File',guard,
    '-ScriptFile',runner,'-ReportPath',report,'-MinimumFreeGB','10','-MaximumCommitPercent','75'],
    {cwd:folder,windowsHide:true,stdio:'ignore'});
  const closed=new Promise(done=>child.once('close',done));
  server={child,closed,report,rootPid:null,rootStarted:null,spawnedAt,transport:null};
  return report;
}
async function waitUntil(fn,label,ms=30000) {
  const deadline=Date.now()+ms;
  while(Date.now()<deadline) {
    try {const result=await fn(); if(result) return result;} catch {}
    await new Promise(done=>setTimeout(done,200));
  }
  throw Error('timeout: '+label);
}

// A free-port reservation is not ownership evidence: another process may
// bind after the reservation closes. Connect without sending HTTP, inspect the
// server side of that exact TCP connection, then send all requests over that
// same socket. The private guard job has no breakaway flag, and its root is
// assigned before executing the child script. Reject if the listener's
// process ancestry, the guard, or the recorded root creation time differ.
const ownershipCheck=`
$ErrorActionPreference='Stop'
try {
  $listenerPort=[int]$env:MRFPS_TEST_SERVER_PORT
  $clientPort=[int]$env:MRFPS_TEST_CLIENT_PORT
  $rootId=[int]$env:MRFPS_TEST_ROOT_PID
  $guardId=[int]$env:MRFPS_TEST_GUARD_PID
  $expected=[datetime]::Parse($env:MRFPS_TEST_ROOT_STARTED).ToUniversalTime()
  $spawned=[datetime]::Parse($env:MRFPS_TEST_GUARD_SPAWNED).ToUniversalTime()
  $connections=@(Get-NetTCPConnection -LocalAddress '127.0.0.1' -LocalPort $listenerPort -RemoteAddress '127.0.0.1' -RemotePort $clientPort -State Established)
  if ($connections.Count -ne 1) { throw 'connection owner unavailable' }
  $ownerId=[int]$connections[0].OwningProcess
  $guardProcess=Get-Process -Id $guardId
  $guardStart=$guardProcess.StartTime.ToUniversalTime()
  if ($guardStart -lt $spawned.AddSeconds(-2) -or $guardStart -gt $spawned.AddSeconds(30)) { throw 'guard identity changed' }
  $rootProcess=Get-Process -Id $rootId
  if ([Math]::Abs(($rootProcess.StartTime.ToUniversalTime()-$expected).TotalMilliseconds) -gt 2) { throw 'job root identity changed' }
  $rootInfo=Get-CimInstance -ClassName Win32_Process -Filter "ProcessId=$rootId"
  if (-not $rootInfo -or [int]$rootInfo.ParentProcessId -ne $guardId) { throw 'job root parent changed' }
  $seen=@{}; $current=$ownerId; $younger=$null; $found=$false
  for ($depth=0; $depth -lt 32; $depth++) {
    if ($current -le 0 -or $seen.ContainsKey($current)) { throw 'process ancestry invalid' }
    $seen[$current]=$true
    $item=Get-CimInstance -ClassName Win32_Process -Filter "ProcessId=$current"
    if (-not $item) { throw 'process ancestry unavailable' }
    $started=(Get-Process -Id $current).StartTime.ToUniversalTime()
    if ($younger -and $started -gt $younger.AddMilliseconds(2)) { throw 'process identifier reused' }
    if ($current -eq $rootId) {
      if ([Math]::Abs(($started-$expected).TotalMilliseconds) -gt 2) { throw 'job root identifier reused' }
      $found=$true; break
    }
    $younger=$started; $current=[int]$item.ParentProcessId
  }
  if (-not $found) { throw 'listener is outside the owned job ancestry' }
  $again=@(Get-NetTCPConnection -LocalAddress '127.0.0.1' -LocalPort $listenerPort -RemoteAddress '127.0.0.1' -RemotePort $clientPort -State Established)
  if ($again.Count -ne 1 -or [int]$again[0].OwningProcess -ne $ownerId) { throw 'connection owner changed' }
  if ([Math]::Abs(((Get-Process -Id $guardId).StartTime.ToUniversalTime()-$guardStart).TotalMilliseconds) -gt 2 -or
      [Math]::Abs(((Get-Process -Id $rootId).StartTime.ToUniversalTime()-$expected).TotalMilliseconds) -gt 2) { throw 'guard identity changed' }
  exit 0
} catch { exit 1 }
`;
function closeTransport(owned) {
  if (!owned?.transport) return;
  owned.transport.agent.destroy();
  owned.transport.socket.destroy();
  owned.transport=null;
}
async function connectOwned(owned) {
  const socket=createConnection({host:'127.0.0.1',port});
  // Avoid unhandled errors if the peer disappears during ownership checking.
  socket.on('error',()=>{});
  try {
    await new Promise((resolve,reject)=>{
      const timer=setTimeout(()=>reject(Error('local connection timed out')),3000);
      socket.once('connect',()=>{clearTimeout(timer);resolve();});
      socket.once('error',error=>{clearTimeout(timer);reject(error);});
    });
    if (socket.localAddress!=='127.0.0.1' || socket.remoteAddress!=='127.0.0.1' || socket.remotePort!==port ||
        !Number.isSafeInteger(socket.localPort) || socket.localPort<=0) throw Error('unexpected connection endpoints');
    await execFileAsync('powershell.exe',['-NoProfile','-NonInteractive','-EncodedCommand',Buffer.from(ownershipCheck,'utf16le').toString('base64')],{
      cwd:tempRoot,windowsHide:true,timeout:12000,env:{...process.env,
        MRFPS_TEST_SERVER_PORT:String(port),MRFPS_TEST_CLIENT_PORT:String(socket.localPort),
        MRFPS_TEST_ROOT_PID:String(owned.rootPid),MRFPS_TEST_ROOT_STARTED:owned.rootStarted,
        MRFPS_TEST_GUARD_PID:String(owned.child.pid),MRFPS_TEST_GUARD_SPAWNED:new Date(owned.spawnedAt).toISOString()}});
    if (server!==owned || owned.child.exitCode!==null || owned.child.signalCode!==null || socket.destroyed)
      throw Error('owned connection closed before use');
    const agent=new Agent({keepAlive:true,maxSockets:1});
    let used=false;
    agent.createConnection=()=>{
      if (used || socket.destroyed) throw Error('verified connection cannot be replaced');
      used=true;
      return socket;
    };
    return {socket,agent};
  } catch(error) { socket.destroy(); throw error; }
}
async function ready(report) {
  await waitUntil(async()=>{
    const owned=server;
    if(!owned || owned.child.exitCode!==null || owned.child.signalCode!==null) throw Error('guard exited');
    const state=JSON.parse(await readFile(report,'utf8'));
    if(state.jobAssigned!==true || state.status!=='running' || !Number.isSafeInteger(state.rootProcessId) ||
       state.rootProcessId<=0 || !Number.isFinite(Date.parse(state.rootProcessStartedAtUtc))) return false;
    owned.rootPid=state.rootProcessId; owned.rootStarted=state.rootProcessStartedAtUtc;
    try {
      if(!owned.transport) owned.transport=await connectOwned(owned);
      return (await request('/api/admin/status')).status===200;
    } catch(error) {
      closeTransport(owned);
      throw error;
    }
  },'guarded local Worker');
}
async function stop() {
  if (!server) return;
  const owned=server, child=owned.child;
  closeTransport(owned);
  if (child.exitCode===null && child.signalCode===null) {
    if (!child.kill()) throw Error('could not close the owned test guard');
  }
  let timer;
  try { await Promise.race([owned.closed,new Promise((_,reject)=>{timer=setTimeout(()=>reject(Error('guard did not close')),15000);})]); }
  finally { clearTimeout(timer); }
  // The job is private to the guard and has KILL_ON_JOB_CLOSE. The guard's
  // process closure closes that job, but its children can take time to exit.
  // A missing report/PID is not evidence of safe shutdown; keep the fixture.
  if (!owned.rootPid) {
    const state=JSON.parse(await readFile(owned.report,'utf8'));
    if (state.jobAssigned!==true || !Number.isSafeInteger(state.rootProcessId) || state.rootProcessId<=0)
      throw Error('cannot verify the owned job root');
    owned.rootPid=state.rootProcessId;
  }
  await waitUntil(async()=>{
    try {process.kill(owned.rootPid,0); return false;}
    catch(error) {if(error.code==='ESRCH') return true; throw error;}
  },'owned job root exit',15000);
  // Probe availability by binding, without issuing a request to a replacement
  // listener that may have taken the port.
  await waitUntil(async()=>{
    const probe=createServer();
    try {
      await new Promise((done,reject)=>{probe.once('error',reject); probe.listen({port,host:'127.0.0.1',exclusive:true},done);});
      return true;
    } catch(error) {if(error.code==='EADDRINUSE') return false; throw error;}
    finally {if(probe.listening) await new Promise(done=>probe.close(done));}
  },'local Worker port release',15000);
  server=null;
}

const exclusiveCheck=`
$ErrorActionPreference='Stop'
function Check-Tree([string]$path) {
  foreach ($item in @(Get-ChildItem -LiteralPath $path -Force)) {
    if (($item.Attributes -band [IO.FileAttributes]::ReparsePoint) -ne 0) { throw 'fixture reparse point' }
    if ($item.PSIsContainer) { Check-Tree $item.FullName }
    else {
      $stream=[IO.File]::Open($item.FullName,[IO.FileMode]::Open,[IO.FileAccess]::Read,[IO.FileShare]::None)
      $stream.Dispose()
    }
  }
}
try { Check-Tree $env:MRFPS_FIXTURE_DIR; exit 0 }
catch {
  $cause=$_.Exception
  while ($cause.InnerException) { $cause=$cause.InnerException }
  if (($cause.HResult -band 0xffff) -in @(32,33)) { exit 20 }
  exit 21
}
`;
async function cleanupFixture() {
  // Delete only the exact absolute directory created by this process; reject
  // changed paths and reparse points, including descendants.
  for (let attempt=0;attempt<12;attempt++) {
    if (await realpath(folder)!==actual || dirname(actual)!==tempRoot ||
        !/^mrfps-receipt-migration-[a-zA-Z0-9]{6}$/.test(basename(actual)))
      throw Error('unsafe fixture directory during cleanup');
    try {
      await execFileAsync('powershell.exe',['-NoProfile','-NonInteractive','-Command',exclusiveCheck],{
        cwd:tempRoot,windowsHide:true,timeout:10000,env:{...process.env,MRFPS_FIXTURE_DIR:actual}});
    } catch(error) {
      if (error.code!==20 || attempt===11) throw Error('fixture file handles were not released');
      await new Promise(done=>setTimeout(done,250));
      continue;
    }
    try {await rm(actual,{recursive:true,force:false}); return;}
    catch(error) {
      if (!['EBUSY','ENOTEMPTY','EPERM'].includes(error.code) || attempt===11) throw error;
      await new Promise(done=>setTimeout(done,250));
    }
  }
  throw Error('fixture cleanup retries exhausted');
}
function request(path,body) {
  const owned=server, transport=owned?.transport;
  if (!transport || owned.child.exitCode!==null || owned.child.signalCode!==null || transport.socket.destroyed)
    return Promise.reject(Error('no verified owned Worker connection'));
  if (typeof path!=='string' || !path.startsWith('/api/') || /[\r\n]/.test(path))
    return Promise.reject(Error('invalid fixture path'));
  const payload=body===undefined?null:Buffer.from(JSON.stringify(body));
  const headers={authorization:auth};
  if (payload) {headers['content-type']='application/json'; headers['content-length']=String(payload.length);}
  return new Promise((resolve,reject)=>{
    const req=httpRequest({host:'127.0.0.1',port,path,method:payload?'POST':'GET',headers,agent:transport.agent},res=>{
      const chunks=[]; let size=0;
      res.on('data',chunk=>{
        size+=chunk.length;
        if (size>65536) {req.destroy(Error('fixture response too large')); return;}
        chunks.push(chunk);
      });
      res.once('aborted',()=>reject(Error('fixture response aborted')));
      res.once('error',reject);
      res.once('end',()=>{
        const bytes=Buffer.concat(chunks);
        resolve({status:res.statusCode,json:async()=>JSON.parse(bytes.toString('utf8')),
                 arrayBuffer:async()=>Uint8Array.from(bytes).buffer});
      });
    });
    req.setTimeout(5000,()=>req.destroy(Error('fixture request timed out')));
    req.once('error',reject);
    req.end(payload);
  });
}
async function exchange(role,code,expected) {
  const response=await request('/api/device/exchange',{role,code});
  assert.equal(response.status,expected);
  await response.arrayBuffer(); // Never print or persist the fixture bearer.
}
try {
  await ready(start());
  const signer=await generateBackup('local-receipt-migration-fixture'), fingerprint='a'.repeat(64), owner=crypto.randomUUID();
  assert.equal((await request('/api/admin/owner/start',{owner})).status,200);
  const unused=await newCode('player',owner,fingerprint,signer);
  const consumed=await newCode('admin',owner,fingerprint,signer);
  assert.equal((await request('/api/admin/publish',unused.body)).status,200);
  assert.equal((await request('/api/admin/publish',consumed.body)).status,200);
  await exchange('admin',consumed.code,200);
  await stop();
  await writeFile(join(src,'worker.js'),current);
  await ready(start());
  const status=await (await request('/api/admin/status?owner='+owner)).json();
  assert.equal(status.owner_state,'active');
  assert.equal(status.codes.find(row=>row.id===unused.body.id)?.consumed,0);
  assert.equal(status.codes.find(row=>row.id===consumed.body.id)?.consumed,1);
  for(const old of [unused,consumed]) {
    const response=await request('/api/admin/publish',old.body);
    assert.equal(response.status,409);
    assert.equal((await response.json()).error,'legacy_issuance');
  }
  await exchange('player',unused.code,200);
  await exchange('admin',consumed.code,401);
  const next=await newCode('player',owner,fingerprint,signer);
  assert.equal((await request('/api/admin/publish',next.body)).status,200);
  assert.equal((await request('/api/admin/publish',unused.body)).status,409);
  await exchange('player',next.code,200);
  console.log('PASS: previous Worker restart, unused/consumed migration, stale retry and fresh code');
} finally {
  await stop();
  await cleanupFixture();
  console.log('PASS: owned guard/job stopped and disposable fixture removed');
}
