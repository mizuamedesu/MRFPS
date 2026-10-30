import { generateBackup, restoreBackup, newCode } from '/crypto.js';

const $ = id => document.getElementById(id);
const roles = ['player', 'inspector', 'admin'];
let owner = crypto.randomUUID();
let key = null;
let running = false;
let starting = false;
let stopping = 0;
let startEpoch = 0;
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
  const result = await response.json();
  if (!response.ok) { const error=Error(`通信エラー (${response.status})`); error.code=result.error; throw error; }
  return result;
}
let rosterBusy = false;
let rosterDeadline = 0, pingDeadlines = [];
function expireParticipants() {
  const now=performance.now();
  for (const ping of pingDeadlines) if (now >= ping.deadline) ping.cell.textContent='不明';
  if (rosterDeadline && now >= rosterDeadline) {
    rosterDeadline=0; pingDeadlines=[]; $('participants').replaceChildren();
    $('participants-state').textContent='サーバー未接続';
  }
}
async function refreshParticipants() {
  if (rosterBusy) return;
  rosterBusy = true;
  try {
    const started=performance.now();
    const response=await fetch('/api/admin/participants',{cache:'no-store',signal:AbortSignal.timeout(5000)});
    if (!response.ok) throw Error('roster request');
    const result=await response.json();
    const now=performance.now(), elapsed=Math.max(0,now-started);
    const body = $('participants');
    body.replaceChildren(); pingDeadlines=[]; rosterDeadline=0;
    if (!result.online || !Array.isArray(result.participants) || !Number.isFinite(result.expires_in_ms) || result.expires_in_ms<=elapsed) {
      $('participants-state').textContent = 'サーバー未接続';
      return;
    }
    rosterDeadline=now+result.expires_in_ms-elapsed;
    const labels = {player:'プレイヤー', inspector:'観戦', admin:'管理者', bot:'Bot'};
    for (const entry of result.participants) {
      const row = document.createElement('tr');
      for (const value of [entry.name || `#${entry.id}`, labels[entry.role] || '不明']) {
        const cell=document.createElement('td'); cell.textContent=value; row.appendChild(cell);
      }
      const ping=document.createElement('td');
      if (Number.isInteger(entry.ping_ms) && entry.ping_ms>=0 && Number.isFinite(entry.ping_expires_in_ms) && entry.ping_expires_in_ms>elapsed) {
        ping.textContent=`${entry.ping_ms} ms`;
        pingDeadlines.push({cell:ping,deadline:now+entry.ping_expires_in_ms-elapsed});
      } else ping.textContent='不明';
      row.appendChild(ping);
      body.appendChild(row);
    }
    $('participants-state').textContent = `${result.participants.length} 人`;
  } catch {
    rosterDeadline=0; pingDeadlines=[]; $('participants').replaceChildren();
    $('participants-state').textContent = '通信エラー';
  } finally { rosterBusy = false; }
}
void refreshParticipants();
setInterval(() => { expireParticipants(); }, 500);
setInterval(() => { void refreshParticipants(); }, 2000);
document.addEventListener('visibilitychange',expireParticipants);
function clearCodes() {
  current = {};
  for (const role of roles) { $('code-' + role).textContent = '------'; $('time-' + role).textContent = ''; }
}
function render() {
  const now = performance.now();
  for (const role of roles) {
    const data = current[role];
    const seconds = data ? Math.max(0, Math.ceil((data.deadline - now) / 1000)) : 0;
    $('code-' + role).textContent = seconds ? data.code : '------';
    $('time-' + role).textContent = seconds ? `残り ${seconds} 秒` : '';
  }
}
async function publish(role, takeover = false) {
  const entry = await newCode(role, owner, fingerprint, key);
  if (!running) return;
  if (takeover) entry.body.takeover = true;
  const started=performance.now();
  const accepted=await api('/api/admin/publish', entry.body);
  if (!accepted || accepted.ok !== true || accepted.id !== entry.body.id || accepted.consumed === true ||
      !Number.isSafeInteger(accepted.expires_at) || !Number.isSafeInteger(accepted.server_now) ||
      accepted.expires_at <= accepted.server_now || accepted.expires_at > entry.body.expires_at ||
      accepted.expires_at > accepted.server_now + 30) throw Error('invalid issuance response');
  // Start from the authoritative server lifetime and subtract the full request
  // time. This is conservative even if this browser's wall clock is wrong.
  const now=performance.now();
  const remain=Math.max(0,(accepted.expires_at-accepted.server_now)*1000-(now-started));
  if (running) current[role] = { code: entry.code, id: entry.body.id, expires_at:accepted.expires_at, deadline:now+remain };
}
async function tick(force = false, takeover = false) {
  if (!running || busy) return;
  busy = (async () => {
    const response = force ? null : await api('/api/admin/status?owner=' + encodeURIComponent(owner));
    if (response && response.owner_state !== 'active') {
      running = false; clearCodes(); $('state').textContent = '停止中'; message('発行が停止されました。'); return;
    }
    const status = response ? response.codes : [];
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
      if (force || !row || !mine || row.id !== mine.id || row.consumed || mine.deadline <= performance.now() + 1000) await publish(role, takeover);
    }
    render();
    if (running) $('state').textContent = '発行中';
  })();
  try { await busy; } catch (error) {
    if (error.code === 'owner_stopped') { running=false; clearCodes(); $('state').textContent='停止中'; message('発行が停止されました。'); }
    else { $('state').textContent = '接続できません'; message('管理サービスとの通信を確認してください。'); }
  }
  finally { busy = null; }
}
async function stop(revoke) {
  ++startEpoch;
  running = false;
  const stoppedOwner=owner;
  stopping++;
  // Hide locally cached codes immediately even if the remote revoke is delayed
  // or fails. The UI must not claim a confirmed stop until the server replies.
  clearCodes();
  $('state').textContent = revoke ? '停止確認中' : '停止中';
  try {
    if (busy) await busy.catch(() => {});
    if (revoke) await api('/api/admin/revoke', {owner:stoppedOwner});
    $('state').textContent = '停止中';
  } catch(error) { $('state').textContent='停止未確認'; throw error; }
  finally { stopping--; }
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
async function startIssuance(takeover) {
  if (running || starting || stopping) return;
  if (!key) { message('先に署名鍵を作成または復元してください。'); return; }
  if (!/^[0-9a-f]{64}$/.test(fingerprint)) { message('先にサーバーの指紋を保存してください。'); return; }
  const candidate=crypto.randomUUID(), epoch=++startEpoch;
  starting=true; message('');
  try {
    await api('/api/admin/owner/start',{owner:candidate});
    if (epoch!==startEpoch) { await api('/api/admin/revoke',{owner:candidate}); return; }
    owner=candidate; running=true; clearCodes(); await tick(true,takeover);
  } catch { $('state').textContent='接続できません'; message('発行を開始できませんでした。'); }
  finally { starting=false; }
}
$('codes-start').onclick = () => { void startIssuance(false); };
$('codes-takeover').onclick = () => { void startIssuance(true); };
$('codes-stop').onclick = async () => { try { await stop(true); message('コードを無効にしました。'); } catch (e) { message(e.message); } };
setInterval(() => { render(); void tick(); }, 1500);
try { key = await loadKey(); showKey(); $('state').textContent = '停止中'; }
catch { $('state').textContent = '保存領域を開けません'; message('ブラウザのサイトデータを許可してください。'); }
