"""Verify private GHCR metadata and anonymous manifest/layer denial.

Only status/digest metadata is emitted. Registry credentials and bearer tokens
exist in memory only. This does not change any package permission or visibility.
"""
import argparse
import base64
import json
import hashlib
import os
from pathlib import Path
import re
import urllib.error
import urllib.parse
import urllib.request

MANIFEST_TYPES = ", ".join(("application/vnd.docker.distribution.manifest.v2+json", "application/vnd.oci.image.manifest.v1+json"))
DENIED = {401, 403, 404, 410}


class StripCrossOriginAuthorization(urllib.request.HTTPRedirectHandler):
    def redirect_request(self, request, fp, code, message, headers, newurl):
        target = super().redirect_request(request, fp, code, message, headers, newurl)
        if target and urllib.parse.urlsplit(request.full_url).netloc != urllib.parse.urlsplit(newurl).netloc:
            target.remove_header("Authorization")
        return target


def request(url, authorization=None, *, accept="application/json", one_byte=False):
    headers = {"Accept": accept, "User-Agent": "MRFPS-private-registry-boundary"}
    if authorization:
        headers["Authorization"] = authorization
    if one_byte:
        headers["Range"] = "bytes=0-0"
    opener = urllib.request.build_opener(StripCrossOriginAuthorization())
    try:
        with opener.open(urllib.request.Request(url, headers=headers), timeout=20) as response:
            data = response.read(1 if one_byte else 2 * 1024 * 1024 + 1)
            if len(data) > 2 * 1024 * 1024:
                raise ValueError("REGISTRY_RESPONSE_SIZE_LIMIT")
            return response.status, data, {key.lower(): value for key, value in response.headers.items()}
    except urllib.error.HTTPError as error:
        error.close()
        return error.code, b"", {}


def token_for(owner, package, authorization=None):
    url = "https://ghcr.io/token?" + urllib.parse.urlencode({"service": "ghcr.io", "scope": f"repository:{owner}/{package}:pull"})
    status, raw, _ = request(url, authorization)
    if status != 200:
        return status, None
    value = json.loads(raw)
    token = value.get("token") or value.get("access_token")
    if not isinstance(token, str) or not token:
        raise ValueError("REGISTRY_TOKEN_RESPONSE_INVALID")
    return status, "Bearer " + token


