# Contributing to TrueStore

Bug reports, fixes, command implementations, and documentation improvements are welcome.
Please follow the [code of conduct](CODE_OF_CONDUCT.md) in project spaces.
For larger changes, describe the problem and proposed behavior in an issue so others can
discuss the approach before implementation.

## Build and run

Develop on Linux or macOS with these dependencies installed:

- A C++23 compiler and a C compiler for the vendored Rax library.
- CMake 3.20 or newer for C++23 support.
- OpenSSL Crypto libraries and development headers.
- Python 3 for the integration tests.

From the repository root:

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug
cmake --build build -j
./build/server --port 6379
```

The server accepts Redis commands over RESP. A Redis CLI client can be used for manual
checks. Stop the manually started server when finished; the integration tests start
their own server processes on temporary ports.

## Run tests

Run an individual suite by passing the server executable:

```sh
python3 tests/auth_test.py build/server
python3 tests/bitmaps_test.py build/server
```

Run all integration suites before submitting code changes:

```sh
for test in tests/*_test.py; do
  python3 "$test" build/server || exit 1
done
```

Add targeted regression coverage when changing command behavior. Use the socket and
server helpers in `tests/support.py`. Documentation changes and mechanical moves do
not need new tests.

## Project layout

- `src/store/` contains data-type commands and storage state.
- `src/auth/`, `src/replication/`, and `src/pubsub/` contain their respective command implementations.
- `src/command/` dispatches commands and manages per-client command state.
- `src/resp/` parses and encodes the wire protocol.
- `src/server/` and `src/net/` manage connections and socket I/O.
- `src/config/` parses startup configuration; `src/store/config_commands.cpp` implements Redis `CONFIG` commands.
- `src/aof/` and `src/rdb/` handle persistence.
- `src/stream/` contains the stream data structure.
- `tests/` contains integration tests; `third_party/` contains vendored dependencies.

## Code and commit conventions

Follow the surrounding C++ style, including two-space indentation. Keep changes focused,
use clear names, and avoid documentation-style comments that restate the implementation.
Preserve Redis response types and error behavior. Consider how command changes interact
with transactions, authentication, persistence, replication, and key expiration.

Use small commits with short subjects in this exact format:

```text
type(scope) : description
```

Examples:

```text
feat(bitmap) : add bit position queries
fix(auth) : validate authentication arguments
docs(contributing) : clarify test setup
```

Do not commit generated build output, caches, or local data files. Preserve third-party
copyright and license notices.

## Pull requests and bug reports

Describe the problem, the resulting behavior, and the checks you ran. Include a small
reproduction for bugs, along with the relevant commands, expected replies, actual replies,
and platform details. Remove passwords and other sensitive data from examples and logs.

Keep unrelated changes in separate pull requests. Update build inputs when adding or
moving source files.

Contributions to TrueStore are submitted under the project's [MIT License](LICENSE).
