"""Offline Ed25519 issuer/verifier for the strict MRFPS admission-token profile."""
from __future__ import annotations

import argparse
import base64
import ctypes
import getpass
import hashlib
import json
import os
import pathlib
import re
import sys
import time
import uuid

from cryptography.exceptions import InvalidSignature
from cryptography.hazmat.primitives import serialization
from cryptography.hazmat.primitives.asymmetric.ed25519 import Ed25519PrivateKey, Ed25519PublicKey

ISSUER = "mrfps-issuer"
SUBJECT = re.compile(r"[A-Za-z0-9][A-Za-z0-9._-]{0,63}\Z", re.ASCII)
AUDIENCE = re.compile(r"[A-Za-z0-9][A-Za-z0-9._:/-]{0,127}\Z", re.ASCII)
JTI = re.compile(r"[0-9a-f]{8}-[0-9a-f]{4}-4[0-9a-f]{3}-[89ab][0-9a-f]{3}-[0-9a-f]{12}\Z")
HEADER = {"alg": "EdDSA", "typ": "JWT"}
CLAIMS = {"iss", "aud", "sub", "role", "iat", "nbf", "exp", "jti", "dtls_fp"}
FINGERPRINT = re.compile(r"[0-9a-f]{64}\Z")


class TokenError(ValueError):
    pass


def b64url(data: bytes) -> str:
    return base64.urlsafe_b64encode(data).rstrip(b"=").decode("ascii")


def unb64url(value: str, limit: int) -> bytes:
    if not value or len(value) > limit * 2 or not re.fullmatch(r"[A-Za-z0-9_-]+", value):
        raise TokenError("AUTH_INVALID")
    try:
        decoded = base64.b64decode(value + "=" * (-len(value) % 4), altchars=b"-_", validate=True)
    except ValueError as exc:
        raise TokenError("AUTH_INVALID") from exc
    if len(decoded) > limit or b64url(decoded) != value:
        raise TokenError("AUTH_INVALID")
    return decoded


def strict_json(data: bytes) -> dict:
    if b"\\" in data:
        raise TokenError("AUTH_INVALID")

    def unique(pairs):
        result = {}
        for key, value in pairs:
            if key in result:
                raise TokenError("AUTH_INVALID")
            result[key] = value
        return result

    try:
        result = json.loads(data.decode("ascii"), object_pairs_hook=unique,
                            parse_int=lambda value: int(value) if re.fullmatch(r"0|[1-9][0-9]*", value) else (_ for _ in ()).throw(TokenError("AUTH_INVALID")),
                            parse_float=lambda _: (_ for _ in ()).throw(TokenError("AUTH_INVALID")),
                            parse_constant=lambda _: (_ for _ in ()).throw(TokenError("AUTH_INVALID")))
    except (ValueError, UnicodeError) as exc:
        raise TokenError("AUTH_INVALID") from exc
    if not isinstance(result, dict):
        raise TokenError("AUTH_INVALID")
    return result


def validate_claims(claims: dict, audience: str, role: str, now: int | None = None) -> None:
    now = int(time.time()) if now is None else now
    if set(claims) != CLAIMS or claims.get("iss") != ISSUER:
        raise TokenError("AUTH_INVALID")
    for name, pattern in (("sub", SUBJECT), ("aud", AUDIENCE), ("jti", JTI), ("dtls_fp", FINGERPRINT)):
        if not isinstance(claims[name], str) or not pattern.fullmatch(claims[name]):
            raise TokenError("AUTH_INVALID")
    if claims["aud"] != audience:
        raise TokenError("AUTH_AUDIENCE")
    if claims["role"] not in ("player", "inspector", "admin") or claims["role"] != role:
        raise TokenError("AUTH_ROLE")
    for name in ("iat", "nbf", "exp"):
        if type(claims[name]) is not int or not 0 <= claims[name] <= 9007199254740991:
            raise TokenError("AUTH_INVALID")
    if not (claims["iat"] <= claims["nbf"] < claims["exp"]) or not 1 <= claims["exp"] - claims["iat"] <= 3600:
        raise TokenError("AUTH_INVALID")
    if claims["exp"] + 5 <= now:
        raise TokenError("AUTH_EXPIRED")
    if claims["iat"] > now + 5 or claims["nbf"] > now + 5:
        raise TokenError("AUTH_INVALID")


