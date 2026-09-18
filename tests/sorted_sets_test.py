"""Run with: python3 tests/sorted_sets_test.py /path/to/server"""

import pathlib
import random
import tempfile
import time
import unittest

from support import Client, ErrorReply, RunningServer, encode


class SortedSetTests(unittest.TestCase):
    def setUp(self):
        temporary = tempfile.TemporaryDirectory()
        self.addCleanup(temporary.cleanup)
        self.root = pathlib.Path(temporary.name)
        self.flags = ["--appendonly", "yes", "--appendfsync", "always"]
        self.server = RunningServer(self.root, *self.flags)
        self.addCleanup(self.server.stop)

    def command(self, *args):
        return self.server.command(*args)

    def seed(self):
        self.assertEqual(self.command("ZADD", "set", 100, "foo", 100, "bar",
                                      20, "baz", 30.1, "caz", 40.2, "paz"), 5)
        return [b"baz", b"caz", b"paz", b"bar", b"foo"]

    def restart(self):
        self.server.stop()
        self.server = RunningServer(self.root, *self.flags)
        self.addCleanup(self.server.stop)

    def test_create_add_and_update_scores(self):
        self.assertEqual(self.command("ZADD", "set", 8.0, "Sam"), 1)
        self.assertEqual(self.command("ZADD", "set", 6.1, "Ford"), 1)
        self.assertEqual(self.command("ZADD", "set", 5.5, "Sam"), 0)
        self.assertEqual(self.command("ZRANGE", "set", 0, -1), [b"Sam", b"Ford"])
        self.assertEqual(self.command("ZCARD", "set"), 2)

    def test_duplicate_members_in_one_command_count_once(self):
        self.assertEqual(self.command("ZADD", "set", 1, "a", 3, "a", 2, "b"), 2)
        self.assertEqual(self.command("ZSCORE", "set", "a"), b"3")
        self.assertEqual(self.command("ZCARD", "set"), 2)
        self.assertEqual(self.command("ZRANGE", "set", 0, -1), [b"b", b"a"])

    def test_ranks_use_score_then_lexicographical_order(self):
        members = self.seed()
        for rank, member in enumerate(members):
            self.assertEqual(self.command("ZRANK", "set", member), rank)
        self.assertIsNone(self.command("ZRANK", "set", "missing"))
        self.assertIsNone(self.command("ZRANK", "missing", "foo"))
        self.command("ZADD", "set", -1, "foo")
        self.assertEqual(self.command("ZRANK", "set", "foo"), 0)
        self.assertEqual(self.command("ZRANK", "set", "bar"), 4)

    def test_ranges_are_inclusive_and_accept_negative_indices(self):
        members = self.seed()
        cases = [
            (0, 2, members[:3]), (2, 99, members[2:]), (99, 100, []),
            (3, 1, []), (-2, -1, members[-2:]), (0, -3, members[:-2]),
            (-100, 1, members[:2]), (0, -100, []), (-5, -5, members[:1]),
            (-1, -2, []), (0, 9223372036854775807, members),
            (-9223372036854775808, -1, members),
        ]
        for start, stop, expected in cases:
            with self.subTest(start=start, stop=stop):
                self.assertEqual(self.command("ZRANGE", "set", start, stop), expected)
        self.assertEqual(self.command("ZRANGE", "missing", 0, -1), [])

    def test_cardinality_and_missing_keys(self):
        self.assertEqual(self.command("ZCARD", "missing"), 0)
        self.seed()
        self.assertEqual(self.command("ZCARD", "set"), 5)
        self.command("ZADD", "set", 200, "foo")
        self.assertEqual(self.command("ZCARD", "set"), 5)
        self.command("ZREM", "set", "foo")
        self.assertEqual(self.command("ZCARD", "set"), 4)

    def test_score_formatting_and_updates(self):
        for score, expected in (("30.1", b"30.1"), ("100.99", b"100.99"),
                                ("20.0", b"20"), ("-0", b"0"),
                                ("+inf", b"inf"), ("-inf", b"-inf")):
            with self.subTest(score=score):
                self.command("ZADD", "set", score, "member")
                self.assertEqual(self.command("ZSCORE", "set", "member"), expected)
        self.assertIsNone(self.command("ZSCORE", "set", "missing"))
        self.assertIsNone(self.command("ZSCORE", "missing", "member"))
        self.command("ZADD", "set", "0.12345678901234567", "precise")
        self.assertEqual(float(self.command("ZSCORE", "set", "precise")),
                         float("0.12345678901234567"))

    def test_invalid_scores_do_not_partially_modify_the_set(self):
        self.command("ZADD", "set", 1, "original")
        for score in ("nan", "NaN", "", "1x", " 1", "1 ", "1e9999", b"1\x00"):
            with self.subTest(score=score):
                reply = self.command("ZADD", "set", 2, "new", score, "invalid")
                self.assertIsInstance(reply, ErrorReply)
                self.assertEqual(self.command("ZRANGE", "set", 0, -1), [b"original"])
                self.assertIsInstance(self.command("ZADD", "missing", score, "x"), ErrorReply)
                self.assertEqual(self.command("TYPE", "missing"), b"none")

    def test_remove_members_and_delete_empty_key(self):
        self.seed()
        self.assertEqual(self.command("ZREM", "set", "baz"), 1)
        self.assertEqual(self.command("ZREM", "set", "missing"), 0)
        self.assertEqual(self.command("ZREM", "set", "bar", "bar", "foo"), 2)
        self.assertEqual(self.command("ZRANGE", "set", 0, -1), [b"caz", b"paz"])
        self.assertEqual(self.command("ZREM", "set", "caz", "paz"), 2)
        self.assertEqual(self.command("TYPE", "set"), b"none")
        self.assertEqual(self.command("KEYS", "set"), [])
        self.assertEqual(self.command("ZREM", "set", "caz"), 0)
        self.assertEqual(self.command("ZADD", "set", 1, "new"), 1)

    def test_wrong_type_is_rejected_for_every_sorted_set_command(self):
        self.command("SET", "string", "v")
        self.command("RPUSH", "list", "v")
        self.command("XADD", "stream", "1-0", "field", "v")
        for key in ("string", "list", "stream"):
            for name, args in (("ZADD", (1, "x")), ("ZRANK", ("x",)),
                               ("ZRANGE", (0, -1)), ("ZCARD", ()),
                               ("ZSCORE", ("x",)), ("ZREM", ("x",))):
                with self.subTest(key=key, command=name):
                    reply = self.command(name, key, *args)
                    self.assertIsInstance(reply, ErrorReply)
                    self.assertTrue(reply.startswith(b"WRONGTYPE"))

    def test_sorted_sets_reject_incompatible_commands(self):
        self.command("ZADD", "set", 1, "a")
        for args in (("GET", "set"), ("INCR", "set"), ("RPUSH", "set", "b"),
                     ("LPUSH", "set", "b"), ("LPOP", "set"), ("LLEN", "set"),
                     ("LRANGE", "set", 0, -1), ("BLPOP", "set", 0),
                     ("XADD", "set", "1-0", "f", "v"),
                     ("XRANGE", "set", "-", "+"), ("XREAD", "STREAMS", "set", "0")):
            with self.subTest(args=args):
                reply = self.command(*args)
                self.assertIsInstance(reply, ErrorReply)
                self.assertTrue(reply.startswith(b"WRONGTYPE"))
        self.assertEqual(self.command("ZRANGE", "set", 0, -1), [b"a"])

    def test_set_replaces_a_sorted_set_only_on_success(self):
        self.command("ZADD", "set", 1, "a")
        self.assertIsInstance(self.command("SET", "set", "v", "PX", "bad"), ErrorReply)
        self.assertEqual(self.command("TYPE", "set"), b"zset")
        self.assertEqual(self.command("SET", "set", "value"), b"OK")
        self.assertEqual(self.command("TYPE", "set"), b"string")
        self.assertEqual(self.command("GET", "set"), b"value")
        self.command("SET", "expired", "value", "PX", 1)
        time.sleep(0.01)
        self.assertEqual(self.command("ZADD", "expired", 2, "new"), 1)

    def test_binary_members_and_case_sensitive_ties(self):
        members = [b"a", b"A", b"", b"\xff", b"\x00", b"a\x00", b"a\r\n"]
        for member in members:
            self.assertEqual(self.command("ZADD", "set", 1, member), 1)
        self.assertEqual(self.command("ZRANGE", "set", 0, -1), sorted(members))
        self.assertEqual(self.command("TYPE", "set"), b"zset")
        self.assertEqual(self.command("KEYS", "s*"), [b"set"])

    def test_invalid_arity_indices_and_range_options(self):
        for args in (("ZADD",), ("ZADD", "set", 1), ("ZRANK", "set"),
                     ("ZRANGE", "set", 0), ("ZCARD",), ("ZSCORE", "set"),
                     ("ZREM", "set"), ("ZRANGE", "set", "1x", -1),
                     ("ZRANGE", "set", 0, "9223372036854775808"),
                     ("ZRANGE", "set", 0, -1, "UNKNOWN")):
            with self.subTest(args=args):
                self.assertIsInstance(self.command(*args), ErrorReply)
        self.command("ZADD", "set", 2, "b", 1, "a")
        self.assertEqual(self.command("ZRANGE", "set", 0, -1, "WITHSCORES"),
                         [b"a", b"1", b"b", b"2"])

    def test_randomized_updates_match_an_independent_model(self):
        randomizer = random.Random(12)
        model = {}
        for iteration in range(180):
            member = ("member-%02d" % randomizer.randrange(30)).encode()
            if randomizer.randrange(4) == 0:
                expected = int(member in model)
                model.pop(member, None)
                self.assertEqual(self.command("ZREM", "set", member), expected)
            else:
                score = randomizer.randrange(-50, 51) / 4
                expected = int(member not in model)
                model[member] = score
                self.assertEqual(self.command("ZADD", "set", score, member), expected)
            if iteration % 9 == 0:
                ordered = sorted(model, key=lambda item: (model[item], item))
                self.assertEqual(self.command("ZRANGE", "set", 0, -1), ordered)
                self.assertEqual(self.command("ZCARD", "set"), len(model))
                for rank, item in enumerate(ordered):
                    self.assertEqual(self.command("ZRANK", "set", item), rank)

    def test_aof_logs_only_writes_and_restores_sorted_sets(self):
        first = ("ZADD", "set", 1, "a", 2, "b")
        second = ("ZADD", "set", 0, "b")
        third = ("ZREM", "set", "a")
        self.command(*first)
        self.command("ZSCORE", "set", "a")
        self.command(*second)
        self.command("ZRANGE", "set", 0, -1)
        self.command(*third)
        path = self.root / "appendonlydir" / "appendonly.aof.1.incr.aof"
        expected = encode(*first) + encode(*second) + encode(*third)
        self.assertEqual(path.read_bytes(), expected)
        self.restart()
        self.assertEqual(self.command("ZRANGE", "set", 0, -1), [b"b"])
        self.assertEqual(self.command("ZSCORE", "set", "b"), b"0")
        self.assertEqual(path.read_bytes(), expected)

    def test_sorted_set_transactions_are_persisted(self):
        self.command("MULTI")
        self.assertEqual(self.command("ZADD", "set", 1, "a", 2, "b"), b"QUEUED")
        self.command("ZREM", "set", "a")
        self.command("ZRANGE", "set", 0, -1)
        self.assertEqual(self.command("EXEC"), [2, 1, [b"b"]])
        self.restart()
        self.assertEqual(self.command("ZRANGE", "set", 0, -1), [b"b"])

    def test_sorted_set_writes_reach_replicas(self):
        with RunningServer(self.root / "replica", *self.flags, "--replicaof",
                           "127.0.0.1", str(self.server.port)) as replica:
            self.command("ZADD", "set", 5, "b", 2, "a")
            self.command("ZREM", "set", "b")
            self.assertEqual(self.command("WAIT", 1, 2000), 1)
            self.assertEqual(replica.command("ZRANGE", "set", 0, -1), [b"a"])

    def test_subscribed_clients_cannot_modify_sorted_sets(self):
        client = Client(self.server.port)
        self.addCleanup(client.close)
        client.command("SUBSCRIBE", "channel")
        self.assertIsInstance(client.command("ZADD", "set", 1, "a"), ErrorReply)
        self.assertEqual(self.command("ZCARD", "set"), 0)


if __name__ == "__main__":
    unittest.main()
