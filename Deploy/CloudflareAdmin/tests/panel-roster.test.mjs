import test from 'node:test';
import assert from 'node:assert/strict';
import { readFileSync } from 'node:fs';
import vm from 'node:vm';

const source=readFileSync(new URL('../src/panel.browser.js',import.meta.url),'utf8');
const implementation=source.slice(source.indexOf('let rosterBusy = false;'),source.indexOf('void refreshParticipants();'));
class Node {
  constructor() { this.textContent=''; this.children=[]; }
  appendChild(child) { this.children.push(child); }
  replaceChildren() { this.children=[]; }
}
function fixture() {
  let elapsed=1000, response;
  const nodes={participants:new Node(),'participants-state':new Node()};
  const globals={performance:{now:()=>elapsed},$:name=>nodes[name],document:{createElement:()=>new Node()},
    AbortSignal:{timeout:()=>({})},fetch:async()=>{ elapsed+=100; if (response instanceof Error) throw response;
      return {ok:true,json:async()=>response}; }};
  const context=vm.createContext(globals);
  vm.runInContext(implementation+'\nglobalThis.refresh=refreshParticipants;globalThis.expire=expireParticipants;',context);
  return {nodes,context,setResponse:value=>{response=value;},setTime:value=>{elapsed=value;}};
}

test('panel renders plain text and expires ping and roster using monotonic time',async()=>{
  const f=fixture();
  f.setResponse({online:true,expires_in_ms:10000,participants:[{id:1,name:'<img onerror=alert(1)>',role:'player',ping_ms:42,ping_expires_in_ms:5000},{id:0,name:'BOT',role:'bot',ping_ms:null,ping_expires_in_ms:null}]});
  await f.context.refresh();
  const row=f.nodes.participants.children[0];
  assert.equal(row.children[0].textContent,'<img onerror=alert(1)>');
  assert.equal(row.children[2].textContent,'42 ms');
  assert.equal(f.nodes.participants.children[1].children[1].textContent,'Bot');
  assert.equal(f.nodes.participants.children[1].children[2].textContent,'不明');
  f.setTime(6001); f.context.expire();
  assert.equal(row.children[2].textContent,'不明');
  assert.equal(f.nodes.participants.children.length,2);
  f.setTime(11001); f.context.expire();
  assert.equal(f.nodes.participants.children.length,0);
  assert.equal(f.nodes['participants-state'].textContent,'サーバー未接続');
});

test('a failed or already expired response never preserves an old list',async()=>{
  const f=fixture();
  f.setResponse({online:true,expires_in_ms:9000,participants:[{id:1,name:'user',role:'admin',ping_ms:null,ping_expires_in_ms:null}]});
  await f.context.refresh(); assert.equal(f.nodes.participants.children.length,1);
  f.setResponse(new Error('network')); await f.context.refresh();
  assert.equal(f.nodes.participants.children.length,0); assert.equal(f.nodes['participants-state'].textContent,'通信エラー');
  f.setResponse({online:true,expires_in_ms:50,participants:[{id:1,name:'user',role:'admin'}]});
  await f.context.refresh(); assert.equal(f.nodes.participants.children.length,0);
  assert.equal(f.nodes['participants-state'].textContent,'サーバー未接続');
});
