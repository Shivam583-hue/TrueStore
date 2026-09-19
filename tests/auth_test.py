import hashlib
import pathlib
import tempfile
import unittest

from support import ErrorReply, RunningServer


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


if __name__ == "__main__":
    unittest.main()
