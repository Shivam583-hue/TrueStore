"""RESP socket helpers shared by the integration tests."""

import pathlib
import socket
import subprocess
import sys
import tempfile
import time


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


class Client:
    def __init__(self, port):
        self.sock = socket.create_connection(("127.0.0.1", port), 2)
        self.sock.settimeout(3)
        self.stream = self.sock.makefile("rb")

    def command(self, *args):
        self.sock.sendall(encode(*args))
        return self.read()

    def read(self):
        return read_reply(self.stream)

    def close(self):
        self.stream.close()
        self.sock.close()


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

