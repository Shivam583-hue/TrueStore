"""Run with: python3 tests/aof_test.py /path/to/server"""

import pathlib
import socket
import subprocess
import sys
import tempfile
import time
import unittest


BINARY = str(pathlib.Path(sys.argv.pop(1)).resolve())


def encode(*args):
    values = [arg if isinstance(arg, bytes) else str(arg).encode() for arg in args]
    return b"*%d\r\n" % len(values) + b"".join(
        b"$%d\r\n" % len(value) + value + b"\r\n" for value in values
    )


class ErrorReply(bytes):
    pass


def read_reply(stream):
    line = stream.readline()
    if not line.endswith(b"\r\n"):
        raise AssertionError("Missing RESP response: %r" % line)
    kind, value = line[:1], line[1:-2]
    if kind == b"+":
        return value
    if kind == b"-":
        return ErrorReply(value)
    if kind == b":":
        return int(value)
    if kind in (b"$", b"*"):
        size = int(value)
        if size == -1:
            return None
        if kind == b"*":
            return [read_reply(stream) for _ in range(size)]
        data = stream.read(size)
        if len(data) != size or stream.read(2) != b"\r\n":
            raise AssertionError("Invalid bulk string response")
        return data
    raise AssertionError("Unexpected RESP response: %r" % line)


def unused_port():
    with socket.socket() as sock:
        sock.bind(("127.0.0.1", 0))
        return sock.getsockname()[1]


class RunningServer:
    def __init__(self, directory, *flags):
        self.port = unused_port()
        self.log = tempfile.TemporaryFile()
        self.sock = None
        self.stream = None
        self.process = subprocess.Popen(
            [BINARY, "--port", str(self.port), "--dir", str(directory), *flags],
            stdout=self.log,
            stderr=self.log,
        )
        try:
            deadline = time.monotonic() + 5
            while time.monotonic() < deadline:
                if self.process.poll() is not None:
                    self.log.seek(0)
                    raise AssertionError(self.log.read().decode())
                try:
                    self.sock = socket.create_connection(("127.0.0.1", self.port), 1)
                    self.sock.settimeout(3)
                    self.stream = self.sock.makefile("rb")
                    if self.command("PING") != b"PONG":
                        raise AssertionError("Server did not respond to PING")
                    return
                except ConnectionRefusedError:
                    time.sleep(0.01)
            raise AssertionError("Server did not start")
        except BaseException:
            self.stop()
            raise

    def command(self, *args):
        self.sock.sendall(encode(*args))
        return read_reply(self.stream)

    def stop(self):
        if self.process.poll() is None:
            self.process.kill()
        self.process.wait(timeout=5)
        if self.stream:
            self.stream.close()
        if self.sock:
            self.sock.close()
        self.log.close()

    def __enter__(self):
        return self

    def __exit__(self, *exc):
        self.stop()


class AofTests(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory()
        self.addCleanup(self.temporary.cleanup)
        self.root = pathlib.Path(self.temporary.name)
        self.directory = self.root / "logs"
        self.manifest = self.directory / "changes.aof.manifest"
        self.active = self.directory / "custom.1.incr.aof"
        self.flags = [
            "--appendonly", "yes", "--appenddirname", "logs",
            "--appendfilename", "changes.aof", "--appendfsync", "always",
        ]

    def seed(self, content=b""):
        self.directory.mkdir()
        self.manifest.write_text("file custom.1.incr.aof seq 1 type i\n")
        self.active.write_bytes(content)

    def assert_startup_fails(self):
        result = subprocess.run(
            [BINARY, "--port", str(unused_port()), "--dir", str(self.root), *self.flags],
            stdout=subprocess.PIPE, stderr=subprocess.PIPE, timeout=5,
        )
        self.assertNotEqual(result.returncode, 0)
        self.assertIn(b"AOF", result.stderr)

    def test_creates_manifest_and_incremental_file(self):
        with RunningServer(self.root, *self.flags):
            self.assertEqual(
                self.manifest.read_bytes(), b"file changes.aof.1.incr.aof seq 1 type i\n"
            )
            self.assertEqual((self.directory / "changes.aof.1.incr.aof").read_bytes(), b"")

    def test_preserves_existing_manifest_and_custom_filename(self):
        self.seed()
        before = self.manifest.read_bytes()
        with RunningServer(self.root, *self.flags):
            self.assertEqual(self.manifest.read_bytes(), before)
            self.assertFalse((self.directory / "changes.aof.1.incr.aof").exists())
            self.assertEqual(self.active.read_bytes(), b"")

    def test_disabled_aof_creates_nothing(self):
        for flags in ([], ["--appendonly", "no"]):
            with self.subTest(flags=flags), RunningServer(self.root, *flags) as server:
                self.assertEqual(server.command("SET", "key", "value"), b"OK")
                self.assertEqual(list(self.root.iterdir()), [])

    def test_rejects_missing_manifest_target(self):
        self.seed()
        self.active.unlink()
        self.assert_startup_fails()
        self.assertFalse(self.active.exists())

    def test_rejects_invalid_manifest(self):
        self.seed()
        self.manifest.write_text("file ../outside.aof seq 1 type i\n")
        self.assert_startup_fails()


if __name__ == "__main__":
    unittest.main()
