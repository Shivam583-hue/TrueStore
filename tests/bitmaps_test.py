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
        for offset in (-1, 4294967296, "18446744073709551616", "1x", "1.5", " 1", "1 ", "", "00", "-0", "+1", b"1\x00"):
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

    def test_bitcount_over_whole_strings_and_byte_ranges(self):
        self.assertEqual(self.command("BITCOUNT", "missing"), 0)
        self.command("SET", "empty", b"")
        self.assertEqual(self.command("BITCOUNT", "empty"), 0)
        self.command("SET", "bits", "foobar")
        self.assertEqual(self.command("BITCOUNT", "bits"), 26)
        self.command("SET", "bits", b"\xff\x81\x00")
        for start, end, expected in ((0, -1, 10), (0, 0, 8), (1, 1, 2),
                                     (-2, -1, 2), (-100, -100, 8), (0, -100, 8),
                                     (-1, -100, 0), (99, 100, 0), (2, 1, 0),
                                     (-9223372036854775808, 9223372036854775807, 10)):
            with self.subTest(start=start, end=end):
                self.assertEqual(self.command("BITCOUNT", "bits", start, end), expected)
                self.assertEqual(self.command("BITCOUNT", "bits", start, end, "byte"), expected)

    def test_bitcount_masks_partial_bytes_and_negative_bit_ranges(self):
        self.command("SET", "bits", b"\xff\x81\x00")
        for start, end, expected in ((1, 6, 6), (7, 8, 2), (8, 14, 1), (8, 15, 2),
                                     (-16, -9, 2), (-24, -1, 10), (0, -100, 1),
                                     (0, 100, 10), (24, 100, 0), (10, 3, 0)):
            with self.subTest(start=start, end=end):
                self.assertEqual(self.command("BITCOUNT", "bits", start, end, "BIT"), expected)

    def test_bitcount_ranges_match_an_independent_bit_model(self):
        rng = random.Random(815)
        value = bytes(rng.randrange(256) for _ in range(67))
        bits = "".join(format(byte, "08b") for byte in value)
        self.command("SET", "bits", value)
        for _ in range(100):
            start, end = rng.randrange(-600, 600), rng.randrange(-600, 600)
            left = max(0, start + len(bits) if start < 0 else start)
            right = max(0, end + len(bits) if end < 0 else end)
            expected = 0 if start < 0 and end < 0 and start > end else bits[left:right + 1].count("1")
            self.assertEqual(self.command("BITCOUNT", "bits", start, end, "BIT"), expected)

    def test_reversed_negative_count_ranges_remain_empty_before_clamping(self):
        self.command("SET", "bits", b"\xff")
        for unit, first in (("BYTE", 8), ("BIT", 1)):
            self.assertEqual(self.command("BITCOUNT", "bits", -419, -809, unit), 0)
            self.assertEqual(self.command("BITCOUNT", "bits", -419, -419, unit), first)

    def test_bitop_combines_unequal_lengths_and_preserves_binary_bytes(self):
        self.command("SET", "left", b"\xf0\x0f")
        self.command("SET", "right", b"\xcc")
        for operation, expected in (("and", b"\xc0\x00"), ("OR", b"\xfc\x0f"),
                                     ("XOR", b"\x3c\x0f")):
            with self.subTest(operation=operation):
                self.assertEqual(self.command("BITOP", operation, "dest", "left", "right"), 2)
                self.assertEqual(self.command("GET", "dest"), expected)
        self.assertEqual(self.command("BITOP", "NOT", "dest", "left"), 2)
        self.assertEqual(self.command("GET", "dest"), b"\x0f\xf0")
        self.assertEqual(self.command("BITOP", "AND", "dest", "left", "missing"), 2)
        self.assertEqual(self.command("GET", "dest"), b"\x00\x00")
        self.assertEqual(self.command("BITOP", "OR", "dest", "left", "missing"), 2)
        self.assertEqual(self.command("GET", "dest"), b"\xf0\x0f")

    def test_bitop_can_overwrite_a_source_or_destination_of_another_type(self):
        self.command("SET", "left", b"\xf0")
        self.command("SET", "right", b"\x0f\x80")
        self.assertEqual(self.command("BITOP", "OR", "left", "left", "right"), 2)
        self.assertEqual(self.command("GET", "left"), b"\xff\x80")
        self.assertEqual(self.command("BITOP", "XOR", "left", "left", "left"), 2)
        self.assertEqual(self.command("GET", "left"), b"\x00\x00")
        self.command("RPUSH", "list", "x")
        self.command("XADD", "stream", "1-0", "f", "v")
        self.command("ZADD", "zset", 1, "x")
        for key in ("list", "stream", "zset"):
            self.assertEqual(self.command("BITOP", "OR", key, "right"), 2)
            self.assertEqual(self.command("TYPE", key), b"string")
            self.assertEqual(self.command("GET", key), b"\x0f\x80")

    def test_empty_bitop_result_deletes_destination_and_nonempty_result_clears_expiry(self):
        self.command("SET", "empty", b"")
        self.command("RPUSH", "dest", "x")
        self.assertEqual(self.command("BITOP", "OR", "dest", "missing", "empty"), 0)
        self.assertEqual(self.command("TYPE", "dest"), b"none")
        self.assertEqual(self.command("BITOP", "NOT", "absent", "empty"), 0)
        self.assertEqual(self.command("TYPE", "absent"), b"none")
        self.command("SET", "source", b"\x81")
        self.command("SET", "dest", "old", "PX", 200)
        self.command("BITOP", "OR", "dest", "source")
        time.sleep(0.25)
        self.assertEqual(self.command("GET", "dest"), b"\x81")

    def test_bitop_matches_byte_operations_for_many_sources(self):
        rng = random.Random(54)
        values = [bytes(rng.randrange(256) for _ in range(size)) for size in (4, 17, 38)]
        keys = ["a", "b", "c"]
        for key, value in zip(keys, values):
            self.command("SET", key, value)
        for operation in ("AND", "OR", "XOR"):
            expected = bytearray()
            for position in range(38):
                result = 255 if operation == "AND" else 0
                for value in values:
                    byte = value[position] if position < len(value) else 0
                    if operation == "AND":
                        result &= byte
                    elif operation == "OR":
                        result |= byte
                    else:
                        result ^= byte
                expected.append(result)
            self.assertEqual(self.command("BITOP", operation, "dest", *keys), 38)
            self.assertEqual(self.command("GET", "dest"), bytes(expected))

    def test_invalid_bitcount_and_bitop_requests_do_not_modify_destination(self):
        self.command("SET", "dest", "original")
        for args in (("BITCOUNT",), ("BITCOUNT", "dest", 0),
                     ("BITCOUNT", "dest", 0, 1, "invalid"),
                     ("BITCOUNT", "dest", "0x1", 1), ("BITCOUNT", "dest", "00", 1),
                     ("BITCOUNT", "dest", 0, "9223372036854775808"),
                     ("BITOP",), ("BITOP", "AND", "dest"),
                     ("BITOP", "INVALID", "dest", "source"),
                     ("BITOP", "NOT", "dest", "first", "second")):
            with self.subTest(args=args):
                self.assertIsInstance(self.command(*args), ErrorReply)
        self.command("RPUSH", "list", "x")
        self.command("XADD", "stream", "1-0", "f", "v")
        self.command("ZADD", "zset", 1, "x")
        for key in ("list", "stream", "zset"):
            self.assertTrue(self.command("BITCOUNT", key).startswith(b"WRONGTYPE"))
            self.assertTrue(self.command("BITOP", "AND", "dest", "missing", key).startswith(b"WRONGTYPE"))
        self.assertEqual(self.command("GET", "dest"), b"original")

    def test_bitmap_operations_in_transactions_persist_and_replicate(self):
        writes = [("SET", "left", b"\xf0\x80"), ("SET", "right", b"\x0f"),
                  ("BITOP", "OR", "dest", "left", "right")]
        with RunningServer(self.root / "replica", *self.flags, "--replicaof",
                           "127.0.0.1", str(self.server.port)) as replica:
            self.command("MULTI")
            for args in writes:
                self.command(*args)
            self.command("BITCOUNT", "dest")
            self.assertEqual(self.command("EXEC"), [b"OK", b"OK", 2, 9])
            self.assertEqual(self.command("WAIT", 1, 2000), 1)
            self.assertEqual(replica.command("GET", "dest"), b"\xff\x80")
        path = self.root / "appendonlydir" / "appendonly.aof.1.incr.aof"
        self.assertEqual(path.read_bytes(), encode("MULTI") +
                         b"".join(encode(*args) for args in writes) + encode("EXEC"))
        self.restart()
        self.assertEqual(self.command("GET", "dest"), b"\xff\x80")
        self.assertEqual(self.command("BITCOUNT", "dest"), 9)


if __name__ == "__main__":
    unittest.main()