def check_private_package(repository, package, credential, *, allow_absent=False, identity=None, inspect_only=False):
    if not re.fullmatch(r"[A-Za-z0-9_.-]+/[A-Za-z0-9_.-]+", repository) or not re.fullmatch(r"[a-z0-9][a-z0-9_.-]{0,127}", package):
        raise ValueError("REGISTRY_IDENTITY_INVALID")
    owner = repository.split("/")[0]
    status, data, _ = request(f"https://api.github.com/repos/{repository}", "Bearer " + credential)
    if status != 200:
        raise ValueError("PUBLISHER_REPOSITORY_HTTP_" + str(status))
    repo = json.loads(data)
    if repo.get("full_name") != repository or repo.get("private") is not True:
        raise ValueError("PRIVATE_PUBLISHER_REQUIRED")
    status, data, _ = request(f"https://api.github.com/users/{owner}/packages/container/{package}", "Bearer " + credential)
    if allow_absent and status == 404:
        if identity:
            raise ValueError("PINNED_PRIVATE_PACKAGE_DISAPPEARED")
        return {"repository_private": True, "package_absent": True}
    if status != 200:
        raise ValueError("PACKAGE_METADATA_HTTP_" + str(status))
    value = json.loads(data)
    linked_repository = (value.get("repository") or {}).get("full_name")
    basic_match = (value.get("name") == package and value.get("package_type") == "container"
        and value.get("owner", {}).get("login", "").lower() == owner.lower() and value.get("visibility") == "private")
    if inspect_only and basic_match:
        versions_status, versions_raw, _ = request(f"https://api.github.com/users/{owner}/packages/container/{package}/versions?per_page=100&state=active", "Bearer " + credential)
        if versions_status != 200:
            raise ValueError("PACKAGE_VERSION_INSPECTION_HTTP_" + str(versions_status))
        versions = json.loads(versions_raw)
        if (value.get("version_count") != 1 or not isinstance(versions, list) or len(versions) != 1
            or versions[0].get("name") != "sha256:0f94eeae41fcbe2acc2654022fddf033198e116d274f53150ffea6c9437d2993"
            or versions[0].get("metadata", {}).get("container", {}).get("tags") != ["privacy-fixture-v1"]):
            raise ValueError("INITIAL_PRIVATE_FIXTURE_VERSION_HISTORY_MISMATCH")
        return {"repository": repository, "publisher_repository_id": repo.get("id"), "repository_private": True,
                "package": package, "package_id": value.get("id"), "package_private": True,
                "linked_repository": linked_repository, "created_at": value.get("created_at"),
                "version_count": value.get("version_count"), "fixture_digest": versions[0]["name"],
                "fixture_version_id": versions[0]["id"], "fixture_created_at": versions[0].get("created_at"), "identity_verified": False}
    pinned = False
    if identity:
        pinned = (identity.get("repository") == repository and identity.get("publisher_repository_id") == repo.get("id")
                  and identity.get("package") == package and type(identity.get("package_id")) is int
                  and identity["package_id"] > 0 and identity["package_id"] == value.get("id")
                  and identity.get("created_at") == value.get("created_at")
                  and identity.get("creation_workflow_run_id") == 40927353040
                  and identity.get("creation_preflight_absent") is True
                  and identity.get("fixture_digest") == "sha256:0f94eeae41fcbe2acc2654022fddf033198e116d274f53150ffea6c9437d2993")
        if not pinned:
            raise ValueError("PRIVATE_PACKAGE_ID_PIN_MISMATCH")
    if (not basic_match or (linked_repository and linked_repository.lower() != repository.lower())
        or (not linked_repository and not pinned)):
        print("MRFPS_GHCR_IDENTITY " + json.dumps({"name": value.get("name"), "package_type": value.get("package_type"),
              "owner": value.get("owner", {}).get("login"), "repository": linked_repository, "package_id": value.get("id"), "created_at": value.get("created_at"),
              "visibility": value.get("visibility"), "expected_repository": repository}, sort_keys=True))
        raise ValueError("PRIVATE_PACKAGE_OWNERSHIP_OR_VISIBILITY_MISMATCH")
    return {"repository_private": True, "package_private": True, "repository": repository, "package": package,
            "package_id": value.get("id"), "package_id_pinned": pinned, "repository_link_present": bool(linked_repository)}


