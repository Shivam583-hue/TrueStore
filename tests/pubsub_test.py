"""Run with: python3 tests/pubsub_test.py /path/to/server"""

import pathlib
import select
import socket
import tempfile
import time
import unittest

from support import Client, ErrorReply, RunningServer, encode


class PubSubTests(unittest.TestCase):
    def setUp(self):
        temporary = tempfile.TemporaryDirectory()
        self.addCleanup(temporary.cleanup)
        self.root = pathlib.Path(temporary.name)
        self.server = RunningServer(
            self.root, "--appendonly", "yes", "--appendfsync", "always"
        )
        self.addCleanup(self.server.stop)

    def client(self):
        client = Client(self.server.port)
        self.addCleanup(client.close)
        return client

    def test_subscriptions_are_unique_and_per_client(self):
        first, second = self.client(), self.client()
        self.assertEqual(first.command("SUBSCRIBE", "foo"), [b"subscribe", b"foo", 1])
        self.assertEqual(first.command("SUBSCRIBE", "bar"), [b"subscribe", b"bar", 2])
        self.assertEqual(first.command("SUBSCRIBE", "bar"), [b"subscribe", b"bar", 2])
        self.assertEqual(second.command("SUBSCRIBE", "bar"), [b"subscribe", b"bar", 1])

    def test_multi_channel_subscription_and_pipelining(self):
        client = self.client()
        client.sock.sendall(encode("SUBSCRIBE", "foo", "bar", "foo") + encode("PING"))
        for expected in ([b"subscribe", b"foo", 1], [b"subscribe", b"bar", 2],
                         [b"subscribe", b"foo", 2], [b"pong", b""]):
            self.assertEqual(client.read(), expected)

    def test_subscribed_mode_rejects_other_commands(self):
        client = self.client()
        client.command("SUBSCRIBE", "foo")
        for args in (("SET", "key", "v"), ("GET", "key"), ("ECHO", "hi"),
                     ("PUBLISH", "foo", "hi"), ("MULTI",), ("UNKNOWN",)):
            with self.subTest(args=args):
                reply = client.command(*args)
                self.assertIsInstance(reply, ErrorReply)
                self.assertTrue(reply.lower().startswith(
                    b"err can't execute '" + args[0].lower().encode() + b"'"
                ))
        self.assertIsNone(self.server.command("GET", "key"))

    def test_ping_depends_on_the_calling_clients_mode(self):
        client = self.client()
        self.assertEqual(client.command("PING"), b"PONG")
        self.assertEqual(client.command("PING", "hello"), b"hello")
        client.command("SUBSCRIBE", "foo")
        self.assertEqual(client.command("PING"), [b"pong", b""])
        self.assertEqual(client.command("PING", "hello"), [b"pong", b"hello"])
        self.assertEqual(self.server.command("PING"), b"PONG")
        self.assertIsInstance(client.command("PING", "a", "b"), ErrorReply)

    def test_publish_delivers_to_each_matching_client_once(self):
        first, second, other = self.client(), self.client(), self.client()
        first.command("SUBSCRIBE", "foo")
        first.command("SUBSCRIBE", "foo")
        second.command("SUBSCRIBE", "foo")
        other.command("SUBSCRIBE", "bar")
        self.assertEqual(self.server.command("PUBLISH", "foo", "hello"), 2)
        self.assertEqual(first.read(), [b"message", b"foo", b"hello"])
        self.assertEqual(second.read(), [b"message", b"foo", b"hello"])
        self.assertEqual(first.command("PING"), [b"pong", b""])
        self.assertFalse(select.select([other.sock], [], [], 0.05)[0])
        self.assertEqual(self.server.command("PUBLISH", "bar", "world"), 1)
        self.assertEqual(other.read(), [b"message", b"bar", b"world"])
        self.assertEqual(self.server.command("PUBLISH", "missing", "none"), 0)

    def test_binary_channel_names_and_messages(self):
        client = self.client()
        channel = b"channel\x00\r\n"
        payload = b"hello\x00\r\n" + b"x" * 16000
        self.assertEqual(client.command("subscribe", channel), [b"subscribe", channel, 1])
        self.assertEqual(self.server.command("publish", channel, payload), 1)
        self.assertEqual(client.read(), [b"message", channel, payload])
        self.assertEqual(client.command("SUBSCRIBE", ""), [b"subscribe", b"", 2])
        self.assertEqual(self.server.command("PUBLISH", "", ""), 1)
        self.assertEqual(client.read(), [b"message", b"", b""])

    def test_unsubscribe_updates_delivery_and_exits_subscribed_mode(self):
        client = self.client()
        client.command("SUBSCRIBE", "foo")
        client.command("SUBSCRIBE", "bar")
        self.assertEqual(client.command("UNSUBSCRIBE", "missing"),
                         [b"unsubscribe", b"missing", 2])
        self.assertEqual(client.command("UNSUBSCRIBE", "foo"), [b"unsubscribe", b"foo", 1])
        self.assertEqual(self.server.command("PUBLISH", "foo", "ignored"), 0)
        self.assertEqual(self.server.command("PUBLISH", "bar", "delivered"), 1)
        self.assertEqual(client.read(), [b"message", b"bar", b"delivered"])
        self.assertEqual(client.command("UNSUBSCRIBE", "bar"), [b"unsubscribe", b"bar", 0])
        self.assertEqual(client.command("PING"), b"PONG")
        self.assertEqual(client.command("SET", "key", "value"), b"OK")

    def test_unsubscribe_multiple_and_all_channels(self):
        client = self.client()
        self.assertEqual(client.command("UNSUBSCRIBE"), [b"unsubscribe", None, 0])
        client.command("SUBSCRIBE", "a")
        client.command("SUBSCRIBE", "b")
        client.command("SUBSCRIBE", "c")
        client.sock.sendall(encode("UNSUBSCRIBE", "b", "b", "missing"))
        for channel in (b"b", b"b", b"missing"):
            self.assertEqual(client.read(), [b"unsubscribe", channel, 2])
        client.sock.sendall(encode("UNSUBSCRIBE"))
        replies = [client.read(), client.read()]
        self.assertEqual({reply[1] for reply in replies}, {b"a", b"c"})
        self.assertEqual([reply[2] for reply in replies], [1, 0])
        self.assertEqual(client.command("PING"), b"PONG")

    def test_quit_and_reset_remove_subscriptions(self):
        client = self.client()
        client.command("SUBSCRIBE", "foo")
        self.assertEqual(client.command("RESET"), b"RESET")
        self.assertEqual(client.command("PING"), b"PONG")
        self.assertEqual(self.server.command("PUBLISH", "foo", "none"), 0)
        client.command("SUBSCRIBE", "foo")
        self.assertEqual(client.command("QUIT"), b"OK")
        self.assertEqual(client.stream.read(1), b"")
        self.assertEqual(self.server.command("PUBLISH", "foo", "none"), 0)

    def test_disconnected_clients_are_removed(self):
        client = self.client()
        client.command("SUBSCRIBE", "foo")
        client.sock.shutdown(socket.SHUT_RDWR)
        deadline = time.monotonic() + 2
        while self.server.command("PUBLISH", "foo", "after-close") != 0:
            self.assertLess(time.monotonic(), deadline)
            time.sleep(0.01)
        replacement = self.client()
        self.assertEqual(replacement.command("PING"), b"PONG")
        self.assertEqual(self.server.command("PUBLISH", "foo", "ignored"), 0)

    def test_pubsub_commands_are_not_persisted(self):
        client = self.client()
        client.command("SUBSCRIBE", "foo")
        self.server.command("PUBLISH", "foo", "message")
        client.read()
        client.command("UNSUBSCRIBE", "foo")
        log = self.root / "appendonlydir" / "appendonly.aof.1.incr.aof"
        self.assertEqual(log.read_bytes(), b"")

    def test_publish_in_transaction_delivers_only_after_exec(self):
        client = self.client()
        client.command("SUBSCRIBE", "foo")
        self.server.command("MULTI")
        self.assertEqual(self.server.command("PUBLISH", "foo", "message"), b"QUEUED")
        self.assertFalse(select.select([client.sock], [], [], 0.05)[0])
        self.assertEqual(self.server.command("EXEC"), [1])
        self.assertEqual(client.read(), [b"message", b"foo", b"message"])

    def test_missing_arguments_return_errors(self):
        client = self.client()
        self.assertIsInstance(client.command("SUBSCRIBE"), ErrorReply)
        self.assertEqual(client.command("PING"), b"PONG")
        self.assertIsInstance(client.command("PUBLISH", "foo"), ErrorReply)


if __name__ == "__main__":
    unittest.main()
