"""Small Windows atomic-writer/WSL reader regression; no runner or Docker."""
import json
import os
from pathlib import Path
import shutil
import subprocess
import tempfile
import threading
import time
import unittest


@unittest.skipUnless(os.name == "nt" and shutil.which("wsl"), "Windows/WSL report sharing")
class GuardInteropTests(unittest.TestCase):
    def test_windows_replace_and_wsl_readers_keep_a_valid_short_lease(self):
        with tempfile.TemporaryDirectory(prefix="MRFPS-guard-report-") as directory:
            root = Path(directory)
            report = root / "report.json"
            payload = {"status": "running", "lastSample": {"availablePhysicalGB": 12, "commitPercent": 50}}
            report.write_text(json.dumps(payload), encoding="utf-8")
            reader = root / "reader.py"
            reader.write_text('''import json,sys,time
from pathlib import Path
sys.path.insert(0,sys.argv[1])
from ephemeral_runner import guard_reason
reasons={}
for index in range(600):
    reason=guard_reason(Path(sys.argv[2]))
    if reason: reasons[reason]=reasons.get(reason,0)+1
    time.sleep(0.001)
print(json.dumps({"reads":600,"failures":reasons}))
''', encoding="utf-8")
            def linux(path):
                value = str(path.resolve()).replace("\\", "/")
                return "/mnt/" + value[0].lower() + value[2:]
            process = subprocess.Popen(["wsl", "-d", "Ubuntu", "-u", "root", "--exec", "python3", linux(reader), linux(Path(__file__).parent), linux(report)],
                                       stdout=subprocess.PIPE, stderr=subprocess.PIPE, creationflags=subprocess.CREATE_NO_WINDOW)
            errors = []
            def writer():
                try:
                    for index in range(240):
                        temporary = root / "report.incoming"
                        temporary.write_text(json.dumps(payload), encoding="utf-8")
                        for attempt in range(20):
                            try:
                                os.replace(temporary, report)
                                break
                            except PermissionError:
                                if attempt == 19:
                                    raise
                                time.sleep(0.002)
                        time.sleep(0.002)
                except Exception as error:
                    errors.append(type(error).__name__)
            thread = threading.Thread(target=writer)
            thread.start()
            try:
                stdout, stderr = process.communicate(timeout=15)
                thread.join(timeout=5)
                self.assertEqual(process.returncode, 0, stderr.decode(errors="replace"))
                self.assertEqual(errors, [])
                result = json.loads(stdout)
                self.assertEqual(result["reads"], 600)
                self.assertEqual(result["failures"], {})
            finally:
                if process.poll() is None:
                    process.terminate()
                    process.wait(timeout=5)
                thread.join(timeout=5)


if __name__ == "__main__":
    unittest.main()
