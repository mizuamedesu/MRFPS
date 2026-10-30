// Exercise the CLI against an isolated local read-only status endpoint.
import test from 'node:test';
import assert from 'node:assert/strict';
import { createServer } from 'node:http';
import { execFile } from 'node:child_process';
import { promisify } from 'node:util';
import { mkdtemp, writeFile, unlink, rmdir } from 'node:fs/promises';
import { join } from 'node:path';
import { tmpdir } from 'node:os';
import { fileURLToPath } from 'node:url';
const run=promisify(execFile);

test('status CLI returns the DO lifetime or null for an older DO',async t=>{
  const directory=await mkdtemp(join(tmpdir(),'mrfps-status-fixture-'));
  const credentials=join(directory,'fixture.json');
  await writeFile(credentials,JSON.stringify({username:'fixture',password:'fixture-password'}));
  t.after(async()=>{await unlink(credentials);await rmdir(directory);});
  let payload={codes:[],code_lifetime_seconds:300};
  const server=createServer((req,res)=>{
    if (req.method!=='GET' || req.url!=='/api/admin/status' ||
        req.headers.authorization!=='Basic '+Buffer.from('fixture:fixture-password').toString('base64')) {
      res.writeHead(400).end(); return;
    }
    res.writeHead(200,{'content-type':'application/json'}).end(JSON.stringify(payload));
  });
  await new Promise(resolve=>server.listen(0,'127.0.0.1',resolve));
  t.after(()=>new Promise(resolve=>server.close(resolve)));
  const url=`http://127.0.0.1:${server.address().port}`;
  const cli=fileURLToPath(new URL('../admin-cli.mjs',import.meta.url));
  const status=async()=>JSON.parse((await run(process.execPath,[cli,'status','--url',url,'--credentials-file',credentials],{windowsHide:true})).stdout);
  assert.deepEqual(await status(),{codes:[],code_lifetime_seconds:300});
  payload={codes:[]};
  assert.deepEqual(await status(),{codes:[],code_lifetime_seconds:null});
  payload={codes:[],code_lifetime_seconds:'300'};
  assert.deepEqual(await status(),{codes:[],code_lifetime_seconds:null});
});
