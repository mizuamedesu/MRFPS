#!/usr/bin/env node
// Read only the owned Flash game's socket-health marker and DTLS fingerprint.
// The CLI login credential is used only in memory and is never printed.
import { createHash } from 'node:crypto';
import { createRequire } from 'node:module';
import { homedir } from 'node:os';
import { readFile } from 'node:fs/promises';
import { resolve, join } from 'node:path';
import { fileURLToPath } from 'node:url';

const require = createRequire(import.meta.url);
const WebSocket = require('ws');
const ORIGIN = 'https://heterocloud.mizuame.app';
const ORGANIZATION = '019fbda8-eff3-7610-bdd5-8fe89e746c14';
const UUID = /^[0-9a-f]{8}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{12}$/i;
const IMAGE = /^ghcr\.io\/[a-z0-9._/-]+@sha256:[a-f0-9]{64}$/;
const POD = /^[a-z0-9](?:[a-z0-9.-]{0,251}[a-z0-9])?$/;
const SHELL_COMMAND = [
  'stty -echo 2>/dev/null || true',
  '/usr/local/bin/mrfps-flash-healthcheck >/dev/null 2>&1; rc=$?; printf "\\n__MRFPS_HEALTH__%s\\n" "$rc"',
  'p="$(pidof MRFPSServer MRFPSServer-Linux-Shipping 2>/dev/null | awk \'{print $1}\')"',
  'if [ -n "$p" ]; then awk -v n="$p" \'{printf "__MRFPS_START__%s:%s\\n",n,$22}\' "/proc/$p/stat"; fi',
  'grep -oE \'MRFPS_AUTH_TRANSPORT_READY fingerprint=[0-9a-f]{64}\' /opt/mrfps/MRFPS/Saved/Logs/MRFPS.log | tail -n 1 | sed \'s/^MRFPS_AUTH_TRANSPORT_READY fingerprint=/__MRFPS_FP__/\'',
  'printf "__MRFPS_DONE__\\n"',
  'exit',
].join('\n') + '\n';

function canonical(value) {
  if (value === null || typeof value === 'boolean') return JSON.stringify(value);
  if (typeof value === 'number' && Number.isSafeInteger(value)) return JSON.stringify(value);
  if (typeof value === 'string' && !/[^\x00-\x7e]/.test(value)) return JSON.stringify(value);
  if (Array.isArray(value)) return '[' + value.map(canonical).join(',') + ']';
  if (value && typeof value === 'object') {
    return '{' + Object.keys(value).sort().map(key => canonical(key) + ':' + canonical(value[key])).join(',') + '}';
  }
  throw Error('SPEC_CANONICALIZATION_UNVERIFIED');
}

export function specDigest(spec) {
  return createHash('sha256').update(canonical(spec)).digest('hex');
}

export function ownedReadyService(state, service, expectedImage) {
  if (!state || !UUID.test(state.project_id || '') || !UUID.test(state.service_id || '') ||
      !UUID.test(state.deployment_id || '') || state.service_name !== 'mrfps-server' || !IMAGE.test(expectedImage)) {
    throw Error('OWNED_STATE_OR_IMAGE_INVALID');
  }
  const metadata = service?.spec?.metadata || {};
  const live = service?.status?.status || {};
  if (service?.id !== state.service_id || service?.organization_id !== ORGANIZATION ||
      service?.project_id !== state.project_id || service?.name !== state.service_name ||
      metadata.managed_by !== 'MRFPS-cloud-deploy' || metadata.deployment_id !== state.deployment_id ||
      metadata.service_name !== state.service_name || service?.spec?.image !== expectedImage ||
      specDigest(service.spec) !== state.observed_spec_sha256) throw Error('OWNERSHIP_OR_SPEC_MISMATCH');
  if (service.state !== 'ready' || !Number.isSafeInteger(service.generation) ||
      live.observed_generation !== service.generation || live.live_status_unavailable === true ||
      live.ready_replicas !== 1 || live.serving_replicas !== 1) throw Error('SERVICE_NOT_READY');
  return { generation: service.generation, endpoints: live.endpoints || [] };
}

export function oneReadyPod(collection) {
  const items = collection?.items;
  if (!Array.isArray(items) || items.length !== 1 || !POD.test(items[0]?.name || '') ||
      items[0].phase !== 'Running' || items[0].ready !== true) throw Error('POD_NOT_UNIQUE_AND_READY');
  return items[0].name;
}

