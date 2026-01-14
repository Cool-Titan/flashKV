# FlashKV: Multi-Threaded In-Memory Key-Value Cache

FlashKV is a multi-threaded in-memory key-value cache server written in C++20.
It speaks a small JSON protocol over TCP, stores four data types, expires keys
with TTL, tracks LRU access order, and serves many concurrent clients from a
thread-per-connection model.

Built with the standard library only: POSIX sockets, `std::thread`,
`std::mutex`, and a single vendored header for JSON parsing.

## Features

- **4 data types**: STRING, HASH, LIST, SET
- **Thread-per-connection server**: 50+ concurrent clients, verified by test and stress run
- **JSON protocol**: human-readable requests and responses, debuggable with `nc`
- **TTL**: keys expire lazily, checked on access
- **LRU ordering**: every key access updates a recency list
- **Error handling**: every store operation returns `Result<T>` — a value or an error message
- **93 tests**: unit, concurrency, and real-socket integration tests
- **Docker + CI**: multi-stage image, GitHub Actions with a ThreadSanitizer job

## Quick Start

### Prerequisites

- GCC 11+ (C++20) or Clang 14+
- CMake 3.15+
- Google Test (`libgtest-dev`) for the test suite
- Python 3 for the client and stress test

### Build and test

```bash
mkdir build && cd build
cmake .. -DCMAKE_CXX_COMPILER=g++-11
make -j"$(nproc)"
ctest --output-on-failure
```

### Run the server

```bash
./build/flashkv                 # listens on port 6379
./build/flashkv --port 7000     # custom port
./build/flashkv --debug         # DEBUG level logging
```

### Use the Python client

```bash
python3 client/flashkv_client.py SET foo bar
python3 client/flashkv_client.py GET foo
python3 client/flashkv_client.py LPUSH mylist a b c
python3 client/flashkv_client.py LRANGE mylist 0 -1
python3 client/flashkv_client.py --interactive     # REPL
```

### Docker

```bash
docker compose up --build
python3 client/flashkv_client.py SET test value
```

The image is multi-stage: the builder compiles and runs the full test suite (a
failing test fails the image build), and the runtime stage ships only the binary.

## Protocol

One JSON object per request, one newline-terminated JSON object per response.
Requests may be pipelined — several objects in a single write are answered in
order.

Arguments can be given as named fields or as a flat `args` array:

```json
{"cmd": "SET", "key": "x", "value": "hello"}
{"cmd": "SET", "args": ["x", "hello"]}
```

Both forms are equivalent. Command names are case-insensitive.

| Operation | Request | Response |
|---|---|---|
| SET | `{"cmd":"SET","key":"x","value":"hello"}` | `{"status":"OK","data":"OK"}` |
| SET with TTL | `{"cmd":"SET","key":"x","value":"hi","ttl":30}` | `{"status":"OK","data":"OK"}` |
| GET | `{"cmd":"GET","key":"x"}` | `{"status":"OK","data":"hello"}` |
| GET (missing) | `{"cmd":"GET","key":"nope"}` | `{"status":"OK","data":null}` |
| HSET | `{"cmd":"HSET","key":"h","fields":{"f1":"v1","f2":"v2"}}` | `{"status":"OK","data":2}` |
| LPUSH | `{"cmd":"LPUSH","key":"l","elements":["a","b"]}` | `{"status":"OK","data":2}` |
| SADD | `{"cmd":"SADD","key":"s","members":["x","y"]}` | `{"status":"OK","data":2}` |
| LRANGE | `{"cmd":"LRANGE","key":"l","start":0,"stop":-1}` | `{"status":"OK","data":["b","a"]}` |
| Error | `{"cmd":"NOPE"}` | `{"status":"ERROR","error_msg":"ERR unknown command 'NOPE'","data":null}` |

Named fields are read in a fixed order: `key`, `keys`, `field`, `member`,
`value`, `fields`, `elements`, `members`, `ttl`, `seconds`, `count`, `start`,
`stop`.

Try it without any client at all:

```bash
printf '{"cmd":"SET","key":"a","value":"b"}{"cmd":"GET","key":"a"}' | nc localhost 6379
```

