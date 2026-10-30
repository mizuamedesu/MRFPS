"""Linux integration checks for container readiness against actual /proc sockets."""
import importlib.util
import os
import pathlib
import shutil
import socket
import struct
import subprocess
import sys
import tempfile
import threading
import time
import unittest
from unittest import mock

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
                # A matching filename elsewhere must not impersonate the image's server.
                self.assertFalse(HEALTHCHECK.check_health(port=port))
                self.assertTrue(HEALTHCHECK.check_health(port=port, expected_executables={str(executable)}))
                process.stdin.close()
                process.wait(timeout=5)
                self.assertFalse(HEALTHCHECK.check_health(port=port, expected_executables={str(executable)}))
            finally:
                if process.poll() is None:
                    process.kill()
                    process.wait(timeout=5)
                process.stdout.close()


@unittest.skipUnless(sys.platform.startswith('linux'), 'Requires Linux abstract sockets and SO_PEERCRED')
class LocalResponderTests(unittest.TestCase):
    def probe(self, responder, expected_pid=None):
        address = b'\0mrfps-health-test-' + os.urandom(8).hex().encode('ascii')
        with socket.socket(socket.AF_UNIX, socket.SOCK_STREAM) as server:
            server.bind(address)
            server.listen(1)
            server.settimeout(3)
            errors = []
            def serve():
                try:
                    client, _ = server.accept()
                    with client:
                        client.settimeout(3)
                        request = bytearray()
                        while len(request) < 21:
                            chunk = client.recv(21 - len(request))
                            if not chunk:
                                return
                            request.extend(chunk)
                        response = responder(bytes(request))
                        if response is not None:
                            client.sendall(response)
                except (OSError, TimeoutError) as error:
                    errors.append(error)
            worker = threading.Thread(target=serve)
            worker.start()
            result = HEALTHCHECK.check_local_responder(7777, expected_pid=os.getpid() if expected_pid is None else expected_pid,
                                                       expected_uid=os.geteuid(), address=address)
            worker.join(timeout=4)
            self.assertFalse(worker.is_alive())
            self.assertFalse(errors)
            return result

    def test_nonce_and_actual_peer_accept_matching_game_state(self):
        self.assertTrue(self.probe(lambda request: request + struct.pack('!HB', 7777, 0x03)))

    def test_wrong_port_nonce_or_auth_state_is_rejected(self):
        for responder in (
            lambda request: request + struct.pack('!HB', 7778, 0x03),
            lambda request: request[:5] + bytes([request[5] ^ 1]) + request[6:] + struct.pack('!HB', 7777, 0x03),
            lambda request: request + struct.pack('!HB', 7777, 0x01),
            lambda request: None,
        ):
            with self.subTest(responder=responder):
                self.assertFalse(self.probe(responder))

    def test_same_uid_but_different_peer_pid_cannot_impersonate_server(self):
        self.assertFalse(self.probe(lambda request: request + struct.pack('!HB', 7777, 0x03), expected_pid=os.getpid() + 1))

    def test_other_process_with_same_uid_cannot_impersonate_expected_pid(self):
        address = b'\0mrfps-health-spoof-' + os.urandom(8).hex().encode('ascii')
        ready_read, ready_write = os.pipe()
        child = os.fork()
        if child == 0:
            os.close(ready_read)
            try:
                with socket.socket(socket.AF_UNIX, socket.SOCK_STREAM) as server:
                    server.settimeout(3)
                    server.bind(address)
                    server.listen(1)
                    os.write(ready_write, b'R')
                    connection, _ = server.accept()
                    connection.close()
                os._exit(0)
            except OSError:
                os._exit(2)
        os.close(ready_write)
        try:
            self.assertEqual(os.read(ready_read, 1), b'R')
            self.assertFalse(HEALTHCHECK.check_local_responder(7777, expected_pid=os.getpid(),
                                                               expected_uid=os.geteuid(), address=address))
        finally:
            os.close(ready_read)
            _, status = os.waitpid(child, 0)
            self.assertTrue(os.WIFEXITED(status) and os.WEXITSTATUS(status) == 0)

    def test_missing_listener_fails_closed(self):
        address = b'\0mrfps-health-missing-' + os.urandom(8).hex().encode('ascii')
        self.assertFalse(HEALTHCHECK.check_local_responder(7777, expected_pid=os.getpid(), expected_uid=os.geteuid(), address=address))


@unittest.skipUnless(sys.platform.startswith('linux') and os.environ.get('MRFPS_HEALTH_FIXTURE_BIN'),
                     'Requires the separately built Linux responder fixture')