export function parseTerminalOutput(output) {
  const clean = output.replace(/\x1b\[[0-?]*[ -/]*[@-~]/g, '');
  const lines = clean.split(/\r?\n/).map(line => line.trim());
  const find = expression => lines.map(line => line.match(expression)).filter(Boolean);
  const health = find(/^__MRFPS_HEALTH__(\d+)$/);
  const start = find(/^__MRFPS_START__(\d+):(\d+)$/);
  const fingerprint = find(/^__MRFPS_FP__([a-f0-9]{64})$/);
  if (health.length !== 1 || health[0][1] !== '0' || start.length !== 1 || fingerprint.length !== 1 ||
      !lines.includes('__MRFPS_DONE__')) throw Error('GAME_RUNTIME_EVIDENCE_INCOMPLETE');
  return { fingerprint: fingerprint[0][1], pid: start[0][1], start_ticks: start[0][2] };
}

export async function shellProbe(url, headers) {
  return await new Promise((resolveProbe, rejectProbe) => {
    let data = '', done = false;
    const socket = new WebSocket(url, { headers, handshakeTimeout: 15000, maxPayload: 65536 });
    const timer = setTimeout(() => finish(Error('EXEC_TIMEOUT')), 25000);
    function finish(error, value) {
      if (done) return;
      done = true;
      clearTimeout(timer);
      socket.terminate();
      if (error) rejectProbe(error); else resolveProbe(value);
    }
    socket.on('open', () => socket.send(Buffer.from(SHELL_COMMAND, 'utf8')));
    socket.on('message', chunk => {
      data += chunk.toString('utf8');
      if (Buffer.byteLength(data) > 65536) return finish(Error('EXEC_OUTPUT_TOO_LARGE'));
      if (/(?:^|\n)__MRFPS_DONE__\r?\n/.test(data)) {
        try { finish(null, parseTerminalOutput(data)); } catch { finish(Error('GAME_RUNTIME_EVIDENCE_INCOMPLETE')); }
      }
    });
    socket.on('error', () => finish(Error('EXEC_CONNECTION_FAILED')));
    socket.on('close', () => finish(Error('EXEC_CLOSED_BEFORE_EVIDENCE')));
  });
}

function credentialPath() {
  if (process.env.HETEROCLOUD_CREDENTIALS_FILE) return resolve(process.env.HETEROCLOUD_CREDENTIALS_FILE);
  if (process.platform === 'win32') {
    if (!process.env.APPDATA) throw Error('HETEROCLOUD_CREDENTIAL_PATH_REQUIRED');
    return join(process.env.APPDATA, 'heterocloud', 'credentials.json');
  }
  if (process.platform === 'darwin') return join(homedir(), 'Library', 'Application Support', 'heterocloud', 'credentials.json');
  return join(process.env.XDG_CONFIG_HOME || join(homedir(), '.config'), 'heterocloud', 'credentials.json');
}

async function main() {
  const args = process.argv.slice(2), options = {};
  while (args.length) {
    const flag = args.shift();
    if (!['--state', '--expected-image'].includes(flag) || !args.length || options[flag]) throw Error('ARGUMENTS_INVALID');
    options[flag] = args.shift();
  }
  if (!options['--state'] || !options['--expected-image']) throw Error('STATE_AND_IMAGE_REQUIRED');
  const state = JSON.parse((await readFile(resolve(options['--state']), 'utf8')).replace(/^\uFEFF/, ''));
  if (!UUID.test(state.service_id || '') || !IMAGE.test(options['--expected-image'])) throw Error('OWNED_STATE_OR_IMAGE_INVALID');
  const credentials = JSON.parse((await readFile(credentialPath(), 'utf8')).replace(/^\uFEFF/, ''));
  const profile = credentials.profiles?.[ORIGIN + '/'];
  if (profile?.organization_id !== ORGANIZATION || typeof profile?.access_token !== 'string' || !profile.access_token) {
    throw Error('HETEROCLOUD_LOGIN_REQUIRED');
  }
  const headers = { Authorization: 'Bearer ' + profile.access_token, 'User-Agent': 'heterocloud-cli', Accept: 'application/json' };
  const resource = `/api/v1/organizations/${ORGANIZATION}/flash/services/${state.service_id}`;
  async function get(path) {
    const response = await fetch(ORIGIN + path, { headers, redirect: 'error', signal: AbortSignal.timeout(20000) });
    if (!response.ok) throw Error('FLASH_API_REQUEST_FAILED');
    return await response.json();
  }
  async function snapshot() {
    const service = await get(resource);
    const view = ownedReadyService(state, service, options['--expected-image']);
    return { ...view, pod: oneReadyPod(await get(resource + '/containers')) };
  }
  const before = await snapshot();
  const wsUrl = 'wss://' + new URL(ORIGIN).host + resource + '/exec?pod=' + encodeURIComponent(before.pod);
  const execHeaders = { Authorization: headers.Authorization, Origin: ORIGIN, 'User-Agent': headers['User-Agent'] };
  const first = await shellProbe(wsUrl, execHeaders);
  const middle = await snapshot();
  const second = await shellProbe(wsUrl, execHeaders);
  const after = await snapshot();
  if (before.generation !== middle.generation || before.generation !== after.generation ||
      before.pod !== middle.pod || before.pod !== after.pod ||
      first.fingerprint !== second.fingerprint || first.pid !== second.pid || first.start_ticks !== second.start_ticks) {
    throw Error('RUNTIME_CHANGED_DURING_VERIFICATION');
  }
  process.stdout.write(JSON.stringify({ service_id: state.service_id, generation: after.generation, pod: after.pod,
    image: options['--expected-image'], fingerprint: second.fingerprint, udp_socket_health_verified: true,
    endpoints: after.endpoints, internet_udp_verified: false }) + '\n');
}

if (process.argv[1] && resolve(process.argv[1]) === fileURLToPath(import.meta.url)) {
  main().catch(error => {
    const code = /^[A-Z0-9_]+$/.test(error.message || '') ? error.message : 'RUNTIME_PROBE_FAILED';
    process.stderr.write('MRFPS_FLASH_RUNTIME_ERROR ' + code + '\n');
    process.exitCode = 1;
  });
}