## Commands

### STRING
| Command | Notes |
|---|---|
| `SET key value [ttl]` | Replaces the value and clears any previous TTL |
| `GET key` | `null` when missing, error on a non-string key |
| `DEL key [key ...]` | Number of keys removed |
| `EXISTS key [key ...]` | Number of keys present |
| `INCR key` / `DECR key` | Missing key starts at 0; TTL is preserved |
| `EXPIRE key seconds` | 1 if set, 0 if the key is gone; TTL <= 0 deletes the key |
| `TTL key` | Seconds left, `-1` with no TTL, `-2` when the key is missing |
| `TYPE key` | `string` / `hash` / `list` / `set` / `none` |
| `KEYS` / `DBSIZE` | All live keys / key count |

### HASH
`HSET key field value [field value ...]` (new fields added) ·
`HGET key field` · `HDEL key field [field ...]` · `HEXISTS key field` ·
`HGETALL key`

### LIST
`LPUSH key element [...]` · `RPUSH key element [...]` ·
`LPOP key [count]` · `RPOP key [count]` · `LLEN key` ·
`LRANGE key start stop` (inclusive, negative indices count back from the end)

### SET
`SADD key member [...]` · `SREM key member [...]` · `SMEMBERS key` (sorted) ·
`SISMEMBER key member` · `SINTER key [key ...]` · `SUNION key [key ...]`

### Other
`PING` · `ECHO message` · `LRUORDER` (access order, least recent first)

Empty containers are deleted: popping the last list element, removing the last
set member, or deleting the last hash field removes the key itself, matching
Redis.

## Architecture

```
CLIENT (Python / nc / telnet)
    | JSON over TCP
Server            accept loop, one detached thread per client
    |
Connection        per-client read/execute/write loop, socket framing
    |
ProtocolHandler   JSON <-> Request/Response
    |
CommandRouter     command dispatch, argument validation
    |
KeyValueStore     std::map + one mutex, lazy TTL, Result<T> errors
    |
LRUCache          std::list + index map, own mutex
```

| Component | File | Responsibility |
|---|---|---|
| `Server` | `src/network/server.cpp` | Listen, accept, connection registry, signal handling |
| `Connection` | `src/network/connection.cpp` | One client: read, frame, execute, respond |
| `ProtocolHandler` | `src/protocol/protocol_handler.cpp` | Parse requests, serialize responses |
| `CommandRouter` | `src/command/command_router.cpp` | Dispatch to 31 command handlers |
| `KeyValueStore` | `src/storage/key_value_store.cpp` | The data, the mutex, TTL and type rules |
| `LRUCache` | `src/storage/lru_cache.cpp` | O(1) access-order tracking |
| `Logger` | `src/utils/logger.cpp` | Timestamped, level-based, thread-safe logging |

### Threading model

- One accept thread, one detached thread per client connection.
- All key-value state sits behind a single `std::mutex` in `KeyValueStore`;
  every public method is a critical section, so read-modify-write commands like
  `INCR` are atomic without extra machinery.
- `LRUCache` has its own mutex. The lock order is always store → LRU, and the
  LRU never calls back into the store, so the pair cannot deadlock.
- The server keeps a registry of live connections. Shutdown closes each client
  socket and waits for the handler threads to drain, so no detached thread can
  outlive the router it points at.
- Connections use a 30 second socket timeout, so a silent client cannot pin a
  thread forever.

## Design Decisions

**Thread-per-connection instead of an event loop.** Simple to write, simple to
debug, and a blocking read per client is easy to reason about. The trade-off is
one OS thread per client, which caps concurrency well below what epoll would
reach. Marked in the code as the upgrade path.

**JSON instead of the Redis RESP protocol.** Human-readable, debuggable with
`nc`, and parsed by one vendored header. The trade-off is that redis-cli cannot
talk to it, and JSON parsing costs more per request than RESP.

**One global mutex instead of sharding.** The store is a single critical
section. Correct by construction and small enough to audit. If lock contention
ever showed up in a profile, the fix is sharding by key hash — noted in the
source.

