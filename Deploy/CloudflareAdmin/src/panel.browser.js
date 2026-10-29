import { generateBackup, restoreBackup, newCode } from '/crypto.js';

const $ = id => document.getElementById(id);
const roles = ['player', 'inspector', 'admin'];
const owner = crypto.randomUUID();
let key = null;
let running = false;
let busy = null;
let current = {};
let fingerprint = localStorage.getItem('mrfps-dtls-fingerprint') || '';
$('fingerprint').value = fingerprint;

function message(text) { $('message').textContent = text; }
function database() {
  return new Promise((resolve, reject) => {
    const request = indexedDB.open('mrfps-admin', 1);
    request.onupgradeneeded = () => request.result.createObjectStore('keys');
    request.onerror = () => reject(request.error);
    request.onsuccess = () => resolve(request.result);
  });
}
async function loadKey() {
  const db = await database();
  try {
    return await new Promise((resolve, reject) => {
      const req = db.transaction('keys', 'readonly').objectStore('keys').get('issuer');
      req.onsuccess = () => resolve(req.result || null);
      req.onerror = () => reject(req.error);
    });
  } finally { db.close(); }
}
async function saveKey(value) {
  const db = await database();
  try {
    await new Promise((resolve, reject) => {
      const tx = db.transaction('keys', 'readwrite');
      tx.objectStore('keys').put(value, 'issuer');
      tx.oncomplete = resolve;
      tx.onerror = () => reject(tx.error);
      tx.onabort = () => reject(tx.error || Error('保存できませんでした'));
    });
  } finally { db.close(); }
}
function showKey() {
  $('key-status').textContent = key ? '保存済み' : '未作成';
  $('public-key').value = key?.publicKey || '';
  $('key-generate').disabled = !!key;
  $('key-download').disabled = !key;
  $('key-copy').disabled = !key;
  $('key-b64').disabled = !key;
}
function promptPassphrase(confirm) {
  const pass = prompt('バックアップ用パスフレーズ（12文字以上）');
  if (pass === null) throw Error('キャンセルしました');
  if (new TextEncoder().encode(pass).length < 12) throw Error('パスフレーズは12文字以上必要です');
  if (confirm && prompt('もう一度入力してください') !== pass) throw Error('パスフレーズが一致しません');
  return pass;
}
async function api(path, body) {
  const response = await fetch(path, body ? { method:'POST', headers:{'content-type':'application/json'}, body:JSON.stringify(body) } : { cache:'no-store' });
  if (!response.ok) throw Error(`通信エラー (${response.status})`);
  return response.json();
}
function clearCodes() {
  current = {};
  for (const role of roles) { $('code-' + role).textContent = '------'; $('time-' + role).textContent = ''; }
}
function render() {
  const now = Math.floor(Date.now() / 1000);
  for (const role of roles) {
    const data = current[role];
    const seconds = data ? Math.max(0, data.expires_at - now) : 0;
    $('code-' + role).textContent = seconds ? data.code : '------';
    $('time-' + role).textContent = seconds ? `残り ${seconds} 秒` : '';
  }
}
async function publish(role, takeover = false) {
  const entry = await newCode(role, owner, fingerprint, key);
  if (!running) return;
  if (takeover) entry.body.takeover = true;
  await api('/api/admin/publish', entry.body);
  if (running) current[role] = { code: entry.code, id: entry.body.id, expires_at:entry.body.expires_at };
}
async function tick(force = false, takeover = false) {
  if (!running || busy) return;
  busy = (async () => {
    const status = force ? [] : (await api('/api/admin/status')).codes;
    const now = Math.floor(Date.now() / 1000);
    for (const role of roles) {
      if (!running) break;
      const row = status.find(item => item.role === role);
      if (!force && row && row.owner !== owner) {
        running = false;
        clearCodes();
        $('state').textContent = '別の端末で発行中';
        return;
      }
      const mine = current[role];
      if (force || !row || !mine || row.id !== mine.id || row.consumed || row.expires_at <= now + 1) await publish(role, takeover);
    }
    render();
    if (running) $('state').textContent = '発行中';
  })();
  try { await busy; } catch { $('state').textContent = '接続できません'; message('管理サービスとの通信を確認してください。'); }
  finally { busy = null; }
}
async function stop(revoke) {
  running = false;
  if (busy) await busy.catch(() => {});
  if (revoke) await api('/api/admin/revoke', {owner});
  clearCodes();
  $('state').textContent = '停止中';
}
$('key-generate').onclick = async () => {
  try {
    if (key) return;
    $('key-generate').disabled = true;
    const candidate = await generateBackup(promptPassphrase(true));
    await saveKey(candidate); key = candidate; showKey(); message('鍵を保存しました。暗号化バックアップを保管してください。');
  } catch (e) { message(e.message); showKey(); }
};
$('key-download').onclick = () => {
  if (!key) return;
  const url = URL.createObjectURL(new Blob([JSON.stringify(key.backup, null, 2)], {type:'application/json'}));
  const a = document.createElement('a'); a.href = url; a.download = 'mrfps-issuer.encrypted.json'; a.click();
  setTimeout(() => URL.revokeObjectURL(url), 1000);
};
$('key-restore').onclick = () => $('backup-file').click();
$('backup-file').onchange = async event => {
  const file = event.target.files[0]; event.target.value = '';
  if (!file) return;
  try {
    if (file.size > 8192) throw Error('バックアップが大きすぎます');
    if (key && !confirm('現在の鍵を復元した鍵に置き換えます。公開鍵もサーバーに再設定してください。')) return;
    await stop(true);
    const candidate = await restoreBackup(JSON.parse(await file.text()), promptPassphrase(false));
    await saveKey(candidate); key = candidate; showKey(); message('鍵を復元しました。');
  } catch (e) { message('復元できません: ' + e.message); }
};
$('key-copy').onclick = async () => { if (key) { await navigator.clipboard.writeText(key.publicKey); message('公開鍵をコピーしました。'); } };
$('key-b64').onclick = async () => { if (key) { await navigator.clipboard.writeText(btoa(key.publicKey)); message('MRFPS_AUTH_PUBLIC_KEY_B64 の値をコピーしました。'); } };
$('fingerprint-save').onclick = async () => {
  const value = $('fingerprint').value.trim().toLowerCase().replace(/:/g, '');
  if (!/^[0-9a-f]{64}$/.test(value)) { message('指紋は64桁の16進数で入力してください。'); return; }
  try {
    if (running) await stop(true);
    fingerprint = value; $('fingerprint').value = value; localStorage.setItem('mrfps-dtls-fingerprint', value); message('指紋を保存しました。');
  } catch (e) { message(e.message); }
};
$('codes-start').onclick = async () => {
  if (running) return;
  if (!key) { message('先に署名鍵を作成または復元してください。'); return; }
  if (!/^[0-9a-f]{64}$/.test(fingerprint)) { message('先にサーバーの指紋を保存してください。'); return; }
  message(''); running = true; clearCodes(); await tick(true);
};
$('codes-takeover').onclick = async () => {
  if (!key || !/^[0-9a-f]{64}$/.test(fingerprint)) { message('鍵と指紋を先に設定してください。'); return; }
  running = true; clearCodes(); await tick(true, true);
};
$('codes-stop').onclick = async () => { try { await stop(true); message('コードを無効にしました。'); } catch (e) { message(e.message); } };
setInterval(() => { render(); void tick(); }, 1500);
try { key = await loadKey(); showKey(); $('state').textContent = '停止中'; }
catch { $('state').textContent = '保存領域を開けません'; message('ブラウザのサイトデータを許可してください。'); }
