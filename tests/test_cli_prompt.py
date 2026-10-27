import fcntl
import json
import os
import pty
import select
import signal
import struct
import subprocess
import termios
import threading
import time
import unittest
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path

BINARY = Path(__file__).resolve().parents[1] / "build" / "taia_ct"
KEY = "sk-taia-pty-secret"


class AuthHandler(BaseHTTPRequestHandler):
    authorization = None
    seen = threading.Event()

    def do_GET(self):
        path = self.path.split("?", 1)[0]
        if path in ("/models", "/v1/models"):
            type(self).authorization = self.headers.get("Authorization")
            type(self).seen.set()
            self.send_json(
                {
                    "object": "list",
                    "data": [
                        {
                            "id": "small",
                            "object": "model",
                            "owned_by": "test",
                            "status": {"value": "loaded"},
                        }
                    ],
                }
            )
        elif path in ("/health", "/v1/health"):
            self.send_json({"status": "ok"})
        else:
            self.send_error(404)

    def send_json(self, body):
        payload = json.dumps(body).encode("utf-8")
        self.send_response(200)
        self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(payload)))
        self.end_headers()
        self.wfile.write(payload)

    def log_message(self, format, *args):
        pass


class AuthServer(ThreadingHTTPServer):
    daemon_threads = True


def read_until(fd, data, needle, timeout):
    deadline = time.monotonic() + timeout
    while needle not in data and time.monotonic() < deadline:
        ready, _, _ = select.select([fd], [], [], 0.05)
        if not ready:
            continue
        try:
            chunk = os.read(fd, 65536)
        except BlockingIOError:
            continue
        except OSError:
            break
        if not chunk:
            break
        data.extend(chunk)
    return data


def collect(fd, data, timeout):
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        ready, _, _ = select.select([fd], [], [], 0.05)
        if not ready:
            continue
        try:
            chunk = os.read(fd, 65536)
        except BlockingIOError:
            continue
        except OSError:
            break
        if not chunk:
            break
        data.extend(chunk)
    return data


