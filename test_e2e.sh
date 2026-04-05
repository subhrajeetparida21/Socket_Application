#!/bin/bash
# Integration test for Socket_Application
set -e

DIR='/mnt/c/Users/pkone/OneDrive/Desktop/Os/Socket_Application'
cd "$DIR"

echo "=== Cleaning up old processes ==="
pkill -f './node' 2>/dev/null || true
pkill -f './server' 2>/dev/null || true
sleep 0.5

echo "=== Starting node 1 on port 9010 ==="
echo '9010' | ./node > /tmp/node1.log 2>&1 &
NODE1_PID=$!

echo "=== Starting node 2 on port 9011 ==="
echo '9011' | ./node > /tmp/node2.log 2>&1 &
NODE2_PID=$!

sleep 1   # let nodes start up

echo "=== Starting load balancer on port 9001 with 2 nodes ==="
printf '9001\n2\n127.0.0.1\n9010\n127.0.0.1\n9011\n' | ./server > /tmp/server.log 2>&1 &
SERVER_PID=$!

sleep 1   # let LB start up

echo ""
echo "=== Running client with sample.c ==="
printf '%s\n127.0.0.1\n9001\n' "$DIR/sample.c" | ./client
CLIENT_EXIT=$?

echo ""
echo "=== Server log ==="
cat /tmp/server.log

echo ""
echo "=== Node 1 log ==="
cat /tmp/node1.log

echo ""
echo "=== Node 2 log ==="
cat /tmp/node2.log

echo ""
echo "=== Cleaning up ==="
kill $NODE1_PID $NODE2_PID $SERVER_PID 2>/dev/null || true

echo "=== Test done (client exit: $CLIENT_EXIT) ==="
