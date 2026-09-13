# MiniDB

MiniDB is a lightweight, persistent key-value database built in C++20 from scratch. It mimics the core ideas of a small Redis-like server: TCP client access, in-memory storage, TTL support, durable writes, and background maintenance.

The project is designed as a practical systems programming exercise and demonstrates how a real database can combine:

- a thread-safe in-memory store
- a WAL-based persistence layer
- periodic snapshot compaction
- a bounded thread pool
- a simple text-based command protocol over TCP

## Features

- `SET key value`
- `GET key`
- `DEL key`
- `EXISTS key`
- `EXPIRE key seconds`
- `PING`
- Time-to-live (TTL) expiration support
- Thread-safe concurrent reads and writes
- Crash recovery with WAL replay
- Snapshot compaction to keep persistence efficient
- Fixed-size worker thread pool for client handling

## Project Structure

```text
.
├── Makefile
├── README.md
├── test.sh
├── data/
│   ├── snapshot.db
│   └── wal.log
├── include/
│   ├── kvstore.hpp
│   ├── persistence.hpp
│   ├── protocol.hpp
│   ├── server.hpp
│   └── threadpool.hpp
└── src/
    └── main.cpp
```

## How It Works

### 1. Storage engine
The `KVStore` class stores key-value entries in an unordered map with a `std::shared_mutex` for safe concurrent access. This allows many reads to proceed in parallel while writes remain exclusive.

### 2. Persistence
Each mutating operation is written to a Write-Ahead Log (`data/wal.log`) before acknowledgment. On startup, the database loads the last snapshot and replays the WAL to restore state.

### 3. Compaction
The persistence layer periodically creates a snapshot of the current state and truncates the WAL. This prevents the log from growing indefinitely and reduces recovery time after restarts.

### 4. Networking
The server listens on a TCP port and accepts client connections. Each connection is processed by a worker in a bounded thread pool, which avoids the overhead of a thread-per-connection model.

### 5. Command protocol
Clients talk to the server using plain text commands terminated by newline characters. The parser interprets commands like `SET`, `GET`, `DEL`, and `EXPIRE` and returns responses in text format.

## Build and Run

Clone the repository and build it:

```bash
make
```

Start the server:

```bash
./minidb 6380
```

You can also run the end-to-end durability demo:

```bash
bash test.sh
```

This script starts the server, writes keys, simulates a hard crash, restarts the service, and verifies that data survives the restart.

## Example Usage

Using `nc` or `telnet`:

```bash
nc localhost 6380
```

Then send commands:

```text
SET name minidb
GET name
EXISTS name
EXPIRE name 30
DEL name
PING
```

Example responses:

```text
+OK
minidb
+1
+OK
+PONG
```

## Architecture Overview

```text
Client --> TCP Server --> ThreadPool --> Protocol Parser --> KVStore
                                             |
                                             +--> Persistence Layer
                                                    (WAL + Snapshot)
```

## Design Notes

- `shared_mutex` is used instead of a single global mutex to improve read concurrency.
- WAL writes are flushed immediately for stronger durability guarantees.
- Snapshot compaction keeps the recovery path fast and bounded.
- The threaded server avoids resource exhaustion while still handling multiple clients.

## Limitations

This is a learning-focused implementation and intentionally keeps the scope narrow. Some limitations include:

- single-node deployment only
- no auth or encryption
- no replication
- values are restricted by the simple text protocol format
- storage is not optimized for huge-scale production workloads

## Future Improvements

Possible next steps include:

- `epoll`/non-blocking I/O for higher scalability
- RESP-style binary-safe protocol support
- LRU eviction or memory caps
- stronger WAL checksums and corruption recovery
- a lightweight CLI client

## License

This project is intended for educational and personal development use.

## Author

Shubhi Parashar
