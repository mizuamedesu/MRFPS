"""Plan or idempotently dispatch an explicitly approved public server publication."""
from __future__ import annotations

import argparse
import json
from pathlib import Path
import re
import subprocess
import sys
import tempfile
import time

import audit_public_bundle
import public_ghcr

REPOSITORY = 'mizuamedesu/MRFPS'
WORKFLOW = 'publish-public-linux-server.yml'


def gh(*args: str) -> str:
    result = subprocess.run(['gh', *args], capture_output=True, timeout=600)
    if result.returncode:
        raise RuntimeError('GitHub CLI failed during ' + (args[0] if args else 'operation'))
    return result.stdout.decode('utf-8', 'strict')


def api(path: str):
    return json.loads(gh('api', path))


def releases() -> list[dict]:
    pages = json.loads(gh('api', '--paginate', '--slurp', f'repos/{REPOSITORY}/releases?per_page=100'))
    if not isinstance(pages, list) or any(not isinstance(page, list) for page in pages):
        raise RuntimeError('RELEASE_LIST_UNVERIFIED')
    return [release for page in pages for release in page]


def find_draft(tag: str) -> dict | None:
    found = [item for item in releases() if item.get('tag_name') == tag]
    if len(found) > 1:
        raise RuntimeError('RELEASE_TAG_AMBIGUOUS')
    if not found:
        return None
    if found[0].get('draft') is not True:
        raise RuntimeError('RELEASE_MUST_REMAIN_DRAFT')
    return found[0]


def validate_asset(release: dict, bundle: str) -> bool:
    assets = release.get('assets')
    if not isinstance(assets, list):
        raise RuntimeError('RELEASE_ASSETS_UNVERIFIED')
    matches = [asset for asset in assets if asset.get('name') == 'linux-server.tar.gz']
    if not matches:
        return False
    if len(matches) != 1 or matches[0].get('digest') != 'sha256:' + bundle or matches[0].get('state') != 'uploaded':
        raise RuntimeError('EXISTING_ASSET_DIGEST_MISMATCH_OR_UNVERIFIED')
    return True


def create_or_reuse_draft(tag: str, source: str, server: str, bundle: str, archive: Path) -> dict:
    release = find_draft(tag)
    if release is None:
        with tempfile.TemporaryDirectory() as tmp:
            notes = Path(tmp)/'notes.txt'
            notes.write_text(f'Approved public server image input. Source {source}. ELF {server}. Remains a draft.\n',encoding='utf-8')
            gh('release','create',tag,'--repo',REPOSITORY,'--draft','--target','main',
               '--title','MRFPS server input '+bundle[:12],'--notes-file',str(notes))
        for _ in range(8):
            release=find_draft(tag)
            if release is not None: break
            time.sleep(.75)
    if release is None:
        raise RuntimeError('DRAFT_CREATION_UNCONFIRMED')
    if not validate_asset(release,bundle):
        # Never clobber an existing asset. A failed upload has unknown outcome;
        # rerunning observes the server-side digest before any retry.
        gh('release','upload',tag,str(archive),'--repo',REPOSITORY)
        for _ in range(20):
            release=find_draft(tag)
            if release is not None and validate_asset(release,bundle): break
            time.sleep(1)
        else:
            raise RuntimeError('UPLOADED_ASSET_DIGEST_UNCONFIRMED')
    return release


def dispatch(tag: str, source: str, bundle: str, server: str) -> dict:
    workflow=api(f'repos/{REPOSITORY}/actions/workflows/{WORKFLOW}')
    if workflow.get('state') != 'active' or workflow.get('path') != '.github/workflows/'+WORKFLOW:
        raise RuntimeError('PUBLIC_WORKFLOW_NOT_ACTIVE_ON_GITHUB')
    title=f'Public server {bundle} {source}'
    runs=json.loads(gh('run','list','--repo',REPOSITORY,'--workflow',WORKFLOW,'--limit','100',
                       '--json','databaseId,displayTitle,status,conclusion'))
    matches=[run for run in runs if run.get('displayTitle') == title]
    for run in matches:
        if run.get('status') != 'completed':
            return {'status':'already_in_progress','run_id':run.get('databaseId')}
        if run.get('conclusion') == 'success':
            return {'status':'already_verified','run_id':run.get('databaseId')}
    gh('workflow','run',WORKFLOW,'--repo',REPOSITORY,'--ref','main',
       '-f','source_commit='+source,'-f','release_tag='+tag,
       '-f','bundle_sha256='+bundle,'-f','server_sha256='+server)
    return {'status':'dispatched','run_id':None}


def run(args: argparse.Namespace) -> dict:
    directory=args.bundle_directory.resolve(strict=True)
    archive=directory/'linux-server.tar.gz'
    manifest=json.loads((directory/'server-artifact.json').read_text(encoding='utf-8-sig'))
    source=args.source_commit
    server=args.server_sha256
    bundle=manifest.get('bundle_sha256','')
    package, image_tag=public_ghcr.validate(bundle,server,source)
    if manifest.get('source_commit') != source or manifest.get('server_sha256') != server:
        raise RuntimeError('MANIFEST_SOURCE_OR_SERVER_MISMATCH')
    repo=api('repos/'+REPOSITORY)
    if repo.get('full_name','').lower() != REPOSITORY.lower() or repo.get('private') is not False or not repo.get('permissions',{}).get('push'):
        raise RuntimeError('PUBLIC_REPOSITORY_OR_WRITE_ACCESS_UNVERIFIED')
    comparison=api(f'repos/{REPOSITORY}/compare/{source}...main')
    if comparison.get('status') not in ('ahead','identical'):
        raise RuntimeError('SOURCE_NOT_IN_MAIN_HISTORY')
    audit=audit_public_bundle.audit(archive,bundle,server)
    tag=f'server-artifacts-{bundle[:12]}-{source[:12]}'
    release=find_draft(tag)
    if release is not None:
        validate_asset(release,bundle)
    result={'mode':args.mode,'repository':REPOSITORY,'release_tag':tag,'bundle_sha256':bundle,
            'server_sha256':server,'source_commit':source,'image':f'ghcr.io/{public_ghcr.OWNER}/{package}:{image_tag}',
            'local_bundle_audit':audit,'source_commit_proven_by_build':False}
    if args.mode == 'publish':
        create_or_reuse_draft(tag,source,server,bundle,archive)
        result.update(dispatch(tag,source,bundle,server))
    return result


def main() -> int:
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--bundle-directory',required=True,type=Path)
    parser.add_argument('--source-commit',required=True)
    parser.add_argument('--server-sha256',required=True)
    parser.add_argument('--mode',choices=('plan','publish'),default='plan')
    try:
        print(json.dumps(run(parser.parse_args()),sort_keys=True))
        return 0
    except (ValueError,RuntimeError,OSError,subprocess.SubprocessError,UnicodeError,json.JSONDecodeError) as exc:
        print(json.dumps({'status':'error','message':str(exc)}),file=sys.stderr)
        return 1


if __name__ == '__main__': raise SystemExit(main())