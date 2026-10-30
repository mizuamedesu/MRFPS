"""Stream-check a public server bundle and its ELF before publishing."""
import argparse
import hashlib
import json
from pathlib import Path
import re
import tarfile

import server_bundle as bundle

PEM = re.compile(rb'-----BEGIN ((?:ENCRYPTED |RSA |EC )?PRIVATE KEY)-----\r?\n[A-Za-z0-9+/=\r\n]{32,16384}-----END \1-----')
JWT = re.compile(rb'(?<![A-Za-z0-9_-])eyJ[A-Za-z0-9_-]{10,1024}\.[A-Za-z0-9_-]{10,2048}\.[A-Za-z0-9_-]{86}(?![A-Za-z0-9_-])')


def audit(archive: Path, expected_bundle: str, expected_server: str) -> dict:
    if bundle.sha256(archive) != bundle.check_hash(expected_bundle):
        raise ValueError('BUNDLE_SHA256_MISMATCH')
    bundle.check_hash(expected_server)
    files, total, seen, server = 0, 0, set(), None
    suspect = set()
    with tarfile.open(archive, 'r|gz') as stream:
        for member in stream:
            bundle.check_member(member.name)
            if not member.isfile() or member.name in seen or member.size < 0:
                raise ValueError('ARCHIVE_MEMBER_INVALID')
            seen.add(member.name)
            files += 1
            total += member.size
            if files > bundle.MAX_FILES or total > bundle.MAX_BYTES:
                raise ValueError('ARCHIVE_LIMIT_EXCEEDED')
            digest, prior, head = hashlib.sha256(), b'', b''
            with stream.extractfile(member) as payload:
                for chunk in iter(lambda: payload.read(1024*1024), b''):
                    if len(head) < 20:
                        head += chunk[:20-len(head)]
                    digest.update(chunk)
                    scan = prior + chunk
                    if PEM.search(scan) or JWT.search(scan):
                        suspect.add(member.name)
                    prior = scan[-32768:]
            if member.name == bundle.PREFIX + bundle.SERVER:
                if (len(head) != 20 or head[:6] != b'\x7fELF\x02\x01' or
                        int.from_bytes(head[18:20], 'little') != 62):
                    raise ValueError('SERVER_MUST_BE_LINUX_AMD64_ELF')
                server = digest.hexdigest()
    if server != expected_server:
        raise ValueError('SERVER_SHA256_MISMATCH')
    if suspect:
        raise ValueError('CREDENTIAL_LIKE_PAYLOAD_REVIEW_REQUIRED')
    return {'passed':True, 'files':files, 'uncompressed_bytes':total,
            'bundle_sha256':expected_bundle, 'server_sha256':expected_server,
            'known_private_key_and_jwt_patterns_found':False,
            'note':'Digest checks do not establish build provenance or detect all possible secrets.'}


def main():
    p=argparse.ArgumentParser(description=__doc__)
    p.add_argument('--archive',required=True,type=Path)
    p.add_argument('--bundle-sha256',required=True)
    p.add_argument('--server-sha256',required=True)
    args=p.parse_args()
    print(json.dumps(audit(args.archive,args.bundle_sha256,args.server_sha256),sort_keys=True))


if __name__ == '__main__': main()