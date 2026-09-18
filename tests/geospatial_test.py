import math
import pathlib
import tempfile
import time
import unittest

from support import Client, ErrorReply, RunningServer, encode


class GeospatialTests(unittest.TestCase):
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
        self.assertEqual(self.command("GEOADD", "Sicily", 13.361389, 38.115556,
                                      "Palermo", 15.087269, 37.502669, "Catania"), 2)

    def restart(self):
        self.server.stop()
        self.server = RunningServer(self.root, *self.flags)
        self.addCleanup(self.server.stop)

    def assert_position(self, position, longitude, latitude):
        self.assertEqual(len(position), 2)
        self.assertAlmostEqual(float(position[0]), longitude, delta=1e-12)
        self.assertAlmostEqual(float(position[1]), latitude, delta=1e-12)

    def test_add_update_and_remove_locations_as_sorted_set_members(self):
        self.seed()
        self.assertEqual(self.command("TYPE", "Sicily"), b"zset")
        self.assertEqual(self.command("KEYS", "Sic*"), [b"Sicily"])
        self.assertEqual(self.command("ZSCORE", "Sicily", "Palermo"), b"3479099956230698")
        self.assertEqual(self.command("ZSCORE", "Sicily", "Catania"), b"3479447370796909")
        self.assertEqual(self.command("ZRANGE", "Sicily", 0, -1), [b"Palermo", b"Catania"])
        self.assertEqual(self.command("GEOADD", "Sicily", 0, 0, "Catania"), 0)
        self.assertEqual(self.command("ZRANGE", "Sicily", 0, -1), [b"Catania", b"Palermo"])
        self.assertEqual(self.command("ZRANK", "Sicily", "Catania"), 0)
        self.assertEqual(self.command("ZCARD", "Sicily"), 2)
        self.assertEqual(self.command("ZREM", "Sicily", "Palermo", "Catania"), 2)
        self.assertEqual(self.command("TYPE", "Sicily"), b"none")

    def test_duplicate_and_binary_members(self):
        member = b"station\x00\r\n\xff"
        self.assertEqual(self.command("geoadd", b"key\x00", 1, 2, member,
                                      13.361389, 38.115556, member, 0, 0, b""), 2)
        self.assertEqual(self.command("ZCARD", b"key\x00"), 2)
        self.assertEqual(self.command("ZSCORE", b"key\x00", member), b"3479099956230698")
        self.assertIsNotNone(self.command("GEOPOS", b"key\x00", member)[0])
        self.assertEqual(self.command("GEOSEARCH", b"key\x00", "FROMMEMBER", member,
                                      "BYRADIUS", 0, "m"), [member])

    def test_encoding_and_decoding_match_redis_across_hemispheres(self):
        fixtures = [
            (-122.4194, 37.7749, 1367859919124626, -122.41940170526505, 37.77490001056578),
            (151.2093, -33.8688, 3252046221964352, 151.2092998623848, -33.86880091934156),
            (-58.3816, -34.6037, 887133777343301, -58.381602466106415, -34.60370022923218),
            (0, 0, 3377699720527872, 2.682209014892578e-6, 1.2673605796694755e-6),
            (-180, -85.05112878, 0, -179.99999731779099, -85.05112751263943),
            (180, 85.05112878, 13510798882111488, 180, 85.05112878),
            (180, 0, 10133099161583616, 180, 1.2673605796694755e-6),
            (0, 85.05112878, 6755399441055744, 2.682209014892578e-6, 85.05112878),
        ]
        for lon, lat, score, decoded_lon, decoded_lat in fixtures:
            with self.subTest(lon=lon, lat=lat):
                self.command("GEOADD", "geo", lon, lat, "place")
                self.assertEqual(int(self.command("ZSCORE", "geo", "place")), score)
                self.assert_position(self.command("GEOPOS", "geo", "place")[0],
                                     decoded_lon, decoded_lat)
                self.command("ZADD", "raw", score, "place")
                self.assert_position(self.command("GEOPOS", "raw", "place")[0],
                                     decoded_lon, decoded_lat)

    def test_missing_positions_use_null_arrays_and_preserve_request_order(self):
        self.seed()
        positions = self.command("GEOPOS", "Sicily", "Catania", "missing", "Palermo", "Catania")
        self.assert_position(positions[0], 15.087267458438873, 37.50266842333162)
        self.assertIsNone(positions[1])
        self.assert_position(positions[2], 13.361389338970184, 38.1155563954963)
        self.assertEqual(positions[0], positions[3])
        self.assertEqual(self.command("GEOPOS", "Sicily"), [])
        self.assertEqual(self.command("GEOPOS", "absent", "a", "b"), [None, None])
        self.server.sock.sendall(encode("GEOPOS", "absent", "a"))
        self.assertEqual(self.server.stream.read(9), b"*1\r\n*-1\r\n")
        self.assertEqual(self.command("TYPE", "absent"), b"none")

    def test_invalid_coordinates_do_not_partially_change_or_create_keys(self):
        self.seed()
        before = self.command("ZRANGE", "Sicily", 0, -1, "WITHSCORES")
        invalid = [(181, 0), (-180.000001, 0), (0, 85.05112879), (0, -85.05112879)]
        invalid += [(value, 0) for value in
                    ("nan", "NaN", "inf", "-inf", "", "1x", " 1", "1 ", "1e9999", b"1\x00")]
        invalid += [(0, "nan"), (0, "inf"), (0, "invalid")]
        for lon, lat in invalid:
            for key in ("Sicily", "absent"):
                with self.subTest(lon=lon, lat=lat, key=key):
                    result = self.command("GEOADD", key, 0, 0, "Palermo", lon, lat, "bad")
                    self.assertIsInstance(result, ErrorReply)
                    self.assertEqual(self.command("ZRANGE", "Sicily", 0, -1, "WITHSCORES"), before)
                    self.assertEqual(self.command("TYPE", "absent"), b"none")

    def test_add_options_control_updates_and_change_counts(self):
        self.assertEqual(self.command("GEOADD", "absent", "XX", 0, 0, "a"), 0)
        self.assertEqual(self.command("TYPE", "absent"), b"none")
        self.seed()
        self.assertEqual(self.command("GEOADD", "Sicily", "NX", 0, 0, "Palermo"), 0)
        self.assertEqual(self.command("ZSCORE", "Sicily", "Palermo"), b"3479099956230698")
        self.assertEqual(self.command("GEOADD", "Sicily", "xx", "ch", 0, 0, "Palermo",
                                      1, 1, "new"), 1)
        self.assertEqual(self.command("GEOADD", "Sicily", "CH", 0, 0, "Palermo"), 0)
        self.assertEqual(self.command("GEOADD", "Sicily", "CH", 1, 1, "Palermo",
                                      2, 2, "new"), 2)
        self.assertEqual(self.command("ZCARD", "Sicily"), 3)

    def test_distances_units_symmetry_and_missing_members(self):
        self.seed()
        self.assertEqual(self.command("GEODIST", "Sicily", "Palermo", "Catania"), b"166274.1516")
        for unit, expected in (("m", b"166274.1516"), ("KM", b"166.2742"),
                               ("mi", b"103.3182"), ("Ft", b"545518.8700")):
            with self.subTest(unit=unit):
                self.assertEqual(self.command("GEODIST", "Sicily", "Palermo", "Catania", unit), expected)
                self.assertEqual(self.command("GEODIST", "Sicily", "Catania", "Palermo", unit), expected)
        self.assertEqual(self.command("GEODIST", "Sicily", "Palermo", "Palermo"), b"0.0000")
        self.assertIsNone(self.command("GEODIST", "Sicily", "missing", "Palermo"))
        self.assertIsNone(self.command("GEODIST", "Sicily", "Palermo", "missing"))
        self.assertIsNone(self.command("GEODIST", "absent", "a", "b"))

    def test_radius_search_by_coordinates_and_member(self):
        self.seed()
        self.assertEqual(self.command("GEOSEARCH", "Sicily", "FROMLONLAT", 15, 37,
                                      "BYRADIUS", 100, "km"), [b"Catania"])
        self.assertCountEqual(self.command("GEOSEARCH", "Sicily", "FROMLONLAT", 15, 37,
                                           "BYRADIUS", 200, "km"), [b"Palermo", b"Catania"])
        self.assertEqual(self.command("GEOSEARCH", "Sicily", "FROMMEMBER", "Palermo",
                                      "BYRADIUS", 0, "m"), [b"Palermo"])
        for unit, radius in (("m", 170000), ("KM", 170), ("ft", 550000), ("mi", 105)):
            with self.subTest(unit=unit):
                self.assertCountEqual(self.command("GEOSEARCH", "Sicily", "FROMMEMBER", "Palermo",
                                                   "BYRADIUS", radius, unit), [b"Palermo", b"Catania"])
        self.assertEqual(self.command("GEOSEARCH", "Sicily", "FROMLONLAT", 0, 0,
                                      "BYRADIUS", 1, "m"), [])
        self.assertEqual(self.command("GEOSEARCH", "absent", "FROMMEMBER", "a",
                                      "BYRADIUS", 1, "m"), [])
        self.assertIsInstance(self.command("GEOSEARCH", "Sicily", "FROMMEMBER", "absent",
                                           "BYRADIUS", 1, "m"), ErrorReply)

    def test_search_sort_count_and_optional_reply_fields(self):
        self.seed()
        search = ("GEOSEARCH", "Sicily", "FROMLONLAT", 15, 37, "BYRADIUS", 200, "km")
        self.assertEqual(self.command(*search, "asc"), [b"Catania", b"Palermo"])
        self.assertEqual(self.command(*search, "DESC"), [b"Palermo", b"Catania"])
        self.assertEqual(self.command(*search, "COUNT", 1), [b"Catania"])
        self.assertEqual(self.command(*search, "DESC", "COUNT", 1), [b"Palermo"])
        any_match = self.command(*search, "COUNT", 1, "ANY")
        self.assertEqual(len(any_match), 1)
        self.assertIn(any_match[0], [b"Catania", b"Palermo"])
        results = self.command(*search, "WITHCOORD", "WITHHASH", "WITHDIST", "ASC")
        self.assertEqual(results[0][:3], [b"Catania", b"56.4413", 3479447370796909])
        self.assertEqual(results[1][:3], [b"Palermo", b"190.4424", 3479099956230698])
        self.assert_position(results[0][3], 15.087267458438873, 37.50266842333162)
        self.assert_position(results[1][3], 13.361389338970184, 38.1155563954963)
        self.assertEqual(self.command("GEOSEARCH", "Sicily", "BYRADIUS", 200, "km",
                                      "ASC", "FROMLONLAT", 15, 37), [b"Catania", b"Palermo"])

    def test_search_crosses_dateline_and_handles_high_latitudes(self):
        self.command("GEOADD", "geo", 179.9, 0, "east", -179.9, 0, "west",
                     0, 0, "far", 0, 85, "north", 1, 85, "near-north")
        self.assertCountEqual(self.command("GEOSEARCH", "geo", "FROMLONLAT", 180, 0,
                                           "BYRADIUS", 12, "km"), [b"east", b"west"])
        self.assertCountEqual(self.command("GEOSEARCH", "geo", "FROMMEMBER", "north",
                                           "BYRADIUS", 10, "km"), [b"north", b"near-north"])
        self.assertLess(float(self.command("GEODIST", "geo", "east", "west", "km")), 23)
        self.command("GEOADD", "geo", 180, 0, "antipode")
        distance = float(self.command("GEODIST", "geo", "far", "antipode"))
        self.assertTrue(math.isfinite(distance))
        self.assertGreater(distance, 20000000)

    def test_invalid_arity_and_options(self):
        for args in (("GEOADD",), ("GEOADD", "geo", 0, 0),
                     ("GEOADD", "geo", 0, 0, "a", 1),
                     ("GEOADD", "geo", "NX", "XX", 0, 0, "a"),
                     ("GEOPOS",), ("GEODIST", "geo", "a"),
                     ("GEODIST", "geo", "a", "b", "bad"),
                     ("GEODIST", "geo", "a", "b", "m", "extra"),
                     ("GEOSEARCH",), ("GEOSEARCH", "geo", "FROMMEMBER", "a")):
            with self.subTest(args=args):
                self.assertIsInstance(self.command(*args), ErrorReply)
        search = ("GEOSEARCH", "geo", "FROMLONLAT", 0, 0)
        for options in (("BYRADIUS", -1, "m"), ("BYRADIUS", "nan", "m"),
                        ("BYRADIUS", "inf", "m"), ("BYRADIUS", "bad", "m"),
                        ("BYRADIUS", 1, "bad"), ("WITHCOORD", "WITHDIST"),
                        ("BYRADIUS", 1, "m", "ANY"),
                        ("BYRADIUS", 1, "m", "COUNT", 0),
                        ("BYRADIUS", 1, "m", "COUNT", "1x"),
                        ("BYRADIUS", 1, "m", "COUNT", "9223372036854775808"),
                        ("BYRADIUS", 1, "m", "COUNT"),
                        ("BYRADIUS", 1, "m", "FROMMEMBER", "a"),
                        ("BYRADIUS", 1, "m", "UNKNOWN")):
            with self.subTest(options=options):
                self.assertIsInstance(self.command(*search, *options), ErrorReply)
        self.assertIsInstance(self.command("GEOSEARCH", "geo", "FROMLONLAT", 181, 0,
                                           "BYRADIUS", 1, "m"), ErrorReply)
        self.assertEqual(self.command("TYPE", "geo"), b"none")

    def test_wrong_types_and_replacing_expired_strings(self):
        self.command("SET", "string", "value")
        self.command("RPUSH", "list", "value")
        self.command("XADD", "stream", "1-0", "field", "value")
        for key in ("string", "list", "stream"):
            for name, args in (("GEOADD", (0, 0, "a")), ("GEOPOS", ("a",)),
                               ("GEODIST", ("a", "b")),
                               ("GEOSEARCH", ("FROMLONLAT", 0, 0, "BYRADIUS", 1, "m"))):
                with self.subTest(key=key, command=name):
                    reply = self.command(name, key, *args)
                    self.assertIsInstance(reply, ErrorReply)
                    self.assertTrue(reply.startswith(b"WRONGTYPE"))
        self.command("SET", "expired", "value", "PX", 1)
        time.sleep(0.01)
        self.assertEqual(self.command("GEOADD", "expired", 0, 0, "a"), 1)
        self.assertEqual(self.command("SET", "expired", "replacement"), b"OK")
        self.assertEqual(self.command("GET", "expired"), b"replacement")

    def test_invalid_sorted_set_scores_do_not_crash_geo_queries(self):
        self.command("ZADD", "raw", "inf", "a", "-inf", "b", -1, "c", "1e100", "d", "1e19", "e")
        self.assertEqual(self.command("GEOPOS", "raw", "a", "b", "c", "d", "e"), [None] * 5)
        self.assertIsNone(self.command("GEODIST", "raw", "a", "b"))
        self.assertEqual(self.command("GEOSEARCH", "raw", "FROMLONLAT", 0, 0,
                                      "BYRADIUS", 30000, "km", "WITHHASH"), [])

    def test_aof_persists_writes_and_replays_location_updates(self):
        first = ("GEOADD", "geo", 0, 0, "a", 13.361389, 38.115556, "b")
        second = ("geoadd", "geo", 15.087269, 37.502669, "a")
        self.command(*first)
        self.command(*second)
        self.command("GEOPOS", "geo", "a")
        self.command("GEODIST", "geo", "a", "b")
        self.command("GEOSEARCH", "geo", "FROMMEMBER", "a", "BYRADIUS", 1, "km")
        self.command("GEOADD", "geo", 0, 0, "b", 181, 0, "invalid")
        path = self.root / "appendonlydir" / "appendonly.aof.1.incr.aof"
        expected = encode(*first) + encode(*second)
        self.assertEqual(path.read_bytes(), expected)
        before = self.command("GEOPOS", "geo", "a", "b")
        self.restart()
        self.assertEqual(self.command("GEOPOS", "geo", "a", "b"), before)
        self.assertEqual(self.command("GEODIST", "geo", "a", "b"), b"166274.1516")
        self.assertEqual(path.read_bytes(), expected)

    def test_transactions_and_replication(self):
        with RunningServer(self.root / "replica", *self.flags, "--replicaof",
                           "127.0.0.1", str(self.server.port)) as replica:
            self.command("MULTI")
            self.assertEqual(self.command("GEOADD", "geo", 0, 0, "a"), b"QUEUED")
            self.command("GEOADD", "geo", 1, 1, "b")
            self.command("GEODIST", "geo", "a", "a")
            self.assertEqual(self.command("EXEC"), [1, 1, b"0.0000"])
            self.assertEqual(self.command("WAIT", 1, 2000), 1)
            positions = self.command("GEOPOS", "geo", "a", "b")
            self.assertEqual(replica.command("GEOPOS", "geo", "a", "b"), positions)
        self.restart()
        self.assertEqual(self.command("GEOPOS", "geo", "a", "b"), positions)
        with RunningServer(self.root / "replica", *self.flags) as restored:
            self.assertEqual(restored.command("GEOPOS", "geo", "a", "b"), positions)

    def test_subscribed_clients_cannot_add_locations(self):
        client = Client(self.server.port)
        self.addCleanup(client.close)
        client.command("SUBSCRIBE", "channel")
        self.assertIsInstance(client.command("GEOADD", "geo", 0, 0, "a"), ErrorReply)
        self.assertEqual(self.command("TYPE", "geo"), b"none")


if __name__ == "__main__":
    unittest.main()
