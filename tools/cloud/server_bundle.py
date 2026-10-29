"""Bounded, deterministic packaging of an already built Linux server.

No Unreal build, credentials, or network access. The archive contains only
Artifacts/Packages/LinuxServer; deployment code comes from the reviewed commit.
"""
from __future__ import annotations

import argparse
import base64
import gzip
import hashlib
import json
import os
from pathlib import Path, PurePosixPath
import re
import stat
import tarfile
import uuid

PREFIX = "Artifacts/Packages/LinuxServer/"
SERVER = "MRFPS/Binaries/Linux/MRFPSServer"
MAX_BYTES = 8 * 1024**3
MAX_FILES = 20000


def sha256(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for block in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(block)
    return digest.hexdigest()


def check_hash(value: str) -> str:
    if not re.fullmatch(r"[0-9a-f]{64}", value):
        raise ValueError("INVALID_SHA256")
    return value


def excluded(relative: PurePosixPath) -> bool:
    return ("saved" in (p.lower() for p in relative.parts)
            or relative.suffix.lower() in {".debug", ".sym", ".log"})


def check_member(name: str) -> PurePosixPath:
    if "\\" in name or not name.startswith(PREFIX):
        raise ValueError("ARCHIVE_PATH_OUTSIDE_PACKAGE")
    raw_relative = name[len(PREFIX):]
    relative = PurePosixPath(raw_relative)
    if (not relative.parts or relative.is_absolute() or ":" in name
            or relative.as_posix() != raw_relative
            or any(part in {".", ".."} for part in name.split("/"))):
        raise ValueError("ARCHIVE_PATH_INVALID")
    if excluded(relative):
        raise ValueError("ARCHIVE_RUNTIME_EXCLUSION")
    if (relative.suffix.lower() in {".jwt", ".key", ".pem", ".pfx", ".p12", ".keystore", ".jks"}
            or any(p.startswith(".env") or p in {".git", ".local"} for p in relative.parts)
            or "issuer-private" in name.lower()):
        raise ValueError("CREDENTIAL_FILE_FORBIDDEN")
    return relative


def check_elf(server: Path) -> None:
    with server.open("rb") as stream:
        header = stream.read(20)
    if (len(header) != 20 or header[:6] != b"\x7fELF\x02\x01"
            or int.from_bytes(header[18:20], "little") != 62):
        raise ValueError("SERVER_MUST_BE_LINUX_AMD64_ELF")


def package_files(root: Path) -> list[tuple[Path, PurePosixPath]]:
    files = []
    total = 0
    for directory, directories, names in os.walk(root, followlinks=False):
        for name in list(directories):
            child = Path(directory) / name
            info = child.lstat()
            if child.is_symlink() or getattr(info, "st_file_attributes", 0) & 0x400:
                raise ValueError("PACKAGE_LINK_FORBIDDEN")
            if excluded(PurePosixPath(child.relative_to(root).as_posix())):
                directories.remove(name)
        for name in names:
            path = Path(directory) / name
            relative = PurePosixPath(path.relative_to(root).as_posix())
            if excluded(relative):
                continue
            info = path.lstat()
            if path.is_symlink() or getattr(info, "st_file_attributes", 0) & 0x400:
                raise ValueError("PACKAGE_LINK_FORBIDDEN")
            if not stat.S_ISREG(info.st_mode):
                raise ValueError("PACKAGE_SPECIAL_FILE_FORBIDDEN")
            check_member(PREFIX + str(relative))
            total += info.st_size
            if total > MAX_BYTES or len(files) >= MAX_FILES:
                raise ValueError("PACKAGE_SIZE_LIMIT")
            files.append((path, relative))
    return sorted(files, key=lambda item: str(item[1]))


def write_json(path: Path, data: dict) -> None:
    path.write_text(json.dumps(data, indent=2) + "\n", encoding="utf-8")


def bundle(root: Path, expected_server: str, source_commit: str, output: Path) -> dict:
    check_hash(expected_server)
    if not re.fullmatch(r"[0-9a-f]{40}", source_commit):
        raise ValueError("SOURCE_COMMIT_REQUIRED")
    root = root.resolve(strict=True)
    if not root.is_dir() or root.is_symlink():
        raise ValueError("PACKAGE_DIRECTORY_INVALID")
    server = root / SERVER
    check_elf(server)
    if sha256(server) != expected_server:
        raise ValueError("SERVER_SHA256_DOES_NOT_MATCH_APPROVED_BUILD")
    files = package_files(root)
    if not (root / "MRFPS/Content/Paks").is_dir():
        raise ValueError("COOKED_CONTENT_MISSING")
    output.mkdir(parents=True, exist_ok=True)
    archive = output / "linux-server.tar.gz"
    temporary = output / "linux-server.tar.gz.incoming"
    try:
        with temporary.open("wb") as raw, gzip.GzipFile(filename="", fileobj=raw, mode="wb", mtime=0) as compressed:
            with tarfile.open(fileobj=compressed, mode="w|") as tar:
                for path, relative in files:
                    info = tarfile.TarInfo(PREFIX + str(relative))
                    info.size = path.stat().st_size
                    info.mode = 0o755 if str(relative) == SERVER else 0o644
                    with path.open("rb") as stream:
                        tar.addfile(info, stream)
        os.replace(temporary, archive)
    finally:
        temporary.unlink(missing_ok=True)
    archive_hash = sha256(archive)
    result = {
        "schema": 1, "source_commit": source_commit, "server_sha256": expected_server,
        "bundle_sha256": archive_hash, "archive": archive.name, "files": len(files),
        "uncompressed_bytes": sum(path.stat().st_size for path, _ in files),
        "image_tag": f"linux-{archive_hash[:12]}-{source_commit[:12]}",
        "release_tag": f"server-artifacts-{archive_hash[:12]}-{source_commit[:12]}",
        "symbol_files_included": False, "credentials_included": False,
        "provenance_scope": "operator-approved build and source commit; a digest alone does not prove compilation freshness",
    }
    write_json(output / "server-artifact.json", result)
    return result


def verify_archive(archive: Path, expected_archive: str, expected_server: str, destination: Path) -> None:
    if sha256(archive) != check_hash(expected_archive):
        raise ValueError("BUNDLE_SHA256_MISMATCH")
    check_hash(expected_server)
    destination = destination.resolve()
    # Validate the whole archive before writing any extracted entry.
    with tarfile.open(archive, "r:gz") as tar:
        members = []
        total = 0
        seen = set()
        for member in tar:
            total += member.size
            if len(members) >= MAX_FILES or total > MAX_BYTES:
                raise ValueError("ARCHIVE_SIZE_LIMIT")
            check_member(member.name)
            if member.type not in {tarfile.REGTYPE, tarfile.AREGTYPE} or member.name in seen or member.size < 0:
                raise ValueError("ARCHIVE_ENTRY_INVALID")
            seen.add(member.name)
            members.append(member)
            target = destination / member.name
            if any(parent.is_symlink() for parent in (target, *target.parents)):
                raise ValueError("EXTRACTION_SYMLINK_FORBIDDEN")
        if PREFIX + SERVER not in seen:
            raise ValueError("ARCHIVE_SERVER_MISSING")
        for member in members:
            target = destination / member.name
            target.parent.mkdir(parents=True, exist_ok=True)
            source = tar.extractfile(member)
            if source is None:
                raise ValueError("ARCHIVE_CONTENT_MISSING")
            with source, target.open("wb") as output:
                for block in iter(lambda: source.read(1024 * 1024), b""):
                    output.write(block)
            target.chmod(member.mode & 0o755)
    server = destination / PREFIX / SERVER
    check_elf(server)
    if sha256(server) != expected_server:
        raise ValueError("EXTRACTED_SERVER_SHA256_MISMATCH")


def flash_manifest(image: str, public_key: bytes, project: str, name: str, region: str) -> dict:
    if not re.fullmatch(r"ghcr\.io/[a-z0-9._/-]+@sha256:[0-9a-f]{64}", image):
        raise ValueError("IMMUTABLE_GHCR_IMAGE_DIGEST_REQUIRED")
    uuid.UUID(project)
    if not re.fullmatch(r"[a-z0-9][a-z0-9-]{0,62}", name + ""):
        raise ValueError("SERVICE_NAME_INVALID")
    if not re.fullmatch(r"[a-z0-9][a-z0-9-]{0,62}", region):
        raise ValueError("REGION_INVALID")
    if b"\x00" in public_key or any(byte > 127 for byte in public_key):
        raise ValueError("PUBLIC_KEY_ASCII_REQUIRED")
    match = re.fullmatch(rb"-----BEGIN PUBLIC KEY-----\r?\n([A-Za-z0-9+/=\r\n]+)-----END PUBLIC KEY-----\s*", public_key)
    if not match:
        raise ValueError("ED25519_PUBLIC_KEY_REQUIRED")
    der = base64.b64decode(re.sub(rb"\s", b"", match.group(1)), validate=True)
    if len(der) != 44 or der[:12] != bytes.fromhex("302a300506032b6570032100"):
        raise ValueError("ED25519_SPKI_REQUIRED")
    return {"project_id": project, "name": name, "spec": {
        "region": region, "image": image, "replicas": 1, "cpu_millis": 2000,
        "memory_mib": 1024, "ephemeral_storage_gib": 8, "rootfs_storage_gib": 2,
        "ports": [{"name": "game-udp", "protocol": "udp", "container_port": 7777}],
        "exposure": {"type": "public", "traffic_mode": "forwarded", "endpoint_mode": "ip", "allowed_source_cidrs": [], "denied_source_cidrs": []},
        "egress": {"mode": "disabled"},
        "env": {"MRFPS_PORT": "7777", "MRFPS_AUTH_PUBLIC_KEY_B64": base64.b64encode(public_key).decode("ascii"), "MRFPS_AUTH_AUDIENCE": "mrfps"},
        "command": [], "args": ["/Game/Maps/MRFPSArena"],
        "metadata": {"managed_by": "MRFPS-cloud-deploy", "game_port": 7777},
    }}


def main() -> None:
    parser = argparse.ArgumentParser()
    commands = parser.add_subparsers(dest="action", required=True)
    pack = commands.add_parser("bundle")
    pack.add_argument("--package", type=Path, required=True)
    pack.add_argument("--server-sha256", required=True)
    pack.add_argument("--source-commit", required=True)
    pack.add_argument("--output", type=Path, required=True)
    verify = commands.add_parser("verify")
    verify.add_argument("--archive", type=Path, required=True)
    verify.add_argument("--bundle-sha256", required=True)
    verify.add_argument("--server-sha256", required=True)
    verify.add_argument("--destination", type=Path, required=True)
    flash = commands.add_parser("flash-manifest")
    flash.add_argument("--image", required=True)
    flash.add_argument("--public-key", type=Path, required=True)
    flash.add_argument("--project-id", required=True)
    flash.add_argument("--name", default="mrfps-server")
    flash.add_argument("--region", required=True)
    flash.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    if args.action == "bundle":
        print(json.dumps(bundle(args.package, args.server_sha256, args.source_commit, args.output)))
    elif args.action == "verify":
        verify_archive(args.archive, args.bundle_sha256, args.server_sha256, args.destination)
        print("MRFPS_CLOUD_ARCHIVE_VERIFIED")
    else:
        result = flash_manifest(args.image, args.public_key.read_bytes(), args.project_id, args.name, args.region)
        args.output.parent.mkdir(parents=True, exist_ok=True)
        write_json(args.output, result)
        print(json.dumps({"manifest": str(args.output), "public_key_only": True, "fixed_public_7777_supported_by_current_api": False}))


if __name__ == "__main__":
    try:
        main()
    except (ValueError, OSError, tarfile.TarError) as error:
        # Fixed codes/file error classes only; do not serialize file contents.
        message = str(error) if isinstance(error, ValueError) else type(error).__name__
        raise SystemExit("MRFPS_CLOUD_ERROR " + message)
