# TrueStore

TrueStore is a C++23 in-memory data store implementing a subset of Redis commands over RESP, with replication, append-only persistence, and specialized data structures.

<p align="center">
  <img src="https://img.shields.io/badge/C%2B%2B-23-blue" alt="C++23">
  <img src="https://img.shields.io/badge/platforms-Linux%20%7C%20macOS-lightgrey" alt="Platforms: Linux | macOS">
  <a href="LICENSE"><img src="https://img.shields.io/badge/license-MIT-green" alt="License: MIT"></a>
  <a href="#project-structure"><img src="https://img.shields.io/badge/source_LOC-4,695-blue" alt="Source LOC: 4,695"></a>
  <a href="#testing"><img src="https://img.shields.io/badge/tests-104_passing-brightgreen" alt="Tests: 104 passing"></a>
</p>

<!-- Demo / terminal GIF: -->

## Overview

TrueStore is a TCP server that accepts Redis-style commands and stores data in memory. It includes strings, lists, streams, sorted sets, bitmaps, and geospatial operations, plus transactions, replication, persistence, Pub/Sub, and authentication.

<!-- ### Why I built it -->


### Redis compatibility

Compatibility means that the implemented commands use Redis command names and RESP reply formats, so they can be exercised with `redis-cli` and clients that send supported requests. It does not mean full Redis command coverage, identical behavior in every edge case, or compatibility with every Redis client initialization sequence. TrueStore is a work in progress, not a drop-in Redis replacement. See [Supported Commands](#supported-commands) and [Limitations](#limitations).

## Highlights

- **Networking and RESP:** a `poll()` event loop, per-connection input buffers, partial request handling, and pipelining.
- **Data model:** binary strings, vector-backed lists, radix-tree streams, and sorted sets with member and score indexes.
- **Transactions and WATCH:** queued commands with `MULTI` / `EXEC`, plus a limited form of optimistic conflict detection.
- **Replication:** a primary/replica handshake, live command propagation, byte offsets, acknowledgements, and `WAIT`.
- **Persistence:** incremental AOF files with manifest-based replay, configurable `fsync`, and loading of a limited RDB subset.
- **Pub/Sub:** channel subscriptions and message delivery to connected subscribers.
- **Authentication:** the default user, password hashes, `AUTH`, and a subset of `ACL`.
- **Specialized structures:** bitmap operations on strings and geospatial coordinates encoded as sorted-set scores.

## Architecture

## How It Works

### Networking and RESP

The server listens on an IPv4 TCP socket and multiplexes connections with `poll()`. Each connection has an input buffer. The RESP parser consumes complete arrays of bulk strings and leaves incomplete requests buffered until more bytes arrive. Multiple complete commands in one buffer are dispatched in order, supporting pipelining.

Replies include simple strings, binary-safe bulk strings, integers, errors, and nested or null arrays. `BLPOP`, blocking `XREAD`, and `WAIT` register waiters with optional deadlines so the event loop can continue serving other connections. Reply writes use `send_all()`; waiting for a slow receiver can still delay the loop.

### Storage Engine

The store uses separate containers for each value type:

| Value | Representation |
| --- | --- |
| Strings | `std::map<std::string, std::string>`; values may contain arbitrary bytes |
| Lists | A hash map of `std::vector<std::string>` values |
| Streams | A hash map of stream objects, each backed by a `rax` radix tree |
| Sorted sets | A hash map from member to score, plus an ordered set of `(score, member)` pairs |
| Bitmaps | Bits addressed within ordinary string bytes, most significant bit first |
| Geo | Encoded coordinates stored in sorted-set scores |

`SET` supports relative expiry through `EX` and `PX`. Expiration deadlines use a steady clock and are checked lazily on relevant access paths; there is no background expiration sweep. Loading RDB timestamps converts their wall-clock deadlines into remaining lifetimes. Expiry and type enforcement are not yet uniform across all commands.

### Transactions

`MULTI` queues commands in per-client state. `EXEC` executes the queue in order on the event-loop thread and returns an array of results. `DISCARD` clears the queue. Execution errors do not roll back earlier writes.

`WATCH` records snapshots of string values, and `EXEC` compares the current values with those snapshots. A difference aborts the transaction with a null array; `UNWATCH` clears the snapshots. This provides limited optimistic locking: it does not track mutations to lists, streams, or sorted sets, and it cannot detect a string changing and then returning to its original value.

### Streams

Stream entries have increasing `milliseconds-sequence` IDs. `XADD` accepts explicit IDs, `*`, and `milliseconds-*`. IDs are encoded as 16-byte big-endian keys, so radix-tree ordering follows numeric ID ordering. Each entry holds a vector of field/value pairs.

`XRANGE` seeks into the radix tree for ordered range reads. `XREAD` supports multiple streams, `COUNT`, and `BLOCK`; `$` starts from the stream's current last entry. Consumer groups and stream trimming are not implemented.

### Replication

A replica connects with `PING`, `REPLCONF listening-port`, `REPLCONF capa psync2`, and `PSYNC ? -1`. The primary replies with `FULLRESYNC` and an RDB payload, then propagates successful write commands as RESP. Replication offsets count bytes, and `REPLCONF GETACK` / `ACK` exchanges let `WAIT` check acknowledgement of the requesting client's writes.

The bootstrap is incomplete: the primary sends a fixed empty RDB, and the replica reads but does not import the received snapshot. Existing primary data is therefore not transferred when a replica joins. Live propagation works after the handshake; there is no backlog-based partial resynchronization, automatic reconnect, or failover. `WAIT` reports replication acknowledgement, not a disk-durability guarantee.

### Persistence

**RDB:** with AOF disabled, `--dir` and `--dbfilename` select an existing file to load at startup. The reader handles string values, integer-encoded strings, and expiry timestamps. It does not write snapshots, decode compressed strings, or restore other data types. Missing files load no entries; parse failures are logged and startup continues.

**AOF:** `--appendonly yes` records successful write commands as RESP in incremental files. A manifest lists the files and their replay order. Transaction writes are enclosed in `MULTI` / `EXEC` for replay. `--appendfsync` accepts `always`, `everysec`, or `no`.

On startup, AOF replay runs through command dispatch before serving clients. Invalid manifests, missing incremental files, incomplete commands, unfinished transactions, or command errors stop AOF startup. There is no automatic repair of a truncated tail, AOF rewrite, or compaction. AOF takes precedence over loading an RDB file.

Commands are recorded as submitted. Relative `SET` expiry is restarted during replay, and automatically generated stream IDs can be regenerated during replay or replication. Recovery does not yet preserve all Redis semantics.

### Pub/Sub

`SUBSCRIBE` and `UNSUBSCRIBE` maintain per-client channel memberships. `PUBLISH` queues messages for connected subscribers and returns the recipient count. Subscribed connections have a restricted command set; `QUIT` or `RESET` also clears subscriptions. Messages and subscription state are not persisted. Pattern subscriptions are not implemented.

### Authentication

The default user starts enabled with no password. `ACL SETUSER default` supports enabling/disabling the user, `nopass`, `resetpass`, and adding/removing passwords or SHA-256 hashes. OpenSSL supplies hashing and constant-time hash comparison. `AUTH` accepts a password alone or `default` plus a password.

Only the default user is supported. There are no per-command, key-pattern, or channel permissions. Authentication state is per connection, and ACL configuration is neither persisted nor replicated.

## Supported Commands

This table lists dispatched commands, not every option supported by Redis. Command names are case-insensitive; option handling varies by command.

| Group | Commands | Scope |
| --- | --- | --- |
| Strings | `SET`, `GET`, `STRLEN`, `INCR` | `SET` accepts `EX` or `PX`; `STRLEN` counts bytes; `INCR` currently uses C++ `int` conversion |
| Lists | `LPUSH`, `RPUSH`, `LLEN`, `LRANGE`, `LPOP`, `BLPOP` | `LPOP` supports a count; `BLPOP` supports multiple keys and a timeout |
| Streams | `XADD`, `XRANGE`, `XREAD` | Range counts, generated IDs, and blocking reads; no consumer groups |
| Transactions | `MULTI`, `EXEC`, `DISCARD`, `WATCH`, `UNWATCH` | String-value snapshot checks for `WATCH` |
| Replication | `INFO`, `REPLCONF`, `PSYNC`, `WAIT` | `INFO` returns replication information; bootstrap limitations apply |
| Persistence | `CONFIG GET` | Reads selected startup settings; no `SAVE`, `BGSAVE`, or `BGREWRITEAOF` |
| Pub/Sub | `SUBSCRIBE`, `UNSUBSCRIBE`, `PUBLISH` | Exact channel subscriptions |
| Sorted Sets | `ZADD`, `ZRANK`, `ZRANGE`, `ZCARD`, `ZSCORE`, `ZREM` | Rank-based ranges and optional `WITHSCORES`; no full Redis option set |
| Geo | `GEOADD`, `GEOPOS`, `GEODIST`, `GEOSEARCH` | Radius searches from a member or coordinates; no `BYBOX` |
| Bitmaps | `SETBIT`, `GETBIT`, `BITCOUNT`, `BITOP` | Byte/bit count ranges; `AND`, `OR`, `XOR`, `NOT` |
| Authentication | `AUTH`, `ACL WHOAMI`, `ACL GETUSER`, `ACL SETUSER` | Default user only |
| General | `PING`, `ECHO`, `TYPE`, `KEYS`, `QUIT`, `RESET` | One logical keyspace; no `SELECT` |

## Building

Develop on Linux or macOS with:

- A C++23 compiler and a C compiler for the vendored Rax library.
- CMake 3.20 or newer for C++23 support.
- OpenSSL Crypto libraries and development headers.
- Python 3 to run integration tests.

```sh
cmake -S . -B build
cmake --build build -j
```

If CMake cannot locate OpenSSL, pass `-DOPENSSL_ROOT_DIR=/path/to/openssl` when configuring.

## Running

The current CMake targets produce `server` and `client`.

```sh
./build/server --port 6379
```

Start a replica in a second terminal:

```sh
./build/server --port 6380 --replicaof 127.0.0.1 6379
```

The server binds to all IPv4 interfaces. There is no bind-address flag or TLS support.

## Client

The bundled client connects to `127.0.0.1:6379`, sends one `PING`, prints the raw reply, and exits. It has no command-line options or interactive prompt.

```sh
./build/client
```

Use `redis-cli` for interactive commands:

```sh
redis-cli -p 6379
```

```text
127.0.0.1:6379> SETBIT mango 1 1
(integer) 0
127.0.0.1:6379> STRLEN mango
(integer) 1
127.0.0.1:6379> BITCOUNT mango
(integer) 1
```

## Configuration

Configuration comes from command-line flags. There is no configuration-file parser or `CONFIG SET` implementation.

| Flag | Default | Meaning |
| --- | --- | --- |
| `--port` | `6379` | TCP listening port |
| `--replicaof` | Unset | Primary host and port, as two arguments or one quoted `"host port"` argument |
| `--dir` | Current working directory | Base directory for persistence |
| `--dbfilename` | Unset | Existing RDB filename to load when AOF is disabled |
| `--appendonly` | `no` | Set to `yes` to enable AOF |
| `--appenddirname` | `appendonlydir` | AOF directory under `--dir` |
| `--appendfilename` | `appendonly.aof` | Base name for the AOF manifest and incremental files |
| `--appendfsync` | `everysec` | `always`, `everysec`, or `no` |

For example:

```sh
./build/server --port 6379 --dir ./data \
  --appendonly yes --appendfsync everysec
```

This creates `data/appendonlydir/appendonly.aof.manifest` and, initially, `data/appendonlydir/appendonly.aof.1.incr.aof`.

`CONFIG GET` accepts one of `dir`, `appendonly`, `appenddirname`, `appendfilename`, or `appendfsync`. Other names return an empty array; wildcard lookup is not implemented.

Configure authentication at runtime through the Redis CLI:

```text
ACL SETUSER default on >example-password
AUTH default example-password
ACL WHOAMI
```

These are commands entered inside `redis-cli`, where `>` is an ACL password prefix. There is no startup password flag, and the configuration resets on restart.

## Testing

Integration tests use Python's standard library, real TCP connections, temporary directories, and server processes on temporary ports. The suites cover strings, bitmaps, sorted sets, geo, AOF recovery, Pub/Sub, and authentication, with replication and transaction checks included in several suites.

Run a single suite:

```sh
python3 tests/strings_test.py build/server
python3 tests/bitmaps_test.py build/server
```

Run all suites sequentially:

```sh
for test in tests/*_test.py; do
  python3 "$test" build/server || exit 1
done
```

For an AddressSanitizer and UndefinedBehaviorSanitizer build with Clang or GCC:

```sh
cmake -S . -B build/sanitizers -DCMAKE_BUILD_TYPE=Debug \
  -DCMAKE_C_FLAGS="-fsanitize=address,undefined -fno-omit-frame-pointer" \
  -DCMAKE_CXX_FLAGS="-fsanitize=address,undefined -fno-omit-frame-pointer" \
  -DCMAKE_EXE_LINKER_FLAGS="-fsanitize=address,undefined"
cmake --build build/sanitizers -j
for test in tests/*_test.py; do
  python3 "$test" build/sanitizers/server || exit 1
done
```

The test harness kills server processes during cleanup, so these runs do not validate graceful shutdown or complete exit-time leak detection. Passing the included tests does not establish full Redis compatibility.


## Project Structure

```text
src/
├── main.cpp / client_main.cpp
├── server/ / net/          TCP event loop and socket helpers
├── resp/ / command/        Protocol parsing, dispatch, and client state
├── store/                 Value storage and data-type commands
├── stream/                Stream IDs and radix-tree storage
├── replication/           Replication command handlers
├── aof/ / rdb/             Persistence and recovery
├── pubsub/ / auth/         Subscriptions and authentication
├── config/                Startup arguments
└── client/                Minimal PING client
third_party/rax/           Vendored radix tree
tests/                     Python integration suites and helpers
CMakeLists.txt             Build targets and dependencies
```

## Design Decisions / Trade-offs

These describe the current implementation and its consequences.

- **Radix-tree streams:** fixed-width, big-endian IDs allow ordered seeks and range iteration in `rax`. The C++ wrapper owns entry payloads and frees them with the tree.
- **Single event-loop thread:** command execution is serialized, with no store mutexes or worker pool. This simplifies shared state, but long commands, slow output, and synchronous disk operations delay other clients.
- **Standard containers:** vectors and ordered sets keep list and sorted-set code compact. Front-of-list insertion/removal moves elements; rank queries walk ordered entries rather than using an order-statistics index. Geo radius searches scan members.
- **Command-log persistence:** reusing RESP and command dispatch keeps replay close to normal execution. Synchronous writes and `fsync` affect latency, while replay still needs canonical expiry timestamps and generated IDs for faithful recovery.
- **Snapshot-based WATCH:** comparing values is simple but weaker than tracking every key mutation. It is not a general locking mechanism.

## Limitations

- These are known limitations I'll fix/implement in upcoming releases.

- This is an incomplete Redis implementation. Hashes, ordinary sets, cluster support, RESP3, `HELLO`, `SELECT`, and many common commands and options are not implemented yet.
- `INCR` uses C++ `int` rather than Redis's full signed 64-bit integer range. Type checks, argument validation, and expiry handling have gaps across commands.
- `WATCH` only observes string-value snapshots. Queued transaction errors are handled during execution; Redis transaction validation semantics are not fully reproduced.
- Replication does not transfer an existing dataset, resume from a backlog, reconnect automatically, or enforce read-only replicas. Transaction commands propagate without preserving a `MULTI` / `EXEC` envelope on the replication link.
- RDB support is a partial loader, with no snapshot writer or checksum validation. AOF supports incremental manifests only, without base snapshots, rewrites, compaction, or tail repair. Relative expiries and generated IDs are not canonicalized for replay.
- The event loop has a fixed array of 200 descriptors, including the listener and any upstream connection. There are no configurable memory limits or eviction policies; slow clients can delay other work.
- Authentication only covers the default user. ACL state is not durable, transport is unencrypted, and startup accepts connections without a password on all IPv4 interfaces.
- The bundled client is a PING demonstration. Native Windows support is absent because the implementation uses POSIX networking and file APIs.

## Acknowledgements

- Redis, for the command model and RESP protocol that TrueStore implements in part.
- Salvatore Sanfilippo and the Rax contributors, for the radix-tree implementation vendored in [`third_party/rax/`](third_party/rax/). Its BSD 3-Clause terms and copyright notices are retained in [the source header](third_party/rax/rax.h).


## Contributing

See the [contribution guide](CONTRIBUTING.md) for development and commit conventions. Participation is covered by the [code of conduct](CODE_OF_CONDUCT.md).

## License

TrueStore is available under the [MIT License](LICENSE). Vendored Rax code retains its [existing license notices](third_party/rax/rax.h).
