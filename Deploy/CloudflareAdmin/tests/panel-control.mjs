// Disposable browser profile and fixture keys; requires a local wrangler dev.
import assert from 'node:assert/strict';
import { chromium } from 'playwright-core';
import { generateBackup } from '../src/crypto.browser.js';
import { mkdtemp, writeFile, readFile, rm } from 'node:fs/promises';
import { existsSync } from 'node:fs';
import { execFile } from 'node:child_process';
import { promisify } from 'node:util';
import { dirname, join, resolve } from 'node:path';
import { tmpdir } from 'node:os';

const runFile=promisify(execFile), base='http://127.0.0.1:18787';
const candidates=[process.env.PROGRAMFILES && `${process.env.PROGRAMFILES}/Google/Chrome/Application/chrome.exe`,
  process.env['PROGRAMFILES(X86)'] && `${process.env['PROGRAMFILES(X86)']}/Microsoft/Edge/Application/msedge.exe`,
  '/Applications/Google Chrome.app/Contents/MacOS/Google Chrome','/usr/bin/google-chrome','/usr/bin/chromium'];
const executablePath=candidates.find(path=>path&&existsSync(path));
if (!executablePath) throw Error('test requires Chromium');
const tempRoot=resolve(tmpdir()), folder=await mkdtemp(join(tempRoot,'mrfps-panel-test-'));
if(dirname(resolve(folder))!==tempRoot) throw Error('unsafe temporary directory');
const profile=join(folder,'profile'), backupA=join(folder,'a.json'), backupB=join(folder,'b.json');
const pass=join(folder,'pass.txt'), creds=join(folder,'creds.json'), publicKey=join(folder,'public.pem');
const password='panel-fixture-passphrase', auth='Basic '+Buffer.from('fixture-user:fixture-password').toString('base64');
const keyA=await generateBackup(password), keyB=await generateBackup(password);
await writeFile(backupA,JSON.stringify(keyA.backup)); await writeFile(backupB,JSON.stringify(keyB.backup));
await writeFile(pass,password); await writeFile(creds,JSON.stringify({username:'fixture-user',password:'fixture-password'}));
const common=['--url',base,'--profile',profile,'--credentials-file',creds];
async function control(command,extra) {
  return runFile(process.execPath,['panel-control.mjs',command,...common,...extra],{cwd:process.cwd(),windowsHide:true});
}
async function status(owner) {
  const suffix=owner?'?owner='+encodeURIComponent(owner):'';
  const result=await fetch(base+'/api/admin/status'+suffix,{headers:{authorization:auth}});
  assert.equal(result.status,200); return result.json();
}
async function stopAll() {
  const result=await fetch(base+'/api/admin/stop-all',{method:'POST',headers:{authorization:auth,'content-type':'application/json'},body:'{}'});
  assert.equal(result.status,200);
}
let browser;
try {
  await stopAll();
  const restoreArgs=file=>['--backup-file',file,'--passphrase-file',pass,'--public-key-file',publicKey];
  const initial=JSON.parse((await control('restore',restoreArgs(backupA))).stdout);
  assert.equal(initial.restored,true);
  assert.equal(await readFile(publicKey,'utf8'),keyA.publicKey);
  const repeated=JSON.parse((await control('restore',restoreArgs(backupA))).stdout);
  assert.equal(repeated.already_installed,true);
  await assert.rejects(control('restore',restoreArgs(backupB)));
  assert.equal(await readFile(publicKey,'utf8'),keyA.publicKey);
  await control('fingerprint',['--fingerprint','a'.repeat(64)]);
  browser=await chromium.launchPersistentContext(profile,{executablePath,headless:true,
    httpCredentials:{username:'fixture-user',password:'fixture-password'},args:['--disable-gpu','--disable-extensions']});
  const page=browser.pages()[0] || await browser.newPage();
  const pageErrors=[];
  page.on('pageerror',error=>pageErrors.push(error.message));
  const navigation=await page.goto(base,{waitUntil:'domcontentloaded'});
  try { await page.waitForFunction(()=>document.getElementById('state')?.textContent==='停止中',null,{timeout:12000}); }
  catch(error) {
    console.error('panel initialization failed',JSON.stringify({status:navigation.status(),state:await page.locator('#state').textContent().catch(()=>null),errors:pageErrors}));
    throw error;
  }
  await page.click('#codes-start');
  await page.waitForFunction(()=>document.getElementById('state')?.textContent==='発行中');
  let live=await status(), owner=live.codes[0]?.owner;
  assert(owner); assert.equal(live.codes.length,3); assert(live.codes.every(x=>x.owner===owner));
  await stopAll();
  await page.waitForFunction(()=>document.getElementById('state')?.textContent==='停止中');
  await new Promise(done=>setTimeout(done,2000));
  assert.equal((await status(owner)).owner_state,'stopped'); assert.equal((await status()).codes.length,0);
  await page.click('#codes-start');
  await page.waitForFunction(()=>document.getElementById('state')?.textContent==='発行中');
  live=await status(); assert(live.codes[0]?.owner && live.codes[0].owner!==owner); owner=live.codes[0].owner;
  await page.click('#codes-stop');
  await page.waitForFunction(()=>document.getElementById('state')?.textContent==='停止中');
  assert.equal((await status(owner)).owner_state,'stopped'); assert.equal((await status()).codes.length,0);
  await page.click('#codes-start');
  await page.waitForFunction(()=>document.getElementById('state')?.textContent==='発行中');
  owner=(await status()).codes[0].owner;
  await browser.close(); browser=null;
  const replacement=JSON.parse((await control('restore',[...restoreArgs(backupB),'--replace','true'])).stdout);
  assert.equal(replacement.restored,true); assert.equal(replacement.issuance_stopped,true);
  assert.equal(await readFile(publicKey,'utf8'),keyB.publicKey);
  assert.equal((await status(owner)).owner_state,'stopped'); assert.equal((await status()).codes.length,0);
  browser=await chromium.launchPersistentContext(profile,{executablePath,headless:true,
    httpCredentials:{username:'fixture-user',password:'fixture-password'},args:['--disable-gpu','--disable-extensions']});
  const page2=browser.pages()[0] || await browser.newPage();
  await page2.goto(base,{waitUntil:'domcontentloaded'});
  await page2.waitForFunction(()=>document.getElementById('state')?.textContent==='停止中');
  await page2.click('#codes-start');
  await page2.waitForFunction(()=>document.getElementById('state')?.textContent==='発行中');
  owner=(await status()).codes[0].owner;
  await browser.close(); browser=null;
  assert.equal(JSON.parse((await control('restore',restoreArgs(backupB))).stdout).already_installed,true);
  assert.equal((await status(owner)).owner_state,'active'); // Same key is a no-op, not a global stop.
  await stopAll();
  console.log('PASS: real panel stop/restart, key restore, replacement and idempotent restore');
} finally {
  if(browser) await browser.close();
  await stopAll().catch(()=>{});
  await rm(folder,{recursive:true,force:true});
}
