import test from 'node:test';
import assert from 'node:assert/strict';
import { createRequire } from 'node:module';
import { ownedReadyService, oneReadyPod, parseTerminalOutput, shellProbe, specDigest } from '../flash-runtime.mjs';

const { WebSocketServer } = createRequire(import.meta.url)('ws');
const fingerprint = 'a'.repeat(64);
const image = 'ghcr.io/example/mrfps@sha256:' + 'b'.repeat(64);
function fixture() {
  const state = { project_id:'00000000-0000-4000-8000-000000000001', service_id:'00000000-0000-4000-8000-000000000002',
    deployment_id:'00000000-0000-4000-8000-000000000003', service_name:'mrfps-server' };
  const spec = { image, metadata:{ managed_by:'MRFPS-cloud-deploy', deployment_id:state.deployment_id, service_name:state.service_name } };
  state.observed_spec_sha256 = specDigest(spec);
  const service = { id:state.service_id, organization_id:'019fbda8-eff3-7610-bdd5-8fe89e746c14',
    project_id:state.project_id, name:state.service_name, state:'ready', generation:2, spec,
    status:{ status:{ observed_generation:2, ready_replicas:1, serving_replicas:1, endpoints:[] } } };
  return { state, service };
}

test('requires owned exact spec and currently observed ready generation', () => {
  const { state, service } = fixture();
  assert.equal(ownedReadyService(state, service, image).generation, 2);
  for (const mutate of [
    x => { x.spec.metadata.deployment_id = 'other'; },
    x => { x.spec.image = image.replace('b'.repeat(64), 'c'.repeat(64)); },
    x => { x.generation++; },
    x => { x.status.status.ready_replicas = 0; },
    x => { x.status.status.live_status_unavailable = true; },
  ]) {
    const copy = structuredClone(service); mutate(copy);
    assert.throws(() => ownedReadyService(state, copy, image));
  }
});

test('requires exactly one running, ready pod', () => {
  const pod = { name:'flash-workload-abc', phase:'Running', ready:true };
  assert.equal(oneReadyPod({items:[pod]}), pod.name);
  assert.throws(() => oneReadyPod({items:[]}));
  assert.throws(() => oneReadyPod({items:[pod,pod]}));
  assert.throws(() => oneReadyPod({items:[{...pod,ready:false}]}));
});

test('parses complete runtime markers, exposes failed health and rejects ambiguity', () => {
  const evidence = `__MRFPS_HEALTH__0\r\n# __MRFPS_START__32:678\r\n# __MRFPS_FP__${fingerprint}\r\n# __MRFPS_DONE__\r\n`;
  assert.deepEqual(parseTerminalOutput('shell command echo\r\n' + evidence), {fingerprint,pid:'32',start_ticks:'678',health_exit_code:0});
  assert.equal(parseTerminalOutput(evidence.replace('__MRFPS_HEALTH__0', '__MRFPS_HEALTH__1')).health_exit_code, 1);
  assert.throws(() => parseTerminalOutput(evidence.replace('__MRFPS_DONE__', 'missing')));
  assert.throws(() => parseTerminalOutput(evidence + `__MRFPS_FP__${fingerprint}\r\n`));
});

test('websocket probe sends a binary command and handles fragmented terminal output', async () => {
  const server = new WebSocketServer({ host:'127.0.0.1', port:0 });
  await new Promise(resolve => server.once('listening', resolve));
  try {
    server.once('connection', socket => {
      socket.once('message', (message, isBinary) => {
        assert.equal(isBinary, true);
        assert.match(message.toString(), /mrfps-flash-healthcheck/);
        socket.send('__MRFPS_HEALTH__0\r\n__MRFPS_START__32:678\r\n__MRFPS_');
        socket.send(`FP__${fingerprint}\r\n# __MRFPS_DONE__\r\n`);
      });
    });
    const result = await shellProbe(`ws://127.0.0.1:${server.address().port}`, {});
    assert.deepEqual(result, {fingerprint,pid:'32',start_ticks:'678',health_exit_code:0});
  } finally { await new Promise(resolve => server.close(resolve)); }
});
