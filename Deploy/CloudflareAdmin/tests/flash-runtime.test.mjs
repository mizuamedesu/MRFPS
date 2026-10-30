import test from 'node:test';
import assert from 'node:assert/strict';
import { createRequire } from 'node:module';
import { ownedReadyService, oneReadyPod, parseTerminalOutput, shellProbe, specDigest, stableRosterInstance, sameDirectEndpoints } from '../flash-runtime.mjs';

const { WebSocketServer } = createRequire(import.meta.url)('ws');
const fingerprint = 'a'.repeat(64);
const image = 'ghcr.io/example/mrfps@sha256:' + 'b'.repeat(64);
function fixture() {
  const state = { project_id:'00000000-0000-4000-8000-000000000001', service_id:'00000000-0000-4000-8000-000000000002',
    deployment_id:'00000000-0000-4000-8000-000000000003', service_name:'mrfps-server' };
  const spec = { image, exposure:{type:'public',traffic_mode:'direct'}, ports:[{name:'game-udp',protocol:'udp',container_port:7777,service_port:30000}],
    metadata:{ managed_by:'MRFPS-cloud-deploy', deployment_id:state.deployment_id, service_name:state.service_name } };
  state.observed_spec_sha256 = specDigest(spec);
  const service = { id:state.service_id, organization_id:'019fbda8-eff3-7610-bdd5-8fe89e746c14',
    project_id:state.project_id, name:state.service_name, state:'ready', generation:2, spec,
    status:{ status:{ observed_generation:2, ready_replicas:1, serving_replicas:1, endpoints:[{name:'game-udp',host:'203.0.113.1',port:30000,protocol:'udp'}] } } };
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

test('requires the status endpoint to match the assigned direct UDP port', () => {
  for (const mutate of [
    x=>{ x.status.status.endpoints=[]; },
    x=>{ x.status.status.endpoints[0].name='other'; },
    x=>{ x.status.status.endpoints[0].port=30001; },
    x=>{ x.status.status.endpoints[0].protocol='tcp'; },
    x=>{ x.status.status.endpoints[0].host='bad host'; },
  ]) {
    const {state,service}=fixture(); mutate(service);
    assert.throws(()=>ownedReadyService(state,service,image),/DIRECT_UDP_ENDPOINT_UNVERIFIED/);
  }
  const {state,service}=fixture(), before=ownedReadyService(state,service,image);
  service.status.status.endpoints[0].host='203.0.113.2';
  assert.equal(sameDirectEndpoints(before,ownedReadyService(state,service,image)),false);
});

test('requires direct public ingress and exactly one allocated game port', () => {
  for (const mutate of [
    x => { x.spec.exposure.traffic_mode='forwarded'; },
    x => { x.spec.exposure.type='internal'; },
    x => { x.spec.ports[0].protocol='tcp'; },
    x => { x.spec.ports[0].service_port=0; },
    x => { x.spec.ports.push({...x.spec.ports[0]}); },
  ]) {
    const {state,service}=fixture(); mutate(service); state.observed_spec_sha256=specDigest(service.spec);
    assert.throws(()=>ownedReadyService(state,service,image),/DIRECT_UDP_GAME_PORT_UNVERIFIED/);
  }
  const {state,service}=fixture();
  assert.equal(ownedReadyService(state,service,image).udp_service_port,30000);
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
  assert.deepEqual(parseTerminalOutput('shell command echo\r\n' + evidence), {fingerprint,pid:'32',start_ticks:'678',health_exit_code:0,roster_instance:null});
  assert.equal(parseTerminalOutput(evidence.replace('__MRFPS_HEALTH__0', '__MRFPS_HEALTH__1')).health_exit_code, 1);
  assert.throws(() => parseTerminalOutput(evidence.replace('# __MRFPS_START__32:678\r\n', '')));
  assert.throws(() => parseTerminalOutput(evidence.replace('__MRFPS_DONE__', 'missing')));
  assert.throws(() => parseTerminalOutput(evidence + `__MRFPS_FP__${fingerprint}\r\n`));
});

test('roster proof requires one matching PID and identical instances on both probes', () => {
  const id='c'.repeat(32);
  const evidence=`__MRFPS_HEALTH__0\n__MRFPS_START__32:678\n__MRFPS_FP__${fingerprint}\n__MRFPS_ROSTER__${id}:32\n__MRFPS_DONE__\n`;
  const first=parseTerminalOutput(evidence);
  assert.equal(first.roster_instance,id);
  assert.equal(stableRosterInstance(first,parseTerminalOutput(evidence)),id);
  const otherPid=parseTerminalOutput(evidence.replace(`${id}:32`,`${id}:33`));
  assert.equal(otherPid.roster_instance,null); assert.equal(stableRosterInstance(first,otherPid),null);
  assert.equal(stableRosterInstance(first,parseTerminalOutput(evidence.replace(id,'d'.repeat(32)))),null);
  assert.equal(parseTerminalOutput(evidence.replace('__MRFPS_DONE__',`__MRFPS_ROSTER__${id}:32\n__MRFPS_DONE__`)).roster_instance,null);
});

test('websocket probe sends a binary command and handles fragmented terminal output', async () => {
  const server = new WebSocketServer({ host:'127.0.0.1', port:0 });
  await new Promise(resolve => server.once('listening', resolve));
  try {
    server.once('connection', socket => {
      socket.once('message', (message, isBinary) => {
        assert.equal(isBinary, true);
        assert.match(message.toString(), /mrfps-flash-healthcheck/);
        assert.match(message.toString(), /NF == 1/);
        socket.send('__MRFPS_HEALTH__0\r\n__MRFPS_START__32:678\r\n__MRFPS_');
        socket.send(`FP__${fingerprint}\r\n# __MRFPS_DONE__\r\n`);
      });
    });
    const result = await shellProbe(`ws://127.0.0.1:${server.address().port}`, {});
    assert.deepEqual(result, {fingerprint,pid:'32',start_ticks:'678',health_exit_code:0,roster_instance:null});
  } finally { await new Promise(resolve => server.close(resolve)); }
});
