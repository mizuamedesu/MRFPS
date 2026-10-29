"""Fetch only reviewed build launchers; never materialize the whole UE repo.

The server is already compiled. GitHub's immutable commit/Contents API binds
these fixed, size-bounded files to the operator-approved source revision.
"""
import argparse
import base64
import hashlib
import json
import os
from pathlib import Path
import re
import shutil
import urllib.error
import urllib.parse
import urllib.request
import uuid

REPOSITORY = "mizuamedesu/MRFPS"
MAX_FILE_BYTES = 256 * 1024
MAX_RESPONSE_BYTES = MAX_FILE_BYTES * 2 + 16384
FILES = (
    ".dockerignore",
    "Deploy/Linux/Dockerfile",
    "Deploy/Linux/Dockerfile.dockerignore",
    "Deploy/Linux/entrypoint.sh",
    "Deploy/Linux/healthcheck.py",
    "Deploy/Linux/auth_config.py",
    "Deploy/Cloud/Flash.Dockerfile",
    "Deploy/Cloud/.dockerignore",
    "Deploy/Cloud/flash-entrypoint.sh",
    "Deploy/Cloud/flash-healthcheck.sh",
    "tools/cloud/server_bundle.py",
)


class NoRedirect(urllib.request.HTTPRedirectHandler):
    def redirect_request(self, *_args, **_kwargs):
        return None


def github_json(path, credential=None):
    headers = {"Accept": "application/vnd.github+json", "User-Agent": "MRFPS-bounded-server-context"}
    if credential:
        headers["Authorization"] = "Bearer " + credential
    url = "https://api.github.com/repos/" + REPOSITORY + "/" + path
    opener = urllib.request.build_opener(NoRedirect())
    try:
        with opener.open(urllib.request.Request(url, headers=headers), timeout=30) as response:
            body = response.read(MAX_RESPONSE_BYTES + 1)
    except urllib.error.HTTPError as error:
        status = error.code
        error.close()
        raise ValueError("SOURCE_API_HTTP_" + str(status)) from None
    if len(body) > MAX_RESPONSE_BYTES:
        raise ValueError("SOURCE_API_RESPONSE_SIZE_LIMIT")
    return json.loads(body)


def fetch(commit, destination, credential=None, api=github_json):
    if not re.fullmatch(r"[0-9a-f]{40}", commit):
        raise ValueError("APPROVED_SOURCE_COMMIT_REQUIRED")
    if destination.exists() or destination.is_symlink():
        raise ValueError("SOURCE_DESTINATION_MUST_NOT_EXIST")
    metadata = api("git/commits/" + commit, credential)
    if not isinstance(metadata, dict) or metadata.get("sha") != commit:
        raise ValueError("SOURCE_COMMIT_RESPONSE_MISMATCH")
    destination.parent.mkdir(parents=True, exist_ok=True)
    stage = destination.with_name(destination.name + ".incoming-" + uuid.uuid4().hex)
    stage.mkdir(mode=0o700)
    records = []
    try:
        for relative in FILES:
            value = api("contents/" + relative + "?" + urllib.parse.urlencode({"ref": commit}), credential)
            if (not isinstance(value, dict) or value.get("type") != "file" or value.get("path") != relative
                or value.get("encoding") != "base64" or type(value.get("size")) is not int
                or not 0 <= value["size"] <= MAX_FILE_BYTES):
                raise ValueError("SOURCE_FILE_METADATA_INVALID")
            content = value.get("content")
            if not isinstance(content, str) or len(content) > MAX_RESPONSE_BYTES:
                raise ValueError("SOURCE_FILE_CONTENT_LIMIT")
            data = base64.b64decode(re.sub(r"\s", "", content), validate=True)
            blob = hashlib.sha1(b"blob " + str(len(data)).encode("ascii") + b"\0" + data).hexdigest()
            if len(data) != value["size"] or blob != value.get("sha"):
                raise ValueError("SOURCE_GIT_BLOB_DIGEST_MISMATCH")
            target = stage / relative
            target.parent.mkdir(parents=True, exist_ok=True)
            target.write_bytes(data)
            target.chmod(0o644)
            records.append({"path": relative, "bytes": len(data), "git_blob_sha": blob,
                            "sha256": hashlib.sha256(data).hexdigest()})
        os.replace(stage, destination)
    finally:
        if stage.exists():
            shutil.rmtree(stage)
    return {"source_repository": REPOSITORY, "source_commit": commit, "files": records,
            "file_count": len(records), "total_bytes": sum(item["bytes"] for item in records),
            "whole_repository_checkout": False, "credential_files_included": False}


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--commit", required=True)
    parser.add_argument("--destination", type=Path, required=True)
    parser.add_argument("--report", type=Path, required=True)
    args = parser.parse_args()
    report = fetch(args.commit, args.destination, os.environ.get("GH_TOKEN"))
    args.report.parent.mkdir(parents=True, exist_ok=True)
    args.report.write_text(json.dumps(report, indent=2), encoding="utf-8")
    print("MRFPS_SERVER_CONTEXT " + json.dumps({key: value for key, value in report.items() if key != "files"}, sort_keys=True))


if __name__ == "__main__":
    try:
        main()
    except (ValueError, OSError) as error:
        value = str(error)
        safe = value if isinstance(error, ValueError) and re.fullmatch(r"[A-Z0-9_]+", value) else type(error).__name__
        raise SystemExit("MRFPS_SERVER_CONTEXT_ERROR " + safe)
