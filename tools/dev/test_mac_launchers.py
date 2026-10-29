"""Mac launcher argv/preflight fixtures. Never runs UE, an app, a compiler or a device."""
import json
import os
import pathlib
import shutil
import subprocess
import sys
import tempfile
import unittest


ROOT = pathlib.Path(__file__).resolve().parents[2]
if sys.platform == "win32":
    BASH = next((str(path) for path in (
        pathlib.Path("C:/Program Files/Git/bin/bash.exe"),
        pathlib.Path("C:/Program Files (x86)/Git/bin/bash.exe"),
    ) if path.is_file()), None)
else:
    BASH = shutil.which("bash")


@unittest.skipUnless(BASH, "Bash is required; no WSL/emulator is started for these tests")
class MacLauncherTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(prefix="mrfps mac scripts ")
        self.addCleanup(self.temp.cleanup)
        self.work = pathlib.Path(self.temp.name)
        self.project = self.work / "fake project"
        scripts = self.project / "Scripts/mac"
        scripts.mkdir(parents=True)
        for name in ("build.sh", "run-client.sh"):
            shutil.copyfile(ROOT / "Scripts/mac" / name, scripts / name)
        (self.project / "MRFPS.uproject").write_text("{}", encoding="utf-8")
        guard = self.project / "tools/dev/mac_resource_guard.py"
        guard.parent.mkdir(parents=True)
        guard.write_text("raise SystemExit(97) # fixture guard must be intercepted\n", encoding="utf-8")
        self.bin = self.work / "tools"
        self.bin.mkdir()
        self.engine = self.work / "fake engine"
        self.record = self.work / "argv.jsonl"
        recorder = self.work / "record.py"
        recorder.write_text(
            "import json,os,sys\n"
            "with open(os.environ['MRFPS_TEST_RECORD'],'a',encoding='utf-8') as f:\n"
            " f.write(json.dumps(sys.argv[1:])+chr(10))\n", encoding="utf-8")
        self.executable(self.bin / "uname", "#!/bin/bash\nprintf 'Darwin\\n'\n")
        self.executable(self.bin / "python3", """#!/bin/bash
set -eu
if [[ "${1##*/}" == mac_resource_guard.py ]]; then
  shift
  [[ "$1" == -- ]] && shift
  exec "$MRFPS_TEST_PYTHON" "$MRFPS_TEST_RECORDER" "$@"
fi
exec "$MRFPS_TEST_PYTHON" "$@"
""")
        version = self.engine / "Engine/Build/Build.version"
        version.parent.mkdir(parents=True)
        version.write_text(json.dumps({"MajorVersion": 5, "MinorVersion": 8, "PatchVersion": 1}), encoding="utf-8")
        for relative in ("Engine/Build/BatchFiles/Mac/Build.sh", "Engine/Build/BatchFiles/RunUAT.sh",
                         "Engine/Binaries/Mac/UnrealEditor.app/Contents/MacOS/UnrealEditor"):
            # A test fails if a fake engine executable is accidentally executed;
            # the fake memory guard records argv instead of spawning its command.
            self.executable(self.engine / relative, "#!/usr/bin/env bash\nexit 97\n")
        self.token = self.work / "access token.jwt"
        self.public = self.work / "issuer public.pem"
        self.token.write_text("DO_NOT_PASS_TOKEN_CONTENT_IN_ARGV", encoding="ascii")
        self.public.write_text("public key fixture", encoding="ascii")
        self.env = dict(os.environ, UE_ROOT=self.engine.as_posix(), PYTHONUTF8="1",
                        MRFPS_TEST_PYTHON=pathlib.Path(sys.executable).as_posix(),
                        MRFPS_TEST_RECORDER=recorder.as_posix(), MRFPS_TEST_RECORD=str(self.record))
        self.env.pop("MRFPS_APP", None)
        self.env["MSYS2_ARG_CONV_EXCL"] = "*" # Preserve /Game paths when a Windows Bash hosts this argv-only test.
        bin_path = self.bin.as_posix()
        if sys.platform == "win32":
            bin_path = "/" + bin_path[0].lower() + bin_path[2:]
        self.env["MRFPS_TEST_BIN"] = bin_path

    @staticmethod
    def executable(path, source):
        path.parent.mkdir(parents=True, exist_ok=True)
        with path.open("w", encoding="utf-8", newline="\n") as stream:
            stream.write(source)
        path.chmod(0o755)

    def invoke(self, script, *args):
        runner = 'export PATH="$MRFPS_TEST_BIN:$PATH"; exec /bin/bash "$@"'
        return subprocess.run([BASH, "--noprofile", "--norc", "-c", runner, "mrfps-mac-test",
                               (self.project / "Scripts/mac" / script).as_posix(), *args],
                              env=self.env, text=True, capture_output=True, encoding="utf-8", timeout=15)

    def recorded(self):
        return [json.loads(line) for line in self.record.read_text(encoding="utf-8").splitlines()] if self.record.exists() else []

    def test_invalid_target_or_extra_args_never_reach_engine(self):
        for args in (("Sever",), ("Editor", "unexpected")):
            with self.subTest(args=args):
                result = self.invoke("build.sh", *args)
                self.assertEqual(result.returncode, 2, result.stderr)
                self.assertEqual(self.recorded(), [])

    def test_wrong_engine_version_fails_before_build(self):
        (self.engine / "Engine/Build/Build.version").write_text(
            json.dumps({"MajorVersion": 5, "MinorVersion": 7}), encoding="utf-8")
        result = self.invoke("build.sh", "Client")
        self.assertNotEqual(result.returncode, 0)
        self.assertEqual(self.recorded(), [])

    def test_client_build_orders_guarded_commands_with_bounded_actions(self):
        result = self.invoke("build.sh", "Client")
        self.assertEqual(result.returncode, 0, result.stderr)
        calls = self.recorded()
        self.assertEqual(len(calls), 4)
        self.assertEqual(calls[0][1:4], ["MRFPSEditor", "Mac", "Development"])
        self.assertEqual(calls[2][1:4], ["MRFPS", "Mac", "Development"])
        for call in (calls[0], calls[2]):
            self.assertIn("-MaxParallelActions=1", call)
            self.assertIn("-NoUBA", call)
            self.assertEqual(len([arg for arg in call if arg.startswith("-Project=")]), 1)
        self.assertIn("-run=MRFPSSetup", calls[1])
        self.assertIn("-NullRHI", calls[1])
        self.assertEqual(calls[3][1], "BuildCookRun")
        self.assertIn("-platform=Mac", calls[3])
        self.assertIn("-skipbuild", calls[3])

    def test_editor_launch_preserves_credential_paths_and_role(self):
        result = self.invoke("run-client.sh", "127.0.0.1:7777", self.token.as_posix(), self.public.as_posix())
        self.assertEqual(result.returncode, 0, result.stderr)
        calls = self.recorded()
        self.assertEqual(len(calls), 1)
        args = calls[0]
        self.assertIn("-game", args)
        self.assertIn("/Game/Maps/MRFPSArena", args)
        self.assertIn("-MRFPSInspector", args)
        self.assertIn("-MRFPSDesktop", args)
        self.assertIn("-nohmd", args)
        self.assertEqual(len([arg for arg in args if arg.startswith("-MRFPSAuthTokenFile=")]), 1)
        self.assertEqual(len([arg for arg in args if arg.startswith("-MRFPSAuthPublicKeyFile=")]), 1)
        self.assertNotIn(self.token.read_text(), " ".join(args))
        self.assertNotIn("-MRFPSCaptureDesktopView", args)

    def test_explicit_packaged_executable_uses_admin_role(self):
        app = self.work / "standalone game.app/Contents/MacOS/MRFPS"
        self.executable(app, "#!/usr/bin/env bash\nexit 97\n")
        self.env["MRFPS_APP"] = app.as_posix()
        result = self.invoke("run-client.sh", "127.0.0.1:7777", self.token.as_posix(), self.public.as_posix(), "admin")
        self.assertEqual(result.returncode, 0, result.stderr)
        calls = self.recorded()
        self.assertEqual(len(calls), 1)
        self.assertIn("-MRFPSAdmin", calls[0])
        self.assertNotIn("-game", calls[0])

    def test_missing_app_or_invalid_role_never_reaches_guard(self):
        self.env["MRFPS_APP"] = (self.work / "missing game").as_posix()
        result = self.invoke("run-client.sh", "127.0.0.1:7777", self.token.as_posix(), self.public.as_posix())
        self.assertEqual(result.returncode, 2, result.stderr)
        self.assertEqual(self.recorded(), [])
        result = self.invoke("run-client.sh", "127.0.0.1:7777", self.token.as_posix(), self.public.as_posix(), "player")
        self.assertEqual(result.returncode, 2, result.stderr)
        self.assertEqual(self.recorded(), [])


