// Contending stop commands after a crashed issuer must reclaim one lock only.
import assert from 'node:assert/strict';
import { createServer } from 'node:http';
import { spawn } from 'node:child_process';
import { mkdtemp, mkdir, writeFile, readFile, utimes, rm } from 'node:fs/promises';
import { dirname, join, resolve } from 'node:path';
import { tmpdir } from 'node:os';

const root=resolve(tmpdir()), folder=await mkdtemp(join(root,'mrfps-admin-lock-'));
if(dirname(resolve(folder))!==root) throw Error('unsafe temporary directory');
const state=join(folder,'state'), credentials=join(folder,'credentials.json');
await mkdir(state); await writeFile(credentials,JSON.stringify({username:'fixture',password:'fixture'}));
await writeFile(join(state,'state.json'),JSON.stringify({version:1,owner:crypto.randomUUID(),roles:{},registered:true,stopped:false}));
await writeFile(join(state,'lock'),'0');
const past=new Date(Date.now()-120000); await utimes(join(state,'lock'),past,past);
let active=0,maximum=0,received=0;
const server=createServer(async (req,res)=>{
  for await(const ignored of req) { void ignored; }
  active++; received++; maximum=Math.max(maximum,active);
  await new Promise(done=>setTimeout(done,300));
  active--; res.writeHead(200,{'content-type':'application/json'}); res.end('{"ok":true}');
});
await new Promise(done=>server.listen(0,'127.0.0.1',done));
const args=['admin-cli.mjs','stop','--url','http://127.0.0.1:'+server.address().port,'--state-dir',state,'--credentials-file',credentials];
function launch() {
  const child=spawn(process.execPath,args,{cwd:process.cwd(),windowsHide:true,stdio:'ignore'});
  return new Promise(done=>child.once('exit',code=>done(code)));
}
try {
  const exits=await Promise.all(Array.from({length:4},launch));
  assert(exits.every(code=>code===0),'all retries should stop successfully');
  assert.equal(received,4); assert.equal(maximum,1,'two processes held the recovered lock simultaneously');
  const final=JSON.parse(await readFile(join(state,'state.json'),'utf8'));
  assert.equal(final.stopped,true); assert.equal(final.stop_pending,false);
  console.log('PASS: concurrent crashed-lock recovery and idempotent stop');
} finally {
  await new Promise(done=>server.close(done));
  await rm(folder,{recursive:true,force:true});
}
