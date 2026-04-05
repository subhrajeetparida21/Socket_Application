#!/bin/bash
# E2E test: start server, wait for it to bind, then connect client,
# then send the "pick file 1" input to server AFTER client is connected.
DIR='/mnt/c/Users/pkone/OneDrive/Desktop/Os/Socket_Application'
cd "$DIR"
pkill -f './server' 2>/dev/null || true
pkill -f './client' 2>/dev/null || true
sleep 0.5

# Create a named pipe for server input so we can control timing
FIFO=/tmp/srv_fifo
rm -f "$FIFO"
mkfifo "$FIFO"

# Keep FIFO open so server doesn't get EOF before we're ready
exec 5>"$FIFO"

# Start server reading from FIFO
./server < "$FIFO" > /tmp/s.log 2>&1 &
SPID=$!
sleep 1   # wait for server to bind port

# Connect client (runs until server disconnects)
printf '127.0.0.1\n9001\n' | ./client > /tmp/c.log 2>&1 &
CPID=$!
sleep 1   # wait for client to connect

# Now send ENTER (trigger job menu) then "1" (pick first .c file)
echo ""  >&5
sleep 0.5
echo "1" >&5
sleep 3   # wait for compile+run to complete

# Close input → server will read EOF and exit
exec 5>&-
sleep 0.5

kill $SPID $CPID 2>/dev/null || true
rm -f "$FIFO"

cp /tmp/s.log "$DIR/srv_out.txt"
cp /tmp/c.log "$DIR/cli_out.txt"

echo "=== SERVER ==="
cat /tmp/s.log
echo ""
echo "=== CLIENT ==="
cat /tmp/c.log
