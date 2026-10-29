#!/usr/bin/env node
// Drive the actual panel in a dedicated persistent Chromium profile.
// The profile, credential, passphrase and encrypted backup all remain local.
import { chromium } from 'playwright-core';
import { readFile, writeFile, stat } from 'node:fs/promises';
import { resolve } from 'node:path';
import { existsSync } from 'node:fs';
import { spawn } from 'node:child_process';
const args = process.argv.slice(2);
const command = args.shift(), options = {};
while (args.length) {
  const name = args.shift();
  if (!name.startsWith('--') || !args.length) throw Error('Expected --name value');
  options[name.slice(2)] = args.shift();
}
function required(name) { if (!options[name]) throw Error(`--${name} is required`); return resolve(options[name]); }
const profile = required('profile');
const base = options.url || 'https://mrfps-admin.mizuame.app';
const targetUrl = new URL(base);
if (targetUrl.protocol !== 'https:' && !(targetUrl.protocol === 'http:' && ['localhost','127.0.0.1','[::1]'].includes(targetUrl.hostname))) {
  throw Error('HTTPS is required except for loopback testing');
}
const candidates = [options.browser, process.env.PROGRAMFILES && `${process.env.PROGRAMFILES}/Google/Chrome/Application/chrome.exe`,
  process.env['PROGRAMFILES(X86)'] && `${process.env['PROGRAMFILES(X86)']}/Microsoft/Edge/Application/msedge.exe`,
  '/Applications/Google Chrome.app/Contents/MacOS/Google Chrome', '/usr/bin/google-chrome', '/usr/bin/chromium'];
const executablePath = candidates.find(path => path && existsSync(path));
if (!executablePath) throw Error('Set --browser to a Chromium executable');
const backgroundFlags = ['--disable-background-timer-throttling','--disable-renderer-backgrounding',
  '--disable-backgrounding-occluded-windows','--disable-features=CalculateNativeWinOcclusion'];
if (command === 'open') {
  // A normal browser process is required: automation normally auto-dismisses JS
  // dialogs. Keep Basic authentication and recovery prompts interactive here.
  const child = spawn(executablePath, [`--user-data-dir=${profile}`, ...backgroundFlags, '--new-window', base], { detached:true, stdio:'ignore' });
  child.unref();
  process.stdout.write('Panel opened in the dedicated local browser profile.\n');
  process.exit(0);
}
const credentials = JSON.parse(await readFile(required('credentials-file'),'utf8'));
const context = await chromium.launchPersistentContext(profile, {
  executablePath, headless:true, httpCredentials:credentials,
  args:['--disable-extensions','--disable-background-networking','--disable-gpu',...backgroundFlags]
});
try {
  const page = context.pages()[0] || await context.newPage();
  page.on('dialog', async dialog => {
    if (dialog.type() === 'prompt') await dialog.accept((await readFile(required('passphrase-file'),'utf8')).replace(/[\r\n]+$/, ''));
    else await dialog.dismiss();
  });
  await page.goto(base, {waitUntil:'domcontentloaded',timeout:30000});
  await page.waitForFunction(() => document.getElementById('state')?.textContent !== '準備中', {timeout:15000});
  if (command === 'init') {
    const backupFile = required('backup-file'), publicFile = required('public-key-file');
    const hasKey = await page.locator('#key-generate').isDisabled();
    if (!hasKey && existsSync(backupFile)) {
      await page.setInputFiles('#backup-file', backupFile);
      await page.waitForFunction(() => document.getElementById('key-generate').disabled, {timeout:15000});
    } else if (!hasKey) {
      await page.click('#key-generate');
      await page.waitForFunction(() => document.getElementById('key-status').textContent === '保存済み', {timeout:15000});
    }
    const publicKey = await page.locator('#public-key').inputValue();
    if (!publicKey.includes('BEGIN PUBLIC KEY')) throw Error('panel did not produce a public key');
    if (existsSync(backupFile)) {
      const backup = JSON.parse(await readFile(backupFile,'utf8'));
      if (backup.public_key !== publicKey) throw Error('backup does not match this browser profile; refusing to overwrite');
    } else {
      const download = page.waitForEvent('download');
      await page.click('#key-download');
      await (await download).saveAs(backupFile);
    }
    await writeFile(publicFile, publicKey, {mode:0o600});
    process.stdout.write(JSON.stringify({ready:true,profile,backup_file:backupFile,public_key_file:publicFile,private_key_sent_to_worker:false})+'\n');
  } else if (command === 'fingerprint') {
    const fingerprint = options.fingerprint || (options['fingerprint-file'] && (await readFile(required('fingerprint-file'),'utf8')).trim());
    if (!fingerprint) throw Error('--fingerprint or --fingerprint-file is required');
    await page.fill('#fingerprint',fingerprint);
    await page.click('#fingerprint-save');
    await page.waitForFunction(() => document.getElementById('message').textContent.includes('保存しました'));
    process.stdout.write(JSON.stringify({saved:true})+'\n');
  } else if (command === 'watch') {
    if (options.fingerprint) { await page.fill('#fingerprint',options.fingerprint); await page.click('#fingerprint-save'); }
    await page.click('#codes-start');
    await page.waitForFunction(() => document.getElementById('state').textContent === '発行中', {timeout:15000});
    process.stdout.write('Code issuance active in local panel.\n');
    await new Promise(resolveDone => context.on('close',resolveDone));
  } else if (command === 'status') {
    process.stdout.write(JSON.stringify({state:await page.locator('#state').textContent(),key:await page.locator('#key-status').textContent(),public_key:await page.locator('#public-key').inputValue(),message:await page.locator('#message').textContent()})+'\n');
  } else if (command === 'screenshot') {
    const path = required('output');
    await page.screenshot({path,animations:'disabled'});
    process.stdout.write(JSON.stringify({screenshot:path})+'\n');
  } else throw Error('commands: init, fingerprint, watch, status, screenshot, open');
} finally { await context.close(); }
