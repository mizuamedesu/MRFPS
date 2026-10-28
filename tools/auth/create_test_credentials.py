"""Mint disposable credentials for actual UE admission/network tests; never production keys."""
import argparse
import base64
import json
import pathlib
import time

from cryptography.hazmat.primitives import serialization
from cryptography.hazmat.primitives.asymmetric.ed25519 import Ed25519PrivateKey
from mrfps_tokens import FINGERPRINT, encode_token, load_private, mint, public_pem, secure_write


def create_keys(directory: pathlib.Path) -> dict:
    key = Ed25519PrivateKey.generate()
    secure_write(directory / "issuer-private.pem", key.private_bytes(serialization.Encoding.PEM,
        serialization.PrivateFormat.PKCS8, serialization.NoEncryption()))
    public = public_pem(key.public_key())
    secure_write(directory / "issuer-public.pem", public)
    manifest = {"scope": "disposable test public-key configuration only", "audience": "mrfps",
                "public_key_path": str((directory / "issuer-public.pem").resolve()),
                "public_key_b64": base64.b64encode(public).decode("ascii")}
    secure_write(directory / "keys.json", json.dumps(manifest, indent=2).encode("utf-8"))
    return manifest


def create(directory: pathlib.Path, dtls_fingerprint: str, inspectors: int = 3) -> dict:
    if not FINGERPRINT.fullmatch(dtls_fingerprint):
        raise ValueError("A real server DTLS fingerprint is required.")
    if (directory / "manifest.json").exists():
        raise ValueError("Use a fresh fixture directory; credentials are single-connection.")
    if not (directory / "keys.json").exists():
        create_keys(directory)
    key = load_private(directory / "issuer-private.pem", None, True)
    public = public_pem(key.public_key())
    now = int(time.time())
    tokens = {}

    def save(name, token):
        path = directory / (name + ".jwt")
        secure_write(path, token.encode("ascii"))
        tokens[name] = str(path.resolve())

    for name, role in [("player-0", "player"), ("player-1", "player"), ("extra-player", "player")] + [(f"inspector-{i}", "inspector") for i in range(inspectors)]:
        save(name, mint(key, name, role, dtls_fingerprint, ttl=3600, now=now)[0])
    for role in ("player", "inspector"):
        token, claims = mint(key, "auth-valid-" + role, role, dtls_fingerprint, ttl=3600, now=now)
        save("valid-" + role, token)
        save("replay-" + role, token)
        save("fake-" + role, mint(Ed25519PrivateKey.generate(), "auth-fake-" + role, role, dtls_fingerprint, ttl=3600, now=now)[0])
        expired = dict(claims, iat=now - 180, nbf=now - 180, exp=now - 120)
        save("expired-" + role, encode_token(key, expired))
        save("audience-" + role, mint(key, "auth-audience-" + role, role, dtls_fingerprint, audience="wrong-audience", ttl=3600, now=now)[0])
        opposite = "inspector" if role == "player" else "player"
        save("role-" + role, mint(key, "auth-role-" + role, opposite, dtls_fingerprint, ttl=3600, now=now)[0])
        wrong_fingerprint = "1" * 64 if dtls_fingerprint == "0" * 64 else "0" * 64
        save("transport-" + role, mint(key, "auth-transport-" + role, role, wrong_fingerprint, ttl=3600, now=now)[0])
    manifest = {"scope": "disposable test credentials only", "created_utc": now, "expires_utc": now + 3600,
                "audience": "mrfps", "public_key_path": str((directory / "issuer-public.pem").resolve()),
                "public_key_b64": base64.b64encode(public).decode("ascii"), "tokens": tokens}
    secure_write(directory / "manifest.json", json.dumps(manifest, indent=2).encode("utf-8"))
    return manifest


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--directory", type=pathlib.Path, required=True)
    parser.add_argument("--inspectors", type=int, default=3)
    parser.add_argument("--keys-only", action="store_true")
    parser.add_argument("--dtls-fingerprint")
    args = parser.parse_args()
    if not 1 <= args.inspectors <= 32:
        parser.error("inspectors must be between 1 and 32")
    if args.keys_only:
        create_keys(args.directory.resolve())
        filename = "keys.json"
    else:
        if not args.dtls_fingerprint:
            parser.error("--dtls-fingerprint is required for issuing tokens")
        create(args.directory.resolve(), args.dtls_fingerprint, args.inspectors)
        filename = "manifest.json"
    print(json.dumps({"manifest_path": str((args.directory / filename).resolve())}))
