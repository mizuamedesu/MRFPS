// Run the actual panel functions with an intentionally wrong wall clock.
import test from 'node:test';
import assert from 'node:assert/strict';
import { readFileSync } from 'node:fs';
import vm from 'node:vm';

const source=readFileSync(new URL('../src/panel.browser.js',import.meta.url),'utf8');
const implementation=source.slice(source.indexOf('function render()'),source.indexOf('async function tick('));

function fixture(response) {
  let elapsed=1000;
  const current={}, nodes={};
  const globals={current,roles:['inspector'],running:true,owner:'fixture-owner',fingerprint:'a'.repeat(64),key:{},
    performance:{now:()=>elapsed},
    $:name=>(nodes[name] ||= {textContent:''}),
    newCode:async()=>({code:'123456',body:{id:'fixture-id',expires_at:130}}),
    api:async()=>{elapsed+=250; return response;}};
  const context=vm.createContext(globals);
  vm.runInContext(implementation+'\nglobalThis.runPublish=publish;globalThis.runRender=render;',context);
  return {current,nodes,context,advance:value=>{elapsed=value;}};
}

test('panel uses server expiry and monotonic time instead of local wall clock',async()=>{
  const f=fixture({ok:true,id:'fixture-id',expires_at:130,server_now:100});
  await f.context.runPublish('inspector'); f.context.runRender();
  assert.equal(f.nodes['code-inspector'].textContent,'123456');
  assert.equal(f.nodes['time-inspector'].textContent,'残り 30 秒');
  f.advance(31000); f.context.runRender();
  assert.equal(f.nodes['code-inspector'].textContent,'------');
});

test('panel refuses invalid or extended server expiry',async()=>{
  for (const bad of [
    {ok:true,id:'fixture-id',expires_at:131,server_now:100},
    {ok:true,id:'fixture-id',expires_at:130,server_now:undefined},
    {ok:true,id:'fixture-id',expires_at:100,server_now:100},
    {ok:true,id:'fixture-id',expires_at:130,server_now:100,consumed:true},
  ]) {
    const f=fixture(bad); await assert.rejects(f.context.runPublish('inspector'));
    assert.equal(f.current.inspector,undefined);
  }
});