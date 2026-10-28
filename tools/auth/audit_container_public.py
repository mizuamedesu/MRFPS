"""Audit an active issuer's recognizable private-key formats using its public key only.

No host private key is accepted/read and no token is minted. This does not claim
to recognize arbitrary raw binary seeds lacking a format or text encoding.
"""
import argparse
import base64
import hashlib
import json
import pathlib
import re
import subprocess
import tarfile
import time

from cryptography.hazmat.primitives import serialization
from cryptography.hazmat.primitives.asymmetric.ed25519 import Ed25519PrivateKey
from cryptography.exceptions import UnsupportedAlgorithm
from mrfps_tokens import load_public

DER_PREFIX = bytes.fromhex("302e020100300506032b657004220420")
PEM = re.compile(rb"-----BEGIN ((?:[A-Z0-9]+ )*PRIVATE KEY)-----[ \t]*\r?\n([A-Za-z0-9+/=\r\n]{16,16384}?)-----END \1-----")
SEED_HEX = re.compile(rb"(?<![A-Fa-f0-9])[A-Fa-f0-9]{64}(?![A-Fa-f0-9])")
SEED_B64 = re.compile(rb"(?<![A-Za-z0-9+/])[A-Za-z0-9+/]{43}=(?![A-Za-z0-9+/=])")
ENCODED_PEM = re.compile(rb"LS0tLS1CRUdJTi[A-Za-z0-9+/=]{20,16384}")
DER_B64 = re.compile(rb"MC4CAQAwBQYDK2VwBCIE[A-Za-z0-9+/]{44}")
DER_HEX = re.compile(re.escape(DER_PREFIX.hex().encode()) + rb"[a-f0-9]{64}", re.I)
ELF_PATH = "opt/mrfps/MRFPS/Binaries/Linux/MRFPSServer"


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--container", required=True)
    parser.add_argument("--public-key", required=True, type=pathlib.Path)
    parser.add_argument("--wsl-distribution")
    parser.add_argument("--expected-elf-sha256", required=True)
    parser.add_argument("--output", required=True, type=pathlib.Path)
    args = parser.parse_args()
    prefix = (["wsl", "-d", args.wsl_distribution, "-u", "root", "--exec", "docker"]
              if args.wsl_distribution else ["docker"])
    expected = load_public(args.public_key).public_bytes(serialization.Encoding.Raw, serialization.PublicFormat.Raw)
    key_matches, encrypted_pem, private_paths, symbols = set(), set(), set(), set()
    unsupported_private_algorithms = set()
    seen_seeds = set()
    candidate_limit_reached = False
    elf = None

    def check_seed(seed, path, kind):
        nonlocal candidate_limit_reached
        if len(seed) != 32 or seed in seen_seeds:
            return
        if len(seen_seeds) >= 65536:
            candidate_limit_reached = True
            return
        seen_seeds.add(seed)
        actual = Ed25519PrivateKey.from_private_bytes(seed).public_key().public_bytes(serialization.Encoding.Raw, serialization.PublicFormat.Raw)
        if actual == expected:
            key_matches.add((path, kind))

    def check_pem(data, path):
        for match in PEM.finditer(data):
            try:
                key = serialization.load_pem_private_key(match.group(0), password=None)
                if isinstance(key, Ed25519PrivateKey):
                    raw = key.private_bytes(serialization.Encoding.Raw, serialization.PrivateFormat.Raw, serialization.NoEncryption())
                    check_seed(raw, path, "PEM")
            except TypeError:
                encrypted_pem.add(path)
            except UnsupportedAlgorithm:
                # Non-Ed25519 test/certificate fixtures can be present in OS
                # packages. Record them rather than mistaking a parser failure
                # for an active Ed25519 issuer-key match or an audit success.
                unsupported_private_algorithms.add(path)
            except ValueError:
                pass

    def scan(data, path):
        check_pem(data, path)
        offset = 0
        while True:
            offset = data.find(DER_PREFIX, offset)
            if offset < 0:
                break
            check_seed(data[offset + len(DER_PREFIX):offset + len(DER_PREFIX) + 32], path, "PKCS8 DER")
            offset += len(DER_PREFIX)
        for match in DER_HEX.finditer(data):
            check_seed(bytes.fromhex(match.group().decode("ascii"))[len(DER_PREFIX):], path, "PKCS8 hex")
        for match in DER_B64.finditer(data):
            decoded = base64.b64decode(match.group())
            if decoded.startswith(DER_PREFIX):
                check_seed(decoded[len(DER_PREFIX):], path, "PKCS8 base64")
        for match in ENCODED_PEM.finditer(data):
            try:
                check_pem(base64.b64decode(match.group(), validate=True), path)
            except ValueError:
                pass
        for match in SEED_HEX.finditer(data):
            check_seed(bytes.fromhex(match.group().decode("ascii")), path, "seed hex")
        for match in SEED_B64.finditer(data):
            check_seed(base64.b64decode(match.group()), path, "seed base64")

    def scan_archive(command):
        nonlocal elf
        process = subprocess.Popen(prefix + command, stdout=subprocess.PIPE, stderr=subprocess.PIPE)
        count, size = 0, 0
        try:
            with tarfile.open(fileobj=process.stdout, mode="r|*") as archive:
                for member in archive:
                    if not member.isfile():
                        continue
                    path = member.name.removeprefix("./")
                    count += 1
                    if pathlib.PurePosixPath(path).name.lower().startswith("issuer-private"):
                        private_paths.add(path)
                    if path in (ELF_PATH + ".debug", ELF_PATH + ".sym"):
                        symbols.add(path)
                    source = archive.extractfile(member)
                    tail, first = b"", b""
                    digest = hashlib.sha256() if path == ELF_PATH else None
                    while True:
                        block = source.read(1024 * 1024)
                        if not block:
                            break
                        if not first:
                            first = block[:20]
                        size += len(block)
                        if digest:
                            digest.update(block)
                        scan(tail + block, path)
                        tail = block[-32768:]
                    source.close()
                    if digest:
                        elf = {"path": path, "bytes": member.size, "sha256": digest.hexdigest(),
                               "elf64_x86_64": first[:6] == b"\x7fELF\x02\x01" and first[18:20] == b"\x3e\x00"}
            if process.wait() != 0:
                raise RuntimeError("Docker export failed; no absence claim was made.")
        finally:
            if process.poll() is None:
                process.kill()
                process.wait()
            process.stdout.close()
            process.stderr.close()
        return {"files": count, "bytes": size}

    raw = subprocess.run(prefix + ["inspect", args.container], capture_output=True, check=True)
    container = json.loads(raw.stdout)[0]
    if not container["State"]["Running"]:
        raise RuntimeError("The game container is not running.")
    environment = dict(value.split("=", 1) for value in container["Config"].get("Env", []))
    private_environment_names = [name for name in environment if "PRIVATE_KEY" in name.upper() or "PASSPHRASE" in name.upper()]
    for name, value in environment.items():
        scan(value.encode("utf-8"), "environment:" + name)
    encoded, literal = environment.get("MRFPS_AUTH_PUBLIC_KEY_B64", ""), environment.get("MRFPS_AUTH_PUBLIC_KEY_PEM", "")
    configured = base64.b64decode(encoded, validate=True) if encoded else literal.encode("ascii")
    key = serialization.load_pem_public_key(configured)
    configured_matches = bool(encoded) != bool(literal) and key.public_bytes(serialization.Encoding.Raw, serialization.PublicFormat.Raw) == expected
    filesystem = scan_archive(["export", args.container])
    saved = scan_archive(["cp", args.container + ":/opt/mrfps/MRFPS/Saved", "-"])
    passed = (configured_matches and not key_matches and not encrypted_pem and not private_paths and not private_environment_names
              and not candidate_limit_reached and not symbols and elf is not None and elf["elf64_x86_64"]
              and elf["sha256"].lower() == args.expected_elf_sha256.lower())
    report = {"timestamp_utc": int(time.time()), "passed": passed, "host_private_key_read_or_used": False,
              "scope": "Public-key-only audit of recognizable active-issuer PEM/PKCS8 DER/hex/base64 and textual seed hex/base64 in exported filesystem, Saved volume and environment; issuer-private filenames and private env names; runtime ELF and symbol exclusion",
              "limitation": "Arbitrary unidentified raw binary seed bytes/unknown encodings cannot be excluded using only a public key. No host signing key or token issuance was used.",
              "container_id": container["Id"], "image_id": container["Image"], "configured_public_key_matches": configured_matches,
              "active_issuer_private_matches": [{"path": path, "format": kind} for path, kind in sorted(key_matches)],
              "encrypted_private_pem_paths": sorted(encrypted_pem), "issuer_private_filename_paths": sorted(private_paths),
              "unsupported_non_ed25519_private_pem_paths": sorted(unsupported_private_algorithms),
              "private_environment_variable_names": private_environment_names, "text_seed_candidates_checked": len(seen_seeds),
              "candidate_limit_reached": candidate_limit_reached, "host_symbols_found_in_image": sorted(symbols),
              "runtime_elf": elf, "filesystem": filesystem, "saved_volume": saved}
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text(json.dumps(report, indent=2), encoding="utf-8")
    print(json.dumps({"passed": passed, "report": str(args.output.resolve()), "files_scanned": filesystem["files"] + saved["files"], "host_private_key_read_or_used": False}))
    return 0 if passed else 1


if __name__ == "__main__":
    raise SystemExit(main())
