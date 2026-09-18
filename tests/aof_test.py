"""Run with: python3 tests/aof_test.py /path/to/server"""

import pathlib
import socket
import subprocess
import tempfile
import unittest

from support import BINARY, ErrorReply, RunningServer, encode, read_reply, unused_port


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

    def test_single_write_uses_manifest_target_before_reply(self):
        self.seed()
        with RunningServer(self.root, *self.flags) as server:
            self.assertEqual(server.command("SET", "key", "value"), b"OK")
            self.assertEqual(self.active.read_bytes(), encode("SET", "key", "value"))
            self.assertFalse((self.directory / "changes.aof.1.incr.aof").exists())

    def test_multiple_writes_filter_reads_and_failures(self):
        self.seed()
        commands = [
            ("SET", "counter", "7"), ("GET", "counter"), ("PING",),
            ("INCR", "counter"), ("ECHO", "hello"),
            ("CONFIG", "GET", "appendonly"), ("LPUSH", "items", "a", "b"),
            ("SET", "invalid"), ("UNKNOWN", "command"),
            ("SET", "counter", "9"),
        ]
        expected = b"".join(encode(*commands[index]) for index in (0, 3, 6, 9))
        with RunningServer(self.root, *self.flags) as server:
            server.sock.sendall(b"".join(encode(*command) for command in commands))
            replies = [read_reply(server.stream) for _ in commands]
            self.assertEqual(replies[0], b"OK")
            self.assertEqual(replies[3], 8)
            self.assertEqual(replies[6], 2)
            self.assertIsInstance(replies[7], ErrorReply)
            self.assertIsInstance(replies[8], ErrorReply)
            self.assertEqual(self.active.read_bytes(), expected)

    def test_binary_values_and_fragmented_commands(self):
        self.seed()
        value = b"spaces\x00and\r\nRESP *3\r\n" + b"x" * 12000
        command = encode("sEt", "key", value)
        with RunningServer(self.root, *self.flags) as server:
            for offset in range(0, len(command), 137):
                server.sock.sendall(command[offset:offset + 137])
            self.assertEqual(read_reply(server.stream), b"OK")
            self.assertEqual(server.command("GET", "key"), value)
            self.assertEqual(self.active.read_bytes(), command)

    def test_transactions_log_only_executed_writes(self):
        self.seed()
        with RunningServer(self.root, *self.flags) as server:
            self.assertEqual(server.command("MULTI"), b"OK")
            self.assertEqual(server.command("SET", "discarded", "1"), b"QUEUED")
            self.assertEqual(self.active.read_bytes(), b"")
            self.assertEqual(server.command("DISCARD"), b"OK")
            self.assertEqual(self.active.read_bytes(), b"")
            server.command("MULTI")
            server.command("SET", "counter", "1")
            server.command("GET", "counter")
            server.command("INCR", "counter")
            self.assertEqual(server.command("EXEC"), [b"OK", b"1", 2])
            self.assertEqual(
                self.active.read_bytes(),
                encode("MULTI") + encode("SET", "counter", "1")
                + encode("INCR", "counter") + encode("EXEC"),
            )

    def test_always_everysec_and_no_sync_modes_write_commands(self):
        self.seed()
        expected = b""
        for mode in ("always", "everysec", "no"):
            with self.subTest(mode=mode), RunningServer(
                self.root, *self.flags, "--appendfsync", mode
            ) as server:
                self.assertEqual(server.command("SET", "mode", mode), b"OK")
                expected += encode("SET", "mode", mode)
                self.assertEqual(self.active.read_bytes(), expected)

    def test_replays_single_command_from_manifest_target(self):
        original = encode("SET", "key", "restored")
        self.seed(original)
        with RunningServer(self.root, *self.flags) as server:
            self.assertEqual(server.command("GET", "key"), b"restored")
            self.assertEqual(self.active.read_bytes(), original)

    def test_replays_multiple_commands_in_order(self):
        original = b"".join([
            encode("SET", "counter", "4"), encode("INCR", "counter"),
            encode("SET", "key", "old"), encode("SET", "key", "new"),
            encode("RPUSH", "items", "a", "b"), encode("LPOP", "items"),
        ])
        self.seed(original)
        with RunningServer(self.root, *self.flags) as server:
            self.assertEqual(server.command("GET", "counter"), b"5")
            self.assertEqual(server.command("GET", "key"), b"new")
            self.assertEqual(server.command("LRANGE", "items", "0", "-1"), [b"b"])
            self.assertEqual(self.active.read_bytes(), original)
            self.assertEqual(server.command("INCR", "counter"), 6)
            self.assertEqual(self.active.read_bytes(), original + encode("INCR", "counter"))

    def test_replay_handles_binary_values_larger_than_read_buffer(self):
        value = b"\x00\r\n*3\r\n" + b"a" * 18000
        original = encode("SET", "large", value) + encode("SET", "tail", "end")
        self.seed(original)
        with RunningServer(self.root, *self.flags) as server:
            self.assertEqual(server.command("GET", "large"), value)
            self.assertEqual(server.command("GET", "tail"), b"end")
            self.assertEqual(self.active.read_bytes(), original)

    def test_restart_after_kill_restores_writes_without_duplicate_logging(self):
        self.seed()
        with RunningServer(self.root, *self.flags) as server:
            self.assertEqual(server.command("INCR", "counter"), 1)
            server.command("MULTI")
            server.command("INCR", "counter")
            server.command("RPUSH", "items", "a", "b")
            self.assertEqual(server.command("EXEC"), [2, 2])
        before = self.active.read_bytes()
        for _ in range(2):
            with RunningServer(self.root, *self.flags) as server:
                self.assertEqual(server.command("GET", "counter"), b"2")
                self.assertEqual(server.command("LRANGE", "items", 0, -1), [b"a", b"b"])
                self.assertEqual(self.active.read_bytes(), before)

    def test_replays_incremental_files_by_sequence_and_appends_to_latest(self):
        self.seed(encode("SET", "counter", "1"))
        newest = self.directory / "newest.aof"
        newest.write_bytes(encode("INCR", "counter"))
        self.manifest.write_text(
            "file newest.aof seq 2 type i\nfile custom.1.incr.aof seq 1 type i\n"
        )
        with RunningServer(self.root, *self.flags) as server:
            self.assertEqual(server.command("GET", "counter"), b"2")
            self.assertEqual(server.command("INCR", "counter"), 3)
            self.assertEqual(newest.read_bytes(), encode("INCR", "counter") * 2)
            self.assertEqual(self.active.read_bytes(), encode("SET", "counter", "1"))

    def test_disabled_aof_does_not_replay_or_modify_existing_files(self):
        original = encode("SET", "key", "persisted")
        self.seed(original)
        with RunningServer(self.root, *self.flags, "--appendonly", "no") as server:
            self.assertIsNone(server.command("GET", "key"))
            self.assertEqual(server.command("SET", "other", "value"), b"OK")
            self.assertEqual(self.active.read_bytes(), original)

    def test_rejects_incomplete_or_invalid_aof_without_changing_it(self):
        self.seed()
        for data in (
            encode("SET", "key", "value")[:-3], b"invalid RESP\r\n",
            encode("SET", "missing-value"), encode("MULTI") + encode("SET", "key", "v"),
        ):
            with self.subTest(data=data):
                self.active.write_bytes(data)
                self.assert_startup_fails()
                self.assertEqual(self.active.read_bytes(), data)

    def test_rdb_loading_remains_available_when_aof_is_disabled(self):
        rdb = b"REDIS0011\xfe\x00\x00\x03key\x03old\xff" + b"\x00" * 8
        (self.root / "dump.rdb").write_bytes(rdb)
        with RunningServer(self.root, "--dbfilename", "dump.rdb") as server:
            self.assertEqual(server.command("GET", "key"), b"old")
        self.seed(encode("SET", "key", "new"))
        with RunningServer(self.root, *self.flags, "--dbfilename", "dump.rdb") as server:
            self.assertEqual(server.command("GET", "key"), b"new")

    def test_replication_writes_are_persisted_on_replica(self):
        self.seed()
        replica_root = self.root / "replica"
        with RunningServer(self.root, *self.flags) as master:
            with RunningServer(
                replica_root, *self.flags, "--replicaof", "127.0.0.1", str(master.port)
            ) as replica:
                self.assertEqual(master.command("SET", "replicated", "value"), b"OK")
                self.assertEqual(master.command("WAIT", 1, 2000), 1)
                self.assertEqual(replica.command("GET", "replicated"), b"value")
        with RunningServer(replica_root, *self.flags) as server:
            self.assertEqual(server.command("GET", "replicated"), b"value")

    def test_blocking_pops_are_logged_as_nonblocking_writes(self):
        self.seed()
        with RunningServer(self.root, *self.flags) as server:
            server.command("RPUSH", "ready", "a", "b")
            self.assertEqual(server.command("BLPOP", "ready", 0), [b"ready", b"a"])
            with socket.create_connection(("127.0.0.1", server.port), 2) as other:
                other.settimeout(3)
                with other.makefile("rb") as stream:
                    other.sendall(encode("BLPOP", "waiting", 0) + encode("PING"))
                    self.assertEqual(read_reply(stream), b"PONG")
                    server.command("RPUSH", "waiting", "x", "y")
                    self.assertEqual(read_reply(stream), [b"waiting", b"x"])
        self.assertEqual(self.active.read_bytes(), b"".join([
            encode("RPUSH", "ready", "a", "b"), encode("LPOP", "ready"),
            encode("RPUSH", "waiting", "x", "y"), encode("LPOP", "waiting"),
        ]))
        with RunningServer(self.root, *self.flags) as server:
            self.assertEqual(server.command("LRANGE", "ready", 0, -1), [b"b"])
            self.assertEqual(server.command("LRANGE", "waiting", 0, -1), [b"y"])


if __name__ == "__main__":
    unittest.main()
