# Simple Client Server Code Runner in C

This version uses only two programs:

- `server`: receives a C file, compiles it, runs it, and sends output back
- `client`: sends the C file to the server and shows the output

## Build on Linux

```bash
gcc -pthread server.c common.c -o server
gcc client.c common.c -o client
```

## Run

Start the server:

```bash
./server
```

It asks for:

- server port

Start the client:

```bash
./client
```

It asks for:

- C source file path
- server IP / hostname
- client port

## Example

Run the server and press Enter to use port `9001`.

Create a file named `sample.c`:

```c
#include <stdio.h>

int main(void) {
    printf("Hello from server side execution\n");
    return 0;
}
```

Run the client and enter:

- `sample.c`
- `127.0.0.1`
- `9001`

The client will print the output returned by the server.

## Notes

- This is a simple teaching demo.
- The server compiles with `gcc`, so Linux should have `gcc` installed.
- The server executes the uploaded code locally.
- There is no sandbox or timeout in this simple version.
