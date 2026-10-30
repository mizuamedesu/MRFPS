import hashlib
import io
from pathlib import Path
import tarfile
import tempfile
import unittest

import audit_public_bundle as audit
import server_bundle as bundle


class PublicBundleTests(unittest.TestCase):
    def make_archive(self, payload=b'normal data', name='asset.bin'):
        temp=tempfile.TemporaryDirectory(); self.addCleanup(temp.cleanup)
        path=Path(temp.name)/'bundle.tar.gz'
        elf=b'\x7fELF\x02\x01'+b'\x00'*12+(62).to_bytes(2,'little')+b'fixture'
        with tarfile.open(path,'w:gz') as tar:
            for path_name,data in [(bundle.PREFIX+bundle.SERVER,elf),(bundle.PREFIX+name,payload)]:
                member=tarfile.TarInfo(path_name);member.size=len(data);tar.addfile(member,io.BytesIO(data))
        return path,bundle.sha256(path),hashlib.sha256(elf).hexdigest()

    def test_valid_archive_and_elf(self):
        archive,hash_,server=self.make_archive()
        self.assertTrue(audit.audit(archive,hash_,server)['passed'])

    def test_private_key_pattern_is_refused(self):
        payload=b'-----BEGIN PRIVATE KEY-----\n'+b'A'*64+b'\n-----END PRIVATE KEY-----'
        archive,hash_,server=self.make_archive(payload)
        with self.assertRaisesRegex(ValueError,'CREDENTIAL_LIKE_PAYLOAD'): audit.audit(archive,hash_,server)

    def test_wrong_elf_or_bundle_and_forbidden_name_are_refused(self):
        archive,hash_,server=self.make_archive()
        with self.assertRaisesRegex(ValueError,'SERVER_SHA256'): audit.audit(archive,hash_,'f'*64)
        with self.assertRaisesRegex(ValueError,'BUNDLE_SHA256'): audit.audit(archive,'f'*64,server)
        archive,hash_,server=self.make_archive(name='secret.pem')
        with self.assertRaisesRegex(ValueError,'CREDENTIAL_FILE'): audit.audit(archive,hash_,server)


if __name__ == '__main__': unittest.main()