def verify(repository, package, tag, credential, actor, identity=None):
    report = check_private_package(repository, package, credential, identity=identity)
    if not re.fullmatch(r"[A-Za-z0-9_][A-Za-z0-9_.-]{0,127}", tag) or not actor:
        raise ValueError("REGISTRY_TAG_OR_ACTOR_INVALID")
    owner = repository.split("/")[0].lower()
    basic = "Basic " + base64.b64encode((actor + ":" + credential).encode()).decode("ascii")
    status, authorized = token_for(owner, package, basic)
    if status != 200:
        raise ValueError("AUTHENTICATED_REGISTRY_TOKEN_HTTP_" + str(status))
    registry = f"https://ghcr.io/v2/{owner}/{package}"
    status, data, headers = request(registry + "/manifests/" + tag, authorized, accept=MANIFEST_TYPES)
    if status != 200:
        raise ValueError("AUTHENTICATED_MANIFEST_HTTP_" + str(status))
    digest = headers.get("docker-content-digest", "")
    if not re.fullmatch(r"sha256:[0-9a-f]{64}", digest):
        raise ValueError("REGISTRY_MANIFEST_DIGEST_INVALID")
    if identity and tag == "privacy-fixture-v1" and digest != identity.get("fixture_digest"):
        raise ValueError("PRIVATE_FIXTURE_DIGEST_PIN_MISMATCH")
    manifest = json.loads(data)
    config_digest = manifest.get("config", {}).get("digest", "")
    if not re.fullmatch(r"sha256:[0-9a-f]{64}", config_digest):
        raise ValueError("REGISTRY_IMAGE_CONFIG_DIGEST_INVALID")
    config_status, config_raw, _ = request(registry + "/blobs/" + config_digest, authorized)
    if config_status != 200 or "sha256:" + hashlib.sha256(config_raw).hexdigest() != config_digest:
        raise ValueError("AUTHENTICATED_IMAGE_CONFIG_DIGEST_MISMATCH")
    labels = json.loads(config_raw).get("config", {}).get("Labels", {})
    if labels.get("org.opencontainers.image.source") != "https://github.com/" + repository:
        raise ValueError("PRIVATE_PUBLISHER_SOURCE_LABEL_MISMATCH")
    if tag == "privacy-fixture-v1" and labels.get("app.mrfps.privacy-fixture") != "nonsecret-v1":
        raise ValueError("PRIVATE_FIXTURE_LABEL_MISMATCH")
    report["source_label_verified"] = True
    # A scratch fixture must contain a non-secret file so a layer can be probed.
    layers = manifest.get("layers", [])
    if not layers:
        raise ValueError("REGISTRY_MANIFEST_LAYER_MISSING")
    largest = max(layers, key=lambda layer: layer.get("size", 0)).get("digest", "")
    if not re.fullmatch(r"sha256:[0-9a-f]{64}", largest):
        raise ValueError("REGISTRY_LAYER_DIGEST_INVALID")
    anonymous_token_status, anonymous = token_for(owner, package)
    if anonymous_token_status not in {200, 401, 403, 404}:
        raise ValueError("ANONYMOUS_TOKEN_STATUS_UNEXPECTED")
    tag_status, _, _ = request(registry + "/manifests/" + tag, anonymous, accept=MANIFEST_TYPES)
    digest_status, _, _ = request(registry + "/manifests/" + digest, anonymous, accept=MANIFEST_TYPES)
    layer_status, _, _ = request(registry + "/blobs/" + largest, anonymous, one_byte=True)
    report.update({"tag": tag, "digest": digest, "authenticated_manifest_status": status,
                   "anonymous_token_status": anonymous_token_status, "anonymous_tag_status": tag_status,
                   "anonymous_digest_status": digest_status, "anonymous_layer_status": layer_status})
    if any(code not in DENIED for code in (tag_status, digest_status, layer_status)):
        raise ValueError("ANONYMOUS_REGISTRY_ACCESS_NOT_DENIED")
    report["anonymous_denied"] = True
    return report


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--repository", required=True)
    parser.add_argument("--package", required=True)
    parser.add_argument("--tag")
    parser.add_argument("--preflight", action="store_true")
    parser.add_argument("--inspect", action="store_true")
    parser.add_argument("--identity-file", type=Path, default=Path(__file__).with_name("private-registry-identity.json"))
    args = parser.parse_args()
    credential = os.environ.get("GH_TOKEN")
    if not credential:
        raise ValueError("SCOPED_JOB_TOKEN_REQUIRED")
    identity = json.loads(args.identity_file.read_text(encoding="utf-8-sig")) if args.identity_file.is_file() else None
    if args.inspect:
        result = check_private_package(args.repository, args.package, credential, inspect_only=True)
        print("MRFPS_GHCR_IDENTITY_INSPECTION " + json.dumps(result, sort_keys=True))
        return
    if args.preflight:
        result = check_private_package(args.repository, args.package, credential, allow_absent=True, identity=identity)
    else:
        result = verify(args.repository, args.package, args.tag or "", credential, os.environ.get("GITHUB_ACTOR", ""), identity=identity)
    print("MRFPS_GHCR_BOUNDARY " + json.dumps(result, sort_keys=True))


if __name__ == "__main__":
    try:
        main()
    except (ValueError, OSError, urllib.error.URLError, KeyError) as error:
        message = str(error) if isinstance(error, ValueError) and re.fullmatch(r"[A-Z0-9_]+", str(error)) else type(error).__name__
        raise SystemExit("MRFPS_GHCR_BOUNDARY_ERROR " + message)