**Lazy TTL expiration.** A key is removed when it is next touched after expiry.
No background sweeper thread, no timers. The trade-off is that an expired key
nobody asks for keeps its memory.

**`Result<T>` instead of exceptions.** Every store operation returns a value or
an error string, so error paths are visible in the signature. The error arm is a
small `ErrorMsg` wrapper rather than a bare `std::string`, because
`std::variant<std::string, std::string>` is a duplicated alternative and makes
`std::holds_alternative` ill-formed.

**LRU ordering without eviction.** The recency list is maintained on every
access, but nothing is ever evicted; memory-bounded eviction is out of scope.

## Testing

```bash
cd build
ctest --output-on-failure
```

93 tests across six suites:

| Suite | Tests | Covers |
|---|---|---|
| `test_json_protocol` | 16 | Parsing both request shapes, error cases, serialization |
| `test_storage_engine` | 36 | All four data types, type mismatches, edge cases |
| `test_ttl` | 11 | Expiry, TTL/EXPIRE semantics, TTL across data types |
| `test_lru` | 10 | Touch/remove/order, promotion on store reads |
| `test_concurrent` | 8 | Parallel mixed operations, atomic INCR, LRU consistency |
| `test_integration` | 12 | End-to-end flows plus real TCP round trips against a live server |

The integration suite starts actual servers on ports 16379-16382, drives them
over real sockets, and checks pipelining, malformed input handling, and 50
concurrent clients.

### ThreadSanitizer

CI runs the concurrency and socket tests under TSan:

```bash
mkdir build-tsan && cd build-tsan
cmake .. -DCMAKE_CXX_FLAGS="-fsanitize=thread -g -O1"
make -j"$(nproc)" flashkv_tests
./flashkv_tests --gtest_filter='ConcurrencyTest.*:ServerIntegrationTest.*'
```

## Stress Testing

```bash
./stress_test/stress_test.sh              # starts a server, runs 50x1000 ops, stops it
./stress_test/stress_test.sh 100 2000     # 100 threads, 2000 ops each
```

Or against an already running server:

```bash
python3 stress_test/stress_test.py --threads 50 --ops 1000
```

Each worker holds one connection and issues a random mix of SET/GET/DEL/LPUSH/
LPOP/SADD/HSET/INCR. The script reports throughput, error rate, and latency
percentiles, and exits non-zero if any operation fails, so it works as a CI gate.

Measured run (50 threads x 500 ops, server and client in one Ubuntu 22.04
container, 4 CPUs):

```
Total operations:  25000
Successful:        25000
Errors:            0
Error rate:        0.00%
Duration:          2.35s
Throughput:        10633 ops/sec
Latency p50:       3.791 ms
Latency p99:       14.415 ms
```

The client is single-process Python, so these numbers describe the pair rather
than the server's ceiling.

## Known Limitations

- In-memory only: no persistence, no snapshots, no AOF.
- Thread-per-connection: concurrency is bounded by OS thread limits.
- LRU tracks order but never evicts; there is no memory cap.
- Single node: no clustering, replication, or sharding.
- JSON protocol: not wire-compatible with Redis.
- POSIX only: sockets and signals are POSIX, so Windows needs WSL or Docker.

## Future Enhancements

- Event loop (epoll/kqueue) to replace thread-per-connection
- Persistence: RDB-style snapshots and an append-only log
- Memory-bounded eviction driven by the existing LRU ordering
- Sharded locking by key hash
- Replication and clustering

## Project Layout

```
src/
  main.cpp                       entry point, argument parsing
  utils/logger.{hpp,cpp}         thread-safe logging
  storage/types.hpp              value types, KeyMetadata, Result<T>
  storage/key_value_store.*      the store and all data-type operations
  storage/lru_cache.*            access ordering
  protocol/protocol_handler.*    JSON parse and serialize
  command/command_router.*       command dispatch
  network/{server,connection}.*  TCP layer
include/nlohmann/json.hpp        vendored single-header JSON (v3.11.3, MIT)
tests/                           six GTest suites
client/flashkv_client.py         CLI and REPL client
stress_test/                     load generator and runner script
```

## License

MIT
