"""Inspect a running game container from the issuer host without sending signing secrets to it."""
import argparse
import base64
import json
import pathlib
import subprocess
import tarfile
import time

from cryptography.hazmat.primitives import serialization
from mrfps_tokens import load_private, public_pem


def docker_prefix(distribution):
    return ["wsl", "-d", distribution, "-u", "root", "--exec", "docker"] if distribution else ["docker"]


def scan_tar(command, needles):
    process = subprocess.Popen(command, stdout=subprocess.PIPE, stderr=subprocess.PIPE)
    matches, files, byte_count = [], 0, 0
    overlap = max(map(len, needles)) - 1
    try:
        with tarfile.open(fileobj=process.stdout, mode="r|*") as archive:
            for member in archive:
                if not member.isfile():
                    continue
                files += 1
                source = archive.extractfile(member)
                tail = b""
                found = False
                while True:
                    block = source.read(1024 * 1024)
                    if not block:
                        break
                    byte_count += len(block)
                    data = tail + block
                    if not found and any(needle in data for needle in needles):
                        matches.append(member.name)
                        found = True
                    tail = data[-overlap:] if overlap else b""
                source.close()
        error = process.stderr.read()
        if process.wait() != 0:
            raise RuntimeError("Docker filesystem export failed; no private-material absence claim was made.")
    finally:
        if process.poll() is None:
            process.kill()
            process.wait()
        process.stdout.close()
        process.stderr.close()
    return {"files_scanned": files, "bytes_scanned": byte_count, "matching_paths": matches}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--container", required=True)
    parser.add_argument("--signing-key", required=True, type=pathlib.Path, help="Host-only issuer key; its path or bytes are never passed into Docker")
    parser.add_argument("--passphrase-file", type=pathlib.Path)
    parser.add_argument("--fixture-key", action="store_true", help="Allow an explicit unencrypted disposable test key")
    parser.add_argument("--wsl-distribution")
    parser.add_argument("--output", required=True, type=pathlib.Path)
    args = parser.parse_args()
    key = load_private(args.signing_key, args.passphrase_file, args.fixture_key)
    seed = key.private_bytes(serialization.Encoding.Raw, serialization.PrivateFormat.Raw, serialization.NoEncryption())
    der = key.private_bytes(serialization.Encoding.DER, serialization.PrivateFormat.PKCS8, serialization.NoEncryption())
    pem = key.private_bytes(serialization.Encoding.PEM, serialization.PrivateFormat.PKCS8, serialization.NoEncryption())
    key_file = args.signing_key.read_bytes()
    needles = {seed, der, pem, pem.replace(b"\n", b"\r\n"), key_file, base64.b64encode(key_file),
               seed.hex().encode("ascii"), seed.hex().upper().encode("ascii"), base64.b64encode(seed),
               base64.urlsafe_b64encode(seed).rstrip(b"="), base64.b64encode(der)}
    prefix = docker_prefix(args.wsl_distribution)
    inspected = subprocess.run(prefix + ["inspect", args.container], capture_output=True, check=True)
    container = json.loads(inspected.stdout)[0]
    if not container["State"]["Running"]:
        raise RuntimeError("The inspected game container is not running.")
    environment = {}
    for entry in container["Config"].get("Env", []):
        name, _, value = entry.partition("=")
        environment[name] = value
    env_secret = any(any(needle in value.encode("utf-8") for needle in needles) for value in environment.values())
    private_names = [name for name in environment if "PRIVATE_KEY" in name.upper() or "PASSPHRASE" in name.upper()]
    encoded, literal = environment.get("MRFPS_AUTH_PUBLIC_KEY_B64", ""), environment.get("MRFPS_AUTH_PUBLIC_KEY_PEM", "")
    configured_public = base64.b64decode(encoded, validate=True) if encoded else literal.encode("ascii")
    public_matches = bool(encoded) != bool(literal) and configured_public.replace(b"\r\n", b"\n").strip() == public_pem(key.public_key()).strip()
    filesystem = scan_tar(prefix + ["export", args.container], needles)
    saved_volume = scan_tar(prefix + ["cp", args.container + ":/opt/mrfps/MRFPS/Saved", "-"], needles)
    passed = public_matches and not env_secret and not private_names and not filesystem["matching_paths"] and not saved_volume["matching_paths"]
    report = {"timestamp_utc": int(time.time()), "passed": passed, "scope": "Active issuer signing-key bytes/PKCS8/PEM/common encodings absent from exported container filesystem, mounted game Saved volume, and environment; transport keys in process memory are a separate identity",
              "container_id": container["Id"], "image_id": container["Image"], "public_key_matches_issuer": public_matches,
              "issuer_material_found_in_environment": env_secret, "private_environment_variable_names": private_names,
              "filesystem": filesystem, "saved_volume": saved_volume}
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text(json.dumps(report, indent=2), encoding="utf-8")
    print(json.dumps({"passed": passed, "report": str(args.output.resolve()), "files_scanned": filesystem["files_scanned"] + saved_volume["files_scanned"]}))
    return 0 if passed else 1


if __name__ == "__main__":
    raise SystemExit(main())