@unittest.skipUnless(BINARY.is_file(), "taia_ct binary not built")
class CliPromptTest(unittest.TestCase):
    def setUp(self):
        AuthHandler.authorization = None
        AuthHandler.seen.clear()
        self.server = AuthServer(("127.0.0.1", 0), AuthHandler)
        self.thread = threading.Thread(target=self.server.serve_forever, daemon=True)
        self.thread.start()
        host, port = self.server.server_address[:2]
        self.url = "http://{}:{}".format(host, port)

    def tearDown(self):
        self.server.shutdown()
        self.server.server_close()
        self.thread.join(timeout=2)

    def open_pty(self, args, extra_env=None):
        master, slave = pty.openpty()
        fcntl.ioctl(slave, termios.TIOCSWINSZ, struct.pack("HHHH", 40, 120, 0, 0))
        check_fd = os.dup(slave)
        before = termios.tcgetattr(check_fd)
        environment = os.environ.copy()
        for name in (
            "TAIA_API_KEY",
            "TAIA_SERVER_URL",
            "TAIA_WORKSPACE",
            "TAIA_STREAM_START_TIMEOUT_SECONDS",
            "TAIA_STREAM_IDLE_TIMEOUT_SECONDS",
        ):
            environment.pop(name, None)
        if extra_env:
            environment.update(extra_env)
        process = subprocess.Popen(
            [str(BINARY), *args],
            stdin=slave,
            stdout=slave,
            stderr=slave,
            env=environment,
            close_fds=True,
        )
        os.close(slave)
        os.set_blocking(master, False)
        return master, check_fd, before, process

    def wait_for_exit(self, process, master, data, timeout=5):
        deadline = time.monotonic() + timeout
        while process.poll() is None and time.monotonic() < deadline:
            data = collect(master, data, 0.05)
        if process.poll() is None:
            process.terminate()
            try:
                process.wait(timeout=2)
            except subprocess.TimeoutExpired:
                process.kill()
                process.wait()
        data = collect(master, data, 0.2)
        return process.returncode, data

    def assert_echo_restored(self, before, check_fd):
        after = termios.tcgetattr(check_fd)
        self.assertEqual(
            after[3] & termios.ECHO,
            before[3] & termios.ECHO,
            "terminal echo was not restored",
        )

    def test_hidden_prompt_authenticates(self):
        master, check_fd, before, process = self.open_pty(
            [self.url, "--prompt-api-key"],
            {"TAIA_API_KEY": "sk-from-environment"},
        )
        data = bytearray()
        try:
            data = read_until(master, data, b"API key: ", 3)
            self.assertIn(b"API key: ", data)
            os.write(master, (KEY + "\n").encode())
            self.assertTrue(AuthHandler.seen.wait(3), "server saw no models request")
            data = read_until(master, data, b"Ask anything", 3)
            self.assertIn(b"Ask anything", data)
            os.write(master, b"q")
            code, data = self.wait_for_exit(process, master, data)
            self.assertEqual(code, 0, data.decode("utf-8", errors="replace"))
            self.assertEqual(AuthHandler.authorization, "Bearer " + KEY)
            self.assertNotIn(KEY.encode(), data)
            self.assert_echo_restored(before, check_fd)
        finally:
            if process.poll() is None:
                process.kill()
                process.wait()
            os.close(master)
            os.close(check_fd)

    def test_empty_key_fails(self):
        master, check_fd, before, process = self.open_pty(["--prompt-api-key"])
        data = bytearray()
        try:
            data = read_until(master, data, b"API key: ", 3)
            os.write(master, b"\n")
            code, data = self.wait_for_exit(process, master, data)
            self.assertNotEqual(code, 0)
            self.assertIn(b"API key cannot be empty", data)
            self.assert_echo_restored(before, check_fd)
        finally:
            if process.poll() is None:
                process.kill()
                process.wait()
            os.close(master)
            os.close(check_fd)

    def test_eof_fails(self):
        master, check_fd, before, process = self.open_pty(["--prompt-api-key"])
        data = bytearray()
        try:
            data = read_until(master, data, b"API key: ", 3)
            os.write(master, b"\x04")
            code, data = self.wait_for_exit(process, master, data)
            self.assertNotEqual(code, 0)
            self.assertIn(b"no API key provided", data)
            self.assert_echo_restored(before, check_fd)
        finally:
            if process.poll() is None:
                process.kill()
                process.wait()
            os.close(master)
            os.close(check_fd)

    def test_ctrl_c_cancels(self):
        master, check_fd, before, process = self.open_pty(["--prompt-api-key"])
        data = bytearray()
        try:
            data = read_until(master, data, b"API key: ", 3)
            os.write(master, b"\x03")
            code, data = self.wait_for_exit(process, master, data)
            self.assertNotEqual(code, 0)
            self.assertIn(b"API key entry cancelled", data)
            self.assert_echo_restored(before, check_fd)
        finally:
            if process.poll() is None:
                process.kill()
                process.wait()
            os.close(master)
            os.close(check_fd)

    def test_signal_restores_terminal(self):
        master, check_fd, before, process = self.open_pty(["--prompt-api-key"])
        data = bytearray()
        try:
            data = read_until(master, data, b"API key: ", 3)
            self.assertIn(b"API key: ", data)
            during = termios.tcgetattr(check_fd)
            self.assertEqual(during[3] & termios.ECHO, 0)
            process.send_signal(signal.SIGTERM)
            try:
                code = process.wait(timeout=2)
            except subprocess.TimeoutExpired:
                self.fail("process did not exit after SIGTERM")
            data = collect(master, data, 0.2)
            self.assertEqual(code, -signal.SIGTERM)
            self.assert_echo_restored(before, check_fd)
        finally:
            if process.poll() is None:
                process.kill()
                process.wait()
            os.close(master)
            os.close(check_fd)


if __name__ == "__main__":
    unittest.main()
