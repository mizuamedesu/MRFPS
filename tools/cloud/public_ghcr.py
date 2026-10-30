"""Inspect an isolated public GHCR package without publishing or exposing tokens."""
from __future__ import annotations

import argparse
import base64
import hashlib
import json
import os
import re

import ghcr_boundary as registry

OWNER = 'mizuamedesu'
REPOSITORY = 'mizuamedesu/MRFPS'
MANIFEST_TYPES = ', '.join((
    'application/vnd.docker.distribution.manifest.v2+json',
    'application/vnd.oci.image.manifest.v1+json',
))


def validate(bundle: str, server: str, commit: str) -> tuple[str, str]:
    if not re.fullmatch(r'[0-9a-f]{64}', bundle) or not re.fullmatch(r'[0-9a-f]{64}', server):
        raise ValueError('SHA256_INVALID')
    if not re.fullmatch(r'[0-9a-f]{40}', commit):
        raise ValueError('SOURCE_COMMIT_INVALID')
    return 'mrfps-server-public-' + bundle[:16], 'linux-' + commit[:12]


def api_metadata(package: str, token: str) -> None:
    status, raw, _ = registry.request(f'https://api.github.com/users/{OWNER}/packages/container/{package}',
                                      'Bearer ' + token, accept='application/vnd.github+json')
    if status != 200:
        raise ValueError('PUBLIC_PACKAGE_METADATA_HTTP_' + str(status))
    value = json.loads(raw)
    if (value.get('name') != package or value.get('package_type') != 'container' or
        value.get('visibility') != 'public' or value.get('owner', {}).get('login', '').lower() != OWNER or
        (value.get('repository') or {}).get('full_name', '').lower() != REPOSITORY.lower()):
        raise ValueError('PUBLIC_PACKAGE_IDENTITY_OR_VISIBILITY_MISMATCH')


def verify_public(bundle: str, server: str, commit: str, token: str) -> dict:
    package, tag = validate(bundle, server, commit)
    api_metadata(package, token)
    status, anonymous = registry.token_for(OWNER, package)
    if status != 200:
        raise ValueError('ANONYMOUS_TOKEN_HTTP_' + str(status))
    base = f'https://ghcr.io/v2/{OWNER}/{package}'
    status, raw, headers = registry.request(base + '/manifests/' + tag, anonymous, accept=MANIFEST_TYPES)
    if status != 200:
        raise ValueError('ANONYMOUS_MANIFEST_HTTP_' + str(status))
    digest = headers.get('docker-content-digest', '')
    if not re.fullmatch(r'sha256:[0-9a-f]{64}', digest) or digest != 'sha256:' + hashlib.sha256(raw).hexdigest():
        raise ValueError('ANONYMOUS_MANIFEST_DIGEST_INVALID')
    manifest = json.loads(raw)
    config = manifest.get('config', {}).get('digest', '')
    layers = manifest.get('layers', [])
    if not re.fullmatch(r'sha256:[0-9a-f]{64}', config) or not isinstance(layers, list) or not layers:
        raise ValueError('IMAGE_MANIFEST_INVALID')
    status, config_raw, _ = registry.request(base + '/blobs/' + config, anonymous)
    if status != 200 or 'sha256:' + hashlib.sha256(config_raw).hexdigest() != config:
        raise ValueError('ANONYMOUS_CONFIG_UNVERIFIED')
    image_config = json.loads(config_raw)
    labels = (image_config.get('config') or {}).get('Labels') or {}
    expected = {'org.opencontainers.image.source': 'https://github.com/' + REPOSITORY,
                'org.opencontainers.image.revision': commit,
                'app.mrfps.bundle.sha256': bundle, 'app.mrfps.server.sha256': server}
    if any(labels.get(key) != value for key, value in expected.items()):
        raise ValueError('PUBLIC_IMAGE_LABEL_MISMATCH')
    layer = max(layers, key=lambda item: item.get('size', 0)).get('digest', '')
    if not re.fullmatch(r'sha256:[0-9a-f]{64}', layer):
        raise ValueError('IMAGE_LAYER_DIGEST_INVALID')
    status, _, _ = registry.request(base + '/blobs/' + layer, anonymous, one_byte=True)
    if status not in (200, 206):
        raise ValueError('ANONYMOUS_LAYER_HTTP_' + str(status))
    return {'status': 'public', 'image': f'ghcr.io/{OWNER}/{package}@{digest}',
            'tag': tag, 'bundle_sha256': bundle, 'server_sha256': server, 'source_commit': commit,
            'anonymous_manifest_and_layer_verified': True}


def preflight(bundle: str, server: str, commit: str, actor: str, token: str) -> dict:
    package, tag = validate(bundle, server, commit)
    basic = 'Basic ' + base64.b64encode((actor + ':' + token).encode()).decode()
    status, authorization = registry.token_for(OWNER, package, basic)
    if status != 200:
        raise ValueError('AUTHENTICATED_REGISTRY_TOKEN_HTTP_' + str(status))
    status, raw, _ = registry.request(f'https://ghcr.io/v2/{OWNER}/{package}/tags/list', authorization)
    if status == 404:
        return {'status': 'absent', 'image': f'ghcr.io/{OWNER}/{package}', 'tag': tag}
    if status != 200:
        raise ValueError('REGISTRY_PACKAGE_STATE_UNVERIFIED_HTTP_' + str(status))
    parsed = json.loads(raw)
    tags = parsed.get('tags')
    if parsed.get('name') != f'{OWNER}/{package}' or not isinstance(tags, list) or tag not in tags:
        raise ValueError('EXISTING_PACKAGE_OR_TAG_UNVERIFIED')
    return verify_public(bundle, server, commit, token)


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('operation', choices=('preflight', 'verify'))
    parser.add_argument('--bundle-sha256', required=True)
    parser.add_argument('--server-sha256', required=True)
    parser.add_argument('--source-commit', required=True)
    args = parser.parse_args()
    token, actor = os.environ.get('GH_TOKEN', ''), os.environ.get('GITHUB_ACTOR', '')
    if not token or not actor:
        raise ValueError('SCOPED_JOB_TOKEN_REQUIRED')
    result = (preflight(args.bundle_sha256, args.server_sha256, args.source_commit, actor, token)
              if args.operation == 'preflight' else
              verify_public(args.bundle_sha256, args.server_sha256, args.source_commit, token))
    print(json.dumps(result, sort_keys=True))
    return 0


if __name__ == '__main__':
    raise SystemExit(main())