class HealthResponderCppIntegration(unittest.TestCase):
    def start(self, port=7777, ready=True, tick=True):
        process = subprocess.Popen([os.environ['MRFPS_HEALTH_FIXTURE_BIN'], str(port), str(int(ready)), str(int(tick))],
                                   stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
        self.assertEqual(process.stdout.readline().strip(), 'READY')
        def cleanup():
            if process.poll() is None:
                process.stdin.close()
                try:
                    process.wait(timeout=5)
                except subprocess.TimeoutExpired:
                    process.kill()
                    process.wait(timeout=5)
            process.stdout.close()
            process.stderr.close()
        self.addCleanup(cleanup)
        return process

    def check(self, process, port=7777, expected_pid=None):
        return HEALTHCHECK.check_local_responder(port, expected_pid=process.pid if expected_pid is None else expected_pid,
                                                 expected_uid=os.geteuid())

    def test_real_responder_matches_process_port_and_fresh_nonce(self):
        process = self.start()
        self.assertTrue(self.check(process))
        self.assertTrue(self.check(process))
        self.assertFalse(self.check(process, expected_pid=process.pid + 1))

    def test_fragmented_request_is_bounded_and_answered(self):
        process = self.start()
        nonce = os.urandom(16)
        with socket.socket(socket.AF_UNIX, socket.SOCK_STREAM) as client:
            client.settimeout(2)
            client.connect(HEALTHCHECK.HEALTH_ADDRESS)
            client.sendall(HEALTHCHECK.HEALTH_MAGIC[:2])
            time.sleep(.05)
            client.sendall(HEALTHCHECK.HEALTH_MAGIC[2:] + nonce)
            response = bytearray()
            while len(response) < 24:
                chunk = client.recv(24 - len(response))
                self.assertTrue(chunk)
                response.extend(chunk)
        self.assertEqual(bytes(response), HEALTHCHECK.HEALTH_MAGIC + nonce + struct.pack('!HB', 7777, 0x03))

    def test_wrong_port_and_unready_auth_are_not_healthy(self):
        process = self.start(port=7778)
        self.assertFalse(self.check(process))
        process.stdin.close(); process.wait(timeout=5)
        other = self.start(ready=False)
        self.assertFalse(self.check(other))

    def test_game_thread_that_stops_ticking_is_not_healthy(self):
        process = self.start(tick=False)
        self.assertFalse(self.check(process))

    def test_stopped_responder_cannot_leave_a_stale_health_endpoint(self):
        process = self.start()
        self.assertTrue(self.check(process))
        process.stdin.close(); process.wait(timeout=5)
        self.assertFalse(self.check(process))

    @unittest.skipUnless(getattr(os, 'geteuid', lambda: -1)() == 10001, 'Requires the production unprivileged UID')
    def test_proc_denial_fallback_requires_exact_executable_and_bound_port(self):
        process = self.start()
        original = HEALTHCHECK.check_local_responder
        with tempfile.TemporaryDirectory(prefix='mrfps-health-proc-') as temporary:
            proc = pathlib.Path(temporary)
            (proc / 'net').mkdir()
            (proc / '1' / 'fd').mkdir(parents=True)
            (proc / 'net' / 'udp').write_text('header\n0: 00000000:1E61 00000000:0000 07 0 0 0 10001 0 42\n')
            (proc / '1' / 'exe').symlink_to('/opt/mrfps/MRFPS/Binaries/Linux/MRFPSServer')
            with mock.patch.dict(os.environ, {'MRFPS_AUTH_PUBLIC_KEY_B64':'', 'MRFPS_AUTH_PUBLIC_KEY_PEM':PUBLIC_TEST_PEM,
                                             'MRFPS_HEALTH_LOCAL_SOCKET':'1'}), \
                 mock.patch.object(HEALTHCHECK, 'check_local_responder', side_effect=lambda port: original(
                     port, expected_pid=process.pid, expected_uid=10001)):
                self.assertTrue(HEALTHCHECK.check_health(proc_root=proc, port=7777))
                (proc / '1' / 'exe').unlink()
                (proc / '1' / 'exe').symlink_to('/tmp/MRFPSServer')
                self.assertFalse(HEALTHCHECK.check_health(proc_root=proc, port=7777))
                (proc / 'net' / 'udp').write_text('header\n')
                self.assertFalse(HEALTHCHECK.check_health(proc_root=proc, port=7777))


if __name__ == '__main__':
    unittest.main(verbosity=2)