def encode_token(key: Ed25519PrivateKey, claims: dict, header: dict | None = None) -> str:
    """Signing primitive; production CLI validates claims before calling it."""
    header_json = json.dumps(HEADER if header is None else header, sort_keys=True, separators=(",", ":"), ensure_ascii=True).encode("ascii")
    payload_json = json.dumps(claims, sort_keys=True, separators=(",", ":"), ensure_ascii=True).encode("ascii")
    signing = (b64url(header_json) + "." + b64url(payload_json)).encode("ascii")
    token = signing.decode("ascii") + "." + b64url(key.sign(signing))
    if len(token) > 2048:
        raise TokenError("Token exceeds the MRFPS size limit.")
    return token


def mint(key: Ed25519PrivateKey, subject: str, role: str, dtls_fingerprint: str, audience: str = "mrfps", ttl: int = 300,
         now: int | None = None) -> tuple[str, dict]:
    now = int(time.time()) if now is None else now
    claims = {"iss": ISSUER, "aud": audience, "sub": subject, "role": role, "iat": now,
              "nbf": now, "exp": now + ttl, "jti": str(uuid.uuid4()), "dtls_fp": dtls_fingerprint}
    validate_claims(claims, audience, role, now)
    return encode_token(key, claims), claims


def verify(token: str, key: Ed25519PublicKey, audience: str = "mrfps", role: str = "player",
           now: int | None = None) -> dict:
    if not isinstance(token, str) or not 1 <= len(token) <= 2048:
        raise TokenError("AUTH_INVALID")
    parts = token.split(".")
    if len(parts) != 3:
        raise TokenError("AUTH_INVALID")
    header = strict_json(unb64url(parts[0], 256))
    if header != HEADER:
        raise TokenError("AUTH_INVALID")
    payload = strict_json(unb64url(parts[1], 1024))
    signature = unb64url(parts[2], 64)
    if len(signature) != 64:
        raise TokenError("AUTH_INVALID")
    try:
        key.verify(signature, (parts[0] + "." + parts[1]).encode("ascii"))
    except (InvalidSignature, ValueError) as exc:
        raise TokenError("AUTH_INVALID") from exc
    validate_claims(payload, audience, role, now)
    return payload


def public_pem(key: Ed25519PublicKey) -> bytes:
    return key.public_bytes(serialization.Encoding.PEM, serialization.PublicFormat.SubjectPublicKeyInfo)


def load_public(path: pathlib.Path) -> Ed25519PublicKey:
    data = path.read_bytes()
    if len(data) > 2048 or b"PRIVATE" in data or data.count(b"-----BEGIN PUBLIC KEY-----") != 1:
        raise TokenError("Expected one Ed25519 SPKI public key.")
    try:
        key = serialization.load_pem_public_key(data)
    except (ValueError, TypeError) as exc:
        raise TokenError("Invalid Ed25519 public key.") from exc
    if not isinstance(key, Ed25519PublicKey):
        raise TokenError("The public key must be Ed25519.")
    if public_pem(key).strip() != data.replace(b"\r\n", b"\n").strip():
        raise TokenError("Expected one canonical Ed25519 SPKI public key.")
    return key


