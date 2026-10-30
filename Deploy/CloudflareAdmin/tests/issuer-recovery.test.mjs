// Exercise the actual CLI ensureCodes function with deterministic local mocks.
import test from 'node:test';
import assert from 'node:assert/strict';
import { readFileSync } from 'node:fs';
import { join } from 'node:path';
import vm from 'node:vm';

const source=readFileSync(new URL('../admin-cli.mjs',import.meta.url),'utf8');
const implementation=source.slice(source.indexOf('async function ensureCodes('),source.indexOf('function checkExchange('));

function fixture(remoteCodes, publishResult={ok:true,consumed:false}, options={}) {
  const now=Math.floor(Date.now()/1000), owner='fixture-owner';
  const pending={code:'111111',body:{role:'player',id:'old-id',owner,expires_at:now+25,token:'fixture-token'}};
  const fresh={code:'222222',body:{role:'player',id:'new-id',owner,expires_at:now+29,token:'fixture-token-2'}};
  const state={owner,registered:true,stopped:false,roles:{player:{code:pending.code,id:'old-id',expires_at:now+25,pending}}};
  const calls={published:[],writes:[],unlinks:[],generated:0};
  const globals={
    opts:{role:'player',...options},roles:['player','inspector','admin'],stateDir:'fixture',join,
    required:name=>{if(name!=='fingerprint') throw Error('bad fixture'); return 'a'.repeat(64);},
    call:async (path,body)=>{
      if(path.startsWith('/api/admin/status')) return {owner_state:'active',codes:remoteCodes(now,owner)};
      if(path==='/api/admin/publish') {
        calls.published.push(body);
        return {ok:true,id:body.id,expires_at:body.expires_at,...publishResult};
      }
      throw Error('unexpected call');
    },
    newCode:async()=>{calls.generated++;return fresh;},issuerSubject:()=> 'fixture-subject',
    saveState:async()=>{},writePrivate:async(path,data)=>{calls.writes.push({path,data});},
    unlink:async path=>{calls.unlinks.push(path);},
  };
  const context=vm.createContext(globals);
  vm.runInContext(implementation+'\nglobalThis.ensure=ensureCodes;',context);
  return {state,calls,ensure:context.ensure,now};
}

test('lost response followed by consumption generates a new issuance ID',async()=>{
  const f=fixture((now,owner)=>[{role:'player',owner,id:'old-id',expires_at:now+25,consumed:1}]);
  await f.ensure(f.state,{});
  assert.equal(f.calls.generated,1); assert.equal(f.calls.published[0].id,'new-id');
  assert.equal(f.calls.writes[0].data,'222222\n'); assert.equal(f.state.roles.player.id,'new-id');
});

test('consumption racing a retry is not saved or returned as a live code',async()=>{
  const f=fixture(()=>[],{ok:true,reused:true,consumed:true});
  await assert.rejects(f.ensure(f.state,{}),error=>error.code==='issuance_consumed');
  assert.equal(f.calls.generated,0); assert.equal(f.calls.published[0].id,'old-id');
  assert.equal(f.calls.writes.length,0); assert.equal(f.calls.unlinks.length,1);
  assert.equal(f.state.roles.player.consumed,true); assert.equal('pending' in f.state.roles.player,false);
});

test('an active identical pending issuance is recovered without publishing again',async()=>{
  const f=fixture((now,owner)=>[{role:'player',owner,id:'old-id',expires_at:now+25,consumed:0}]);
  await f.ensure(f.state,{});
  assert.equal(f.calls.published.length,0); assert.equal(f.calls.generated,0);
  assert.equal(f.calls.writes[0].data,'111111\n'); assert.equal('pending' in f.state.roles.player,false);
});

test('a pending ID replaced remotely fails closed without clobbering it',async()=>{
  const f=fixture((now,owner)=>[{role:'player',owner,id:'different-id',expires_at:now+25,consumed:0}]);
  await assert.rejects(f.ensure(f.state,{}),error=>error.code==='issuer_state_changed');
  assert.equal(f.calls.published.length,0); assert.equal(f.calls.generated,0); assert.equal(f.calls.writes.length,0);
});

test('changed expiry is rejected unless an identical receipt is confirmed',async()=>{
  const f=fixture((now,owner)=>[{role:'player',owner,id:'old-id',expires_at:now+24,consumed:0}]);
  await assert.rejects(f.ensure(f.state,{}),error=>error.code==='issuer_state_changed');
  assert.equal(f.calls.published.length,1); assert.equal(f.calls.writes.length,0);
});

test('lost response with a shortened expiry recovers only the same receipt',async()=>{
  const response={reused:true,consumed:false};
  const f=fixture((now,owner)=>[{role:'player',owner,id:'old-id',expires_at:now+24,consumed:0}],response);
  response.expires_at=f.now+24;
  await f.ensure(f.state,{});
  assert.equal(f.calls.published.length,1); assert.equal(f.calls.published[0].id,'old-id');
  assert.equal(f.state.roles.player.expires_at,f.now+24);
  assert.equal(f.calls.writes[0].data,'111111\n');
});

test('a newly published code uses the server-clamped expiry',async()=>{
  const response={ok:true,consumed:false};
  const f=fixture(()=>[],response); response.expires_at=f.now+24;
  await f.ensure(f.state,{});
  assert.equal(f.state.roles.player.expires_at,f.now+24);
  assert.equal(f.calls.writes[0].data,'111111\n');
});

test('an explicitly authorized takeover can recover a pending issuance',async()=>{
  const f=fixture(now=>[{role:'player',owner:'another-owner',id:'other-id',expires_at:now+25,consumed:0}],
                  {ok:true,consumed:false},{takeover:'true'});
  await f.ensure(f.state,{});
  assert.equal(f.calls.generated,0); assert.equal(f.calls.published[0].id,'old-id');
  assert.equal(f.calls.published[0].takeover,true);
});

test('a foreign live issuer cannot be replaced without explicit takeover',async()=>{
  const f=fixture(now=>[{role:'player',owner:'another-owner',id:'other-id',expires_at:now+25,consumed:0}]);
  await assert.rejects(f.ensure(f.state,{}),error=>error.code==='owner_conflict');
  assert.equal(f.calls.published.length,0);
});

test('a malformed or mismatched publish response preserves pending and never returns the code',async()=>{
  for (const bad of [{id:'wrong-id'}, {expires_at:0}, {ok:false}, {consumed:'true'}]) {
    const f=fixture(()=>[],bad);
    await assert.rejects(f.ensure(f.state,{}),error=>error.code==='invalid_response');
    assert.equal(f.calls.writes.length,0); assert.equal(f.calls.unlinks.length,0);
    assert.equal(f.state.roles.player.pending.body.id,'old-id');
  }
});
