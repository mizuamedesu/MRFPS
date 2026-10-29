#!/usr/bin/env node
// Local, retry-safe equivalents of panel operations. Never print bearer material.
import { readFile, open, stat, mkdir, writeFile, rename, unlink, chmod } from 'node:fs/promises';
import { createPublicKey, createHash, verify } from 'node:crypto';
import { execFileSync } from 'node:child_process';
import { homedir } from 'node:os';
import { join, resolve } from 'node:path';
import { generateBackup, restoreBackup, newCode } from './src/crypto.browser.js';

const values = process.argv.slice(2), command = values.shift(), opts = {};
while (values.length) {
  const name = values.shift();
  if (!name.startsWith('--') || !values.length) throw Error('Expected --name value');
  opts[name.slice(2)] = values.shift();
}
const base = (opts.url || 'https://mrfps-admin.mizuame.app').replace(/\/$/, '');
const targetUrl = new URL(base);
if (targetUrl.protocol !== 'https:' && !(targetUrl.protocol === 'http:' && ['localhost','127.0.0.1','[::1]'].includes(targetUrl.hostname))) {
  throw Error('HTTPS is required except for loopback testing');
}
const roles = ['player', 'inspector', 'admin'];
const stateDir = resolve(opts['state-dir'] || join(process.env.LOCALAPPDATA || process.env.XDG_STATE_HOME || join(homedir(), '.local', 'state'), 'MRFPS', 'admin-cli'));
const statePath = join(stateDir, 'state.json');
function required(name) { if (!opts[name]) throw Error(`--${name} is required`); return opts[name]; }
async function readPassphrase() { return (await readFile(required('passphrase-file'), 'utf8')).replace(/[\r\n]+$/, ''); }
async function loadKey() { return restoreBackup(JSON.parse(await readFile(required('key-file'), 'utf8')), await readPassphrase()); }
async function basic() {
  const creds = JSON.parse(await readFile(required('credentials-file'), 'utf8'));
  if (typeof creds.username !== 'string' || typeof creds.password !== 'string' || creds.username.includes(':')) throw Error('invalid credentials file');
  return 'Basic ' + Buffer.from(`${creds.username}:${creds.password}`).toString('base64');
}
async function call(path, body, auth = true) {
  const headers = {};
  if (auth) headers.authorization = await basic();
  if (body) headers['content-type'] = 'application/json';
  const r = await fetch(base + path, { method: body ? 'POST' : 'GET', headers, body: body ? JSON.stringify(body) : undefined, redirect: 'error' });
  const result = await r.json();
  if (!r.ok) throw Error(`${r.status} ${result.error || 'request failed'}`);
  return result;
}
function output(value) { process.stdout.write(JSON.stringify(value) + '\n'); }
async function ensureDirectory() {
  await mkdir(stateDir, {recursive:true, mode:0o700});
  if (process.platform === 'win32') {
    const script = "$ErrorActionPreference='Stop'; $dir=New-Object System.IO.DirectoryInfo($env:MRFPS_ADMIN_STATE_DIRECTORY); $acl=$dir.GetAccessControl([System.Security.AccessControl.AccessControlSections]::Access); $acl.SetAccessRuleProtection($true,$false); foreach($a in @($acl.Access)){$acl.RemoveAccessRuleSpecific($a)}; $id=[System.Security.Principal.WindowsIdentity]::GetCurrent().User; $rule=New-Object System.Security.AccessControl.FileSystemAccessRule($id,'FullControl','ContainerInherit,ObjectInherit','None','Allow'); $acl.AddAccessRule($rule); $dir.SetAccessControl($acl)";
    execFileSync('powershell.exe', ['-NoProfile','-NonInteractive','-Command',script], {windowsHide:true,stdio:'ignore',env:{...process.env,MRFPS_ADMIN_STATE_DIRECTORY:stateDir}});
  } else await chmod(stateDir, 0o700);
}
async function writePrivate(path, contents) {
  const temporary = path + '.' + crypto.randomUUID() + '.tmp';
  try {
    await writeFile(temporary, contents, { flag:'wx', mode:0o600 });
    await rename(temporary, path);
  } finally { await unlink(temporary).catch(e => { if (e.code !== 'ENOENT') throw e; }); }
}
async function readState() {
  try {
    const state = JSON.parse(await readFile(statePath,'utf8'));
    if (state.version !== 1 || typeof state.owner !== 'string') throw Error('invalid state');
    return state;
  } catch (e) { if (e.code !== 'ENOENT') throw e; return {version:1, owner:crypto.randomUUID(), roles:{}}; }
}
async function withState(fn) {
  await ensureDirectory();
  const lockPath = join(stateDir, 'lock');
  let lock;
  try { lock = await open(lockPath, 'wx', 0o600); }
  catch (e) {
    if (e.code !== 'EEXIST') throw e;
    let stale = false;
    try {
      const holder = Number(await readFile(lockPath,'utf8'));
      if (!Number.isSafeInteger(holder) || holder < 1) stale = (Date.now() - (await stat(lockPath)).mtimeMs) > 60000;
      else try { process.kill(holder,0); } catch (failure) { if (failure.code === 'ESRCH') stale = true; }
    } catch (failure) { if (failure.code === 'ENOENT') return withState(fn); throw failure; }
    if (!stale) throw Error('another local admin command is running');
    await unlink(lockPath).catch(failure => { if (failure.code !== 'ENOENT') throw failure; });
    return withState(fn);
  }
  try { await lock.writeFile(String(process.pid)); return await fn(await readState()); }
  finally { await lock.close(); await unlink(lockPath).catch(failure => { if (failure.code !== 'ENOENT') throw failure; }); }
}
async function saveState(state) { await writePrivate(statePath, JSON.stringify(state)); }
async function ensureKey() {
  const file = required('key-file');
  try { await stat(file); return await loadKey(); }
  catch (e) { if (e.code !== 'ENOENT') throw e; }
  const result = await generateBackup(await readPassphrase());
  const handle = await open(file, 'wx', 0o600);
  try { await handle.writeFile(JSON.stringify(result.backup, null, 2) + '\n'); } finally { await handle.close(); }
  return result;
}
async function ensureCodes(state, key) {
  const fingerprint = required('fingerprint').toLowerCase().replace(/:/g, '');
  if (!/^[0-9a-f]{64}$/.test(fingerprint)) throw Error('invalid fingerprint');
  const keyHash = createHash('sha256').update(key.publicKey).digest('hex');
  const selected = opts.role ? [opts.role] : roles;
  if (!selected.every(role => roles.includes(role))) throw Error('invalid role');
  const remote = (await call('/api/admin/status')).codes;
  const now = Math.floor(Date.now()/1000);
  const changedIdentity = state.fingerprint && (state.fingerprint !== fingerprint || state.keyHash !== keyHash);
  if (changedIdentity) {
    await call('/api/admin/revoke', {owner:state.owner});
    state.roles = {};
  }
  state.fingerprint = fingerprint; state.keyHash = keyHash;
  for (const role of selected) {
    const row = remote.find(item => item.role === role);
    const local = state.roles[role];
    if (row && row.owner !== state.owner && row.expires_at > now && opts.takeover !== 'true') throw Error(`owner_conflict: ${role}; use explicit --takeover true to switch issuers`);
    if (!changedIdentity && row && local && row.owner === state.owner && row.id === local.id && !row.consumed && row.expires_at > now + 1) {
      delete local.pending;
      await writePrivate(join(stateDir, role + '.code'), local.code + '\n');
      continue;
    }
    // Persist the prepared issuance before the HTTP request. If a reply is lost,
    // the retry can recover the same code and ID from the durable local state.
    let entry = local?.pending && local.expires_at > now + 1 ? local.pending : await newCode(role, state.owner, fingerprint, key);
    if (opts.takeover === 'true') entry.body.takeover = true;
    state.roles[role] = {code:entry.code,id:entry.body.id,expires_at:entry.body.expires_at,pending:entry};
    await saveState(state);
    await call('/api/admin/publish', entry.body);
    delete state.roles[role].pending;
    await saveState(state);
    await writePrivate(join(stateDir, role + '.code'), entry.code + '\n');
  }
  await saveState(state);
  return selected.map(role => ({role, code_file:join(stateDir,role+'.code'), expires_at:state.roles[role].expires_at}));
}
function checkExchange(result, role, expectedKey, fingerprint) {
  if (typeof result.token !== 'string' || result.token.length > 2048 || result.public_key !== expectedKey) throw Error('issuer public key mismatch');
  const parts = result.token.split('.');
  const valid = parts.length === 3 && verify(null,Buffer.from(parts[0]+'.'+parts[1]),createPublicKey(expectedKey),Buffer.from(parts[2],'base64url'));
  const claims = valid ? JSON.parse(Buffer.from(parts[1],'base64url')) : {};
  if (!valid || claims.iss !== 'mrfps-issuer' || claims.aud !== 'mrfps' || claims.role !== role ||
      claims.exp !== result.expires_at || claims.exp <= Math.floor(Date.now()/1000) || claims.dtls_fp !== fingerprint) {
    throw Error('signature, expiry, role or DTLS fingerprint verification failed');
  }
  return claims;
}
async function exportExchange() {
  const id = required('operation-id'), role = required('role');
  if (!/^[0-9a-f]{8}-[0-9a-f]{4}-4[0-9a-f]{3}-[89ab][0-9a-f]{3}-[0-9a-f]{12}$/.test(id) || !roles.includes(role)) throw Error('invalid operation ID or role');
  const code = (await readFile(required('code-file'),'utf8')).trim();
  if (!/^\d{6}$/.test(code)) throw Error('code file must contain six digits');
  const fingerprint = required('fingerprint').toLowerCase().replace(/:/g, '');
  if (!/^[0-9a-f]{64}$/.test(fingerprint)) throw Error('invalid fingerprint');
  const publicKey = (await readFile(required('public-key-file'),'utf8')).replace(/\r\n/g,'\n');
  const recordPath = join(stateDir,'exchange-'+id+'.json');
  const tokenPath = join(stateDir,'exchange-'+id+'.jwt');
  const identity = {id, role, code_hash:createHash('sha256').update(role+':'+code).digest('hex'), fingerprint,
    public_key_hash:createHash('sha256').update(publicKey).digest('hex')};
  return withState(async () => {
    let previous;
    try { previous = JSON.parse(await readFile(recordPath,'utf8')); } catch (e) { if (e.code !== 'ENOENT') throw e; }
    if (previous) {
      for (const [name,value] of Object.entries(identity)) if (previous[name] !== value) throw Error('operation ID already identifies a different exchange');
      if (previous.status !== 'complete') throw Error('exchange outcome unknown; obtain a new code and operation ID');
      const token = await readFile(tokenPath,'utf8');
      checkExchange({token,public_key:publicKey,expires_at:previous.expires_at},role,publicKey,fingerprint);
      return {saved:true,reused:true,role,token_file:tokenPath,expires_at:previous.expires_at};
    }
    // Persist before the single-use exchange. A crash then cannot silently
    // consume the same code again, even if the HTTP response was lost.
    await writePrivate(recordPath,JSON.stringify({...identity,status:'started'}));
    const result = await call('/api/device/exchange',{role,code},false);
    checkExchange(result,role,publicKey,fingerprint);
    await writePrivate(tokenPath,result.token);
    await writePrivate(recordPath,JSON.stringify({...identity,status:'complete',expires_at:result.expires_at}));
    return {saved:true,reused:false,role,token_file:tokenPath,expires_at:result.expires_at};
  });
}
async function main() {
  if (command === 'key-ensure') {
    const key = await ensureKey(); output({created_or_existing:true,public_key:key.publicKey});
  } else if (command === 'public-key') {
    const key = await loadKey(); output({public_key:key.publicKey,MRFPS_AUTH_PUBLIC_KEY_B64:Buffer.from(key.publicKey).toString('base64')});
  } else if (command === 'status') {
    const remote = await call('/api/admin/status');
    output({codes:remote.codes});
  } else if (command === 'stop') {
    await withState(async state => {
      await call('/api/admin/revoke', {owner:state.owner});
      for (const role of roles) await unlink(join(stateDir,role+'.code')).catch(e => {if(e.code!=='ENOENT')throw e;});
      state.roles={}; await saveState(state);
    });
    output({stopped:true});
  } else if (command === 'ensure' || command === 'watch') {
    const key = await loadKey();
    do {
      const result = await withState(state => ensureCodes(state,key));
      if (command === 'ensure') {output({active:result}); break;}
      // A deliberate takeover applies only at startup, not to later poll cycles.
      // Otherwise two active issuers could continually steal codes from each other.
      delete opts.takeover;
      await new Promise(resolveDone => setTimeout(resolveDone,1500));
    } while (true);
  } else if (command === 'exchange-test') {
    const code = (await readFile(required('code-file'),'utf8')).trim(), role = required('role');
    const result = await call('/api/device/exchange',{role,code},false);
    const parts = result.token.split('.');
    const valid = parts.length === 3 && verify(null,Buffer.from(parts[0]+'.'+parts[1]),createPublicKey(result.public_key),Buffer.from(parts[2],'base64url'));
    const claims = valid ? JSON.parse(Buffer.from(parts[1],'base64url')) : {};
    if (!valid || claims.role !== role || claims.exp <= Math.floor(Date.now()/1000)) throw Error('signature or role verification failed');
    output({exchanged:true,signature_verified:true,role,token_expires_at:result.expires_at,dtls_fp:claims.dtls_fp});
  } else if (command === 'exchange-export') {
    output(await exportExchange());
  } else if (command === 'exchange-clean') {
    const id = required('operation-id');
    if (!/^[0-9a-f]{8}-[0-9a-f]{4}-4[0-9a-f]{3}-[89ab][0-9a-f]{3}-[0-9a-f]{12}$/.test(id)) throw Error('invalid operation ID');
    await withState(async () => { for (const extension of ['jwt','json']) await unlink(join(stateDir,'exchange-'+id+'.'+extension)).catch(e => {if(e.code!=='ENOENT')throw e;}); });
    output({cleaned:true});
  } else throw Error('commands: key-ensure, public-key, status, stop, ensure, watch, exchange-test, exchange-export, exchange-clean; see README.md');
}
main().catch(error => { process.stderr.write('MRFPS admin: '+error.message+'\n'); process.exitCode=1; });