def secure_write(path: pathlib.Path, data: bytes) -> None:
    """Create without overwriting; Windows ACL is restricted before any secret is written."""
    path = path.resolve()
    path.parent.mkdir(parents=True, exist_ok=True, mode=0o700)
    if os.name != "nt":
        fd = os.open(path, os.O_WRONLY | os.O_CREAT | os.O_EXCL, 0o600)
    else:
        import msvcrt
        from ctypes import wintypes

        kernel = ctypes.WinDLL("kernel32", use_last_error=True)
        advapi = ctypes.WinDLL("advapi32", use_last_error=True)
        token = wintypes.HANDLE()
        kernel.GetCurrentProcess.restype = wintypes.HANDLE
        kernel.CloseHandle.argtypes = [wintypes.HANDLE]
        kernel.LocalFree.argtypes = [wintypes.HLOCAL]
        advapi.OpenProcessToken.argtypes = [wintypes.HANDLE, wintypes.DWORD, ctypes.POINTER(wintypes.HANDLE)]
        advapi.GetTokenInformation.argtypes = [wintypes.HANDLE, ctypes.c_int, ctypes.c_void_p, wintypes.DWORD, ctypes.POINTER(wintypes.DWORD)]
        advapi.ConvertSidToStringSidW.argtypes = [ctypes.c_void_p, ctypes.POINTER(wintypes.LPWSTR)]
        advapi.ConvertStringSecurityDescriptorToSecurityDescriptorW.argtypes = [wintypes.LPCWSTR, wintypes.DWORD, ctypes.POINTER(ctypes.c_void_p), ctypes.c_void_p]
        if not advapi.OpenProcessToken(kernel.GetCurrentProcess(), 8, ctypes.byref(token)):
            raise ctypes.WinError(ctypes.get_last_error())
        sid_text = wintypes.LPWSTR()
        descriptor = ctypes.c_void_p()
        handle = None
        try:
            needed = wintypes.DWORD()
            advapi.GetTokenInformation(token, 1, None, 0, ctypes.byref(needed))
            info = ctypes.create_string_buffer(needed.value)
            if not advapi.GetTokenInformation(token, 1, info, needed, ctypes.byref(needed)):
                raise ctypes.WinError(ctypes.get_last_error())
            sid = ctypes.cast(info, ctypes.POINTER(ctypes.c_void_p))[0]
            if not advapi.ConvertSidToStringSidW(sid, ctypes.byref(sid_text)):
                raise ctypes.WinError(ctypes.get_last_error())
            if not advapi.ConvertStringSecurityDescriptorToSecurityDescriptorW("D:P(A;;FA;;;" + sid_text.value + ")", 1, ctypes.byref(descriptor), None):
                raise ctypes.WinError(ctypes.get_last_error())

            class SecurityAttributes(ctypes.Structure):
                _fields_ = [("length", wintypes.DWORD), ("descriptor", ctypes.c_void_p), ("inherit", wintypes.BOOL)]

            security = SecurityAttributes(ctypes.sizeof(SecurityAttributes), descriptor, False)
            kernel.CreateFileW.argtypes = [wintypes.LPCWSTR, wintypes.DWORD, wintypes.DWORD, ctypes.POINTER(SecurityAttributes), wintypes.DWORD, wintypes.DWORD, wintypes.HANDLE]
            kernel.CreateFileW.restype = wintypes.HANDLE
            handle = kernel.CreateFileW(str(path), 0x40000000, 0, ctypes.byref(security), 1, 0x80, None)
            if handle == wintypes.HANDLE(-1).value:
                handle = None
                raise ctypes.WinError(ctypes.get_last_error())
            fd = msvcrt.open_osfhandle(handle, os.O_WRONLY | os.O_BINARY)
            handle = None  # fd owns the native handle now.
        finally:
            if handle is not None:
                kernel.CloseHandle(handle)
            if descriptor:
                kernel.LocalFree(descriptor)
            if sid_text:
                kernel.LocalFree(sid_text)
            kernel.CloseHandle(token)
    with os.fdopen(fd, "wb") as output:
        output.write(data)
        output.flush()
        os.fsync(output.fileno())


def read_passphrase(path: pathlib.Path | None, confirm: bool = False) -> bytes:
    if path:
        password = path.read_bytes().rstrip(b"\r\n")
    else:
        if not sys.stdin.isatty():
            raise TokenError("A passphrase file is required when no interactive terminal is available.")
        password = getpass.getpass("Private-key passphrase: ").encode("utf-8")
        if confirm and password != getpass.getpass("Confirm passphrase: ").encode("utf-8"):
            raise TokenError("Passphrases did not match.")
    if len(password) < 12:
        raise TokenError("Use a passphrase of at least 12 bytes.")
    return password


