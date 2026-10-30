"""Regression check for the actual Unreal HTTP peer-verification config."""
from pathlib import Path
import unittest

ROOT=Path(__file__).resolve().parents[2]
SECTION='/script/engine.networksettings'
KEY='n.verifypeer'


def settings(path: Path):
    section=''
    for line in path.read_text(encoding='utf-8-sig').splitlines():
        line=line.strip()
        if not line or line.startswith((';','#')): continue
        if line.startswith('[') and line.endswith(']'):
            section=line[1:-1].strip().lower(); continue
        if section==SECTION and '=' in line:
            key,value=line.split('=',1)
            if key.strip().lstrip('+-!.').lower()==KEY:
                yield value.split(';',1)[0].strip().lower()


class TlsConfigTests(unittest.TestCase):
    def test_peer_verification_is_explicit_and_never_disabled_by_project_engine_config(self):
        root=ROOT/'Config'; main=root/'DefaultEngine.ini'
        self.assertEqual(list(settings(main)),['true'])
        # A platform-specific Engine.ini can supersede the project default.
        for path in root.rglob('*Engine.ini'):
            with self.subTest(config=str(path.relative_to(root))):
                self.assertTrue(all(value=='true' for value in settings(path)))


if __name__=='__main__': unittest.main()
