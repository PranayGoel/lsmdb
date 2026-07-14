#!/usr/bin/env bash
# lsmdb demo script — designed to run under:
#   asciinema rec lsmdb-demo.cast -c "bash demo.sh"
#
# Simulates live typing with pauses so the recording reads naturally.
# Set FAST=1 to skip all delays (useful for testing).

set -euo pipefail
set +m   # suppress "Killed: 9" / "Terminated" job-monitor messages

FAST="${FAST:-0}"

# ── Cleanup any leftover demo processes from a prior run ──────────────────────
for PORT in 6380 6381 7901 7902; do
  PID=$(lsof -ti tcp:$PORT 2>/dev/null | head -1 || true)
  if [[ -n "$PID" ]]; then kill -9 "$PID" 2>/dev/null || true; fi
done
rm -rf /tmp/lsmdb-demo /tmp/lsmdb-bench /tmp/lsmdb-primary /tmp/lsmdb-replica \
       /tmp/lsmdb-server.log /tmp/bench-server.log /tmp/primary.log /tmp/replica.log 2>/dev/null || true
sleep 0.3

BOLD='\033[1m'
DIM='\033[2m'
GREEN='\033[0;32m'
CYAN='\033[0;36m'
YELLOW='\033[1;33m'
RESET='\033[0m'

pause() { [[ "$FAST" == "1" ]] || sleep "${1:-1}"; }

# Print a comment line (dim, no prompt)
comment() {
  echo -e "${DIM}# $*${RESET}"
  pause 0.8
}

# Print a prompt + command, then execute it
run() {
  echo -e "${GREEN}\$${RESET} ${BOLD}$*${RESET}"
  pause 0.4
  eval "$@"
  pause 0.6
}

# Print a section header
section() {
  echo ""
  echo -e "${CYAN}${BOLD}━━━  $*  ━━━${RESET}"
  echo ""
  pause 0.5
}

# ──────────────────────────────────────────────
section "lsmdb — an LSM-tree storage engine in C++"
comment "Write-ahead log · Memtable · SSTables · Bloom filters · Compaction"
comment "Networked server · Crash recovery · Primary/replica replication"
pause 1.5

# ──────────────────────────────────────────────
section "Scene 1 — Build"
comment "cmake -S . -B build already done; rebuilding from cached objects..."
run "cmake --build build --parallel 2>&1 | tail -8"
pause 1

# ──────────────────────────────────────────────
section "Scene 2 — Test Suite"
comment "Every layer: CRC32, WAL, memtable, bloom filters, SSTables,"
comment "compaction, DB API, TCP server, crash recovery, replication"
run "ctest --test-dir build --output-on-failure 2>&1 | grep -E '(Test #|tests passed|failed)'"
pause 1.5

# ──────────────────────────────────────────────
section "Scene 3 — Wire Protocol"
comment "Start the server on port 6380"
mkdir -p /tmp/lsmdb-demo
./build/lsmdb_server /tmp/lsmdb-demo 6380 >/tmp/lsmdb-server.log 2>&1 &
SERVER_PID=$!
sleep 0.4

run "cat /tmp/lsmdb-server.log"
pause 0.5

comment "Talk to it with nc — PING, PUT, GET, DELETE"
run "printf 'PING\\r\\n' | nc 127.0.0.1 6380"
run "printf 'PUT hello world\\r\\n' | nc 127.0.0.1 6380"
run "printf 'GET hello\\r\\n' | nc 127.0.0.1 6380"
run "printf 'GET missing-key\\r\\n' | nc 127.0.0.1 6380"
run "printf 'DELETE hello\\r\\nGET hello\\r\\n' | nc 127.0.0.1 6380"
pause 1

# ──────────────────────────────────────────────
section "Scene 4 — Crash Recovery"
comment "Write 3 keys, then hard-kill the server (SIGKILL — no graceful shutdown)"
run "printf 'PUT key1 value1\\r\\nPUT key2 value2\\r\\nPUT key3 value3\\r\\n' | nc 127.0.0.1 6380"
pause 0.5