def load_private(path: pathlib.Path, passphrase_file: pathlib.Path | None, dev_unencrypted: bool) -> Ed25519PrivateKey:
    data = path.read_bytes()
    if len(data) > 16384:
        raise TokenError("Invalid private-key file.")
    encrypted = data.startswith(b"-----BEGIN ENCRYPTED PRIVATE KEY-----")
    if not encrypted and not dev_unencrypted:
        raise TokenError("Unencrypted keys require explicit --dev-unencrypted for fixtures.")
    password = read_passphrase(passphrase_file) if encrypted else None
    try:
        key = serialization.load_pem_private_key(data, password)
    except (ValueError, TypeError) as exc:
        raise TokenError("Unable to load the private key; check its passphrase and format.") from exc
    if not isinstance(key, Ed25519PrivateKey):
        raise TokenError("The private key must be Ed25519.")
    return key


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    commands = parser.add_subparsers(dest="command", required=True)
    keygen = commands.add_parser("keygen", help="Generate encrypted PKCS8 private key and SPKI public key offline")
    keygen.add_argument("--directory", required=True, type=pathlib.Path)
    keygen.add_argument("--passphrase-file", type=pathlib.Path)
    keygen.add_argument("--dev-unencrypted", action="store_true", help="Explicitly generate an unencrypted fixture-only private key")
    issue = commands.add_parser("issue", help="Mint one one-connection JWT into a restricted file; never print the bearer token")
    issue.add_argument("--private-key", required=True, type=pathlib.Path)
    issue.add_argument("--passphrase-file", type=pathlib.Path)
    issue.add_argument("--dev-unencrypted", action="store_true")
    issue.add_argument("--subject", required=True)
    issue.add_argument("--role", choices=("player", "inspector", "admin"), required=True)
    issue.add_argument("--audience", default="mrfps")
    issue.add_argument("--dtls-fingerprint", required=True, help="Active server DTLS certificate SHA256 fingerprint (64 lowercase hex)")
    issue.add_argument("--ttl", type=int, default=300)
    issue.add_argument("--output", required=True, type=pathlib.Path)
    inspect = commands.add_parser("verify", help="Verify a token file with a public key and print claims only; replay is enforced by the server")
    inspect.add_argument("--public-key", required=True, type=pathlib.Path)
    inspect.add_argument("--token-file", required=True, type=pathlib.Path)
    inspect.add_argument("--audience", default="mrfps")
    inspect.add_argument("--role", choices=("player", "inspector", "admin"), required=True)
    inspect.add_argument("--dtls-fingerprint", help="Optional active-server fingerprint match check")
    args = parser.parse_args()
    try:
        if args.command == "keygen":
            if args.dev_unencrypted and args.passphrase_file:
                raise TokenError("Choose encrypted keys or explicit fixture-only unencrypted keys.")
            private_path = args.directory / "issuer-private.pem"
            public_path = args.directory / "issuer-public.pem"
            if private_path.exists() or public_path.exists():
                raise TokenError("Key files already exist; choose a new directory.")
            encryption = serialization.NoEncryption() if args.dev_unencrypted else serialization.BestAvailableEncryption(read_passphrase(args.passphrase_file, confirm=True))
            key = Ed25519PrivateKey.generate()
            secure_write(private_path, key.private_bytes(serialization.Encoding.PEM, serialization.PrivateFormat.PKCS8, encryption))
            secure_write(public_path, public_pem(key.public_key()))
            print(json.dumps({"private_key_path": str(private_path.resolve()), "public_key_path": str(public_path.resolve()), "public_key_sha256": hashlib.sha256(public_pem(key.public_key())).hexdigest(), "encrypted": not args.dev_unencrypted}))
        elif args.command == "issue":
            key = load_private(args.private_key, args.passphrase_file, args.dev_unencrypted)
            token, claims = mint(key, args.subject, args.role, args.dtls_fingerprint, args.audience, args.ttl)
            secure_write(args.output, token.encode("ascii"))
            print(json.dumps({"token_file": str(args.output.resolve()), "subject": claims["sub"], "role": claims["role"], "expires_utc": claims["exp"], "one_connection": True}))
        else:
            claims = verify(args.token_file.read_text("ascii").strip(), load_public(args.public_key), args.audience, args.role)
            if args.dtls_fingerprint and claims["dtls_fp"] != args.dtls_fingerprint:
                raise TokenError("AUTH_TRANSPORT")
            print(json.dumps({"verified": True, "claims": claims, "replay_checked": False}, sort_keys=True))
        return 0
    except (TokenError, OSError) as exc:
        print("MRFPS auth: " + str(exc), file=sys.stderr)
        return 1


if __name__ == "__main__":
    raise SystemExit(main())