class MacPluginConfigurationTests(unittest.TestCase):
    def test_configured_input_classes_have_a_mac_plugin_dependency(self):
        project = json.loads((ROOT / "MRFPS.uproject").read_text(encoding="utf-8"))
        self.assertTrue(project["DisableEnginePluginsByDefault"])
        openxr = next(plugin for plugin in project["Plugins"] if plugin["Name"] == "OpenXR")
        self.assertNotIn("Mac", openxr["PlatformAllowList"])
        settings = (ROOT / "Config/DefaultInput.ini").read_text(encoding="utf-8")
        self.assertIn("DefaultPlayerInputClass=/Script/EnhancedInput.EnhancedPlayerInput", settings)
        self.assertIn("DefaultInputComponentClass=/Script/EnhancedInput.EnhancedInputComponent", settings)
        xr = json.loads((ROOT / "Plugins/MRFPSXR/MRFPSXR.uplugin").read_text(encoding="utf-8"))
        dependency = next(plugin for plugin in xr["Plugins"] if plugin["Name"] == "EnhancedInput")
        self.assertTrue(dependency["Enabled"])
        self.assertEqual(dependency["PlatformAllowList"], ["Mac"])
        self.assertIn("Server", dependency["TargetDenyList"])


if __name__ == "__main__":
    unittest.main()
