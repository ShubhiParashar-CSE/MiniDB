# MiniDB

A persistent, multi-threaded, network-accessible key-value store written in
C++20 from scratch (raw POSIX sockets, no external libraries).

Think "a tiny Redis": clients connect over TCP, send text commands, get
text responses, and data survives restarts and crashes.

## Features

- `SET key value`, `GET key`, `DEL key`, `EXISTS key`, `EXPIRE key seconds`, `PING`
- Thread-safe in-memory store (readers-writer lock, not a single global mutex)
- **Durability**: every write is logged to disk (Write-Ahead Log) before being
  acknowledged, so a crash right after a write doesn't lose it
- **Compaction**: state is periodically snapshotted to disk and the WAL is
  truncated, so the log doesn't grow forever
- **TTL/expiry**: lazy expiry on read + a background sweep thread
- **Concurrency**: fixed-size thread pool handles client connections
  (bounded resource use, no thread-per-connection explosion)

## Build & run

```
make
./minidb 6380
```

Then from another terminal:
```
bash test.sh   # runs a full demo including a simulated crash + recovery
```

Or talk to it directly with netcat / telnet:
```
nc localhost 6380
SET foo bar
GET foo
```

## Architecture

```
include/threadpool.hpp   fixed worker-thread pool + task queue
include/kvstore.hpp       the storage engine (hash map + shared_mutex + TTL)
include/persistence.hpp   Write-Ahead Log + snapshot/compaction
include/protocol.hpp      parses text commands, calls store + persistence
include/server.hpp        TCP accept loop, dispatches to thread pool
src/main.cpp               wiring + background maintenance thread
```

Request flow for a `SET`:
1. `Server` accepts the connection, hands it to a `ThreadPool` worker
2. Worker reads a line, `Protocol::handleLine` parses it
3. `KVStore::set` updates the in-memory map (under a write lock)
4. `Persistence::logSet` appends the command to `data/wal.log` and flushes
5. `+OK\r\n` is sent back to the client

Startup flow:
1. Load `data/snapshot.db` (last compacted full state) into the store
2. Replay `data/wal.log` (writes since that snapshot) on top of it
3. Start accepting connections

## Design decisions worth discussing in an interview

- **shared_mutex over a plain mutex**: reads (GET) can run concurrently;
  writes (SET/DEL/EXPIRE) get exclusive access. Trade-off: more overhead
  per-lock than a plain mutex, so it only wins when reads dominate and
  there's real multi-core contention.
- **WAL + snapshot instead of writing full state on every change**: O(1)
  append per write instead of O(n) full rewrite. The snapshot bounds how
  much WAL ever needs replaying on startup.
- **fsync/flush on every WAL write**: durability over raw throughput. A
  production system might batch/group commits for higher throughput at
  the cost of a small durability window — a deliberate trade-off, not an
  oversight.
- **Thread pool over thread-per-connection**: bounded resource usage.
  Trade-off: connections can queue if all workers are busy, whereas
  thread-per-connection never queues (but can exhaust the OS under load).
- **Text protocol over binary**: easy to debug with `nc`/`telnet`, easy to
  parse. Known limitation: values can't contain raw spaces, since parsing
  splits on whitespace. A real protocol (like Redis's RESP) length-prefixes
  each argument to avoid this.

## Known limitations (good to name proactively in an interview)

- Single-node only, no replication
- No authentication/encryption
- Values can't contain spaces (protocol limitation, not engine limitation)
- No crash-safety for a torn/partial WAL line during an actual power-loss
  mid-write (a real system would checksum each WAL entry)
- Compaction and reaping run on a timer, not adaptively under memory pressure

## Natural next steps (if asked "what would you add next?")

- Switch the accept loop to `epoll` for higher connection scalability
- RESP-style length-prefixed protocol to support binary-safe values
- LRU eviction policy when memory is capped
- A simple client library / CLI tool
