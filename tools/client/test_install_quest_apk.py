import argparse
import hashlib
from pathlib import Path
import subprocess
import tempfile
import unittest
from unittest import mock

import install_quest_apk as install


class InstallTests(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self.tmp.cleanup)
        self.root = Path(self.tmp.name)
        self.apk = self.root / 'client.apk'
        self.apk.write_bytes(b'fixture')
        self.digest = hashlib.sha256(b'fixture').hexdigest()
        self.args = argparse.Namespace(adb='adb', aapt=Path('aapt'), serial='fixture', apk=self.apk, install=False)
        self.stack = __import__('contextlib').ExitStack()
        self.addCleanup(self.stack.close)
        self.stack.enter_context(mock.patch.object(install.login, 'ROOT', self.root))
        self.badging = self.stack.enter_context(mock.patch.object(install.subprocess, 'run', return_value=subprocess.CompletedProcess([], 0, b"package: name='org.mrfps.arena'\n")))
        self.adb = self.stack.enter_context(mock.patch.object(install.login, 'run_adb', return_value='device'))
        self.current = self.stack.enter_context(mock.patch.object(install, 'installed_hash', return_value='old'))
        self.guard = self.stack.enter_context(mock.patch.object(install.login, 'assert_quest_share_clear'))
        self.mutate = self.stack.enter_context(mock.patch.object(install.login, 'adb_result', return_value=subprocess.CompletedProcess([], 0, b'Performing Streamed Install\nSuccess\n', b'')))

    def test_dry_check_does_not_install(self):
        self.assertEqual(install.run(self.args)['status'], 'different')
        self.guard.assert_not_called()
        self.mutate.assert_not_called()

    def test_matching_apk_is_idempotent(self):
        self.current.return_value = self.digest
        self.args.install = True
        self.assertEqual(install.run(self.args)['status'], 'already_installed')
        self.guard.assert_not_called()
        self.mutate.assert_not_called()

    def test_install_is_guarded_and_verified(self):
        self.args.install = True
        self.current.side_effect = ['old', self.digest]
        self.assertEqual(install.run(self.args)['status'], 'installed')
        self.guard.assert_called_once()
        self.mutate.assert_called_once()

    def test_permission_prompt_blocks_install(self):
        self.args.install = True
        self.guard.side_effect = RuntimeError('permission prompt')
        with self.assertRaisesRegex(RuntimeError, 'permission prompt'): install.run(self.args)
        self.mutate.assert_not_called()

    def test_bad_package_and_bad_installed_hash_do_not_claim_success(self):
        self.badging.return_value.stdout = b"package: name='other.package'\n"
        with self.assertRaisesRegex(ValueError, 'APK package'): install.run(self.args)
        self.mutate.assert_not_called()
        self.badging.return_value.stdout = b"package: name='org.mrfps.arena'\n"
        self.args.install = True
        self.current.side_effect = ['old', 'other']
        with self.assertRaisesRegex(RuntimeError, 'hash does not match'): install.run(self.args)


if __name__ == '__main__': unittest.main()