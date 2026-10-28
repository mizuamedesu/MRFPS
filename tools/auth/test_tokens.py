import base64
import ctypes
import json
import os
import pathlib
import subprocess
import sys
import tempfile
import unittest

from cryptography.hazmat.primitives import serialization
from cryptography.hazmat.primitives.asymmetric.ed25519 import Ed25519PrivateKey
from mrfps_tokens import TokenError, b64url, encode_token, load_private, load_public, mint, public_pem, secure_write, unb64url, verify


class AdmissionTokens(unittest.TestCase):
    def setUp(self):
        self.key = Ed25519PrivateKey.generate()
        self.now = 1800000000
        self.fingerprint = "a" * 64
        self.token, self.claims = mint(self.key, "alice-01", "player", self.fingerprint, now=self.now)

    def reject(self, claims, reason="AUTH_INVALID", **options):
        with self.assertRaisesRegex(TokenError, reason):
            verify(encode_token(self.key, claims), self.key.public_key(), now=self.now, **options)

    def test_valid_both_roles_and_unique_ids(self):
        self.assertEqual(verify(self.token, self.key.public_key(), now=self.now), self.claims)
        token, claims = mint(self.key, "observer", "inspector", self.fingerprint, now=self.now)
        self.assertEqual(verify(token, self.key.public_key(), role="inspector", now=self.now), claims)
        self.assertNotEqual(claims["jti"], self.claims["jti"])

    def test_admin_requires_exact_signed_role_and_cannot_be_elevated(self):
        token, claims = mint(self.key, "operator-1", "admin", self.fingerprint, now=self.now)
        self.assertEqual(verify(token, self.key.public_key(), role="admin", now=self.now), claims)
        for expected in ("player", "inspector"):
            with self.assertRaisesRegex(TokenError, "AUTH_ROLE"):
                verify(token, self.key.public_key(), role=expected, now=self.now)
        for ordinary in ("player", "inspector"):
            ordinary_token, ordinary_claims = mint(self.key, "ordinary", ordinary, self.fingerprint, now=self.now)
            with self.assertRaisesRegex(TokenError, "AUTH_ROLE"):
                verify(ordinary_token, self.key.public_key(), role="admin", now=self.now)
            header, _, signature = ordinary_token.split(".")
            ordinary_claims["role"] = "admin"
            changed_payload = b64url(json.dumps(ordinary_claims, separators=(",", ":")).encode("ascii"))
            with self.assertRaisesRegex(TokenError, "AUTH_INVALID"):
                verify(header + "." + changed_payload + "." + signature, self.key.public_key(), role="admin", now=self.now)

    def test_wrong_signature(self):
        with self.assertRaisesRegex(TokenError, "AUTH_INVALID"):
            verify(self.token, Ed25519PrivateKey.generate().public_key(), now=self.now)

    def test_wrong_audience_and_role(self):
        self.reject(dict(self.claims, aud="another-server"), "AUTH_AUDIENCE")
        self.reject(dict(self.claims, role="inspector"), "AUTH_ROLE")

    def test_expiry_and_future(self):
        self.reject(dict(self.claims, iat=self.now-120, nbf=self.now-120, exp=self.now-60), "AUTH_EXPIRED")
        self.reject(dict(self.claims, iat=self.now+10, nbf=self.now+10, exp=self.now+60))

    def test_integer_and_ttl_profile(self):
        for field in ("iat", "nbf", "exp"):
            self.reject(dict(self.claims, **{field: True}))
            self.reject(dict(self.claims, **{field: 1.0}))
        self.reject(dict(self.claims, exp=self.claims["iat"]+3601))
        self.reject(dict(self.claims, nbf=self.claims["exp"]))
        self.reject(dict(self.claims, iat=-1))

    def test_safe_subject_audience_uuid(self):
        for subject in ("", "-leading", "x"*65, "user?admin", "日本語"):
            self.reject(dict(self.claims, sub=subject))
        self.reject(dict(self.claims, jti="00000000-0000-4000-8000-00000000000A"))
        self.reject(dict(self.claims, aud="bad?room"))
        self.reject(dict(self.claims, dtls_fp="not-a-fingerprint"))

    def test_no_unknown_or_missing_claims(self):
        self.reject(dict(self.claims, admin=True))
        missing = dict(self.claims)
        del missing["jti"]
        self.reject(missing)

    def test_algorithm_confusion_and_extra_header(self):
        for header in ({"alg":"none","typ":"JWT"}, {"alg":"HS256","typ":"JWT"}, {"alg":"EdDSA","typ":"JWT","kid":"one"}):
            with self.assertRaisesRegex(TokenError, "AUTH_INVALID"):
                verify(encode_token(self.key, self.claims, header), self.key.public_key(), now=self.now)

    def test_compact_base64_and_escaped_duplicate_json(self):
        with self.assertRaises(TokenError):
            unb64url("AB", 64)
        header, payload, signature = self.token.split(".")
        for invalid in (self.token+"=", self.token+"\n", header+"=."+payload+"."+signature, "abc", "x"*2049):
            with self.assertRaises(TokenError):
                verify(invalid, self.key.public_key(), now=self.now)

    def test_signed_duplicate_escaped_and_noncanonical_json_rejected(self):
        payload = json.dumps(self.claims, separators=(",", ":")).encode("ascii")
        header = b'{"alg":"EdDSA","typ":"JWT"}'
        mutations = [
            (header, payload[:-1] + b',"sub":"mallory"}'),
            (header, payload.replace(b'alice-01', b'alice\\u002d01')),
            (b'{"alg":"EdDSA","typ":"JWT","alg":"EdDSA"}', payload),
            (header, b'\xff'),
        ]
        for modified_header, modified_payload in mutations:
            signing = (b64url(modified_header) + '.' + b64url(modified_payload)).encode('ascii')
            token = signing.decode('ascii') + '.' + b64url(self.key.sign(signing))
            with self.assertRaises(TokenError):
                verify(token, self.key.public_key(), now=self.now)

    def test_secure_files_and_encrypted_private_key(self):
        root = pathlib.Path(__file__).resolve().parents[2] / ".local/auth-tests"
        root.mkdir(parents=True, exist_ok=True)
        temporary = tempfile.TemporaryDirectory(prefix="keys-", dir=root)
        directory = pathlib.Path(temporary.name).resolve()
        self.assertTrue(directory.is_relative_to(root.resolve()))
        try:
            private_path = directory / "private.pem"
            public_path = directory / "public.pem"
            password_path = directory / "passphrase"
            password = b"test-only-passphrase-12345"
            secure_write(password_path, password)
            secure_write(private_path, self.key.private_bytes(serialization.Encoding.PEM, serialization.PrivateFormat.PKCS8, serialization.BestAvailableEncryption(password)))
            secure_write(public_path, public_pem(self.key.public_key()))
            loaded = load_private(private_path, password_path, False)
            self.assertEqual(public_pem(loaded.public_key()), public_pem(load_public(public_path)))
            with self.assertRaises(OSError):
                secure_write(private_path, b"must not overwrite")
            if os.name == "nt":
                from ctypes import wintypes
                advapi = ctypes.WinDLL("advapi32", use_last_error=True)
                advapi.GetFileSecurityW.argtypes = [wintypes.LPCWSTR,wintypes.DWORD,ctypes.c_void_p,wintypes.DWORD,ctypes.POINTER(wintypes.DWORD)]
                size = wintypes.DWORD()
                advapi.GetFileSecurityW(str(private_path), 4, None, 0, ctypes.byref(size))
                descriptor = ctypes.create_string_buffer(size.value)
                self.assertTrue(advapi.GetFileSecurityW(str(private_path), 4, descriptor, size, ctypes.byref(size)))
                control = wintypes.WORD()
                revision = wintypes.DWORD()
                self.assertTrue(advapi.GetSecurityDescriptorControl(descriptor, ctypes.byref(control), ctypes.byref(revision)))
                self.assertTrue(control.value & 0x1000, "Private-key DACL must disable inheritance")
                dacl = ctypes.c_void_p()
                present, defaulted = wintypes.BOOL(), wintypes.BOOL()
                advapi.GetSecurityDescriptorDacl.argtypes = [ctypes.c_void_p,ctypes.POINTER(wintypes.BOOL),ctypes.POINTER(ctypes.c_void_p),ctypes.POINTER(wintypes.BOOL)]
                self.assertTrue(advapi.GetSecurityDescriptorDacl(descriptor,ctypes.byref(present),ctypes.byref(dacl),ctypes.byref(defaulted)))
                self.assertTrue(present.value and dacl.value)
                self.assertEqual(ctypes.cast(dacl,ctypes.POINTER(ctypes.c_ushort))[2], 1, "Only the issuer's user ACE should be present")
            else:
                self.assertEqual(private_path.stat().st_mode & 0o777, 0o600)

            cli = pathlib.Path(__file__).with_name('mrfps_tokens.py')
            key_directory = directory / 'cli-issuer'
            generated = subprocess.run([sys.executable,str(cli),'keygen','--directory',str(key_directory),'--passphrase-file',str(password_path)], capture_output=True,text=True)
            self.assertEqual(generated.returncode, 0, generated.stderr)
            self.assertTrue((key_directory / 'issuer-private.pem').read_bytes().startswith(b'-----BEGIN ENCRYPTED PRIVATE KEY-----'))
            token_path = directory / 'client.jwt'
            issued = subprocess.run([sys.executable,str(cli),'issue','--private-key',str(key_directory / 'issuer-private.pem'),'--passphrase-file',str(password_path),'--subject','cli-user','--role','player','--dtls-fingerprint',self.fingerprint,'--output',str(token_path)], capture_output=True,text=True)
            self.assertEqual(issued.returncode, 0, issued.stderr)
            self.assertNotIn(token_path.read_text('ascii'), issued.stdout)
            self.assertNotIn('BEGIN ENCRYPTED PRIVATE KEY', generated.stdout + issued.stdout)

            from create_test_credentials import create
            fixture_directory = directory / 'pin-fixtures'
            fixture = create(fixture_directory, '0' * 64, inspectors=1)
            trusted_public = load_public(pathlib.Path(fixture['public_key_path']))
            for role in ('player', 'inspector'):
                signed_wrong_pin = pathlib.Path(fixture['tokens']['transport-' + role]).read_text('ascii')
                verified = verify(signed_wrong_pin, trusted_public, role=role)
                self.assertEqual(len(verified), 9)
                self.assertEqual(verified['dtls_fp'], '1' * 64)
        finally:
            self.assertTrue(directory.is_relative_to(root.resolve()))
            temporary.cleanup()


if __name__ == "__main__":
    unittest.main(verbosity=2)
