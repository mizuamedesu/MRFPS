"""Linux integration checks for container readiness against actual /proc sockets."""
import importlib.util
import os
import pathlib
import shutil
import socket
import subprocess
import sys
import tempfile
import unittest

ROOT = pathlib.Path(__file__).resolve().parent.parent
sys.path.insert(0, str(ROOT / 'Deploy/Linux'))
SPEC = importlib.util.spec_from_file_location('healthcheck', ROOT / 'Deploy/Linux/healthcheck.py')
HEALTHCHECK = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(HEALTHCHECK)
from auth_config import validate_environment

PUBLIC_TEST_PEM = '-----BEGIN PUBLIC KEY-----\nMCowBQYDK2VwAyEA11qYAYKxCrfVS/7TyWQHOg7hcvPapiMlrwIaaPcHURo=\n-----END PUBLIC KEY-----\n'


class AuthConfigTests(unittest.TestCase):
    def test_missing_ambiguous_and_private_key_fail_closed(self):
        self.assertFalse(validate_environment({}))
        self.assertFalse(validate_environment({'MRFPS_AUTH_PUBLIC_KEY_B64':'YWJj','MRFPS_AUTH_PUBLIC_KEY_PEM':PUBLIC_TEST_PEM}))
        self.assertFalse(validate_environment({'MRFPS_AUTH_PUBLIC_KEY_PEM':'-----BEGIN PRIVATE KEY-----\nsecret\n-----END PRIVATE KEY-----'}))

    def test_public_key_encodings_and_bad_audience(self):
        import base64
        self.assertTrue(validate_environment({'MRFPS_AUTH_PUBLIC_KEY_PEM':PUBLIC_TEST_PEM}))
        self.assertTrue(validate_environment({'MRFPS_AUTH_PUBLIC_KEY_B64':base64.b64encode(PUBLIC_TEST_PEM.encode('ascii')).decode('ascii')}))
        self.assertFalse(validate_environment({'MRFPS_AUTH_PUBLIC_KEY_PEM':PUBLIC_TEST_PEM,'MRFPS_AUTH_AUDIENCE':'bad audience'}))
        self.assertFalse(validate_environment({'MRFPS_AUTH_PUBLIC_KEY_B64':'not base64'}))


@unittest.skipUnless(sys.platform.startswith('linux'), 'Requires Linux /proc socket ownership')
class ContainerHealthIntegration(unittest.TestCase):
    def setUp(self):
        # RFC8032 public test vector only; no private key enters this environment.
        self.prior_public = os.environ.get('MRFPS_AUTH_PUBLIC_KEY_PEM')
        self.prior_encoded = os.environ.pop('MRFPS_AUTH_PUBLIC_KEY_B64', None)
        os.environ['MRFPS_AUTH_PUBLIC_KEY_PEM'] = PUBLIC_TEST_PEM

    def tearDown(self):
        if self.prior_public is None:
            os.environ.pop('MRFPS_AUTH_PUBLIC_KEY_PEM', None)
        else:
            os.environ['MRFPS_AUTH_PUBLIC_KEY_PEM'] = self.prior_public
        if self.prior_encoded is not None:
            os.environ['MRFPS_AUTH_PUBLIC_KEY_B64'] = self.prior_encoded

    def test_unrelated_udp_socket_cannot_report_server_healthy(self):
        with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as other:
            other.bind(('127.0.0.1', 0))
            self.assertFalse(HEALTHCHECK.check_health(port=other.getsockname()[1]))

    def test_actual_server_named_process_socket_and_shutdown(self):
        with tempfile.TemporaryDirectory(prefix='mrfps-health-') as temporary:
            executable = pathlib.Path(temporary) / 'MRFPSServer'
            shutil.copyfile(sys.executable, executable)
            executable.chmod(0o755)
            code = 'import socket,sys; s=socket.socket(socket.AF_INET,socket.SOCK_DGRAM); s.bind(("127.0.0.1",0)); print(s.getsockname()[1],flush=True); sys.stdin.read()'
            process = subprocess.Popen([str(executable), '-c', code], stdin=subprocess.PIPE, stdout=subprocess.PIPE, text=True)
            try:
                port = int(process.stdout.readline().strip())
                self.assertTrue(HEALTHCHECK.check_health(port=port))
                process.stdin.close()
                process.wait(timeout=5)
                self.assertFalse(HEALTHCHECK.check_health(port=port))
            finally:
                if process.poll() is None:
                    process.kill()
                    process.wait(timeout=5)
                process.stdout.close()


if __name__ == '__main__':
    unittest.main(verbosity=2)
