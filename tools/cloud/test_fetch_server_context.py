import base64
import hashlib
from pathlib import Path
import tempfile
import unittest

import fetch_server_context as context


class ContextTests(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory()
        self.addCleanup(self.temporary.cleanup)
        self.root = Path(self.temporary.name)
        self.destination = self.root / "server-source"
        self.commit = "a" * 40
        self.calls = []

    def api(self, path, credential=None):
        self.calls.append(path)
        if path == "git/commits/" + self.commit:
            return {"sha": self.commit}
        relative = path.removeprefix("contents/").split("?", 1)[0]
        self.assertIn(relative, context.FILES)
        self.assertTrue(path.endswith("ref=" + self.commit))
        payload = b"public launcher fixture\n"
        return {"type": "file", "path": relative, "encoding": "base64", "size": len(payload),
                "sha": hashlib.sha1(b"blob " + str(len(payload)).encode() + b"\0" + payload).hexdigest(),
                "content": base64.b64encode(payload).decode()}

    def test_only_fixed_small_files_are_materialized_and_credentials_never_reported(self):
        result = context.fetch(self.commit, self.destination, "fixture-credential", api=self.api)
        files = sorted(str(path.relative_to(self.destination)).replace("\\", "/") for path in self.destination.rglob("*") if path.is_file())
        self.assertEqual(files, sorted(context.FILES))
        self.assertEqual(result["file_count"], 11)
        self.assertFalse(result["whole_repository_checkout"])
        self.assertNotIn("fixture-credential", str(result))
        self.assertEqual(len(self.calls), 12)

    def test_commit_mismatch_or_invalid_ref_creates_no_destination(self):
        with self.assertRaisesRegex(ValueError, "SOURCE_COMMIT_RESPONSE_MISMATCH"):
            context.fetch(self.commit, self.destination, api=lambda *_: {"sha": "b" * 40})
        with self.assertRaisesRegex(ValueError, "APPROVED_SOURCE_COMMIT_REQUIRED"):
            context.fetch("main", self.destination, api=self.api)
        self.assertFalse(self.destination.exists())

    def test_wrong_path_symlink_or_oversize_is_rejected_without_partial_context(self):
        for alteration in ({"path": "../../outside"}, {"type": "symlink"}, {"size": context.MAX_FILE_BYTES + 1}):
            def api(path, credential=None):
                value = self.api(path, credential)
                if path.startswith("contents/"):
                    value.update(alteration)
                return value
            with self.subTest(alteration=alteration), self.assertRaisesRegex(ValueError, "SOURCE_FILE_METADATA_INVALID"):
                context.fetch(self.commit, self.destination, api=api)
            self.assertFalse(self.destination.exists())
            self.assertEqual(list(self.root.glob("*.incoming-*")), [])

    def test_blob_digest_size_or_content_tampering_is_rejected_atomically(self):
        for alteration in ({"sha": "b" * 40}, {"size": 0}, {"content": base64.b64encode(b"different").decode()}):
            def api(path, credential=None):
                value = self.api(path, credential)
                if path.startswith("contents/") and "Dockerfile" in path:
                    value.update(alteration)
                return value
            with self.subTest(alteration=alteration), self.assertRaisesRegex(ValueError, "GIT_BLOB_DIGEST_MISMATCH"):
                context.fetch(self.commit, self.destination, api=api)
            self.assertFalse(self.destination.exists())
            self.assertEqual(list(self.root.glob("*.incoming-*")), [])

    def test_existing_destination_is_never_overwritten(self):
        self.destination.mkdir()
        marker = self.destination / "unrelated.txt"
        marker.write_text("leave intact")
        with self.assertRaisesRegex(ValueError, "DESTINATION_MUST_NOT_EXIST"):
            context.fetch(self.commit, self.destination, api=self.api)
        self.assertEqual(marker.read_text(), "leave intact")
        self.assertEqual(self.calls, [])


if __name__ == "__main__":
    unittest.main()
