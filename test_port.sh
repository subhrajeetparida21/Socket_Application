#!/bin/bash
DIR='/mnt/c/Users/pkone/OneDrive/Desktop/Os/Socket_Application'
cd "$DIR"
pkill -f './server' 2>/dev/null || true
pkill -f './client' 2>/dev/null || true
sleep 0.3

# Start server in background with empty stdin (server will block on fgets)
# We pipe nothing so fgets() blocks waiting - acceptor thread runs independently
./server < /dev/null > /tmp/s3.log 2>&1 &
SPID=$!
sleep 1.5

# Check if port is open
if nc -z 127.0.0.1 9001 2>/dev/null; then
    echo "PORT_OPEN"
else
    echo "PORT_CLOSED"
fi

cat /tmp/s3.log
kill $SPID 2>/dev/null
