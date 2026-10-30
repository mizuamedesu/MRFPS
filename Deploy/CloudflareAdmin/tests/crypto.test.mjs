import { test } from 'node:test';
import assert from 'node:assert/strict';
import { createPublicKey, verify } from 'node:crypto';
import { generateBackup, restoreBackup, newCode } from '../src/crypto.browser.js';

test('encrypted backup restores the same non-extractable issuer and rejects wrong password', async () => {
  const key = await generateBackup('only-a-test-passphrase');
  assert.equal(key.privateKey.extractable, false);
  assert(!JSON.stringify(key.backup).includes('PRIVATE KEY'));
  await assert.rejects(() => restoreBackup(key.backup, 'another-test-password'));
  const restored = await restoreBackup(key.backup, 'only-a-test-passphrase');
  assert.equal(restored.publicKey, key.publicKey);
  assert.equal(restored.privateKey.extractable, false);
  const other = await generateBackup('only-a-test-passphrase');
  await assert.rejects(() => restoreBackup({ ...key.backup, public_key: other.publicKey }, 'only-a-test-passphrase'));
});

test('one-time code and signed token match the strict server profile for each role', async () => {
  const key = await generateBackup('only-a-test-passphrase');
  for (const role of ['player', 'inspector', 'admin']) {
    const before = Math.floor(Date.now() / 1000);
    const entry = await newCode(role, crypto.randomUUID(), 'ab'.repeat(32), key);
    assert.match(entry.code, /^\d{6}$/);
    assert(entry.body.expires_at >= before + 299 && entry.body.expires_at <= before + 301);
    const [header, payload, sig] = entry.body.token.split('.');
    assert(verify(null, Buffer.from(header + '.' + payload), createPublicKey(key.publicKey), Buffer.from(sig, 'base64url')));
    assert.deepEqual(JSON.parse(Buffer.from(header, 'base64url')), { alg:'EdDSA', typ:'JWT' });
    const claims = JSON.parse(Buffer.from(payload, 'base64url'));
    assert.deepEqual(Object.keys(claims).sort(), ['aud','dtls_fp','exp','iat','iss','jti','nbf','role','sub']);
    assert.equal(claims.role, role);
    assert.equal(claims.dtls_fp, 'ab'.repeat(32));
    assert.equal(claims.exp - claims.iat, 3600);
  }
});
