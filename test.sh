#!/bin/bash
# Demo script: starts the server, runs a sequence of commands, kills it
# WITHOUT a clean shutdown, restarts it, and proves the data survived.
set -e
PORT=6380

cd "$(dirname "$0")"
make
rm -rf data server.log

echo "== starting server on port $PORT =="
./minidb $PORT > server.log 2>&1 &
PID=$!
sleep 1

talk() {
    exec 3<>/dev/tcp/127.0.0.1/$PORT
    printf "%s\r\n" "$1" >&3
    read -r RESPONSE <&3
    echo ">> $1"
    echo "<< $RESPONSE"
    exec 3<&- 3>&-
}

talk "SET name minidb"
talk "SET version 1.0"
talk "GET name"
talk "EXISTS version"
talk "EXPIRE name 100"
talk "DEL version"
talk "GET version"

echo ""
echo "== simulating a hard crash (kill -9, no graceful shutdown) =="
kill -9 $PID
sleep 0.5

echo "== restarting server =="
./minidb $PORT > server2.log 2>&1 &
PID2=$!
sleep 1

echo "== verifying data survived the crash =="
talk "GET name"
talk "EXISTS version"

kill -9 $PID2 2>/dev/null || true
echo ""
echo "== done. see data/wal.log and data/snapshot.db for on-disk state =="