echo -e "${YELLOW}${BOLD}\$ kill -9 ${SERVER_PID}   # SIGKILL — no flush, no cleanup${RESET}"
pause 0.4
kill -9 "$SERVER_PID" 2>/dev/null || true
sleep 0.5
echo -e "${DIM}# server gone${RESET}"
pause 0.8

comment "Restart on the same data directory..."
rm -f /tmp/lsmdb-server.log
./build/lsmdb_server /tmp/lsmdb-demo 6380 >/tmp/lsmdb-server.log 2>&1 &
SERVER2_PID=$!
sleep 0.4

comment "Every acknowledged write survived — WAL replay"
run "printf 'GET key1\\r\\nGET key2\\r\\nGET key3\\r\\n' | nc 127.0.0.1 6380"
pause 1

kill "$SERVER2_PID" 2>/dev/null || true
sleep 0.2

# ──────────────────────────────────────────────
section "Scene 5 — Benchmark"
comment "16 concurrent clients, 5000 PUT+GET pairs each = 160,000 ops"
mkdir -p /tmp/lsmdb-bench
./build/lsmdb_server /tmp/lsmdb-bench 6381 >/tmp/bench-server.log 2>&1 &
BENCH_PID=$!
sleep 0.4

run "./build/lsmdb_bench 127.0.0.1 6381 16 5000"
pause 1

kill "$BENCH_PID" 2>/dev/null || true
sleep 0.2

# ──────────────────────────────────────────────
section "Scene 6 — Primary / Replica Replication"
comment "Start the primary on port 7901"
mkdir -p /tmp/lsmdb-primary /tmp/lsmdb-replica
./build/lsmdb_server /tmp/lsmdb-primary 7901 >/tmp/primary.log 2>&1 &
PRIMARY_PID=$!
sleep 0.4
run "cat /tmp/primary.log"

comment "Write two keys before the replica joins"
run "printf 'PUT alpha one\\r\\nPUT beta two\\r\\n' | nc 127.0.0.1 7901"
pause 0.5

comment "Start replica — pulls a full snapshot, then gets every write in real time"
./build/lsmdb_server /tmp/lsmdb-replica 7902 --replica-of 127.0.0.1 7901 >/tmp/replica.log 2>&1 &
REPLICA_PID=$!
sleep 0.6
run "cat /tmp/replica.log"

comment "Snapshot: alpha and beta are already there"
run "printf 'GET alpha\\r\\nGET beta\\r\\n' | nc 127.0.0.1 7902"
pause 0.5

comment "Direct writes to the replica are rejected"
run "printf 'PUT should-fail x\\r\\n' | nc 127.0.0.1 7902"
pause 0.5

comment "Write gamma to primary AFTER replica subscribed"
run "printf 'PUT gamma three\\r\\n' | nc 127.0.0.1 7901"
sleep 0.2
comment "Immediately visible on replica (live replication)"
run "printf 'GET gamma\\r\\n' | nc 127.0.0.1 7902"
pause 0.8

comment "Kill the primary outright"
echo -e "${YELLOW}${BOLD}\$ kill -9 ${PRIMARY_PID}   # primary gone${RESET}"
pause 0.4
kill -9 "$PRIMARY_PID" 2>/dev/null || true
sleep 0.4

comment "Replica is still running — all data intact"
run "printf 'GET alpha\\r\\nGET beta\\r\\nGET gamma\\r\\n' | nc 127.0.0.1 7902"
pause 1

kill "$REPLICA_PID" 2>/dev/null || true

# ──────────────────────────────────────────────
section "Done"
comment "Source, design log, and CI: https://github.com/PranayGoel/lsmdb"
echo ""

# Cleanup
rm -rf /tmp/lsmdb-demo /tmp/lsmdb-bench /tmp/lsmdb-primary /tmp/lsmdb-replica \
       /tmp/lsmdb-server.log /tmp/bench-server.log /tmp/primary.log /tmp/replica.log 2>/dev/null || true
