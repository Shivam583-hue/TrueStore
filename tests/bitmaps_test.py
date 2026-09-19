import pathlib
import random
import tempfile
import time
import unittest

from support import Client, ErrorReply, RunningServer, encode


class BitmapTests(unittest.TestCase):
    def setUp(self):
        temporary = tempfile.TemporaryDirectory()
        self.addCleanup(temporary.cleanup)
        self.root = pathlib.Path(temporary.name)
        self.flags = ["--appendonly", "yes", "--appendfsync", "always"]
        self.server = RunningServer(self.root, *self.flags)
        self.addCleanup(self.server.stop)

    def command(self, *args):
        return self.server.command(*args)

    def restart(self):
        self.server.stop()
        self.server = RunningServer(self.root, *self.flags)
        self.addCleanup(self.server.stop)

    def test_creating_setting_and_clearing_bits_returns_previous_value(self):
        self.assertEqual(self.command("setbit", "bits", 0, 1), 0)
        self.assertEqual(self.command("GET", "bits"), b"\x80")
        self.assertEqual(self.command("SETBIT", "bits", 0, 1), 1)
        self.assertEqual(self.command("SETBIT", "bits", 7, 1), 0)
        self.assertEqual(self.command("GET", "bits"), b"\x81")
        self.assertEqual(self.command("SETBIT", "bits", 0, 0), 1)
        self.assertEqual(self.command("GET", "bits"), b"\x01")
        self.assertEqual(self.command("TYPE", "bits"), b"string")
        self.assertEqual(self.command("GETBIT", "bits", 0), 0)
        self.assertEqual(self.command("GETBIT", "bits", 7), 1)

    def test_existing_strings_are_read_and_updated_as_bits(self):
        self.command("SET", "bits", b"foo\x00\xff")
        expected = [int(bit) for byte in b"foo\x00\xff" for bit in format(byte, "08b")]
        self.assertEqual([self.command("GETBIT", "bits", bit) for bit in range(40)], expected)
        self.assertEqual(self.command("SETBIT", "bits", 7, 1), 0)
        self.assertEqual(self.command("GET", "bits"), b"goo\x00\xff")
        self.assertEqual(self.command("SET", "bits", "new"), b"OK")
        self.assertEqual(self.command("GET", "bits"), b"new")

    def test_growth_zero_fills_and_missing_reads_do_not_create_keys(self):
        self.assertEqual(self.command("GETBIT", "missing", 4294967295), 0)
        self.assertEqual(self.command("TYPE", "missing"), b"none")
        self.assertEqual(self.command("SETBIT", "bits", 16, 0), 0)
        self.assertEqual(self.command("GET", "bits"), b"\x00" * 3)
        self.command("SETBIT", "bits", 8, 1)
        self.command("SETBIT", "bits", 262143, 1)
        value = self.command("GET", "bits")
        self.assertEqual(value, b"\x00\x80" + b"\x00" * 32765 + b"\x01")
        self.assertEqual(self.command("GETBIT", "bits", 262144), 0)

    def test_invalid_offsets_bits_and_arity_leave_values_unchanged(self):
        self.command("SET", "bits", "original")
        for offset in (-1, 4294967296, "18446744073709551616", "1x", "1.5", " 1", "1 ", "", b"1\x00"):
            for name, suffix in (("SETBIT", (1,)), ("GETBIT", ())):
                with self.subTest(offset=offset, name=name):
                    self.assertIsInstance(self.command(name, "bits", offset, *suffix), ErrorReply)
        for bit in (-1, 2, "1x", "1.0", ""):
            self.assertIsInstance(self.command("SETBIT", "missing", 0, bit), ErrorReply)
        for args in (("SETBIT",), ("SETBIT", "bits", 0), ("SETBIT", "bits", 0, 1, "extra"),
                     ("GETBIT", "bits"), ("GETBIT", "bits", 0, "extra")):
            self.assertIsInstance(self.command(*args), ErrorReply)
        self.assertEqual(self.command("GET", "bits"), b"original")
        self.assertEqual(self.command("TYPE", "missing"), b"none")

    def test_wrong_types_are_rejected(self):
        self.command("RPUSH", "list", "x")
        self.command("XADD", "stream", "1-0", "f", "v")
        self.command("ZADD", "zset", 1, "x")
        for key in ("list", "stream", "zset"):
            for args in (("GETBIT", key, 0), ("SETBIT", key, 0, 1)):
                self.assertTrue(self.command(*args).startswith(b"WRONGTYPE"))

    def test_expiration_is_preserved_and_expired_keys_can_be_recreated(self):
        self.command("SET", "bits", b"\xff", "PX", 200)
        self.assertEqual(self.command("SETBIT", "bits", 1, 0), 1)
        time.sleep(0.25)
        self.assertEqual(self.command("GETBIT", "bits", 0), 0)
        self.assertEqual(self.command("SETBIT", "bits", 8, 1), 0)
        self.assertEqual(self.command("GET", "bits"), b"\x00\x80")

    def test_random_updates_match_a_bit_array(self):
        rng = random.Random(91)
        expected = [0] * 512
        for _ in range(150):
            offset, bit = rng.randrange(512), rng.randrange(2)
            self.assertEqual(self.command("SETBIT", "bits", offset, bit), expected[offset])
            expected[offset] = bit
            self.assertEqual(self.command("GETBIT", "bits", offset), bit)
        actual = self.command("GET", "bits")
        self.assertEqual([int(bit) for byte in actual for bit in format(byte, "08b")],
                         expected[:len(actual) * 8])

    def test_bit_writes_are_persisted_and_replicated(self):
        writes = [("SETBIT", "bits", 0, 1), ("SETBIT", "bits", 18, 1), ("SETBIT", "bits", 0, 0)]
        with RunningServer(self.root / "replica", *self.flags, "--replicaof",
                           "127.0.0.1", str(self.server.port)) as replica:
            for args in writes:
                self.command(*args)
            self.command("GETBIT", "bits", 18)
            self.command("SETBIT", "bits", -1, 1)
            self.assertEqual(self.command("WAIT", 1, 2000), 1)
            self.assertEqual(replica.command("GET", "bits"), b"\x00\x00\x20")
        path = self.root / "appendonlydir" / "appendonly.aof.1.incr.aof"
        self.assertEqual(path.read_bytes(), b"".join(encode(*args) for args in writes))
        self.restart()
        self.assertEqual(self.command("GET", "bits"), b"\x00\x00\x20")

    def test_transactions_and_watch_observe_bit_changes(self):
        self.command("SETBIT", "bits", 0, 1)
        self.command("WATCH", "bits")
        self.command("MULTI")
        self.assertEqual(self.command("GETBIT", "bits", 0), b"QUEUED")
        other = Client(self.server.port)
        self.addCleanup(other.close)
        other.command("SETBIT", "bits", 0, 0)
        self.assertIsNone(self.command("EXEC"))
        self.command("MULTI")
        self.command("SETBIT", "bits", 7, 1)
        self.command("GETBIT", "bits", 7)
        self.assertEqual(self.command("EXEC"), [0, 1])
        self.restart()
        self.assertEqual(self.command("GET", "bits"), b"\x01")


if __name__ == "__main__":
    unittest.main()
