import pathlib
import tempfile
import time
import unittest

from support import ErrorReply, RunningServer


class StringTests(unittest.TestCase):
    def setUp(self):
        temporary = tempfile.TemporaryDirectory()
        self.addCleanup(temporary.cleanup)
        self.server = RunningServer(pathlib.Path(temporary.name))
        self.addCleanup(self.server.stop)

    def command(self, *args):
        return self.server.command(*args)

    def test_strlen_counts_bytes_including_nulls(self):
        for value, expected in ((b"", 0), (b"hello", 5),
                                (b"\x00\xff\x00", 3), ("é", 2)):
            with self.subTest(value=value):
                self.command("SET", "string", value)
                self.assertEqual(self.command("sTrLeN", "string"), expected)

    def test_strlen_missing_and_expired_keys_returns_zero(self):
        self.assertEqual(self.command("STRLEN", "missing"), 0)
        self.assertEqual(self.command("TYPE", "missing"), b"none")
        self.command("SET", "expiring", "value", "PX", 100)
        time.sleep(0.15)
        self.assertEqual(self.command("STRLEN", "expiring"), 0)
        self.assertEqual(self.command("TYPE", "expiring"), b"none")

    def test_strlen_tracks_bitmap_growth(self):
        self.assertEqual(self.command("SETBIT", "mango", 1, 1), 0)
        self.assertEqual(self.command("STRLEN", "mango"), 1)
        self.command("SETBIT", "mango", 7, 1)
        self.assertEqual(self.command("STRLEN", "mango"), 1)
        self.command("SETBIT", "mango", 8, 0)
        self.assertEqual(self.command("STRLEN", "mango"), 2)
        self.command("SETBIT", "mango", 262143, 1)
        self.assertEqual(self.command("STRLEN", "mango"), 32768)

    def test_strlen_rejects_wrong_types_and_invalid_arity(self):
        self.command("RPUSH", "list", "value")
        self.command("XADD", "stream", "1-0", "field", "value")
        self.command("ZADD", "zset", 1, "value")
        for key in ("list", "stream", "zset"):
            with self.subTest(key=key):
                reply = self.command("STRLEN", key)
                self.assertIsInstance(reply, ErrorReply)
                self.assertTrue(reply.startswith(b"WRONGTYPE"))
        for args in (("STRLEN",), ("STRLEN", "missing", "extra")):
            with self.subTest(args=args):
                reply = self.command(*args)
                self.assertIsInstance(reply, ErrorReply)
                self.assertEqual(reply, b"ERR wrong number of arguments for 'strlen' command")

    def test_strlen_observes_queued_writes_in_transactions(self):
        self.command("MULTI")
        self.assertEqual(self.command("SETBIT", "bits", 16, 1), b"QUEUED")
        self.assertEqual(self.command("STRLEN", "bits"), b"QUEUED")
        self.assertEqual(self.command("EXEC"), [0, 3])


if __name__ == "__main__":
    unittest.main()
