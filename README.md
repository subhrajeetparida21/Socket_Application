# Simple Lab Load Balancer in C

This is a small Linux-style demo with three programs:

- `server`: accepts lab nodes and client uploads
- `node`: runs on each lab machine and reports its current system load
- `client`: uploads a C file and receives the output

## How it works

1. Each lab machine runs `node` and connects to the central `server`.
2. When a user runs `client`, the source file is sent to the `server`.
3. The `server` asks all connected nodes for their current load average.
4. The least-loaded free node is selected.
5. The chosen node compiles the C file with `gcc`, runs it, captures the output, and sends it back.
6. The `server` forwards the output to the client.

## Build on Linux

```bash
gcc -pthread server.c common.c -o server
gcc node.c common.c -o node
gcc client.c common.c -o client
```

## Run

Start the server:

```bash
./server
```

It will ask:

- node port
- client port

Start lab nodes on up to 10 Linux systems:

```bash
./node
```

It will ask:

- server IP / hostname
- node port

Submit a C file from a client machine:

```bash
./client
```

It will ask:

- C source file path
- server IP / hostname
- client port

## Default ports

- Node registration port: `9000`
- Client submission port: `9001`

You can change them:

```bash
./server
./node
./client
```

## Example test file

```c
#include <stdio.h>

int main(void) {
    printf("Hello from the lab node!\n");
    return 0;
}
```

## Simple assumptions

- Nodes are trusted Linux machines with `gcc` installed.
- Only one job runs at a time on each node.
- The server picks from currently connected free nodes only.
- This is a teaching/demo version, so it does not sandbox user code.

## Suggested improvement ideas

- Add execution timeout so infinite loops do not hang a node.
- Add security sandboxing with `chroot`, containers, or restricted users.
- Send compiler errors back in detail instead of a generic message.
- Store job history and node status in logs.
