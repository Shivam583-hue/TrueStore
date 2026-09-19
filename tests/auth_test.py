import hashlib
import pathlib
import tempfile
import unittest

from support import Client, ErrorReply, RunningServer, encode


class AuthTests(unittest.TestCase):
    def setUp(self):
        temporary = tempfile.TemporaryDirectory()
        self.addCleanup(temporary.cleanup)
        self.root = pathlib.Path(temporary.name)
        self.server = RunningServer(self.root, "--appendonly", "yes", "--appendfsync", "always")
        self.addCleanup(self.server.stop)

    def command(self, *args):
        return self.server.command(*args)

    def user(self):
        response = self.command("ACL", "GETUSER", "default")
        return dict(zip(response[::2], response[1::2]))

    def connect(self):
        client = Client(self.server.port)
        self.addCleanup(client.close)
        return client

    def test_default_identity_and_user_properties(self):
        self.assertEqual(self.command("acl", "whoami"), b"default")
        self.assertEqual(self.user()[b"flags"], [b"on", b"nopass"])
        self.assertEqual(self.user()[b"passwords"], [])
        self.assertEqual(self.user()[b"commands"], b"+@all")
        self.assertIsNone(self.command("ACL", "GETUSER", "missing"))
        self.assertIsNone(self.command("ACL", "GETUSER", "Default"))

    def test_passwords_are_sha256_hashes_and_are_not_duplicated(self):
        passwords = [b"secret", b"", b"binary\x00\r\n\xff", b"long" * 1000]
        for password in passwords:
            self.assertEqual(self.command("ACL", "SETUSER", "default", b">" + password), b"OK")
        self.command("ACL", "SETUSER", "default", b">secret")
        self.assertEqual(self.user()[b"flags"], [b"on"])
        self.assertEqual(self.user()[b"passwords"],
                         [hashlib.sha256(password).hexdigest().encode() for password in passwords])

    def test_nopass_resetpass_and_password_removal(self):
        self.command("ACL", "SETUSER", "default", ">one", ">two")
        self.command("ACL", "SETUSER", "default", "<one")
        self.assertEqual(self.user()[b"passwords"], [hashlib.sha256(b"two").hexdigest().encode()])
        self.command("ACL", "SETUSER", "default", "nopass")
        self.assertEqual(self.user()[b"flags"], [b"on", b"nopass"])
        self.assertEqual(self.user()[b"passwords"], [])
        self.command("ACL", "SETUSER", "default", "resetpass")
        self.assertEqual(self.user()[b"flags"], [b"on"])
        self.assertEqual(self.user()[b"passwords"], [])

    def test_hashed_password_rules_and_enabled_flag(self):
        digest = hashlib.sha256(b"secret").hexdigest()
        self.command("ACL", "SETUSER", "default", "#" + digest, "off")
        self.assertEqual(self.user()[b"flags"], [b"off"])
        self.assertEqual(self.user()[b"passwords"], [digest.encode()])
        self.command("ACL", "SETUSER", "default", "!" + digest, "on", "nopass")
        self.assertEqual(self.user()[b"flags"], [b"on", b"nopass"])
        self.assertEqual(self.user()[b"passwords"], [])

    def test_invalid_rules_and_arity(self):
        for args in (("ACL",), ("ACL", "WHOAMI", "extra"), ("ACL", "GETUSER"),
                     ("ACL", "GETUSER", "default", "extra"), ("ACL", "SETUSER"),
                     ("ACL", "unknown")):
            with self.subTest(args=args):
                self.assertIsInstance(self.command(*args), ErrorReply)
        for rule in ("unknown", "", "#abc", "#" + "z" * 64):
            with self.subTest(rule=rule):
                self.assertIsInstance(self.command("ACL", "SETUSER", "default", ">new", rule), ErrorReply)
                self.assertEqual(self.user()[b"flags"], [b"on", b"nopass"])
                self.assertEqual(self.user()[b"passwords"], [])

    def test_acl_configuration_is_not_written_to_the_data_aof(self):
        self.command("ACL", "SETUSER", "default", ">secret")
        path = self.root / "appendonlydir" / "appendonly.aof.1.incr.aof"
        self.assertEqual(path.read_bytes(), b"")

    def test_nopass_auth_forms_and_invalid_arity(self):
        self.assertIsInstance(self.command("AUTH", "anything"), ErrorReply)
        self.assertEqual(self.command("AUTH", "default", "anything"), b"OK")
        self.assertIsInstance(self.command("AUTH", "missing", "anything"), ErrorReply)
        for args in (("AUTH",), ("AUTH", "default", "secret", "extra")):
            self.assertIsInstance(self.command(*args), ErrorReply)

    def test_new_connections_require_authentication_without_affecting_existing_clients(self):
        existing = self.connect()
        self.assertEqual(existing.command("PING"), b"PONG")
        self.command("ACL", "SETUSER", "default", ">secret")
        self.assertEqual(existing.command("PING"), b"PONG")
        self.assertEqual(self.command("PING"), b"PONG")
        fresh = self.connect()
        for args in (("PING",), ("GET", "key"), ("SET", "key", "value"),
                     ("ACL", "WHOAMI"), ("ACL", "SETUSER", "default", "nopass"),
                     ("MULTI",), ("WATCH", "key"), ("SUBSCRIBE", "channel"),
                     ("PSYNC", "?", -1), ("REPLCONF", "ACK", 0)):
            with self.subTest(args=args):
                self.assertEqual(fresh.command(*args), ErrorReply(b"NOAUTH Authentication required."))
        self.assertIsNone(self.command("GET", "key"))
        self.assertEqual(fresh.command("AUTH", "secret"), b"OK")
        self.assertEqual(fresh.command("ACL", "WHOAMI"), b"default")
        self.assertEqual(fresh.command("SET", "key", "value"), b"OK")

    def test_authentication_is_per_connection_and_failed_auth_preserves_state(self):
        self.command("ACL", "SETUSER", "default", ">one", ">two")
        first, second = self.connect(), self.connect()
        self.assertEqual(first.command("AUTH", "default", "two"), b"OK")
        for args in (("AUTH", "bad"), ("AUTH", "missing", "one")):
            self.assertTrue(second.command(*args).startswith(b"WRONGPASS"))
            self.assertTrue(second.command("PING").startswith(b"NOAUTH"))
        self.assertTrue(first.command("AUTH", "bad").startswith(b"WRONGPASS"))
        self.assertEqual(first.command("PING"), b"PONG")
        self.assertEqual(second.command("AUTH", "one"), b"OK")

    def test_binary_and_empty_passwords_authenticate(self):
        for password in (b"", b"binary\x00\r\n\xff", b"long" * 1000):
            with self.subTest(password_length=len(password)):
                self.command("ACL", "SETUSER", "default", "resetpass", b">" + password)
                client = self.connect()
                self.assertEqual(client.command("AUTH", "default", password), b"OK")

    def test_reset_password_changes_and_disabled_users(self):
        self.command("ACL", "SETUSER", "default", ">secret")
        client = self.connect()
        self.assertEqual(client.command("AUTH", "secret"), b"OK")
        self.assertEqual(client.command("RESET"), b"RESET")
        self.assertTrue(client.command("PING").startswith(b"NOAUTH"))
        self.command("ACL", "SETUSER", "default", "off")
        self.assertTrue(client.command("AUTH", "secret").startswith(b"WRONGPASS"))
        self.command("ACL", "SETUSER", "default", "on", "resetpass", ">new")
        self.assertTrue(client.command("AUTH", "secret").startswith(b"WRONGPASS"))
        self.assertEqual(client.command("AUTH", "new"), b"OK")
        self.command("ACL", "SETUSER", "default", "nopass")
        self.assertEqual(self.connect().command("PING"), b"PONG")

    def test_pipelined_auth_and_transactions(self):
        self.command("ACL", "SETUSER", "default", ">secret")
        client = self.connect()
        client.sock.sendall(encode("AUTH", "secret") + encode("MULTI") +
                            encode("SET", "key", "value") + encode("EXEC"))
        self.assertEqual([client.read() for _ in range(4)], [b"OK", b"OK", b"QUEUED", [b"OK"]])
        path = self.root / "appendonlydir" / "appendonly.aof.1.incr.aof"
        self.assertEqual(path.read_bytes(), encode("MULTI") + encode("SET", "key", "value") + encode("EXEC"))
        self.server.stop()
        self.server = RunningServer(self.root, "--appendonly", "yes")
        self.addCleanup(self.server.stop)
        self.assertEqual(self.command("GET", "key"), b"value")

    def test_replication_stream_remains_authenticated(self):
        with RunningServer(self.root / "replica", "--replicaof", "127.0.0.1",
                           str(self.server.port)) as replica:
            replica.command("ACL", "SETUSER", "default", ">replica-secret")
            self.command("SET", "replicated", "value")
            self.assertEqual(self.command("WAIT", 1, 2000), 1)
            self.assertEqual(replica.command("GET", "replicated"), b"value")


if __name__ == "__main__":
    unittest.main()
